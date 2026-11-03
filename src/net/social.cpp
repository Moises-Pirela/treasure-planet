#include "social.h"
#include "protocol.h"
#include <stdio.h>
#include <string.h>

const char *party_result_string(int result) {
    switch (result) {
    case PARTY_OK: return "ok";
    case PARTY_ERROR_UNKNOWN_PLAYER: return "no such player online";
    case PARTY_ERROR_FULL: return "the party is full";
    case PARTY_ERROR_NOT_LEADER: return "only the leader may do that";
    case PARTY_ERROR_ALREADY_IN_ONE: return "already in a party";
    case PARTY_ERROR_NO_INVITE: return "no invitation to accept";
    case PARTY_ERROR_NOT_IN_PARTY: return "not in a party";
    case PARTY_ERROR_NOT_A_MEMBER: return "not a member of your party";
    default: return "?";
    }
}

// ---------------------------------------------------------------------------
// Encoding: u16 body size, body, u64 SipHash(body) for the signed messages.
// ---------------------------------------------------------------------------
static void secret_keys(const net_zone_table *zones, uint64_t *k0, uint64_t *k1) {
    uint64_t a = 0x243f6a8885a308d3ull, b = 0x13198a2e03707344ull;   // distinct from tickets and the directory
    for (const char *c = zones->secret; *c; ++c) {
        a = (a ^ (unsigned char)*c) * 0x100000001b3ull;
        b = (b ^ (unsigned char)*c) * 0x100000001b3ull;
    }
    *k0 = a;
    *k1 = b;
}
static void write_text(cw_writer *w, const char *text, size_t capacity) {
    uint8_t length = (uint8_t)strnlen(text, capacity - 1);
    cw_writer_u8(w, length);
    cw_writer_bytes(w, text, length);
}
static bool read_text(cw_reader *r, char *text, size_t capacity) {
    uint8_t length = 0;
    cw_reader_u8(r, &length);
    if (length >= capacity) return false;
    memset(text, 0, capacity);
    return cw_reader_bytes(r, text, length);
}
static void write_signed(cw_writer *w, const unsigned char *body, size_t size, const net_zone_table *secret) {
    uint64_t k0, k1;
    secret_keys(secret, &k0, &k1);
    cw_writer_u16(w, (uint16_t)size);
    cw_writer_bytes(w, body, size);
    cw_writer_u64(w, net_siphash(body, size, k0, k1));
}
static bool read_signed(cw_reader *r, cw_reader *body, const net_zone_table *secret) {
    uint16_t size = 0;
    cw_reader_u16(r, &size);
    const unsigned char *data = (const unsigned char *)cw_reader_view(r, size);
    uint64_t signature = 0;
    if (!data || !cw_reader_u64(r, &signature)) return false;
    uint64_t k0, k1;
    secret_keys(secret, &k0, &k1);
    if (net_siphash(data, size, k0, k1) != signature) return false;
    *body = cw_reader_make(data, size);
    return true;
}

void social_presence_write(cw_writer *w, uint32_t session, uint64_t account, const char *name, const char *platform,
                           const char *server, const char *world, int kind, const net_zone_table *secret) {
    unsigned char body[256];
    cw_writer b;
    cw_writer_init_fixed(&b, body, sizeof(body));
    cw_writer_u32(&b, session);
    cw_writer_u64(&b, account);
    write_text(&b, name, ACCOUNT_NAME_CAPACITY);
    write_text(&b, platform, ACCOUNT_PLATFORM_CAPACITY);
    write_text(&b, server, NET_ZONE_NAME);
    write_text(&b, world, NET_ZONE_NAME);
    cw_writer_u8(&b, (uint8_t)kind);
    write_signed(w, body, b.size, secret);
}

void social_party_op_write(cw_writer *w, uint64_t account, int op, const char *target_name, const net_zone_table *secret) {
    unsigned char body[64];
    cw_writer b;
    cw_writer_init_fixed(&b, body, sizeof(body));
    cw_writer_u64(&b, account);
    cw_writer_u8(&b, (uint8_t)op);
    write_text(&b, target_name ? target_name : "", ACCOUNT_NAME_CAPACITY);
    write_signed(w, body, b.size, secret);
}

void party_view_write(cw_writer *w, const party_view *view) {
    cw_writer_u64(w, view->party);
    cw_writer_u8(w, (uint8_t)view->count);
    for (int i = 0; i < view->count; ++i) {
        const party_member_view *m = &view->members[i];
        cw_writer_u64(w, m->account);
        write_text(w, m->name, ACCOUNT_NAME_CAPACITY);
        write_text(w, m->world, NET_ZONE_NAME);
        write_text(w, m->server, NET_ZONE_NAME);
        cw_writer_u8(w, (uint8_t)((m->online ? 1 : 0) | (m->leader ? 2 : 0)));
    }
    cw_writer_u64(w, view->invite);
    write_text(w, view->invited_by, ACCOUNT_NAME_CAPACITY);
    cw_writer_u8(w, view->last_result);
}
bool party_view_read(cw_reader *r, party_view *view) {
    memset(view, 0, sizeof(*view));
    uint8_t count = 0;
    cw_reader_u64(r, &view->party);
    cw_reader_u8(r, &count);
    if (count > SOCIAL_PARTY_SIZE) return false;
    view->count = count;
    for (int i = 0; i < view->count; ++i) {
        party_member_view *m = &view->members[i];
        uint8_t flags = 0;
        cw_reader_u64(r, &m->account);
        if (!read_text(r, m->name, ACCOUNT_NAME_CAPACITY) || !read_text(r, m->world, NET_ZONE_NAME)
            || !read_text(r, m->server, NET_ZONE_NAME)) return false;
        cw_reader_u8(r, &flags);
        m->online = (flags & 1) != 0;
        m->leader = (flags & 2) != 0;
    }
    cw_reader_u64(r, &view->invite);
    if (!read_text(r, view->invited_by, ACCOUNT_NAME_CAPACITY)) return false;
    cw_reader_u8(r, &view->last_result);
    return cw_reader_ok(r);
}

void social_party_state_write(cw_writer *w, uint64_t account, const party_view *view, const net_zone_table *secret) {
    unsigned char body[512];
    cw_writer b;
    cw_writer_init_fixed(&b, body, sizeof(body));
    cw_writer_u64(&b, account);
    party_view_write(&b, view);
    write_signed(w, body, b.size, secret);
}
bool social_party_state_read(cw_reader *r, uint64_t *account, party_view *view, const net_zone_table *secret) {
    cw_reader body;
    if (!read_signed(r, &body, secret)) return false;
    cw_reader_u64(&body, account);
    return party_view_read(&body, view);
}

static social_player *player_of(social *s, uint64_t account);

void social_character_get_write(cw_writer *w, uint64_t account, const net_zone_table *secret) {
    unsigned char body[8];
    cw_writer b;
    cw_writer_init_fixed(&b, body, sizeof(body));
    cw_writer_u64(&b, account);
    write_signed(w, body, b.size, secret);
}
void social_character_write(cw_writer *w, uint64_t account, const character *c, const net_zone_table *secret) {
    unsigned char body[16 + (SLOT_COUNT + STASH_SIZE) * 12];
    cw_writer b;
    cw_writer_init_fixed(&b, body, sizeof(body));
    cw_writer_u64(&b, account);
    character_write(&b, c);
    write_signed(w, body, b.size, secret);
}
bool social_character_read(cw_reader *r, uint64_t *account, character *c, const net_zone_table *secret) {
    cw_reader body;
    if (!read_signed(r, &body, secret)) return false;
    cw_reader_u64(&body, account);
    return character_read(&body, c);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
void social_set_save_path(social *s, const char *path) {
    strncpy(s->save_path, path ? path : "", sizeof(s->save_path) - 1);
}

// "TPSV", u32 version, u32 count, then per player: u64 account, name, platform, character.
bool social_save(social *s) {
    if (!s->save_path[0]) return false;
    cw_writer w;
    cw_writer_init(&w);
    cw_writer_bytes(&w, "TPSV", 4);
    cw_writer_u32(&w, SOCIAL_SAVE_VERSION);
    size_t count_at = w.size;
    cw_writer_u32(&w, 0);
    uint32_t count = 0;
    for (int i = 0; i < SOCIAL_MAX_PLAYERS; ++i) {
        const social_player *p = &s->players[i];
        if (!p->used) continue;
        cw_writer_u64(&w, p->account);
        write_text(&w, p->name, ACCOUNT_NAME_CAPACITY);
        write_text(&w, p->platform, ACCOUNT_PLATFORM_CAPACITY);
        character_write(&w, &p->gear);
        ++count;
    }
    cw_writer_patch_u32(&w, count_at, count);
    cw_file_error error;
    bool ok = cw_file_write(s->save_path, w.data, w.size, &error);
    cw_writer_destroy(&w);
    if (ok) {
        s->dirty = false;
        s->last_saved = net_seconds();
    }
    return ok;
}

bool social_load(social *s, const char *path) {
    cw_buffer buffer;
    cw_file_error error;
    if (!cw_file_read(path, CW_FILE_DEFAULT_MAX_SIZE, &buffer, &error)) return false;
    cw_reader r = cw_reader_make(buffer.data, buffer.size);
    char magic[4];
    uint32_t version = 0, count = 0;
    cw_reader_bytes(&r, magic, 4);
    cw_reader_u32(&r, &version);
    cw_reader_u32(&r, &count);
    bool ok = cw_reader_ok(&r) && memcmp(magic, "TPSV", 4) == 0 && version == SOCIAL_SAVE_VERSION;
    for (uint32_t i = 0; ok && i < count; ++i) {
        uint64_t account = 0;
        char name[ACCOUNT_NAME_CAPACITY], platform[ACCOUNT_PLATFORM_CAPACITY];
        character gear;
        cw_reader_u64(&r, &account);
        ok = read_text(&r, name, sizeof(name)) && read_text(&r, platform, sizeof(platform)) && character_read(&r, &gear);
        if (!ok) break;
        social_player *p = player_of(s, account);
        if (!p) {
            for (int k = 0; k < SOCIAL_MAX_PLAYERS && !p; ++k)
                if (!s->players[k].used) p = &s->players[k];
            if (!p) break;
            memset(p, 0, sizeof(*p));
            p->used = true;
            p->account = account;
        }
        memcpy(p->name, name, sizeof(p->name));
        memcpy(p->platform, platform, sizeof(p->platform));
        p->gear = gear;
    }
    cw_buffer_free(&buffer);
    return ok;
}

character *social_character(social *s, uint64_t account, const char *name) {
    social_player *p = player_of(s, account);
    if (!p) {
        for (int i = 0; i < SOCIAL_MAX_PLAYERS && !p; ++i)
            if (!s->players[i].used) p = &s->players[i];
        if (!p) return NULL;
        memset(p, 0, sizeof(*p));
        p->used = true;
        p->account = account;
        if (name) strncpy(p->name, name, ACCOUNT_NAME_CAPACITY - 1);
    }
    return &p->gear;
}

// ---------------------------------------------------------------------------
// The service
// ---------------------------------------------------------------------------
void social_init(social *s, const net_zone_table *secret, social_deliver_fn deliver, void *user) {
    memset(s, 0, sizeof(*s));
    s->secret = secret;
    s->deliver = deliver;
    s->user = user;
    s->next_party = 1;
}

static social_player *player_of(social *s, uint64_t account) {
    for (int i = 0; i < SOCIAL_MAX_PLAYERS; ++i)
        if (s->players[i].used && s->players[i].account == account) return &s->players[i];
    return NULL;
}
const social_player *social_find(const social *s, uint64_t account) { return player_of((social *)s, account); }

static bool same_name(const char *a, const char *b) {
    for (;; ++a, ++b) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return false;
        if (!x) return true;
    }
}
const social_player *social_find_name(const social *s, const char *name) {
    for (int i = 0; i < SOCIAL_MAX_PLAYERS; ++i)
        if (s->players[i].used && same_name(s->players[i].name, name)) return &s->players[i];
    return NULL;
}

static social_party *party_of(social *s, uint64_t id) {
    if (!id) return NULL;
    for (int i = 0; i < SOCIAL_MAX_PARTIES; ++i)
        if (s->parties[i].used && s->parties[i].id == id) return &s->parties[i];
    return NULL;
}

bool social_view(const social *s, uint64_t account, party_view *out) {
    memset(out, 0, sizeof(*out));
    const social_player *p = social_find(s, account);
    if (!p) return false;
    out->last_result = p->last_result;
    out->invite = p->invite;
    memcpy(out->invited_by, p->invited_by, sizeof(out->invited_by));
    const social_party *party = party_of((social *)s, p->party);
    if (!party) return true;
    out->party = party->id;
    for (int i = 0; i < party->count; ++i) {
        const social_player *m = social_find(s, party->members[i]);
        if (!m) continue;
        party_member_view *v = &out->members[out->count++];
        v->account = m->account;
        memcpy(v->name, m->name, sizeof(v->name));
        memcpy(v->world, m->world, sizeof(v->world));
        memcpy(v->server, m->server, sizeof(v->server));
        v->online = m->online;
        v->leader = m->account == party->leader;
    }
    return true;
}

// Sends a player their current view, through the server hosting them.
static void push(social *s, uint64_t account) {
    social_player *p = player_of(s, account);
    if (!p || !p->online || !s->deliver) return;
    party_view view;
    social_view(s, account, &view);
    unsigned char payload[600];
    cw_writer w;
    cw_writer_init_fixed(&w, payload, sizeof(payload));
    social_party_state_write(&w, account, &view, s->secret);
    s->deliver(s->user, p->via, MSG_SOC_PARTY_STATE, payload, w.size);
    ++s->pushes;
}
static void push_party(social *s, const social_party *party) {
    for (int i = 0; i < party->count; ++i) push(s, party->members[i]);
}

void social_presence(social *s, net_address via, uint32_t session, uint64_t account, const char *name,
                     const char *platform, const char *server, const char *world, int kind, double now) {
    social_player *p = player_of(s, account);
    if (!p) {
        for (int i = 0; i < SOCIAL_MAX_PLAYERS && !p; ++i)
            if (!s->players[i].used) p = &s->players[i];
        if (!p) return;
        memset(p, 0, sizeof(*p));
        p->used = true;
        p->account = account;
        if (kind == PRESENCE_LEFT) return;   // someone we never saw arrive
    }
    // Only the connection the player is on (server and session: sessions are per
    // server) may refresh or end their presence.
    if (kind != PRESENCE_ARRIVED && p->online && (p->session != session || strcmp(p->server, server) != 0)) return;
    p->session = session;
    bool online = kind != PRESENCE_LEFT;
    bool changed = p->online != online || strcmp(p->world, world) != 0 || strcmp(p->name, name) != 0;
    strncpy(p->name, name, ACCOUNT_NAME_CAPACITY - 1);
    strncpy(p->platform, platform, ACCOUNT_PLATFORM_CAPACITY - 1);
    strncpy(p->server, server, NET_ZONE_NAME - 1);
    strncpy(p->world, world, NET_ZONE_NAME - 1);
    p->via = via;
    p->online = online;
    p->last_seen = now;
    ++s->presences;
    // A party hears when a member moves or comes and goes. An arrival is a new
    // connection that knows nothing yet, so it is always told.
    social_party *party = party_of(s, p->party);
    if (party && changed) push_party(s, party);
    else if (kind == PRESENCE_ARRIVED || (online && changed)) push(s, account);
}

static void remove_member(social *s, social_party *party, uint64_t account) {
    for (int i = 0; i < party->count; ++i)
        if (party->members[i] == account) {
            memmove(party->members + i, party->members + i + 1, (size_t)(party->count - i - 1) * sizeof(uint64_t));
            --party->count;
            break;
        }
    social_player *p = player_of(s, account);
    if (p && p->party == party->id) p->party = 0;
    if (party->count == 0) party->used = false;
    else if (party->leader == account) party->leader = party->members[0];
}

static social_party *create_party(social *s, social_player *leader) {
    social_party *party = NULL;
    for (int i = 0; i < SOCIAL_MAX_PARTIES && !party; ++i)
        if (!s->parties[i].used) party = &s->parties[i];
    if (!party) return NULL;
    memset(party, 0, sizeof(*party));
    party->used = true;
    party->id = s->next_party++;
    party->leader = leader->account;
    party->members[party->count++] = leader->account;
    leader->party = party->id;
    return party;
}

int social_party_request(social *s, uint64_t account, int op, const char *target_name, double now) {
    social_player *me = player_of(s, account);
    if (!me) return PARTY_ERROR_UNKNOWN_PLAYER;
    ++s->ops;
    int result = PARTY_OK;
    social_party *party = party_of(s, me->party);
    social_party *to_push = party;
    switch (op) {
    case PARTY_CREATE:
        if (party) result = PARTY_ERROR_ALREADY_IN_ONE;
        else to_push = create_party(s, me);
        break;
    case PARTY_INVITE: {
        // Inviting while alone makes the party; only its leader may invite.
        if (!party) party = to_push = create_party(s, me);
        social_player *target = (social_player *)social_find_name(s, target_name ? target_name : "");
        if (!party) result = PARTY_ERROR_FULL;
        else if (party->leader != me->account) result = PARTY_ERROR_NOT_LEADER;
        else if (!target || !target->online) result = PARTY_ERROR_UNKNOWN_PLAYER;
        else if (target->party) result = PARTY_ERROR_ALREADY_IN_ONE;
        else if (party->count >= SOCIAL_PARTY_SIZE) result = PARTY_ERROR_FULL;
        else {
            target->invite = party->id;
            memcpy(target->invited_by, me->name, sizeof(target->invited_by));
            target->invited_at = now;
            push(s, target->account);
        }
        break;
    }
    case PARTY_ACCEPT: {
        social_party *invited = party_of(s, me->invite);
        if (party) result = PARTY_ERROR_ALREADY_IN_ONE;
        else if (!invited) result = PARTY_ERROR_NO_INVITE;
        else if (invited->count >= SOCIAL_PARTY_SIZE) result = PARTY_ERROR_FULL;
        else {
            invited->members[invited->count++] = me->account;
            me->party = invited->id;
            to_push = invited;
        }
        me->invite = 0;
        me->invited_by[0] = '\0';
        break;
    }
    case PARTY_DECLINE:
        if (!me->invite) result = PARTY_ERROR_NO_INVITE;
        me->invite = 0;
        me->invited_by[0] = '\0';
        break;
    case PARTY_LEAVE:
        if (!party) result = PARTY_ERROR_NOT_IN_PARTY;
        else remove_member(s, party, me->account);
        break;
    case PARTY_KICK:
    case PARTY_PROMOTE: {
        social_player *target = (social_player *)social_find_name(s, target_name ? target_name : "");
        if (!party) result = PARTY_ERROR_NOT_IN_PARTY;
        else if (party->leader != me->account) result = PARTY_ERROR_NOT_LEADER;
        else if (!target || target->party != party->id) result = PARTY_ERROR_NOT_A_MEMBER;
        else if (op == PARTY_KICK) {
            remove_member(s, party, target->account);
            push(s, target->account);   // told they are out
        } else {
            party->leader = target->account;
        }
        break;
    }
    default:
        result = PARTY_ERROR_NO_INVITE;
        break;
    }
    me->last_result = (uint8_t)result;
    if (to_push && to_push->used) push_party(s, to_push);
    push(s, me->account);   // the result, whatever else happened
    me->last_result = PARTY_OK;
    return result;
}

void social_handle(social *s, net_address from, uint8_t type, cw_reader payload, double now) {
    cw_reader body;
    if (!read_signed(&payload, &body, s->secret)) {
        ++s->rejected;
        return;
    }
    switch (type) {
    case MSG_SOC_PRESENCE: {
        uint64_t account = 0;
        char name[ACCOUNT_NAME_CAPACITY], platform[ACCOUNT_PLATFORM_CAPACITY], server[NET_ZONE_NAME], world[NET_ZONE_NAME];
        uint8_t kind = 0;
        uint32_t session = 0;
        cw_reader_u32(&body, &session);
        cw_reader_u64(&body, &account);
        if (!read_text(&body, name, sizeof(name)) || !read_text(&body, platform, sizeof(platform))
            || !read_text(&body, server, sizeof(server)) || !read_text(&body, world, sizeof(world))) return;
        cw_reader_u8(&body, &kind);
        if (!cw_reader_ok(&body) || kind > PRESENCE_LEFT) return;
        social_presence(s, from, session, account, name, platform, server, world, kind, now);
        break;
    }
    case MSG_SOC_PARTY_OP: {
        uint64_t account = 0;
        uint8_t op = 0;
        char target[ACCOUNT_NAME_CAPACITY];
        cw_reader_u64(&body, &account);
        cw_reader_u8(&body, &op);
        if (!read_text(&body, target, sizeof(target)) || !cw_reader_ok(&body)) return;
        social_party_request(s, account, op, target, now);
        break;
    }
    case MSG_SOC_CHARACTER_GET: {
        uint64_t account = 0;
        cw_reader_u64(&body, &account);
        if (!cw_reader_ok(&body) || !s->deliver) return;
        const character *gear = social_character(s, account, NULL);
        if (!gear) return;
        unsigned char answer[NET_MAX_PAYLOAD];
        cw_writer w;
        cw_writer_init_fixed(&w, answer, sizeof(answer));
        social_character_write(&w, account, gear, s->secret);
        s->deliver(s->user, from, MSG_SOC_CHARACTER, answer, w.size);
        break;
    }
    case MSG_SOC_CHARACTER_SET: {
        uint64_t account = 0;
        character gear;
        cw_reader_u64(&body, &account);
        if (!character_read(&body, &gear)) return;
        character *mine = social_character(s, account, NULL);
        if (!mine || gear.version < mine->version) return;   // an older copy: the server that sent it is behind
        *mine = gear;
        s->dirty = true;
        ++s->character_sets;
        break;
    }
    default:
        break;
    }
}

void social_update(social *s, double now) {
    if (s->dirty && s->save_path[0] && now - s->last_saved > SOCIAL_SAVE_EVERY) social_save(s);
    for (int i = 0; i < SOCIAL_MAX_PLAYERS; ++i) {
        social_player *p = &s->players[i];
        if (!p->used) continue;
        if (p->online && now - p->last_seen > SOCIAL_PRESENCE_EXPIRY) {
            p->online = false;
            social_party *party = party_of(s, p->party);
            if (party) push_party(s, party);
        }
        if (p->invite && now - p->invited_at > SOCIAL_INVITE_EXPIRY) {
            p->invite = 0;
            p->invited_by[0] = '\0';
            push(s, p->account);
        }
    }
}
