#pragma once
#include "base.h"
#include "osc_out.h"

// Instance-to-instance synchronization.
//
// Each Orca instance that has sync enabled binds a UDP socket and periodically
// announces its state (tempo, transport, position) on it. The first instance to
// come up is the master; everyone else follows. Followers adopt the master's
// tempo and transport, and continuously correct their frame position so that
// their grids stay aligned with the master's.
//
// The wire format is plain OSC with 32-bit int arguments, so any OSC-capable
// tool can observe or inject sync messages. Address paths are:
//
//   /orca/sync/hello   <id> <flags> <bpm> <tick> <rate_num> <rate_den> <playing> <started> <grid_ms>
//   /orca/sync/state   <id> <flags> <bpm> <tick> <rate_num> <rate_den> <playing> <started> <grid_ms>
//
// `flags` bit 0 is "I am the master" and `playing` is the sender's transport
// state. A hello is sent in reply to an unknown peer, and periodically, so a
// late-joining instance can find the master without waiting for the next state
// beat. A sender with only six values is treated as playing (older version).

enum { Osync_path_max = 32 };

typedef enum {
  Osync_role_follower = 0,
  Osync_role_master = 1,
} Osync_role;

typedef struct Osync_peer {
  U32 id;
  U64 last_seen_ms;
  bool is_master;
  bool playing;   // master's transport state
  I32 bpm;
  U32 tick_low;   // low 32 bits of the master's frame counter
  I32 rate_num;
  I32 rate_den;
  U32 grid_ms_low; // low 32 bits of the master's base-beat grid origin
  struct sockaddr_storage addr;
  socklen_t addr_len;
  struct Osync_peer *next;
} Osync_peer;

typedef struct Osync {
  Oosc_dev *dev; // owned; bound to the sync port
  U32 self_id;
  Osync_role role;
  bool enabled;
  U64 last_announce_ms;
  U64 last_master_seen_ms;
  bool have_master;
  U64 started_ms;   // when we joined; used to delay claiming master
  U32 self_grid_ms; // low 32 bits of our base-beat grid origin, for replies
  bool ever_heard;  // any sync packet at all, from anyone
  // Port we are bound to. For unicast each instance takes its own port in a
  // range starting at `port_base`; for multicast this is just `port_base`.
  U16 port;
  U16 port_base;
  bool multicast;
  Osync_peer *peers;
} Osync;

// Set up the sync socket.
//
// `bind_addr` is the address to use (e.g. "127.0.0.1" or a multicast group);
// `bind_port` is the numeric port as a string. `dest_addr`/`dest_port` give
// the destination for announcements and may be NULL to mean "same as bind".
//
// In unicast mode each instance claims its own port, starting at `bind_port`
// and walking upward, because two sockets sharing one port would have their
// incoming datagrams split between them by the kernel. In multicast mode all
// instances share the group's port, which is the normal way to reach every
// instance at once.
//
// Returns false on failure, in which case `s` is left zeroed and safe to
// deinit.
bool osync_init(Osync *s, U32 self_id, char const *bind_addr,
                char const *bind_port, char const *dest_addr,
                char const *dest_port, bool multicast);

void osync_deinit(Osync *s);

// Process any pending incoming sync packets. Call once per main-loop
// iteration. Returns true if a received packet changed the local tempo or
// transport, in which case the caller should re-derive its clock.
bool osync_poll(Osync *s);

// Send a state announcement. `playing` is the sender's transport state and
// `grid_ms_low` is the system-monotonic origin of its base (1x) beat grid, so
// followers can lock to the exact beat instead of the packet arrival time.
// `hello` selects /orca/sync/hello instead of /orca/sync/state.
void osync_announce(Osync *s, bool is_master, bool playing, I32 bpm,
                    U32 tick_low, I32 rate_num, I32 rate_den,
                    U32 grid_ms_low, bool hello);

// True if a master has been heard from recently enough to trust.
bool osync_have_master(Osync const *s);

// Master's tempo, or -1 if unknown.
I32 osync_master_bpm(Osync const *s);

// Master's frame counter (low 32 bits, zero-extended), or -1 if unknown.
I64 osync_master_tick(Osync const *s);

// Master's transport state from its most recent packet. False when no master
// is known, so a follower never starts playing on its own.
bool osync_master_playing(Osync const *s);

// Low 32 bits of the master's base-beat grid origin, or 0 if unknown.
U32 osync_master_grid_ms(Osync const *s);

// Set the low 32 bits of this instance's base-beat grid origin. It is carried
// in announcements so followers can lock to the exact beat.
void osync_set_grid_ms(Osync *s, U32 grid_ms_low);

// Number of peers currently known.
Usz osync_peer_count(Osync const *s);

// Role, for the UI.
Osync_role osync_role(Osync const *s);