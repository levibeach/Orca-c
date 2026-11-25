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
// How often to re-announce our name so peers that missed the startup packet
// (or restarted) can recover it without rejoining.
enum { Osync_name_interval_ms = 4000 };
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

U64 osync_now_ms(void) { return now_ms(); }

static char const path_hello[] = "/orca/sync/hello";
static char const path_state[] = "/orca/sync/state";
static char const path_name[] = "/orca/sync/name";
static char const path_edit[] = "/orca/grid/edit";
static char const path_chunk[] = "/orca/grid/chunk";

enum { Osync_snapshot_interval_ms = 2000 };

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
  // Snapshot rev 0 is treated as "nothing yet" by followers, so start at 1
  // to let a fresh master publish its initial grid dimensions immediately.
  s->grid_rev = 1;
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
    // Bind wildcard on the group's port. The socket opts into address/port
    // reuse so every local instance can receive the same multicast traffic.
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

static void broadcast_snapshot(Osync *s);

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

// Encode/decode the one string-bearing packet in the protocol. OSC pads every
// argument to a four-byte boundary, including the trailing NUL.
static Usz build_name_packet(char *buf, Usz size, U32 id, char const *name) {
  Usz path_len = strlen(path_name);
  Usz pos = (path_len + 4) & ~(Usz)3;
  if (pos + 8 > size)
    return 0;
  memcpy(buf, path_name, path_len + 1);
  for (Usz i = path_len + 1; i < pos; ++i)
    buf[i] = 0;
  buf[pos] = ',';
  buf[pos + 1] = 'i';
  buf[pos + 2] = 's';
  buf[pos + 3] = 0;
  pos += 4;
  U32 u = htonl(id);
  memcpy(buf + pos, &u, sizeof u);
  pos += 4;
  Usz nlen = strlen(name);
  if (nlen >= Osync_name_max)
    nlen = Osync_name_max - 1;
  if (pos + ((nlen + 4) & ~(Usz)3) > size)
    return 0;
  memcpy(buf + pos, name, nlen);
  buf[pos + nlen] = 0;
  Usz padded = (nlen + 4) & ~(Usz)3;
  memset(buf + pos + nlen, 0, padded - nlen);
  return pos + padded;
}

static bool parse_name_packet(char const *data, Usz size, U32 *out_id,
                              char *out, Usz cap) {
  if (size < 16)
    return false;
  Usz path_len = strnlen(data, size);
  if (path_len == size)
    return false;
  Usz pos = (path_len + 4) & ~(Usz)3;
  if (pos + 4 > size)
    return false;
  char const *tags = data + pos;
  if (tags[0] != ',' || tags[1] != 'i' || tags[2] != 's')
    return false;
  Usz tags_len = strnlen(tags, size - pos);
  pos += (tags_len + 4) & ~(Usz)3;
  if (pos + 8 > size)
    return false;
  U32 u;
  memcpy(&u, data + pos, sizeof u);
  *out_id = ntohl(u);
  pos += 4;
  Usz nlen = strnlen(data + pos, size - pos);
  if (nlen >= cap)
    nlen = cap - 1;
  memcpy(out, data + pos, nlen);
  out[nlen] = 0;
  return true;
}

// Send an int-only packet to one destination, or to the configured target
// when no destination is given (multicast group).
static void send_ints(Osync *s, char const *path, I32 const *vals, Usz count,
                      struct sockaddr const *dest, socklen_t dest_len) {
  if (!s->dev || !s->enabled)
    return;
  char buf[1024];
  Usz n = build_packet(buf, sizeof buf, path, vals, count);
  if (!n)
    return;
  if (dest && dest_len)
    oosc_send_datagram_to(s->dev, buf, n, dest, dest_len);
  else
    oosc_send_datagram(s->dev, buf, n);
}

static void send_name(Osync *s, struct sockaddr const *dest,
                      socklen_t dest_len) {
  if (!s->dev || !s->enabled)
    return;
  char buf[128];
  Usz n = build_name_packet(buf, sizeof buf, s->self_id, s->self_name);
  if (!n)
    return;
  if (dest && dest_len)
    oosc_send_datagram_to(s->dev, buf, n, dest, dest_len);
  else
    oosc_send_datagram(s->dev, buf, n);
}

// Broadcast an int-only packet the same way state announcements are sent.
static void broadcast_ints(Osync *s, char const *path, I32 const *vals,
                           Usz count) {
  if (s->multicast) {
    send_ints(s, path, vals, count, NULL, 0);
  } else {
    enum { Port_scan_span = 16 };
    for (U16 i = 0; i < Port_scan_span; ++i) {
      U16 port = (U16)(s->port_base + i);
      if (port == s->port)
        continue;
      struct sockaddr_in to;
      memset(&to, 0, sizeof to);
      to.sin_family = AF_INET;
      to.sin_port = htons(port);
      to.sin_addr.s_addr = s->target_ip;
      send_ints(s, path, vals, count, (struct sockaddr const *)&to, sizeof to);
    }
  }
  for (Osync_peer const *p = s->peers; p; p = p->next) {
    if (p->addr_len)
      send_ints(s, path, vals, count,
                (struct sockaddr const *)&p->addr, p->addr_len);
  }
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
  I32 vals[16];
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
  // Shared-grid presence. Appended so older peers still parse the first nine.
  vals[9] = (I32)s->self_cursor_y;
  vals[10] = (I32)s->self_cursor_x;
  vals[11] = (I32)s->self_cursor_h;
  vals[12] = (I32)s->self_cursor_w;
  vals[13] = (I32)s->self_color;
  vals[14] = (I32)s->edit_mode;
  vals[15] = (I32)s->grid_shared;
  char buf[1024];
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


Osync_role osync_role(Osync const *s) { return s->role; }

static void edit_enqueue(Osync *s, U32 author, U32 rev, Usz y, Usz x,
                         Glyph glyph) {
  if (s->edit_queue_len >= Osync_edit_queue_max) {
    s->edit_queue_head = (s->edit_queue_head + 1) % Osync_edit_queue_max;
    --s->edit_queue_len;
  }
  Usz slot = (s->edit_queue_head + s->edit_queue_len) % Osync_edit_queue_max;
  s->edit_queue[slot].peer_id = author;
  s->edit_queue[slot].rev = rev;
  s->edit_queue[slot].y = y;
  s->edit_queue[slot].x = x;
  s->edit_queue[slot].glyph = glyph;
  ++s->edit_queue_len;
}

// Master-side duplicate detection must be per author. Followers each number
// their own requests from 1; one shared counter silently drops the first edit
// from every follower after the first.
static bool seen_edit(Osync *s, U32 author, U32 seq) {
  for (Usz i = 0; i < s->seen_edit_len; ++i) {
    Usz slot = (s->seen_edit_head + i) % Osync_seen_edit_max;
    if (s->seen_edits[slot].author == author && s->seen_edits[slot].seq == seq)
      return true;
  }
  return false;
}

static void remember_edit(Osync *s, U32 author, U32 seq) {
  if (s->seen_edit_len == Osync_seen_edit_max) {
    s->seen_edit_head = (s->seen_edit_head + 1) % Osync_seen_edit_max;
    --s->seen_edit_len;
  }
  Usz slot = (s->seen_edit_head + s->seen_edit_len) % Osync_seen_edit_max;
  s->seen_edits[slot].author = author;
  s->seen_edits[slot].seq = seq;
  ++s->seen_edit_len;
}

static void chunk_enqueue(Osync *s, U32 rev, U32 index, U32 count, Usz w,
                          Usz h, char const *packed, Usz n) {
  if (n > Osync_chunk_cells)
    n = Osync_chunk_cells;
  if (s->chunk_queue_len >= Osync_chunk_queue_max) {
    s->chunk_queue_head = (s->chunk_queue_head + 1) % Osync_chunk_queue_max;
    --s->chunk_queue_len;
  }
  Usz slot = (s->chunk_queue_head + s->chunk_queue_len) % Osync_chunk_queue_max;
  s->chunk_queue[slot].rev = rev;
  s->chunk_queue[slot].index = index;
  s->chunk_queue[slot].count = count;
  s->chunk_queue[slot].width = w;
  s->chunk_queue[slot].height = h;
  s->chunk_queue[slot].n = n;
  memcpy(s->chunk_queue[slot].glyphs, packed, n);
  ++s->chunk_queue_len;
}

// Handle one incoming packet. Returns true if the caller should re-derive its
// clock (tempo or transport changed).
static bool handle_packet(Osync *s, char const *data, Usz size,
                          struct sockaddr const *src, socklen_t src_len) {
  if (size > 0) {
    Usz raw_len = strnlen(data, size);
    if (raw_len == sizeof path_name - 1 &&
        memcmp(data, path_name, sizeof path_name - 1) == 0) {
      U32 id = 0;
      char name[Osync_name_max];
      if (parse_name_packet(data, size, &id, name, sizeof name) &&
          id != s->self_id) {
        Osync_peer *p = peer_get_or_add(s, id);
        if (p) {
          memcpy(p->name, name, sizeof name);
          p->last_seen_ms = now_ms();
          if (src_len > 0 && (Usz)src_len <= sizeof p->addr) {
            memcpy(&p->addr, src, (size_t)src_len);
            p->addr_len = src_len;
          }
          if (!p->name_sent) {
            send_name(s, src, src_len);
            p->name_sent = true;
          }
        }
      }
      return false;
    }
  }

  char const *path = NULL;
  Usz path_len = 0;
  I32 vals[24]; // snapshot chunks carry 6 header ints + 16 packed-word ints
  Isz n = oosc_parse_int32s(data, size, &path, &path_len, vals,
                            ORCA_ARRAY_COUNTOF(vals));
  if (n < 2)
    return false;

  if (path_is(path, path_len, "orca/grid/edit")) {
    if (n < 8)
      return false;
    U32 rev = (U32)vals[0], broadcaster = (U32)vals[1];
    if ((U8)vals[7] != s->edit_mode)
      return false; // mixed modes cannot edit each other's grids
    U32 author = (U32)vals[2], seq = (U32)vals[6];
    if (author == s->self_id)
      return false; // our own packet, looped back
    if (s->edit_mode == Osync_edit_mode_open) {
      if (author != (U32)vals[1] || author == s->self_id)
        return false;
      edit_enqueue(s, author, rev, (Usz)vals[3], (Usz)vals[4], (char)vals[5]);
      return false;
    }
    if (s->role == Osync_role_master) {
      if (seq != 0 && seen_edit(s, author, seq))
        return false; // duplicate delivery of the same request
      if (seq != 0)
        remember_edit(s, author, seq);
      edit_enqueue(s, author, ++s->grid_rev, (Usz)vals[3], (Usz)vals[4],
                   (char)vals[5]);
      I32 out[8] = {(I32)s->grid_rev, (I32)s->self_id, (I32)author, vals[3],
                    vals[4], vals[5], (I32)seq, (I32)s->edit_mode};
      broadcast_ints(s, path_edit, out, 8);
    } else {
      Osync_peer const *m = find_master(s);
      if (!m || m->id != broadcaster)
        return false; // only the master's rebroadcast is authoritative
      edit_enqueue(s, author, rev, (Usz)vals[3], (Usz)vals[4], (char)vals[5]);
      if (rev > s->grid_rev)
        s->grid_rev = rev; // carry the revision forward if we become master
    }
    return false;
  }

  if (path_is(path, path_len, "orca/grid/chunk")) {
    if (n < 6)
      return false;
    Osync_peer const *m = find_master(s);
    if (!m || m->id != (U32)vals[0])
      return false;
    Usz ncells = (Usz)(n - 6) * 4;
    if (ncells > Osync_chunk_cells)
      ncells = Osync_chunk_cells;
    char packed[Osync_chunk_cells];
    for (Usz i = 0; i < ncells; ++i)
      packed[i] = (char)((vals[6 + i / 4] >> ((i % 4) * 8)) & 0xff);
    chunk_enqueue(s, (U32)vals[1], (U32)vals[2], (U32)vals[3], (Usz)vals[4],
                  (Usz)vals[5], packed, ncells);
    return false;
  }

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
  // Appended presence fields: older peers stop at nine values.
  if (n >= 14) {
    p->cursor_y = (Usz)vals[9];
    p->cursor_x = (Usz)vals[10];
    p->cursor_h = (Usz)vals[11];
    p->cursor_w = (Usz)vals[12];
    p->color = (U8)vals[13];
    p->cursor_seen_ms = now_ms();
  if (n >= 15)
    p->edit_mode = (U8)vals[14];
  if (n >= 16)
    p->grid_shared = vals[15] != 0;
  }
  if (src_len > 0 && (Usz)src_len <= sizeof p->addr) {
    memcpy(&p->addr, src, (size_t)src_len);
    p->addr_len = src_len;
  }
  // A peer that has not told us its name yet gets ours, so both ends of a
  // fresh session can label each other's cursor.
  if (!p->name[0] && !p->name_sent) {
    send_name(s, src, src_len);
    p->name_sent = true;
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
      if (n >= 15 && s->edit_mode != p->edit_mode) {
        s->edit_mode = p->edit_mode;
        changed = true;
      }
      } else {
        // We are the older master. Keep the peer for direct addressing but
        // do not treat it as authoritative.
        p->is_master = false;
        return changed;
      }
    } else {
      s->have_master = true;
      s->last_master_seen_ms = now_ms();
      if (n >= 15 && s->edit_mode != p->edit_mode) {
        s->edit_mode = p->edit_mode;
        changed = true;
      }
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
    send_name(s, src, src_len);
    // A newcomer needs the canonical grid immediately, not up to one period
    // later, so send a snapshot along with the hello reply.
    if (we_master && s->grid_provider) {
      broadcast_snapshot(s);
      s->last_snapshot_ms = now_ms();
    }
  }

  return changed;
}

static void broadcast_snapshot(Osync *s) {
  if (!s->grid_provider || !s->dev)
    return;
  char const *buf = NULL;
  Usz len = 0, w = 0, h = 0;
  s->grid_provider(s->grid_provider_user, &buf, &len, &w, &h);
  if (!buf || !len || !w || !h)
    return;
  U32 count = (U32)((len + Osync_chunk_cells - 1) / Osync_chunk_cells);
  if (count > Osync_chunk_queue_max)
    return; // cannot be reassembled by followers; live deltas still propagate
  for (U32 i = 0; i < count; ++i) {
    Usz first = (Usz)i * Osync_chunk_cells;
    Usz n = len - first;
    if (n > Osync_chunk_cells)
      n = Osync_chunk_cells;
    I32 vals[6 + Osync_chunk_cells / 4];
    vals[0] = (I32)s->self_id;
    vals[1] = (I32)s->grid_rev;
    vals[2] = (I32)i;
    vals[3] = (I32)count;
    vals[4] = (I32)w;
    vals[5] = (I32)h;
    for (Usz j = 0; j < Osync_chunk_cells / 4; ++j) {
      U32 packed = 0;
      for (Usz k = 0; k < 4; ++k) {
        Usz ix = first + j * 4 + k;
        U8 ch = ix < len ? (U8)buf[ix] : (U8)'.';
        packed |= (U32)ch << (8 * k);
      }
      vals[6 + j] = (I32)packed;
    }
    Usz nvals = 6 + Osync_chunk_cells / 4;
    if (s->multicast) {
      send_ints(s, path_chunk, vals, nvals, NULL, 0);
    } else {
      for (Osync_peer const *p = s->peers; p; p = p->next) {
        if (p->addr_len)
          send_ints(s, path_chunk, vals, nvals,
                    (struct sockaddr const *)&p->addr, p->addr_len);
      }
    }
  }
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
  U64 now = now_ms();
  // A name sent once at startup can be lost to a peer that was still booting;
  // repeat it periodically so every participant keeps the right label.
  if (now - s->last_name_ms >= Osync_name_interval_ms) {
    if (s->multicast) {
      send_name(s, NULL, 0);
    } else {
      for (Osync_peer const *p = s->peers; p; p = p->next) {
        if (p->addr_len)
          send_name(s, (struct sockaddr const *)&p->addr, p->addr_len);
      }
    }
    s->last_name_ms = now;
  }

  // Claim the master role only if we have never heard from anyone, or the
  // master has gone quiet. The grace period keeps a freshly started instance
  // from grabbing the role before an existing master's announcement arrives.
  bool silent_since_start = now - s->started_ms >= Osync_claim_master_delay_ms;
  if (s->role == Osync_role_follower && !osync_have_master(s) &&
      (s->ever_heard || silent_since_start)) {
    s->role = Osync_role_master;
    s->have_master = false;
    changed = true;
  }
  if (s->role == Osync_role_master && s->grid_provider &&
      now - s->last_snapshot_ms >= Osync_snapshot_interval_ms) {
    broadcast_snapshot(s);
    s->last_snapshot_ms = now;
  }

  return changed;
}

void osync_set_identity(Osync *s, char const *name, U8 color) {
  snprintf(s->self_name, sizeof s->self_name, "%s", name ? name : "");
  if (!s->self_name[0]) {
    // Unicast instances take consecutive ports, so the slot gives a stable
    // P1/P2/... label. Multicast has one shared port, so spread by id.
    unsigned slot = 1;
    if (!s->multicast && s->port >= s->port_base &&
        (unsigned)(s->port - s->port_base) < 9)
      slot = (unsigned)(s->port - s->port_base) + 1;
    else
      slot = 1 + (s->self_id % 9);
    snprintf(s->self_name, sizeof s->self_name, "P%u", slot);
  }
  s->self_color = color;
  if (!s->enabled)
    return;
  if (s->multicast) {
    send_name(s, NULL, 0);
  } else {
    for (Osync_peer const *p = s->peers; p; p = p->next) {
      if (p->addr_len)
        send_name(s, (struct sockaddr const *)&p->addr, p->addr_len);
    }
  }
}

void osync_set_cursor(Osync *s, Usz y, Usz x, Usz h, Usz w) {
  s->self_cursor_y = y;
  s->self_cursor_x = x;
  s->self_cursor_h = h;
  s->self_cursor_w = w;
}

void osync_send_cell_edit(Osync *s, Usz y, Usz x, Glyph glyph) {
  if (!s->enabled || s->role != Osync_role_follower ||
      s->edit_mode != Osync_edit_mode_protected)
    return;
  Osync_peer const *m = find_master(s);
  if (!m || !m->addr_len)
    return;
  s->last_edit_seq++;
  I32 vals[8] = {0, (I32)s->self_id, (I32)s->self_id, (I32)y, (I32)x,
                 (I32)(U8)glyph, (I32)s->last_edit_seq, (I32)s->edit_mode};
  send_ints(s, path_edit, vals, 8, (struct sockaddr const *)&m->addr,
            m->addr_len);
}

// The master applies its own edits locally, so publish them as deltas without
// queueing them for local application. Followers apply the broadcast directly.
void osync_master_cell_edit(Osync *s, Usz y, Usz x, Glyph glyph) {
  if (!s->enabled || s->role != Osync_role_master)
    return;
  I32 vals[8] = {(I32)++s->grid_rev, (I32)s->self_id, (I32)s->self_id,
                 (I32)y, (I32)x, (I32)(U8)glyph, 0, (I32)s->edit_mode};
  if (s->multicast) {
    send_ints(s, path_edit, vals, 8, NULL, 0);
    return;
  }
  for (Osync_peer const *p = s->peers; p; p = p->next) {
    if (p->addr_len)
      send_ints(s, path_edit, vals, 8, (struct sockaddr const *)&p->addr,
                p->addr_len);
  }
}

void osync_set_edit_mode(Osync *s, U8 mode) {
  s->edit_mode = mode <= Osync_edit_mode_open ? mode : Osync_edit_mode_strict;
}

void osync_open_cell_edit(Osync *s, Usz y, Usz x, Glyph glyph) {
  if (!s->enabled || s->edit_mode != Osync_edit_mode_open)
    return;
  I32 vals[8] = {(I32)++s->grid_rev, (I32)s->self_id, (I32)s->self_id,
                 (I32)y, (I32)x, (I32)(U8)glyph, 0, (I32)s->edit_mode};
  if (s->multicast) {
    send_ints(s, path_edit, vals, 8, NULL, 0);
    return;
  }
  for (Osync_peer const *p = s->peers; p; p = p->next) {
    if (p->addr_len)
      send_ints(s, path_edit, vals, 8, (struct sockaddr const *)&p->addr,
                p->addr_len);
  }
}

void osync_set_grid_provider(Osync *s,
                             void (*fn)(void *user, char const **buf, Usz *len,
                                        Usz *w, Usz *h),
                             void *user) {
  s->grid_provider = fn;
  s->grid_provider_user = user;
}

Usz osync_poll_edits(Osync *s, Osync_edit *out, Usz cap) {
  Usz n = 0;
  while (s->edit_queue_len && n < cap) {
    out[n].peer_id = s->edit_queue[s->edit_queue_head].peer_id;
    out[n].rev = s->edit_queue[s->edit_queue_head].rev;
    out[n].y = s->edit_queue[s->edit_queue_head].y;
    out[n].x = s->edit_queue[s->edit_queue_head].x;
    out[n].glyph = s->edit_queue[s->edit_queue_head].glyph;
    s->edit_queue_head = (s->edit_queue_head + 1) % Osync_edit_queue_max;
    --s->edit_queue_len;
    ++n;
  }
  return n;
}

Usz osync_poll_chunks(Osync *s, Osync_chunk *out, Usz cap) {
  Usz n = 0;
  while (s->chunk_queue_len && n < cap) {
    out[n].rev = s->chunk_queue[s->chunk_queue_head].rev;
    out[n].index = s->chunk_queue[s->chunk_queue_head].index;
    out[n].count = s->chunk_queue[s->chunk_queue_head].count;
    out[n].width = s->chunk_queue[s->chunk_queue_head].width;
    out[n].height = s->chunk_queue[s->chunk_queue_head].height;
    out[n].n = s->chunk_queue[s->chunk_queue_head].n;
    memcpy(out[n].glyphs, s->chunk_queue[s->chunk_queue_head].glyphs,
           out[n].n);
    s->chunk_queue_head = (s->chunk_queue_head + 1) % Osync_chunk_queue_max;
    --s->chunk_queue_len;
    ++n;
  }
  return n;
}

bool osync_peer_cursor(Osync const *s, U32 id, Usz *y, Usz *x, Usz *h, Usz *w,
                       char const **name) {
  U64 cutoff = now_ms() - Osync_peer_timeout_ms;
  for (Osync_peer const *p = s->peers; p; p = p->next) {
    if (p->id != id)
      continue;
    if (!p->grid_shared || p->cursor_seen_ms < cutoff)
      return false;
    *y = p->cursor_y;
    *x = p->cursor_x;
    *h = p->cursor_h;
    *w = p->cursor_w;
    *name = p->name;
    return true;
  }
  return false;
}

Osync_peer const *osync_peers(Osync const *s) { return s->peers; }