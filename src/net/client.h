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
//
// Crafts are the same either way round. The one our player flies is predicted like
// our player (its state is set from each snapshot, exactly, and the inputs since
// are replayed); every other craft is interpolated like another player, and those
// aboard it (ourselves included, as passengers) stand in its frame, so they go
// where it is drawn. Our own position aboard a craft is predicted in its frame,
// which needs nothing of where the craft is: a passenger's prediction holds
// whoever flies.
#ifndef TP_CLIENT_H
#define TP_CLIENT_H

#include "directory.h"
#include "protocol.h"

#define NET_SAMPLE_HISTORY 16
#define NET_FLOWN_CRAFTS 4           // crafts one snapshot may rewind for our player: the one we fly
#define NET_UNSEEN_HIDE (60 * GAME_TICK_HZ)   // ticks without a snapshot before a player is hidden: everyone is drawn, far ones come round slowly
#define NET_INTERP_DELAY 0.1f        // seconds behind the server: two snapshots
#define NET_PREDICTION_HISTORY 128   // inputs kept for replay: two seconds
#define NET_INITIAL_LEAD 6           // ticks a fresh client runs ahead until the first snapshot
#define NET_CORRECTION_SNAP 2.0f     // metres of misprediction beyond which the view jumps
#define NET_CORRECTION_HALF_LIFE 0.08f   // seconds for half a correction to fade
#define NET_CONNECT_RETRY 0.25
#define NET_CONNECT_TIMEOUT 10.0      // a crowded server paces its joins
#define NET_RECONNECT_ATTEMPTS 5      // tries after a lost connection, before giving up
#define NET_RATE_GAIN 0.02f          // tick rate change per queued input away from the server's target
#define NET_RATE_LIMIT 0.10f         // the most the tick rate is bent either way
#define NET_OFFSET_WINDOW 40         // snapshots over which the server clock offset is tracked: two seconds

enum net_client_state { NC_CONNECTING, NC_LOADING, NC_CONNECTED, NC_DISCONNECTED };

struct net_sample {
    uint32_t tick;
    cw_entity_id aboard;         // the ship or craft `position` is in, or CW_INVALID_ENTITY
    v3 position, velocity;       // velocity carries a player on between rare updates
    float yaw, pitch;
    bool craft;                  // a craft's sample: it turns, too
    quat orientation;
    v3 spin;                     // in the craft's axes
};
#define NET_EXTRAPOLATE 1.0f         // seconds a player keeps walking past their last sample
// Client-only component on every other player.
struct Replica {
    net_sample samples[NET_SAMPLE_HISTORY];   // a ring, ticks ascending from `head`
    int count, head;
    uint32_t last_seen_tick;
    float flash;                 // seconds left of the flash that shows a hit landed on them
};
#define NET_HIT_FLASH 0.3f

struct net_identity {
    char platform[ACCOUNT_PLATFORM_CAPACITY];   // "local:alice", "steam:7656..." (accounts.h)
    char name[ACCOUNT_NAME_CAPACITY];
    char region[NET_REGION_NAME];               // "eu", "na"...: the shards to prefer
    unsigned char proof[NET_PROOF_CAPACITY];    // what the platform gave us to prove it
    size_t proof_size;
};
// "local:<name>" with the given name; what a client uses without a platform.
net_identity net_identity_local(const char *name);

struct predicted_input {
    uint32_t seq;
    PlayerInput input;
    float view_tick;             // the server tick the other players were rendered at
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
    char world_name[GAME_NAME_CAPACITY];   // the dungeon we are in, or "" for the universe
    float interp_delay;
    bool lightweight;
    uint32_t nonce;
    uint8_t secret[CRYPTO_KEY_SIZE], public_key[CRYPTO_KEY_SIZE];   // our half of the handshake
    net_identity identity;
    net_address introducer;      // a directory to ask for an introduction while connecting; ip 0: none
    uint32_t punches_received;   // packets the server sent to open our NAT before we connected
    double started, last_connect_sent, last_introduction_asked;
    unsigned char ticket[NET_TICKET_SIZE];    // presented with MSG_CONNECT after a hand-off
    size_t ticket_size;
    unsigned char token[NET_TOKEN_SIZE];      // ...and the directory's connect token, when it sent us
    size_t token_size;
    uint64_t challenge;          // the server's challenge to repeat; 0 until it sends one
    uint32_t challenges_received;
    char zone[NET_ZONE_NAME];    // the server's name, from the welcome

    // Prediction. The clock's dt is bent a little either way to keep the server's
    // input queue at its target: that is how the client's tick rate follows the
    // server's clock rather than its own.
    cw_clock clock;
    double last_frame;
    bool clock_started;
    uint8_t input_target;
    int8_t input_lead;           // as of the last snapshot
    float rate;                  // ticks per server tick, steered around 1
    bool act_latched;            // a swing seen this frame, kept until a tick takes it
    bool use_latched;            // ...and the use key, likewise
    predicted_input history[NET_PREDICTION_HISTORY];   // by seq
    uint32_t input_seq;
    double input_sent_at[64];    // by seq, for the round trip
    v3 correction;               // view offset hiding the last misprediction, in the frame of...
    cw_entity_id correction_frame;   // ...the craft we were aboard, or CW_INVALID_ENTITY: the world's
    uint32_t resyncs;            // times the tick counter had to move
    float last_error;            // metres, of the last reconciliation
    float craft_error;           // ...and of the craft we fly, where it had been predicted to be
    float hit_flash;             // seconds left of the flash that shows we were hit

    // Snapshots.
    snapshot_ring received;
    uint32_t latest_tick;        // newest snapshot held, acknowledged in every input
    uint32_t acked_input_seq;
    float rtt;                   // input sent to input seen applied in a snapshot: the network
                                 // both ways plus the server's jitter buffer and snapshot interval
    uint32_t snapshots_received, snapshots_dropped;
    uint32_t updates_received;   // partial records (MSG_UPDATE) applied
    // The server's clock as seen here: estimated server tick = now * hz + tick_offset.
    // Each snapshot gives one sample; the largest over the window is the one that
    // travelled fastest and so the truest, and the window lets it follow drift.
    double offsets[NET_OFFSET_WINDOW];
    int offset_count, offset_next;
    double tick_offset;
    bool has_offset;
    double render_tick;          // where the others are drawn, in server ticks
    float snapshot_interval;     // seconds between snapshots as they actually come (a loaded server slows down)

    // The party, as the social service last told us (through the server).
    party_view party;
    uint32_t party_updates;
    // Our gear, as the server has it; the last thing picked up, for the HUD.
    character gear;
    bool gear_known;
    uint32_t gear_updates;
    item last_pickup;
    float pickup_flash;

    // Hand-off. Once redirected, our inputs are sent without movement (the server
    // froze us anyway) until the other zone is showing or the attempt has failed.
    bool redirect_pending;
    net_address redirect_address;
    unsigned char redirect_ticket[NET_TICKET_SIZE];
    size_t redirect_ticket_size;
    unsigned char scratch[NET_MAX_PACKET];
};

struct net_client_options {
    net_address server;
    const unsigned char *ticket; // from a redirect, or NULL
    size_t ticket_size;
    const unsigned char *token;  // from the directory, or NULL
    size_t token_size;
    float interp_delay;          // 0 takes NET_INTERP_DELAY
    net_socket *socket;          // to connect from (taken over), or NULL to open one
    net_address introducer;      // a directory to ask to introduce us to the server (NAT); ip 0: none
    const net_identity *identity;   // NULL: a local identity with a made-up name
    bool lightweight;            // load testing: send inputs and take snapshots, but neither predict nor interpolate
};
// `g` must be a fresh CW_CLIENT game (no universe built: it arrives from the server).
bool net_client_init_options(net_client *c, game *g, const net_client_options *options);
bool net_client_init(net_client *c, game *g, net_address server, const unsigned char *ticket, size_t ticket_size,
                     float interp_delay);
bool net_client_connect(net_client *c, game *g, const char *address, float interp_delay);
void net_client_destroy(net_client *c);
// One frame: receives (reconciling when a snapshot arrives), runs the ticks that are
// due with `in` as the player's input, interpolates the others, flushes.
void net_client_frame(net_client *c, const PlayerInput *in, double now);
// Leaves, saying why (net_leave_reason): after a quit the body lingers for a while.
void net_client_disconnect(net_client *c, int reason);
// Tells the server the redirect it sent did not work out, so it unfreezes us.
void net_client_handoff_failed(net_client *c);
// Asks the social service (through the server) for a party change: a party_op and,
// for invite, kick and promote, the other player's name.
void net_client_party_request(net_client *c, int op, const char *target_name);
// The other player our look direction rests on, within `reach` metres (the capsule
// test the server uses for swings): their name into `name`, false when none.
bool net_client_player_in_view(const net_client *c, float reach, char name[ACCOUNT_NAME_CAPACITY]);
// Loot our look direction rests on within `reach` metres, false when none.
bool net_client_loot_in_view(const net_client *c, float reach, item *out);
// Asks to wear the item in stash slot `index`.
void net_client_equip(net_client *c, int index);
// For rendering predicted entities between ticks.
float net_client_alpha(const net_client *c);
// Added to the local player's rendered position while a misprediction fades, in
// the world's axes.
v3 net_client_view_offset(const net_client *c);
// False for a player whose snapshots stopped (out of interest, or its spawn is still
// on the way); such an entity should not be drawn.
bool net_client_entity_visible(const net_client *c, cw_entity_id id);
// The craft a player stands in, as this client has it: ours from our prediction,
// anyone else's from their newest sample. CW_INVALID_ENTITY when none.
cw_entity_id net_client_craft_of(const net_client *c, cw_entity_id player);

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
    net_identity identity;
    bool lightweight;
    // After a lost connection the session connects again to the same server, where
    // the body lingers for the player: the same entity, where it was. A few tries.
    int reconnects, reconnect_failures;
    // Through a directory: the place asked for is resolved to a server first, from
    // the socket the link then connects on (so the introduction opens the right hole).
    net_address directory;       // ip 0: none
    char place[NET_ZONE_NAME];
    net_socket *resolver;
    double resolve_started, last_query;
    net_zone_table table;        // the directory's last answer
    unsigned char token[NET_TOKEN_SIZE];   // ...and its connect token, presented to the zone
    size_t token_size;
    double token_at;             // when it was issued (net_seconds), to know when it is stale
    bool token_refused;          // the directory would not vouch for us
    char reconnect_zone[NET_ZONE_NAME];   // resolving a token for this zone, to reconnect to it at...
    net_address reconnect_to;
    int handoffs, handoffs_failed;
    double handoff_started;      // when the current redirect was received
    float last_handoff_seconds;  // frozen time of the last successful hand-off
    const char *failure;
};

// `identity` may be NULL for a local identity with a made-up name.
bool net_session_init(net_session *s, const char *address, float interp_delay, const net_identity *identity);
// Asks the directory at `address` which server owns `place` (NULL: the hub) and joins it.
bool net_session_init_directory(net_session *s, const char *address, const char *place, float interp_delay,
                                const net_identity *identity);
void net_session_destroy(net_session *s);
void net_session_frame(net_session *s, const PlayerInput *in, double now);
// The session's state: NC_DISCONNECTED with `failure` set once it is over, and
// never that while a hand-off is in progress.
net_client_state net_session_state(const net_session *s);
void net_session_party_request(net_session *s, int op, const char *target_name);
// The party as last heard, through whichever link is current.
const party_view *net_session_party(const net_session *s);
void net_session_equip(net_session *s, int stash_index);

#endif
