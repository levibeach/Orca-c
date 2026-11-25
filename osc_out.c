#include "osc_out.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>

struct Oosc_dev {
  int fd;
  // Just keep the whole list around, since juggling the strict-aliasing
  // problems with sockaddr_storage is not worth it.
  struct addrinfo *chosen;
  struct addrinfo *head;
};

Oosc_udp_create_error oosc_dev_create_udp_ex(Oosc_dev **out_ptr,
                                            char const *dest_addr,
                                            char const *dest_port,
                                            char const *bind_addr,
                                            int bind_flags, int *out_errno,
                                            bool reuse_addr);

Oosc_udp_create_error oosc_dev_create_udp(Oosc_dev **out_ptr,
                                         char const *dest_addr,
                                         char const *dest_port) {
  return oosc_dev_create_udp_ex(out_ptr, dest_addr, dest_port, NULL, 0, NULL,
                                false);
}

Oosc_udp_create_error oosc_dev_create_udp_ex(Oosc_dev **out_ptr,
                                            char const *dest_addr,
                                            char const *dest_port,
                                            char const *bind_addr,
                                            int bind_flags, int *out_errno,
                                            bool reuse_addr) {
  if (out_errno)
    *out_errno = 0;
  struct addrinfo hints = {0};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = 0;
  hints.ai_flags = AI_ADDRCONFIG;
  struct addrinfo *chosen = NULL;
  struct addrinfo *head = NULL;
  int err = getaddrinfo(dest_addr, dest_port, &hints, &head);
  if (err != 0) {
#if 0
    fprintf(stderr, "Failed to get address info, error: %d\n", errno);
#endif
    return Oosc_udp_create_error_getaddrinfo_failed;
  }
  // Special behavior: if no hostname was provided, we'll get loopback(s) from
  // getaddrinfo. Which is good. But on systems with both an ipv4 and ipv6
  // address, we might get the ipv6 address listed first. And the OSC server,
  // for example Plogue Bidule, might not support ipv6. And defaulting to the
  // ipv6 loopback wouldn't work, in that case. So if there's no hostname, and
  // we find an ipv4 address in the results, prefer to use that.
  //
  // Actually let's just prefer ipv4 completely for now
#if 0
  if (!dest_addr) {
#endif
  {
    for (struct addrinfo *a = head; a; a = a->ai_next) {
      if (a->ai_family != AF_INET)
        continue;
      chosen = a;
      break;
    }
  }
  if (!chosen)
    chosen = head;
#if 0
  for (struct addrinfo* a = head; a; a = a->ai_next) {
    char buff[INET6_ADDRSTRLEN];
    char const* str = NULL;
    if (a->ai_family == AF_INET) {
      str = inet_ntop(AF_INET, &((struct sockaddr_in*)a->ai_addr)->sin_addr,
                      buff, sizeof(buff));
    } else if (a->ai_family == AF_INET6) {
      str = inet_ntop(AF_INET6, &((struct sockaddr_in6*)a->ai_addr)->sin6_addr,
                      buff, sizeof(buff));
    }
    if (str) {
      fprintf(stderr, "ntop name: %s\n", str);
    } else {
      fprintf(stderr, "inet_ntop error\n");
    }
  }
#endif
  int udpfd =
      socket(chosen->ai_family, chosen->ai_socktype, chosen->ai_protocol);
  if (udpfd < 0) {
#if 0
    fprintf(stderr, "Failed to open UDP socket, error number: %d\n", errno);
#endif
    freeaddrinfo(head);
    if (out_errno)
      *out_errno = errno;
    return Oosc_udp_create_error_couldnt_open_socket;
  }
  // Optionally bind the socket so that we can also receive datagrams on it.
  // Used by the instance-to-instance sync feature. The output socket is left
  // unbound, as before.
  if (bind_addr) {
    struct addrinfo bhints = {0};
    bhints.ai_family = chosen->ai_family;
    bhints.ai_socktype = SOCK_DGRAM;
    bhints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
    struct addrinfo *bhead = NULL;
    // `bind_flags` nonzero means "bind to the wildcard address on dest_port"
    // instead of resolving bind_addr. Used when joining a multicast group,
    // where the socket must receive everything sent to the port regardless of
    // which local interface it arrived on.
    char const *bnode = bind_flags ? NULL : bind_addr;
    char const *bport = dest_port;
    int berr = getaddrinfo(bnode, bport, &bhints, &bhead);
    if (berr != 0 || !bhead) {
      close(udpfd);
      freeaddrinfo(head);
      if (out_errno)
        *out_errno = berr != 0 ? EADDRNOTAVAIL : EINVAL;
      return Oosc_udp_create_error_getaddrinfo_failed;
    }
    // Several instances on one machine want the same sync port. SO_REUSEADDR
    // allows the bind; SO_REUSEPORT makes multicast delivery reach every
    // socket instead of only the first one bound.
    if (reuse_addr || bind_flags) {
      int one = 1;
      setsockopt(udpfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
      setsockopt(udpfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    }
    if (bind(udpfd, bhead->ai_addr, bhead->ai_addrlen) < 0) {
#if 0
      fprintf(stderr, "Bind failed: %d\n", errno);
#endif
      if (out_errno)
        *out_errno = errno;
      freeaddrinfo(bhead);
      close(udpfd);
      freeaddrinfo(head);
      return Oosc_udp_create_error_couldnt_open_socket;
    }
    freeaddrinfo(bhead);
  }
  Oosc_dev *dev = malloc(sizeof(Oosc_dev));
  dev->fd = udpfd;
  dev->chosen = chosen;
  dev->head = head;
  *out_ptr = dev;
  return Oosc_udp_create_error_ok;
}

void oosc_dev_destroy(Oosc_dev *dev) {
  close(dev->fd);
  freeaddrinfo(dev->head);
  free(dev);
}

// Receive one datagram. Returns the number of bytes read, or -1 if nothing was
// available (or an error occurred). The sender address is stored in
// `out_src`, which must be at least sizeof(struct sockaddr_storage) bytes, and
// its length in `out_src_len`. `dev` must have been created with a bind
// address.
Isz oosc_recv_datagram(Oosc_dev *dev, void *out_buf, Usz buf_size,
                       struct sockaddr *out_src, socklen_t *out_src_len) {
  socklen_t slen = sizeof(struct sockaddr_storage);
  ssize_t res = recvfrom(dev->fd, out_buf, buf_size, MSG_DONTWAIT, out_src,
                         &slen);
  if (res < 0)
    return -1;
  if (out_src_len)
    *out_src_len = slen;
  return (Isz)res;
}

// Send a datagram to a specific address instead of the configured
// destination. Used to reply to the peer that sent us something.
void oosc_send_datagram_to(Oosc_dev *dev, char const *data, Usz size,
                           struct sockaddr const *dest, socklen_t dest_len) {
  (void)sendto(dev->fd, data, size, 0, dest, dest_len);
}

int oosc_dev_fd(Oosc_dev const *dev) { return dev->fd; }

// Parse an OSC packet into up to `max_vals` 32-bit ints. On success, returns
// the number of values written to `out_vals` (>= 0) and stores the address
// path (without its leading slash) in `out_path`/`out_path_len`. Returns -1 if
// the packet was not a recognized OSC int message.
Isz oosc_parse_int32s(char const *data, Usz size, char const **out_path,
                      Usz *out_path_len, I32 *out_vals, Usz max_vals) {
  if (size < 4 || data[0] != '/')
    return -1;
  Usz path_start = 1;
  Usz pos = 1;
  while (pos < size && data[pos] != '\0')
    ++pos;
  if (pos == size)
    return -1; // unterminated address path
  Usz path_len = pos - path_start;
  ++pos; // skip the nul
  // OSC pads each string to a 4-byte boundary relative to the start of that
  // string, not the packet. (Packets here always start with the address, so
  // the two coincide, but computing it relative is correct either way.)
  pos = (pos + 3) & ~(Usz)3;
  if (out_path)
    *out_path = data + path_start;
  if (out_path_len)
    *out_path_len = path_len;
  if (pos >= size)
    return 0; // no arguments
  if (data[pos] != ',')
    return -1;
  // The typetag string is ",iiii..." -- count the types it lists, then skip
  // past the whole string (terminator plus padding) to reach the arguments.
  Usz wanted = 0;
  Usz scan = pos + 1;
  while (scan < size && data[scan] != '\0') {
    char t = data[scan];
    if (t != 'i' && t != 'f')
      return -1;
    ++wanted;
    ++scan;
  }
  if (scan == size)
    return -1; // unterminated typetag string
  pos = (scan + 1 + 3) & ~(Usz)3;
  if (wanted > max_vals)
    return -1; // more values than the caller can hold
  Usz n = 0;
  for (; n < wanted; ++n) {
    if (pos + 4 > size)
      return -1;
    union {
      U32 u;
      I32 i;
    } pun;
    U32 u_ne;
    memcpy(&u_ne, data + pos, sizeof(u_ne));
    pun.u = ntohl(u_ne);
    out_vals[n] = pun.i;
    pos += 4;
  }
  return (Isz)n;
}

void oosc_send_datagram(Oosc_dev *dev, char const *data, Usz size) {
  ssize_t res = sendto(dev->fd, data, size, 0, dev->chosen->ai_addr,
                       dev->chosen->ai_addrlen);
  (void)res;
  // TODO handle this in UI somehow
#if 0
  if (res < 0) {
    fprintf(stderr, "UDP message send failed\n");
    exit(1);
  }
#endif
}

// Encode an OSC message holding `count` 32-bit ints into `buffer`. Returns the
// packet length, or 0 if it would not fit.
//
// OSC requires each string (address, and the typetag list as one string) to be
// null-terminated and padded with nulls to a 4-byte boundary. The typetags are
// a single string such as ",iii", not one padded string per argument.
static Usz oosc_encode_int32s(char *buffer, Usz buffer_size,
                              char const *osc_address, I32 const *vals,
                              Usz count) {
  Usz path_len = strlen(osc_address);
  Usz pos = (path_len + 1 + 3) & ~(Usz)3; // address plus its nul, padded
  Usz tags_len = 1 + count + 1;       // ',' + 'i'*count + '\0'
  Usz tags_padded = (tags_len + 3) & ~(Usz)3;
  Usz needed = pos + tags_padded + count * sizeof(I32);
  if (needed > buffer_size)
    return 0;
  memcpy(buffer, osc_address, path_len + 1);
  for (Usz i = path_len + 1; i < pos; ++i)
    buffer[i] = 0;
  buffer[pos] = ',';
  for (Usz i = 0; i < count; ++i)
    buffer[pos + 1 + i] = 'i';
  buffer[pos + 1 + count] = 0;
  for (Usz i = tags_len; i < tags_padded; ++i)
    buffer[pos + i] = 0;
  pos += tags_padded;
  for (Usz i = 0; i < count; ++i) {
    union {
      I32 i;
      U32 u;
    } pun;
    pun.i = vals[i];
    U32 u_ne = htonl(pun.u);
    memcpy(buffer + pos, &u_ne, sizeof(u_ne));
    pos += sizeof(u_ne);
  }
  return pos;
}

void oosc_send_int32s_to(Oosc_dev *dev, char const *osc_address,
                         I32 const *vals, Usz count,
                         struct sockaddr const *dest, socklen_t dest_len) {
  char buffer[2048];
  Usz n = oosc_encode_int32s(buffer, sizeof(buffer), osc_address, vals, count);
  if (!n)
    return;
  oosc_send_datagram_to(dev, buffer, n, dest, dest_len);
}

void oosc_send_int32s(Oosc_dev *dev, char const *osc_address, I32 const *vals,
                      Usz count) {
  char buffer[2048];
  Usz n = oosc_encode_int32s(buffer, sizeof(buffer), osc_address, vals, count);
  if (!n)
    return;
  oosc_send_datagram(dev, buffer, n);
}

void susnote_list_init(Susnote_list *sl) {
  sl->buffer = NULL;
  sl->count = 0;
  sl->capacity = 0;
}

void susnote_list_deinit(Susnote_list *sl) { free(sl->buffer); }

void susnote_list_clear(Susnote_list *sl) { sl->count = 0; }

void susnote_list_add_notes(Susnote_list *sl, Susnote const *restrict notes,
                            Usz added_count, Usz *restrict start_removed,
                            Usz *restrict end_removed) {
  Susnote *buffer = sl->buffer;
  Usz count = sl->count;
  Usz cap = sl->capacity;
  Usz rem = count + added_count;
  Usz needed_cap = rem + added_count;
  if (cap < needed_cap) {
    cap = needed_cap < 16 ? 16 : orca_round_up_power2(needed_cap);
    buffer = realloc(buffer, cap * sizeof(Susnote));
    sl->capacity = cap;
    sl->buffer = buffer;
  }
  *start_removed = rem;
  Usz i_in = 0;
  for (; i_in < added_count; ++i_in) {
    Susnote this_in = notes[i_in];
    for (Usz i_old = 0; i_old < count; ++i_old) {
      Susnote this_old = buffer[i_old];
      if (this_old.chan_note == this_in.chan_note) {
        buffer[i_old] = this_in;
        buffer[rem] = this_old;
        ++rem;
        goto next_in;
      }
    }
    buffer[count] = this_in;
    ++count;
  next_in:;
  }
  sl->count = count;
  *end_removed = rem;
}

void susnote_list_advance_time(Susnote_list *sl, double delta_time,
                               Usz *restrict start_removed,
                               Usz *restrict end_removed,
                               double *soonest_deadline) {
  Susnote *restrict buffer = sl->buffer;
  Usz count = sl->count;
  *end_removed = count;
  float delta_float = (float)delta_time;
  float soonest = 1.0f;
  for (Usz i = 0; i < count;) {
    Susnote sn = buffer[i];
    sn.remaining -= delta_float;
    if (sn.remaining > 0.001) {
      if (sn.remaining < soonest)
        soonest = sn.remaining;
      buffer[i].remaining = sn.remaining;
      ++i;
    } else {
      --count;
      buffer[i] = buffer[count];
      buffer[count] = sn;
    }
  }
  *start_removed = count;
  *soonest_deadline = (double)soonest;
  sl->count = count;
}

void susnote_list_remove_by_chan_mask(Susnote_list *sl, Usz chan_mask,
                                      Usz *restrict start_removed,
                                      Usz *restrict end_removed) {
  Susnote *restrict buffer = sl->buffer;
  Usz count = sl->count;
  *end_removed = count;
  for (Usz i = 0; i < count;) {
    Susnote sn = buffer[i];
    Usz chan = sn.chan_note >> 8;
    if (chan_mask & 1u << chan) {
      --count;
      buffer[i] = buffer[count];
      buffer[count] = sn;
    } else {
      ++i;
    }
  }
  *start_removed = count;
  sl->count = count;
}

double susnote_list_soonest_deadline(Susnote_list const *sl) {
  float soonest = 1.0f;
  Susnote const *buffer = sl->buffer;
  for (Usz i = 0, n = sl->count; i < n; ++i) {
    float rem = buffer[i].remaining;
    if (rem < soonest)
      soonest = rem;
  }
  return (double)soonest;
}
