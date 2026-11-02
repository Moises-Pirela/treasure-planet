// The network client: connects to a zone server, keeps a replica of its world in a
// CW_CLIENT game, and sends the local player's input. Headless, so it is tested
// without a window and the windowed client is a thin loop around it.
//
// The replica is not simulated. Entities and their components arrive reliably
// (spawns, despawns), positions arrive in unreliable snapshots, and for each moving
// entity the client keeps a short history of samples. Rendering happens
// interp_delay behind the server's clock, between two samples, so motion is smooth
// at NET_SNAPSHOT_HZ and a lost snapshot costs nothing. The cost is that the local
// player also sees their own movement one round trip plus the delay late; client
// side prediction will hide that later, and this layout (authoritative samples kept
// apart from the rendered Transform) is where it plugs in.
#ifndef TP_CLIENT_H
#define TP_CLIENT_H

#include "protocol.h"

#define NET_SAMPLE_HISTORY 16
#define NET_INTERP_DELAY 0.1f        // seconds behind the server: two snapshots
#define NET_CONNECT_RETRY 0.25
#define NET_CONNECT_TIMEOUT 5.0

enum net_client_state { NC_CONNECTING, NC_LOADING, NC_CONNECTED, NC_DISCONNECTED };

struct net_sample {
    uint32_t tick;
    v3 position;
    float yaw, pitch;
};
// Client-only component on every entity that appears in snapshots.
struct Replica {
    net_sample samples[NET_SAMPLE_HISTORY];   // a ring, ticks ascending from `head`
    int count, head;
    uint32_t last_seen_tick;
};

struct net_client {
    net_socket *socket;
    net_connection connection;
    net_address server;
    net_client_state state;
    const char *failure;         // set when state is NC_DISCONNECTED
    game *replica;               // the caller's game, role CW_CLIENT; empty until connected
    cw_component_id c_replica;
    cw_entity_id player;
    float interp_delay;
    uint32_t nonce;
    double started, last_connect_sent;

    PlayerInput pending;         // since the last MSG_INPUT; jump accumulates
    bool has_pending;
    PlayerInput history[NET_INPUT_REDUNDANCY];
    int history_count;
    uint32_t input_seq;
    double input_sent_at[64];    // by seq, for the round trip
    double last_input_sent;

    // The server's clock as seen from here: estimated tick = now * hz + tick_offset.
    double tick_offset;
    bool has_offset;
    uint32_t latest_tick;
    uint32_t acked_input_seq;
    float rtt;
    uint32_t snapshots_received;
    unsigned char scratch[NET_MAX_PACKET];
};

// `g` must be a fresh CW_CLIENT game (no universe built: it arrives from the server).
bool net_client_init(net_client *c, game *g, const char *address, float interp_delay);
void net_client_destroy(net_client *c);
// Receives, retries the connection, detects the timeout, flushes. Once per frame.
void net_client_update(net_client *c, double now);
// The local player's input this frame; sent at the tick rate, jumps never dropped.
void net_client_send_input(net_client *c, const PlayerInput *in, double now);
// Writes interpolated positions into the replica's Transforms (position and previous
// both, so any alpha renders the same point). Call before game_client_frame.
void net_client_resolve(net_client *c, double now);
void net_client_disconnect(net_client *c);
// False for an entity whose snapshots stopped (out of interest, or its spawn is still
// on the way); such an entity should not be drawn.
bool net_client_entity_visible(const net_client *c, cw_entity_id id);
double net_client_server_tick(const net_client *c, double now);

#endif
