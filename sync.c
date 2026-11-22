// For struct ip_mreq and friends under -std=c99. Must come first.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "sync.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "sokol_time.h"

// How long a peer can go unheard before we forget it.
enum { Osync_peer_timeout_ms = 6000 };
// How often to broadcast state.
enum { Osync_announce_interval_ms = 250 };
// How stale a master can be before we stop treating it as authoritative.
enum { Osync_master_timeout_ms = 3000 };
// How long to wait before deciding nobody else is the master and claiming the
// role ourselves. Must exceed the time an existing master takes to answer a
// hello, plus scheduling slack.
enum { Osync_claim_master_delay_ms = 800 };

// Use a system-wide monotonic clock. stm_now() is relative to each process's
// own stm_setup(), so its values are neither comparable between instances nor
// safe for expiry arithmetic during the first few seconds after startup.
static U64 now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (U64)ts.tv_sec * 1000u + (U64)(ts.tv_nsec / 1000000);
}

static char const path_hello[] = "/orca/sync/hello";
static char const path_state[] = "/orca/sync/state";

static bool path_is(char const *path, Usz path_len, char const *want) {
  Usz want_len = strlen(want);
  return path_len == want_len && memcmp(path, want, path_len) == 0;
}

// Resolve a numeric or named IPv4 address to network byte order. Returns
// false for hostnames that resolve only to IPv6.
static bool resolve_ipv4(char const *name, U32 *out_addr) {
  if (!name)
    return false;
  struct addrinfo hints = {0};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo *res = NULL;
  if (getaddrinfo(name, NULL, &hints, &res) != 0 || !res)
    return false;
  struct sockaddr_in const *sin = (struct sockaddr_in const *)res->ai_addr;
  *out_addr = sin->sin_addr.s_addr;
  freeaddrinfo(res);
  return true;
}

static void peer_remove(Osync *s, Osync_peer *victim) {
  Osync_peer **link = &s->peers;
  while (*link) {
    if (*link == victim) {
      *link = victim->next;
      free(victim);
      return;
    }
    link = &(*link)->next;
  }
}

bool osync_init(Osync *s, U32 self_id, char const *bind_addr,
                char const *bind_port, char const *dest_addr,
                char const *dest_port, bool multicast) {
  memset(s, 0, sizeof(*s));
  s->self_id = self_id;
  // Start as a follower. If nobody answers within the takeover delay, poll()
  // promotes us to master. This is what lets a second instance adopt the
  // first one's tempo instead of both claiming to be the clock.
  s->role = Osync_role_follower;
  s->started_ms = now_ms();
  s->last_master_seen_ms = s->started_ms;
  s->multicast = multicast;

  // A NULL destination means "talk to whoever we bound to", which for a
  // multicast group is the group itself.
  char const *daddr = dest_addr ? dest_addr : bind_addr;
  char const *dport = dest_port ? dest_port : bind_port;

  long base = strtol(bind_port, NULL, 10);
  if (base < 1 || base > 65535)
    return false;
  s->port_base = (U16)base;

  int err = 0;
  Oosc_udp_create_error oe;
  bool bound = false;

  // Remember where discovery packets should go. Unicast sweeps the configured
  // address; multicast sends to the group via the device's default target.
  if (!resolve_ipv4(daddr, &s->target_ip))
    s->target_ip = htonl(INADDR_LOOPBACK);

  if (multicast) {
    // Bind the wildcard address on the group's port. Passing NULL here skips
    // the bind entirely in oosc_dev_create_udp_ex(), leaving the socket on a
    // random port -- which is why multicast sync never worked.
    oe = oosc_dev_create_udp_ex(&s->dev, daddr, dport, "0.0.0.0", 1, &err,
                                true);
    if (oe == Oosc_udp_create_error_ok) {
      s->port = s->port_base;
      bound = true;
    }
  } else {
    // Unicast: each instance needs its own port. Two sockets bound to the
    // same port would have incoming datagrams divided between them by the
    // kernel, so an instance would only ever see part of the traffic. Probe
    // upward from the base port until one is free.
    for (U16 candidate = s->port_base; candidate != 0; ++candidate) {
      char portbuf[16];
      snprintf(portbuf, sizeof portbuf, "%u", (unsigned)candidate);
      // A non-NULL bind_addr with bind_flags == 0 binds to that address, and
      // the port comes from dest_port. Pass the candidate as both so the bind
      // lands on it. No SO_REUSEADDR: we want a port nobody else holds.
      oe = oosc_dev_create_udp_ex(&s->dev, portbuf, portbuf, "0.0.0.0", 0,
                                  &err, false);
      if (oe == Oosc_udp_create_error_ok) {
        s->port = candidate;
        bound = true;
        break;
      }
      // EADDRINUSE means another instance already took this port; try the
      // next one. Anything else is a real failure.
      if (err != EADDRINUSE)
        break;
    }
  }

  if (!bound) {
    s->dev = NULL;
    return false;
  }

  if (multicast) {
    // Join the group so this socket receives datagrams addressed to it. Use
    // the same resolved address for the group and for sends; the previous
    // code re-resolved bind_addr but ignored failures, so a bad group left a
    // bound socket that could never receive anything.
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof mreq);
    mreq.imr_multiaddr.s_addr = s->target_ip;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(oosc_dev_fd(s->dev), IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                   sizeof mreq) < 0) {
      oosc_dev_destroy(s->dev);
      s->dev = NULL;
      return false;
    }
    // Keep local loopback delivery on: two instances on this machine should
    // see each other through the group.
    unsigned char loop = 1;
    setsockopt(oosc_dev_fd(s->dev), IPPROTO_IP, IP_MULTICAST_LOOP, &loop,
               sizeof loop);
    // Default is 1, but set it explicitly so LAN delivery is not surprising.
    unsigned char ttl = 1;
    setsockopt(oosc_dev_fd(s->dev), IPPROTO_IP, IP_MULTICAST_TTL, &ttl,
               sizeof ttl);
  }

  s->enabled = true;
  return true;
}

void osync_deinit(Osync *s) {
  if (s->dev) {
    oosc_dev_destroy(s->dev);
    s->dev = NULL;
  }
  Osync_peer *p = s->peers;
  while (p) {
    Osync_peer *next = p->next;
    free(p);
    p = next;
  }
  s->peers = NULL;
  s->enabled = false;
  s->have_master = false;
}

static Osync_peer *peer_find(Osync *s, U32 id) {
  for (Osync_peer *p = s->peers; p; p = p->next) {
    if (p->id == id)
      return p;
  }
  return NULL;
}

static Osync_peer *peer_get_or_add(Osync *s, U32 id) {
  Osync_peer *p = peer_find(s, id);
  if (p)
    return p;
  p = (Osync_peer *)calloc(1, sizeof(Osync_peer));
  if (!p)
    return NULL;
  p->id = id;
  p->next = s->peers;
  s->peers = p;
  return p;
}

static void peers_drop_expired(Osync *s) {
  U64 cutoff = now_ms() - Osync_peer_timeout_ms;
  Osync_peer *p = s->peers;
  while (p) {
    Osync_peer *next = p->next;
    if (p->last_seen_ms < cutoff)
      peer_remove(s, p);
    p = next;
  }
}

// Build one OSC packet holding `vals`, with the given address path.
// Returns the packet length, or 0 if it would not fit.
static Usz build_packet(char *buf, Usz buf_size, char const *path,
                        I32 const *vals, Usz count) {
  Usz path_len = strlen(path);
  Usz pos = (path_len + 4) & ~(Usz)3; // include the nul, pad to 4
  Usz typetags = 1 + count + 1;       // ',', 'i'..., '\0'
  Usz needed = pos + ((typetags + 3) & ~(Usz)3) + count * 4;
  if (needed > buf_size)
    return 0;
  memcpy(buf, path, path_len + 1);
  for (Usz i = path_len + 1; i < pos; ++i)
    buf[i] = 0;
  buf[pos] = ',';
  for (Usz i = 0; i < count; ++i)
    buf[pos + 1 + i] = 'i';
  buf[pos + 1 + count] = 0;
  pos += (typetags + 3) & ~(Usz)3;
  for (Usz i = 0; i < count; ++i) {
    U32 u = htonl((U32)vals[i]);
    memcpy(buf + pos, &u, sizeof u);
    pos += sizeof u;
  }
  return pos;
}

// Send one state packet. `playing` is the sender's transport state, so a
// follower knows whether to run or stay paused instead of inferring it from
// packet arrival.
static void send_state(Osync *s, char const *path, bool is_master, bool playing,
                       I32 bpm, U32 tick_low, I32 rate_num, I32 rate_den,
                       U32 grid_ms_low, struct sockaddr const *dest,
                       socklen_t dest_len) {
  if (!s->dev)
    return;
  I32 vals[9];
  vals[0] = (I32)s->self_id;
  vals[1] = is_master ? 1 : 0;
  vals[2] = bpm;
  vals[3] = (I32)tick_low;
  vals[4] = rate_num;
  vals[5] = rate_den;
  vals[6] = playing ? 1 : 0;
  // The oldest instance wins master election. A pid comparison alone is
  // wrong: a newcomer with a lower pid would steal the role from the
  // instance that has already been running.
  vals[7] = (I32)s->started_ms;
  // Origin of the master's base beat grid, so followers lock to its exact
  // beat rather than to whenever the packet happened to arrive.
  vals[8] = (I32)grid_ms_low;
  char buf[256];
  Usz n = build_packet(buf, sizeof buf, path, vals, ORCA_ARRAY_COUNTOF(vals));
  if (!n)
    return;
  if (dest && dest_len)
    oosc_send_datagram_to(s->dev, buf, n, dest, dest_len);
  else
    oosc_send_datagram(s->dev, buf, n);
}

void osync_announce(Osync *s, bool is_master, bool playing, I32 bpm,
                    U32 tick_low, I32 rate_num, I32 rate_den,
                    U32 grid_ms_low, bool hello) {
  if (!s->enabled || !s->dev)
    return;
  char const *path = hello ? path_hello : path_state;

  if (s->multicast) {
    // One send reaches every instance in the group.
    send_state(s, path, is_master, playing, bpm, tick_low, rate_num, rate_den,
               grid_ms_low, NULL, 0);
  } else {
    // Unicast: sweep the port range so instances that started before us are
    // discovered, and let each one learn our address from the reply.
    enum { Port_scan_span = 16 };
    for (U16 i = 0; i < Port_scan_span; ++i) {
      U16 port = (U16)(s->port_base + i);
      if (port == s->port)
        continue; // that is us
      struct sockaddr_in to;
      memset(&to, 0, sizeof to);
      to.sin_family = AF_INET;
      to.sin_port = htons(port);
      to.sin_addr.s_addr = s->target_ip;
      send_state(s, path, is_master, playing, bpm, tick_low, rate_num, rate_den,
                 grid_ms_low, (struct sockaddr const *)&to, sizeof to);
    }
  }

  // Also reach peers we already know directly, since they may sit outside the
  // scanned range or on another host.
  for (Osync_peer const *p = s->peers; p; p = p->next) {
    if (!p->addr_len)
      continue;
    send_state(s, path, is_master, playing, bpm, tick_low, rate_num, rate_den,
               grid_ms_low, (struct sockaddr const *)&p->addr, p->addr_len);
  }
  s->last_announce_ms = now_ms();
}

static Osync_peer const *find_master(Osync const *s);

bool osync_have_master(Osync const *s) {
  // Derive this from the master peer itself. Keeping a separate flag let us
  // get stuck believing a master exists after that peer announced it was no
  // longer master: followers then waited forever without a real master.
  Osync_peer const *m = find_master(s);
  if (!m)
    return false;
  return now_ms() - m->last_seen_ms < Osync_master_timeout_ms;
}

static Osync_peer const *find_master(Osync const *s) {
  for (Osync_peer const *p = s->peers; p; p = p->next) {
    if (p->is_master)
      return p;
  }
  return NULL;
}

I32 osync_master_bpm(Osync const *s) {
  Osync_peer const *m = find_master(s);
  return m ? m->bpm : -1;
}

I64 osync_master_tick(Osync const *s) {
  Osync_peer const *m = find_master(s);
  return m ? (I64)m->tick_low : -1;
}

// The master's transport state from its most recent packet. Returns false if
// no master is known, so a follower does not start playing on its own.
bool osync_master_playing(Osync const *s) {
  Osync_peer const *m = find_master(s);
  return m ? m->playing : false;
}

U32 osync_master_grid_ms(Osync const *s) {
  Osync_peer const *m = find_master(s);
  return m ? m->grid_ms_low : 0;
}

void osync_set_grid_ms(Osync *s, U32 grid_ms_low) {
  s->self_grid_ms = grid_ms_low;
}

Usz osync_peer_count(Osync const *s) {
  Usz n = 0;
  for (Osync_peer const *p = s->peers; p; p = p->next)
    ++n;
  return n;
}

Osync_role osync_role(Osync const *s) { return s->role; }

// Handle one incoming packet. Returns true if the caller should re-derive its
// clock (tempo or transport changed).
static bool handle_packet(Osync *s, char const *data, Usz size,
                          struct sockaddr const *src, socklen_t src_len) {
  char const *path = NULL;
  Usz path_len = 0;
  I32 vals[9];
  Isz n = oosc_parse_int32s(data, size, &path, &path_len, vals,
                            ORCA_ARRAY_COUNTOF(vals));
  if (n < 6)
    return false;
  // Older senders omit later fields. Assume they are playing and tie on
  // start time so the id tiebreak still applies.
  bool peer_playing = n >= 7 ? vals[6] != 0 : true;
  U32 peer_started = n >= 8 ? (U32)vals[7] : (U32)s->started_ms;
  U32 peer_grid_ms = n >= 9 ? (U32)vals[8] : 0;
  bool is_hello = path_is(path, path_len, "orca/sync/hello");
  bool is_state = path_is(path, path_len, "orca/sync/state");
  if (!is_hello && !is_state)
    return false;

  U32 sender = (U32)vals[0];
  if (sender == s->self_id)
    return false; // our own multicast loopback
  // Any valid sync packet means the group is alive, even if this particular
  // peer is not the master.
  s->ever_heard = true;

  bool peer_is_master = vals[1] != 0;
  I32 bpm = vals[2];
  if (bpm < 1)
    return false; // nonsense; ignore rather than divide by zero later

  Osync_peer *p = peer_get_or_add(s, sender);
  if (!p)
    return false;

  p->last_seen_ms = now_ms();
  p->is_master = peer_is_master;
  p->playing = peer_playing;
  p->bpm = bpm;
  p->tick_low = (U32)vals[3];
  p->rate_num = vals[4];
  p->rate_den = vals[5] == 0 ? 1 : vals[5];
  p->grid_ms_low = peer_grid_ms;
  if (src_len > 0 && (Usz)src_len <= sizeof p->addr) {
    memcpy(&p->addr, src, (size_t)src_len);
    p->addr_len = src_len;
  }

  bool changed = false;

  if (peer_is_master) {
    // The instance that started first keeps the role. Comparing pids alone
    // lets a newcomer with a lower pid steal master from the running clock.
    // The id is only a tiebreak for simultaneous starts.
    if (s->role == Osync_role_master) {
      bool peer_senior =
          peer_started < (U32)s->started_ms ||
          (peer_started == (U32)s->started_ms && sender < s->self_id);
      if (peer_senior) {
        s->role = Osync_role_follower;
        s->have_master = true;
        s->last_master_seen_ms = now_ms();
        changed = true;
      } else {
        // We are the older master. Keep the peer for direct addressing but
        // do not treat it as authoritative.
        p->is_master = false;
        return changed;
      }
    } else {
      s->have_master = true;
      s->last_master_seen_ms = now_ms();
    }
  }

  if (is_hello) {
    // Answer so the newcomer can settle immediately instead of waiting for
    // our next periodic state packet. Only a master answers as a master, so
    // the newcomer can tell who is actually in charge.
    bool we_master = s->role == Osync_role_master;
    osync_announce(s, we_master, we_master ? peer_playing : false,
                   we_master ? bpm : 0, we_master ? (U32)vals[3] : 0, 1, 1,
                   we_master ? s->self_grid_ms : 0, false);
  }

  return changed;
}

bool osync_poll(Osync *s) {
  if (!s->enabled || !s->dev)
    return false;

  bool changed = false;
  char buf[1024];
  for (;;) {
    struct sockaddr_storage src;
    socklen_t src_len = sizeof src;
    Isz got = oosc_recv_datagram(s->dev, buf, sizeof buf,
                                 (struct sockaddr *)&src, &src_len);
    if (got < 0)
      break;
    if (handle_packet(s, buf, (Usz)got, (struct sockaddr const *)&src, src_len))
      changed = true;
  }

  peers_drop_expired(s);

  // Claim the master role only if we have never heard from anyone, or the
  // master has gone quiet. The grace period keeps a freshly started instance
  // from grabbing the role before an existing master's announcement arrives.
  U64 now = now_ms();
  bool silent_since_start = now - s->started_ms >= Osync_claim_master_delay_ms;
  if (s->role == Osync_role_follower && !osync_have_master(s) &&
      (s->ever_heard || silent_since_start)) {
    s->role = Osync_role_master;
    s->have_master = false;
    changed = true;
  }

  return changed;
}