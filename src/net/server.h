// A zone server: authoritative worlds, one UDP socket, a table of clients. It owns
// the truth and clients send it nothing but inputs.
//
// Worlds: a server hosts its zone's universe (every ship and every place, with the
// players standing in the places it owns) and any number of dungeon instances,
// each a world of its own opened for an instance key when the first ticket for it
// arrives and closed a while after the last player leaves. A client is in exactly
// one world; moving between them, within a server or across servers, is the same
// hand-off with a ticket (zones.h). An instance host (zones file: `instances`) has
// no universe and only opens instances.
//
// Per tick the server: reads every packet (inputs, joins, leaves), applies one
// queued input per client, steps every world, broadcasts what each world spawned or
// despawned to its clients (reliably), checks who walked into a place that leads
// elsewhere, and every NET_SNAPSHOT_HZ sends each client a snapshot of the players
// it is interested in, delta-coded against the last one the client acknowledged.
// Single-threaded: the scaling unit is the server process, and there can be many.
//
// Ticks follow the wall clock, not the loop: universe time is wall time minus the
// shared epoch, and the tick counter is kept where that says it should be, by
// catching up after a stall (to a point, then skipping) and by moving universe time
// itself after a real clock jump. Two servers therefore agree about the time, and
// so about every ship, without exchanging a byte.
//
// A hand-off freezes the player here (inputs ignored, physics on) the moment the
// redirect goes out, so the ticket stays true until the other world takes over; the
// client then leaves, or reports failure and is thawed, or the wait times out.
// Tickets are refused when replayed, and a player arriving by ticket replaces any
// stale connection of the same account.
#ifndef TP_SERVER_H
#define TP_SERVER_H

#include "directory.h"
#include "protocol.h"
#include "social.h"

#define SERVER_INPUT_QUEUE 64        // inputs a client may be ahead by before old ones are dropped
#define SERVER_INPUT_TARGET 2        // inputs held before consuming starts: the jitter buffer
#define SERVER_INPUT_MAX_AHEAD 8     // queued inputs beyond which the server skips to the newest
#define SERVER_HANDOFF_RETRY 3.0     // seconds before a redirect is repeated to a client still here
#define SERVER_HANDOFF_TIMEOUT 4.0   // seconds a frozen player waits for the hand-off before unfreezing
#define SERVER_MAX_CATCHUP 60        // ticks simulated in one update when behind the clock; the rest are skipped
#define SERVER_REBASE_SECONDS 5.0    // a clock jump larger than this moves universe time rather than waits
#define SERVER_TICKET_MEMORY 256     // recently accepted tickets remembered, to refuse a replay
#define SERVER_INSTANCE_LINGER 60.0  // seconds an empty instance stays open for its players to come back
#define SERVER_BANDWIDTH 32768       // bytes per second per client by default
#define SERVER_TRAIL_TICKS 64        // ticks of every player's past positions kept: about a second
#define SERVER_KEY_ROTATION 10.0     // seconds the server's half of the key exchange is reused for
#define SERVER_ACCEPTS_PER_SECOND 128   // joins handled per second; the rest retry a moment later
#define SERVER_INBOUND_PACKETS 200      // packets per second a client may send (it sends 60, and keepalives)
#define SERVER_INBOUND_BYTES 32768      // bytes per second a client may send (it sends about 2 KB)
#define SERVER_UNCONNECTED_PER_IP 10.0  // handshake packets per second from one address, with a burst of
#define SERVER_UNCONNECTED_BURST 30.0   // ...this many: a few clients behind one NAT joining together
#define SERVER_CHALLENGE_ROTATION 10.0  // seconds a challenge stays good for (twice this at most)
#define SERVER_TOKEN_MEMORY 4096        // connect tokens remembered, to refuse a replay from elsewhere
#define SERVER_SNAPSHOT_LOAD_UP 0.5     // seconds per second spent on snapshots before their rate halves
#define SERVER_SNAPSHOT_LOAD_DOWN 0.2   // ...and below which it doubles back
#define SERVER_RESELECT_EVERY 4         // snapshots a client's chosen set of players is reused for
#define SERVER_LINGER_QUIT 10.0         // seconds a body stays after its player quit
#define SERVER_LINGER_TIMEOUT 60.0      // ...after its player's connection was lost
#define SERVER_SNAPSHOT_MIN 96       // bytes a snapshot may always use: the header and ourselves

// What a client is told about, by class of entity: scenery (ships, planets, the
// places) always; players and loot within a radius of the client's player, with a
// record sent when they first come within it and a despawn once they have been
// beyond it (and a margin) for `forget` seconds. A radius of 0 means always, which
// is the default: a small world sends everything, a crowded one sets --interest.
enum server_relevance_class { RELEVANCE_SCENERY, RELEVANCE_PLAYER, RELEVANCE_LOOT, RELEVANCE_CLASS_COUNT };
struct server_relevance {
    float radius;                // metres; 0: always relevant
    double forget;               // seconds out of range before the client is told to drop it
};
#define SERVER_FORGET 5.0            // default `forget`
#define SERVER_FORGET_MARGIN 1.25    // an entity is dropped beyond radius * margin, not at the radius itself

struct server_config {
    uint16_t port;               // 0 picks a free one (tests); NET_DEFAULT_PORT otherwise
    int max_clients;
    server_relevance relevance[RELEVANCE_CLASS_COUNT];
    double timeout;              // seconds of silence before a client is dropped
    bool verbose;                // log joins and leaves to stderr
    bool trace;                  // also log every client's position once a second
    double universe_epoch;       // wall-clock second at which universe time is zero. Every zone
                                 // must agree; 0 makes universe time the Unix time itself
    // Where this server sits among the others, one of three ways: a static table
    // (`zones`, with `zone` this server's index), a directory (`directory` set, and
    // the zone name, places and kind given here), or neither: alone, owning
    // everything and hosting its own instances.
    const net_zone_table *zones;
    int zone;
    net_address directory;       // ip 0: none
    char zone_name[NET_ZONE_NAME];
    char owns[NET_ZONE_PLACES][NET_ZONE_NAME];
    int owns_count;
    bool instance_host;
    char region[NET_REGION_NAME];   // where this server is: "eu", "na"...
    uint16_t target;             // players to fill this shard to before another is preferred (0: no preference)
    net_address public_address;  // published to the directory; ip 0 publishes what it sees
    const char *secret;          // the zone secret, when not from a table
    double instance_linger;      // 0 takes SERVER_INSTANCE_LINGER
    const char *save_path;       // characters on disk, when this server runs its own backend (no directory)
    double linger_quit, linger_timeout;   // seconds a body stays after a quit / after a timeout (0: defaults)
    uint32_t bandwidth;          // bytes per second sent to each client at most; 0 takes SERVER_BANDWIDTH.
                                 // Snapshots shrink to fit (farthest players first), the baseline is paced
    uint32_t total_bandwidth;    // bytes per second sent by this server at most, shared out among its
                                 // clients (0: no total); a client's cap is the lesser of the two
    uint32_t inbound_packets, inbound_bytes;   // what a client may send per second; 0 takes the defaults
    double unconnected_per_ip;   // handshake packets per second per address; 0 takes the default, -1 none
    bool require_token;          // admit nobody without a connect token (or a ticket); set when there is
                                 // a directory to issue them
};
server_config server_config_default(void);

// HANDING_OFF: redirected to another world and frozen (inputs ignored, physics on)
// until the client confirms by leaving, reports failure, or the wait times out.
// LINGERING: the connection is gone (a quit, a timeout) but the body stays in the
// world, idle, for a while: the player may be back, and then it is theirs again,
// where they left it, with everything on it. Then it is despawned.
enum server_client_state { CLIENT_FREE, CLIENT_ACTIVE, CLIENT_HANDING_OFF, CLIENT_LEAVING, CLIENT_LINGERING };
// Why a client is detached from its body, which decides how long the body lingers.
enum server_leave { SERVER_LEAVE_QUIT, SERVER_LEAVE_TIMEOUT, SERVER_LEAVE_HANDOFF, SERVER_LEAVE_REPLACED };

// Inputs arrive a few per packet and are consumed one per tick, so movement is
// simulated for exactly the steps the client predicted.
struct input_queue {
    PlayerInput inputs[SERVER_INPUT_QUEUE];
    float view_ticks[SERVER_INPUT_QUEUE];   // what the client was rendering others at, per input
    bool present[SERVER_INPUT_QUEUE];
    uint32_t last_applied, newest;
    PlayerInput last;
    float last_view_tick;
    bool started, primed;        // primed: holding enough inputs to consume one per tick
    uint32_t starved, skipped;   // ticks without an input; inputs dropped for being late
};

// Where a player was, tick by tick, for lag compensation: a swing is judged against
// the others where the swinger saw them, which is where they were a little earlier.
// Positions are relative to the ship a player stood on, as the client draws them.
struct Trail {
    v3 positions[SERVER_TRAIL_TICKS];
    cw_entity_id aboard[SERVER_TRAIL_TICKS];
    uint32_t ticks[SERVER_TRAIL_TICKS];
};

// A change to part of an entity's record: which components (1 << net_component).
struct server_change {
    cw_entity_id id;
    uint32_t components;
};

enum server_world_kind { WORLD_ZONE, WORLD_INSTANCE };
struct server;
struct server_world {
    server *owner;
    int kind;                    // server_world_kind
    game *g;                     // the zone's universe, or an instance's dungeon
    cw_component_id c_trail;     // server-only, on every player
    char dungeon[GAME_NAME_CAPACITY];
    uint64_t key;                // instance key; 0 for the zone
    int client_count;
    double empty_since;          // when the last client left; instances close a while after
    double opened_at;
    cw_id_list spawned, despawned;   // what the last tick created and destroyed, to broadcast
    server_change *updated;      // components that changed (gear), to send to those who know the entity
    size_t updated_count, updated_capacity;
};

struct server_client {
    server_client_state state;
    net_connection connection;
    uint8_t server_public[CRYPTO_KEY_SIZE], client_public[CRYPTO_KEY_SIZE];   // this session's handshake
    server_world *world;
    cw_entity_id player;
    uint64_t account;
    char platform[ACCOUNT_PLATFORM_CAPACITY];   // the platform identity behind the account, for tickets and presence
    input_queue inputs;
    bool swinging;               // this tick's input swings
    float swing_view_tick;
    uint32_t ack_tick;           // newest snapshot the client confirmed, the delta base
    snapshot_ring sent;
    character gear;              // the player's equipment and stash, the backend's copy while here
    bool gear_known;             // false until the backend has answered; no pickups before that
    uint64_t party;              // as the social service last told us; the instance key when set
    party_view fireteam;         // ...and where its members are, for keeping them on one shard
    char region[NET_REGION_NAME];
    double last_presence;
    uint32_t *last_relevant;     // per entity id: the tick it was last relevant to this client (in a
                                 // snapshot, or within the loot radius); 0: never
    size_t last_relevant_capacity;
    uint64_t *known;             // per entity id: whether the client has been sent its record
    size_t known_words;
    cw_entity_id *selection;     // the players chosen for this client's snapshots, reused a few times
    int selection_count, selection_capacity;
    uint32_t selection_tick;
    double joined_at, redirected_at, handoff_started;
    double linger_until;         // while CLIENT_LINGERING
    int leave_reason;            // net_leave_reason from the client's MSG_DISCONNECT
    uint32_t snapshots_sent, handoffs_failed;
    uint32_t forgotten;          // records despawned for having been out of interest long enough
    uint32_t updates_sent;       // partial records sent
};

struct used_ticket {
    uint64_t signature;
    double issued;
};
// A connect token is good once, except from the address that used it (a client
// coming back after losing its connection brings the same one).
struct used_token {
    uint64_t nonce;
    net_address from;
};

struct server {
    server_config config;
    net_zone_table zones;        // the live table: copied from the config's, kept fresh by the
                                 // directory, or just this server
    char zone_name[NET_ZONE_NAME];
    double last_registered;
    uint32_t tables_received, punches_sent;
    social *local_social;        // the social service in-process, when there is no directory
    uint32_t party_states_relayed;
    game zone;                   // the universe; built only when this server is a zone
    server_world *zone_world;    // the world around `zone`, or NULL on an instance host
    server_world **worlds;
    int world_count, world_capacity;
    net_socket *socket;
    server_client *clients;      // config.max_clients slots
    int client_count;
    uint32_t next_session;
    uint64_t tick;               // every world ticks once per server tick
    double tick_base;            // universe time at server tick zero
    double last_trace;
    uint32_t ticks_skipped;      // simulation the clock made us skip (stalls longer than the catch-up)
    uint32_t rebases;            // times universe time had to move to follow a clock jump
    uint32_t instances_opened, instances_closed;
    uint32_t superseded;         // stale connections replaced by a ticketed arrival of the same account
    uint32_t swings, hits;       // swings judged, swings that landed
    uint32_t pickups;
    uint32_t reattached, lingered, finalized;
    double time_receive, time_ticks, time_snapshots, time_flush;   // seconds spent since the last trace
    uint32_t accepts_since_trace;
    used_ticket tickets[SERVER_TICKET_MEMORY];
    int next_ticket;
    used_token *tokens;          // SERVER_TOKEN_MEMORY of them, a ring
    int next_token;
    // The handshake's defences: a per-address limit on packets that have no
    // connection, and a stateless challenge (SipHash of the address and the
    // connect's contents under a key of ours, rotated) that a connect must echo
    // before it costs us anything: a spoofed address never gets an answer back.
    net_limiter unconnected;
    uint64_t challenge_key[2];
    uint32_t challenges_sent, challenges_passed, challenges_failed;
    uint32_t tokens_refused;     // connects without a valid token where one was required
    uint32_t credentials_refused;   // tickets and tokens that failed their signature or were used before
    snapshot scratch_snapshot;   // the snapshot under construction
    int *client_by_address;      // open-addressed hash of client indices by peer address
    size_t hash_capacity;
    // Our half of the key exchange: one pair per SERVER_KEY_ROTATION rather than per
    // client, so a burst of joins costs one multiplication each, not two. A leak of
    // the secret exposes only the sessions of those few seconds.
    uint8_t key_secret[CRYPTO_KEY_SIZE], key_public[CRYPTO_KEY_SIZE];
    double key_made_at;
    int accepts_this_second;
    double accepts_second_started;
    int snapshot_every;          // ticks between snapshots: 3 at 20 Hz, more under load
    double load_measured_at;
    void *candidates;            // snapshot building scratch (server.cpp's candidate array)
    size_t candidate_capacity;
    unsigned char scratch[NET_MAX_PACKET];
    unsigned char *big;          // NET_MAX_MESSAGE bytes, for the rare record larger than a packet
};

bool server_init(server *s, const server_config *config);
void server_destroy(server *s);
// Everything the server does, driven by the caller's loop. `now` is net_seconds();
// the tick itself follows the wall clock (net_wall_seconds), so that universe time
// stays in step with every other zone whatever the loop does.
void server_update(server *s, double now);
uint16_t server_port(const server *s);
// The tick the wall clock says the worlds should have reached.
uint64_t server_target_tick(const server *s);
// Universe time now.
double server_time(const server *s);
// The client whose player this is (ids are per world, so prefer the account form
// when instances are open), or NULL.
server_client *server_client_of(server *s, cw_entity_id player);
server_client *server_client_of_account(server *s, uint64_t account);
// Where a player with no ticket starts: the spawn of the first place this zone owns.
v3 server_spawn(const server *s);
// The open instance of a dungeon for a key, or NULL.
server_world *server_find_instance(server *s, const char *dungeon, uint64_t key);
// The table tickets are signed and checked with, and hand-offs are looked up in.
const net_zone_table *server_zones(const server *s);
// This server's index in that table, or -1 when it is not listed.
int server_self(const server *s);

#endif
