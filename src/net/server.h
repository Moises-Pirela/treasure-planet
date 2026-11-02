// A zone server: one authoritative game, one UDP socket, a table of clients. It
// owns the truth and clients send it nothing but inputs.
//
// One process runs one of these per region of the universe it hosts. Sharding is
// running more of them: each owns a disjoint set of entities (the hub, a planet) and
// hands clients over with MSG_REDIRECT when they cross a boundary. Nothing in here
// is global, so a process may also host several zones side by side.
//
// Per tick the server: reads every packet (inputs, joins, leaves), copies each
// client's latest input onto its player, steps the game, broadcasts what was spawned
// or despawned and which ships departed or arrived (reliably), and every
// NET_SNAPSHOT_HZ sends each client the positions
// of the dynamic entities it is interested in (unreliably). Single-threaded: the
// scaling unit is the zone, not the core.
#ifndef TP_SERVER_H
#define TP_SERVER_H

#include "protocol.h"

struct server_config {
    uint16_t port;               // 0 picks a free one (tests); NET_DEFAULT_PORT otherwise
    int max_clients;
    float interest_radius;       // metres: other players farther than this are left out of
                                 // a client's snapshots; 0 sends everyone. Ships always go.
    double timeout;              // seconds of silence before a client is dropped
    bool verbose;                // log joins and leaves to stderr
};
server_config server_config_default(void);

enum server_client_state { CLIENT_FREE, CLIENT_ACTIVE, CLIENT_LEAVING };

struct server_client {
    server_client_state state;
    net_connection connection;
    cw_entity_id player;
    PlayerInput input;           // newest received; applied before every tick
    uint32_t last_input_seq;
    bool has_input;
    double joined_at;
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
    double last_update;
    bool clock_started;
    cw_id_list spawned, despawned;   // what the last tick created and destroyed, to broadcast
    cw_observer *ship_changes;       // ships that departed or arrived, to broadcast
    unsigned char scratch[NET_MAX_PACKET];
};

bool server_init(server *s, const server_config *config);
void server_destroy(server *s);
// Everything the server does, driven by the caller's loop. `now` is net_seconds().
void server_update(server *s, double now);
uint16_t server_port(const server *s);
// The client whose player this is, or NULL.
server_client *server_client_of(server *s, cw_entity_id player);

#endif
