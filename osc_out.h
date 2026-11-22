#pragma once
#include "base.h"

// The receive API below needs sockaddr/socklen_t. Including the socket headers
// here is the least surprising option: every consumer of the receive API would
// need them anyway.
#include <sys/socket.h>
#include <sys/types.h>

typedef struct Oosc_dev Oosc_dev;

typedef enum {
  Oosc_udp_create_error_ok = 0,
  Oosc_udp_create_error_getaddrinfo_failed = 1,
  Oosc_udp_create_error_couldnt_open_socket = 2,
} Oosc_udp_create_error;

// If `bind_addr` is non-NULL, the socket is bound to that address (and
// `dest_port`) so that datagrams can also be received on it. `bind_addr ==
// NULL` preserves the original send-only behavior. If `bind_flags` is
// nonzero, bind to the wildcard address instead of `bind_addr`, which is what
// multicast reception needs. When `reuse_addr` is true, SO_REUSEADDR is set so
// several instances on one machine can share the port. `out_errno`, when
// non-NULL, receives errno-style detail on failure.
Oosc_udp_create_error oosc_dev_create_udp_ex(Oosc_dev **out_ptr,
                                            char const *dest_addr,
                                            char const *dest_port,
                                            char const *bind_addr,
                                            int bind_flags, int *out_errno,
                                            bool reuse_addr);
Oosc_udp_create_error oosc_dev_create_udp(Oosc_dev **out_ptr,
                                         char const *dest_addr,
                                         char const *dest_port);
void oosc_dev_destroy(Oosc_dev *dev);

// Send a raw UDP datagram.
void oosc_send_datagram(Oosc_dev *dev, char const *data, Usz size);

// Send a raw UDP datagram to a specific address. Used to reply to a peer that
// contacted us, since `oosc_send_datagram` always targets the configured
// destination.
void oosc_send_datagram_to(Oosc_dev *dev, char const *data, Usz size,
                           struct sockaddr const *dest, socklen_t dest_len);

// Send an OSC int message to a specific address. Used by the sync protocol,
// which addresses each peer individually rather than the single configured
// destination.
void oosc_send_int32s_to(Oosc_dev *dev, char const *osc_address,
                         I32 const *vals, Usz count,
                         struct sockaddr const *dest, socklen_t dest_len);

// Receive one datagram without blocking. Returns the byte count, or -1 if
// nothing was available. `out_src` must have room for a sockaddr_storage.
Isz oosc_recv_datagram(Oosc_dev *dev, void *out_buf, Usz buf_size,
                       struct sockaddr *out_src, socklen_t *out_src_len);

int oosc_dev_fd(Oosc_dev const *dev);

// Parse an OSC packet holding 32-bit ints. On success returns the number of
// values (>= 0) and optionally reports the address path; returns -1 if the
// packet is not an OSC int message we understand.
Isz oosc_parse_int32s(char const *data, Usz size, char const **out_path,
                      Usz *out_path_len, I32 *out_vals, Usz max_vals);

// Send a list/array of 32-bit integers in OSC format to the specified "osc
// address" (a path like /foo) as a UDP datagram.
void oosc_send_int32s(Oosc_dev *dev, char const *osc_address, I32 const *vals,
                      Usz count);

// Susnote is for handling MIDI note sustains -- each MIDI on event should be
// matched with a MIDI note-off event. The duration/sustain length of a MIDI
// note is specified when it is first triggered, so the orca VM itself is not
// responsible for sending the note-off event. We keep a list of currently 'on'
// notes so that they can have a matching 'off' sent at the correct time.
typedef struct {
  float remaining;
  U16 chan_note;
} Susnote;

typedef struct {
  Susnote *buffer;
  Usz count, capacity;
} Susnote_list;

void susnote_list_init(Susnote_list *sl);
void susnote_list_deinit(Susnote_list *sl);
void susnote_list_clear(Susnote_list *sl);
void susnote_list_add_notes(Susnote_list *sl, Susnote const *restrict notes,
                            Usz count, Usz *restrict start_removed,
                            Usz *restrict end_removed);
void susnote_list_advance_time(
    Susnote_list *sl, double delta_time, Usz *restrict start_removed,
    Usz *restrict end_removed,
    // 1.0 if no notes remain or none are shorter than 1.0
    double *soonest_deadline);
void susnote_list_remove_by_chan_mask(Susnote_list *sl, Usz chan_mask,
                                      Usz *restrict start_removed,
                                      Usz *restrict end_removed);

// Returns 1.0 if no notes remain or none are shorter than 1.0
double susnote_list_soonest_deadline(Susnote_list const *sl);
