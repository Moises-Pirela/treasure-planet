// The network client: one connection to one zone server, a replica of its world in a
// CW_CLIENT game, and the local player's prediction. Headless, so it is tested
// without a window; the windowed client is a thin loop around net_session (below),
// which owns one of these at a time and two during a hand-off.
//
// What the client simulates: ships, exactly, because they follow universe time; and
// its own player, by running the real movement systems on the real inputs one tick
// ahead of the server. Every input is numbered and kept with the position it led
// to. A snapshot says which input the server had applied when it was taken, so the
// client sets its player to the server's state, replays the inputs after that one
// with game_replay_tick, and compares: an identical result costs nothing, a
// different one (a ship moved under us, a collision the server saw first) is
// corrected at once in the simulation and smoothed away in the picture through a
// view offset that decays. The replay also anchors the client's tick counter to the
// server's, which is what keeps the predicted ships where the server has them.
//
// What it does not simulate: every other player. Those arrive in snapshots and are
// kept as a short history of samples, rendered NET_INTERP_DELAY behind the server's
// clock between the two samples around that time. Samples of a player aboard a ship
// are relative to the ship, so they stand on the deck the client is predicting.
#ifndef TP_CLIENT_H
#define TP_CLIENT_H

#include "protocol.h"

#define NET_SAMPLE_HISTORY 16
#define NET_INTERP_DELAY 0.1f        // seconds behind the server: two snapshots
#define NET_PREDICTION_HISTORY 128   // inputs kept for replay: two seconds
#define NET_INITIAL_LEAD 6           // ticks a fresh client runs ahead until the first snapshot
#define NET_CORRECTION_SNAP 2.0f     // metres of misprediction beyond which the view jumps
#define NET_CORRECTION_HALF_LIFE 0.08f   // seconds for half a correction to fade
#define NET_CONNECT_RETRY 0.25
#define NET_CONNECT_TIMEOUT 5.0

enum net_client_state { NC_CONNECTING, NC_LOADING, NC_CONNECTED, NC_DISCONNECTED };

struct net_sample {
    uint32_t tick;
    cw_entity_id aboard;         // the ship `position` is relative to, or CW_INVALID_ENTITY
    v3 position;
    float yaw, pitch;
};
// Client-only component on every other player.
struct Replica {
    net_sample samples[NET_SAMPLE_HISTORY];   // a ring, ticks ascending from `head`
    int count, head;
    uint32_t last_seen_tick;
};

struct predicted_input {
    uint32_t seq;
    PlayerInput input;
    v3 position;                 // where this input left the player
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
    uint64_t account;
    float interp_delay;
    uint32_t nonce;
    double started, last_connect_sent;
    unsigned char ticket[NET_TICKET_SIZE];    // presented with MSG_CONNECT after a hand-off
    size_t ticket_size;

    // Prediction.
    cw_clock clock;
    double last_frame;
    bool clock_started;
    predicted_input history[NET_PREDICTION_HISTORY];   // by seq
    uint32_t input_seq;
    double input_sent_at[64];    // by seq, for the round trip
    v3 correction;               // view offset hiding the last misprediction
    uint32_t resyncs;            // times the tick counter had to move
    float last_error;            // metres, of the last reconciliation

    // Snapshots.
    snapshot_ring received;
    uint32_t latest_tick;        // newest snapshot held, acknowledged in every input
    uint32_t acked_input_seq;
    float rtt;                   // input sent to input seen applied in a snapshot: the network
                                 // both ways plus the server's jitter buffer and snapshot interval
    uint32_t snapshots_received, snapshots_dropped;
    double tick_offset;          // estimated server tick = now * hz + tick_offset
    bool has_offset;

    // Hand-off.
    bool redirect_pending;
    net_address redirect_address;
    unsigned char redirect_ticket[NET_TICKET_SIZE];
    size_t redirect_ticket_size;
    unsigned char scratch[NET_MAX_PACKET];
};

// `g` must be a fresh CW_CLIENT game (no universe built: it arrives from the server).
// A ticket (from a redirect) may be NULL.
bool net_client_init(net_client *c, game *g, net_address server, const unsigned char *ticket, size_t ticket_size,
                     float interp_delay);
bool net_client_connect(net_client *c, game *g, const char *address, float interp_delay);
void net_client_destroy(net_client *c);
// One frame: receives (reconciling when a snapshot arrives), runs the ticks that are
// due with `in` as the player's input, interpolates the others, flushes.
void net_client_frame(net_client *c, const PlayerInput *in, double now);
void net_client_disconnect(net_client *c);
// For rendering predicted entities between ticks.
float net_client_alpha(const net_client *c);
// Added to the local player's rendered position while a misprediction fades.
v3 net_client_view_offset(const net_client *c);
// False for a player whose snapshots stopped (out of interest, or its spawn is still
// on the way); such an entity should not be drawn.
bool net_client_entity_visible(const net_client *c, cw_entity_id id);

// ---------------------------------------------------------------------------
// A session: the player's connection across zones. Holds the current link and,
// while a redirect is in progress, the next one; the picture switches to the new
// zone's world only once that is complete, with the view offset bridging the two
// predictions, so a hand-off is a hitch of one frame at most.
// ---------------------------------------------------------------------------
struct net_session {
    game *world;
    net_client *link;
    game *next_world;
    net_client *next_link;
    float interp_delay;
    int handoffs;
    const char *failure;
};

bool net_session_init(net_session *s, const char *address, float interp_delay);
void net_session_destroy(net_session *s);
void net_session_frame(net_session *s, const PlayerInput *in, double now);
// The current link's state; NC_DISCONNECTED with `failure` set once it is over.
net_client_state net_session_state(const net_session *s);

#endif
