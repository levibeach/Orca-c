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
// The wire format is plain OSC. Tempo/transport messages carry 32-bit int
// arguments; the name message carries one string. Address paths are:
//
//   /orca/sync/hello|state  <id> <flags> <bpm> <tick> <rate_num> <rate_den> <playing> <started> <grid_ms> [<cursor_y> <cursor_x> <cursor_h> <cursor_w> <color>]
//   /orca/sync/name         <id> <name (string)>
//   /orca/grid/edit         <rev> <broadcaster_id> <author_id> <y> <x> <glyph> <seq> <mode>
//   /orca/grid/chunk        <broadcaster_id> <rev> <index> <count> <width> <height> <packed glyphs...>
//
// Grid edits are master-authoritative: followers send cell edits, the master
// assigns increasing revisions and broadcasts them, and periodically sends a
// full snapshot in bounded chunks so late joiners and dropped packets recover.
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

// Master: followers request edits and the master owns cell state.
// Open: every participant applies its own edits and broadcasts them directly.
typedef enum {
  Osync_edit_mode_master = 0,
  Osync_edit_mode_open = 1,
} Osync_edit_mode;

// A default 73x17 grid is ~20 chunks; keep room for substantially larger ones
// so a complete snapshot can be assembled in one pass.
enum { Osync_name_max = 24, Osync_edit_queue_max = 64, Osync_chunk_cells = 64, Osync_chunk_queue_max = 256 };

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
  // Shared-grid presence. Name is sent once at startup; cursor/color ride on
  // every state packet so late joiners see existing cursors immediately.
  char name[Osync_name_max];
  bool name_sent;
  U8 color;
  U8 edit_mode; // peer's edit mode, from state announcements
  Usz cursor_y, cursor_x, cursor_h, cursor_w;
  U64 cursor_seen_ms;
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
  U32 target_ip; // network-order IPv4 used for the unicast discovery sweep
  char self_name[Osync_name_max];
  U8 self_color;
  U8 edit_mode; // local mode; announced to peers
  U64 last_name_ms;
  Usz self_cursor_y, self_cursor_x, self_cursor_h, self_cursor_w;
  // Master-authoritative grid state. The master assigns revisions and
  // periodically sends a full snapshot, so late joiners and dropped packets
  // recover without a request/ack handshake.
  U32 grid_rev;
  U32 last_edit_seq;
  // Snapshot provider, installed by the editor. Returns a pointer to a
  // row-major glyph buffer valid for the duration of the send.
  void (*grid_provider)(void *user, char const **buf, Usz *len, Usz *w, Usz *h);
  void *grid_provider_user;
  U64 last_snapshot_ms;
  struct {
    U32 rev, index, count;
    Usz width, height, n;
    char glyphs[Osync_chunk_cells];
  } chunk_queue[Osync_chunk_queue_max];
  Usz chunk_queue_head, chunk_queue_len;
  struct {
    U32 peer_id, rev;
    Usz y, x;
    Glyph glyph;
  } edit_queue[Osync_edit_queue_max];
  Usz edit_queue_head, edit_queue_len;
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


// Role, for the UI.
Osync_role osync_role(Osync const *s);

// ---- Shared-grid collaboration API ----

typedef struct {
  U32 peer_id, rev;
  Usz y, x;
  Glyph glyph;
} Osync_edit;

// Set the name/color announced to peers. Must be called after osync_init.
void osync_set_identity(Osync *s, char const *name, U8 color);

// Select master-authoritative or open peer-to-peer editing. Applies locally
// and is announced to peers on the next state packet.
void osync_set_edit_mode(Osync *s, U8 mode);
U8 osync_edit_mode(Osync const *s);

// Publish our current cursor (and selection size) in state announcements.
void osync_set_cursor(Osync *s, Usz y, Usz x, Usz h, Usz w);

// Follower -> master: request that a cell be set. The master assigns the
// revision, applies it locally and broadcasts it to everyone.
void osync_send_cell_edit(Osync *s, Usz y, Usz x, Glyph glyph);

// Master -> followers: broadcast an edit made locally by the master. The
// master already applied it to its own grid, so it is not queued locally.
void osync_master_cell_edit(Osync *s, Usz y, Usz x, Glyph glyph);

// Open mode: apply locally, then broadcast to all known peers. The receiver
// applies the packet directly; there is no master revision assignment.
void osync_open_cell_edit(Osync *s, Usz y, Usz x, Glyph glyph);

// Install the master's grid provider. Called periodically by osync_poll() to
// snapshot the canonical grid for late joiners.
void osync_set_grid_provider(Osync *s,
                             void (*fn)(void *user, char const **buf, Usz *len,
                                        Usz *w, Usz *h),
                             void *user);

// One received snapshot chunk, for the editor to assemble into a grid.
typedef struct {
  U32 rev, index, count;
  Usz width, height, n;
  char glyphs[Osync_chunk_cells];
} Osync_chunk;

// Drain queued snapshot chunks. Returns the number written to `out`.
Usz osync_poll_chunks(Osync *s, Osync_chunk *out, Usz cap);

// Drain queued edits from peers. Returns the number written to `out`.
Usz osync_poll_edits(Osync *s, Osync_edit *out, Usz cap);

// Peers with a fresh cursor, for the remote-cursor overlay and HUD legend.
bool osync_peer_cursor(Osync const *s, U32 id, Usz *y, Usz *x, Usz *h, Usz *w,
                       char const **name);

// Linked list of known peers, for the remote-cursor overlay and HUD legend.
Osync_peer const *osync_peers(Osync const *s);