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
#ifndef TP_SERVER_H
#define TP_SERVER_H

#include "protocol.h"

#define SERVER_INPUT_QUEUE 64        // inputs a client may be ahead by before old ones are dropped
#define SERVER_INPUT_TARGET 2        // inputs held before consuming starts: the jitter buffer
#define SERVER_INPUT_MAX_AHEAD 8     // queued inputs beyond which the server skips to the newest
#define SERVER_HANDOFF_RETRY 3.0     // seconds before a redirect is repeated to a client still here

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

enum server_client_state { CLIENT_FREE, CLIENT_ACTIVE, CLIENT_LEAVING };

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
    double joined_at, redirected_at;
    uint32_t snapshots_sent;
};

struct server {
    server_config config;
    game zone;                   // the authoritative game: this server's region of the universe
    net_socket *socket;
    server_client *clients;      // config.max_clients slots
    int client_count;
    uint32_t next_session;
    cw_clock clock;
    double last_update, last_trace;
    bool clock_started;
    cw_id_list spawned, despawned;   // what the last tick created and destroyed, to broadcast
    snapshot scratch_snapshot;   // the snapshot under construction
    unsigned char scratch[NET_MAX_PACKET];
};

bool server_init(server *s, const server_config *config);
void server_destroy(server *s);
// Everything the server does, driven by the caller's loop. `now` is net_seconds().
void server_update(server *s, double now);
uint16_t server_port(const server *s);
// The client whose player this is, or NULL.
server_client *server_client_of(server *s, cw_entity_id player);
// Where a player with no ticket starts: the spawn of the first place this zone owns.
v3 server_spawn(const server *s);

#endif
