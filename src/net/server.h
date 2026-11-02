// A zone server: one authoritative game, one UDP socket, a table of clients. It
// owns the truth and clients send it nothing but inputs.
//
// One process runs one of these. Sharding is running more of them: each owns some
// of the universe's places (zones.h), every one simulates the whole static universe
// and every ship (ships are a function of universe time, so they agree without
// talking), and a player who walks into a place another zone owns is handed over
// with a signed ticket. Nothing in here is global, so a process may host several.
//
// Per tick the server: reads every packet (inputs, joins, leaves), applies one
// queued input per client, steps the game, broadcasts what was spawned or despawned
// (reliably), checks who crossed into another zone's place, and every
// NET_SNAPSHOT_HZ sends each client a snapshot of the players it is interested in,
// delta-coded against the last one the client acknowledged. Single-threaded: the
// scaling unit is the zone, not the core.
//
// Ticks follow the wall clock, not the loop: universe time is wall time minus the
// shared epoch, and the tick counter is kept where that says it should be, by
// catching up after a stall (to a point, then skipping) and by moving universe time
// itself after a real clock jump. Two zones therefore agree about the time, and so
// about every ship, without exchanging a byte.
//
// A hand-off freezes the player here (inputs ignored, physics on) the moment the
// redirect goes out, so the ticket stays true until the other zone takes over; the
// client then leaves, or reports failure and is thawed, or the wait times out.
// Tickets are refused when replayed, and a player arriving by ticket replaces any
// stale connection of the same account.
#ifndef TP_SERVER_H
#define TP_SERVER_H

#include "protocol.h"

#define SERVER_INPUT_QUEUE 64        // inputs a client may be ahead by before old ones are dropped
#define SERVER_INPUT_TARGET 2        // inputs held before consuming starts: the jitter buffer
#define SERVER_INPUT_MAX_AHEAD 8     // queued inputs beyond which the server skips to the newest
#define SERVER_HANDOFF_RETRY 3.0     // seconds before a redirect is repeated to a client still here
#define SERVER_HANDOFF_TIMEOUT 4.0   // seconds a frozen player waits for the hand-off before unfreezing
#define SERVER_MAX_CATCHUP 60        // ticks simulated in one update when behind the clock; the rest are skipped
#define SERVER_REBASE_SECONDS 5.0    // a clock jump larger than this moves universe time rather than waits
#define SERVER_TICKET_MEMORY 256     // recently accepted tickets remembered, to refuse a replay

struct server_config {
    uint16_t port;               // 0 picks a free one (tests); NET_DEFAULT_PORT otherwise
    int max_clients;
    float interest_radius;       // metres: other players farther than this are left out of
                                 // a client's snapshots; 0 sends everyone
    double timeout;              // seconds of silence before a client is dropped
    bool verbose;                // log joins and leaves to stderr
    bool trace;                  // also log every client's position once a second
    double universe_epoch;       // wall-clock second at which universe time is zero. Every zone
                                 // must agree; 0 makes universe time the Unix time itself
    const net_zone_table *zones; // NULL: one zone owning everything
    int zone;                    // this server's index in the table
};
server_config server_config_default(void);

// HANDING_OFF: redirected to another zone and frozen (inputs ignored, physics on) until
// the client confirms by leaving, reports failure, or the wait times out.
enum server_client_state { CLIENT_FREE, CLIENT_ACTIVE, CLIENT_HANDING_OFF, CLIENT_LEAVING };

// Inputs arrive a few per packet and are consumed one per tick, so movement is
// simulated for exactly the steps the client predicted.
struct input_queue {
    PlayerInput inputs[SERVER_INPUT_QUEUE];
    bool present[SERVER_INPUT_QUEUE];
    uint32_t last_applied, newest;
    PlayerInput last;
    bool started, primed;        // primed: holding enough inputs to consume one per tick
    uint32_t starved, skipped;   // ticks without an input; inputs dropped for being late
};

struct server_client {
    server_client_state state;
    net_connection connection;
    cw_entity_id player;
    uint64_t account;
    input_queue inputs;
    uint32_t ack_tick;           // newest snapshot the client confirmed, the delta base
    snapshot_ring sent;
    double joined_at, redirected_at, handoff_started;
    uint32_t snapshots_sent, handoffs_failed;
};

struct used_ticket {
    uint64_t signature;
    double issued;
};

struct server {
    server_config config;
    game zone;                   // the authoritative game: this server's region of the universe
    net_socket *socket;
    server_client *clients;      // config.max_clients slots
    int client_count;
    uint32_t next_session;
    double last_trace;
    uint32_t ticks_skipped;      // simulation the clock made us skip (stalls longer than the catch-up)
    uint32_t rebases;            // times universe time had to move to follow a clock jump
    cw_id_list spawned, despawned;   // what the last tick created and destroyed, to broadcast
    used_ticket tickets[SERVER_TICKET_MEMORY];
    int next_ticket;
    snapshot scratch_snapshot;   // the snapshot under construction
    unsigned char scratch[NET_MAX_PACKET];
};

bool server_init(server *s, const server_config *config);
void server_destroy(server *s);
// Everything the server does, driven by the caller's loop. `now` is net_seconds();
// the tick itself follows the wall clock (net_wall_seconds), so that universe time
// stays in step with every other zone whatever the loop does.
void server_update(server *s, double now);
// The tick the wall clock says the world should have reached.
uint64_t server_target_tick(const server *s);
uint16_t server_port(const server *s);
// The client whose player this is, or NULL.
server_client *server_client_of(server *s, cw_entity_id player);
// Where a player with no ticket starts: the spawn of the first place this zone owns.
v3 server_spawn(const server *s);

#endif
