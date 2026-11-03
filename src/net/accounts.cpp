#include "accounts.h"
#include <string.h>

uint64_t account_id_for(const char *platform_id) {
    uint64_t h = 0xcbf29ce484222325ull;   // FNV-1a; a real service would mint ids and keep the mapping
    for (const char *c = platform_id; *c; ++c) h = (h ^ (unsigned char)*c) * 0x100000001b3ull;
    return h ? h : 1;
}

bool account_clean_name(const char *name, char out[ACCOUNT_NAME_CAPACITY]) {
    size_t n = 0;
    for (const char *c = name ? name : ""; *c && n < ACCOUNT_NAME_CAPACITY - 1; ++c) {
        unsigned char ch = (unsigned char)*c;
        if (ch < 32 || ch > 126) continue;   // printable ASCII only, for now
        if (ch == ' ' && n == 0) continue;
        out[n++] = (char)ch;
    }
    while (n && out[n - 1] == ' ') --n;
    out[n] = '\0';
    return n > 0;
}

static bool fill(const char *platform_id, const char *claimed_name, const char *provider, account_identity *out) {
    memset(out, 0, sizeof(*out));
    if (strlen(platform_id) >= ACCOUNT_PLATFORM_CAPACITY) return false;
    strcpy(out->platform, platform_id);
    if (!account_clean_name(claimed_name, out->name)) return false;
    out->account = account_id_for(platform_id);
    out->provider = provider;
    return true;
}

// ---------------------------------------------------------------------------
// local: the development mock. "local:<anything>" is whoever says so.
// ---------------------------------------------------------------------------
static bool local_resolve(const char *platform_id, const char *, size_t, const char *claimed_name, account_identity *out) {
    if (strncmp(platform_id, "local:", 6) != 0 || !platform_id[6]) return false;
    return fill(platform_id, claimed_name, "local", out);
}
const account_provider ACCOUNT_PROVIDER_LOCAL = {"local", local_resolve};

// ---------------------------------------------------------------------------
// steam: a stub. The shape of the real thing, with the calls it will make marked.
// ---------------------------------------------------------------------------
static bool steam_resolve(const char *platform_id, const char *proof, size_t proof_size, const char *claimed_name,
                          account_identity *out) {
    // "steam:<SteamID64>": seventeen digits.
    if (strncmp(platform_id, "steam:", 6) != 0) return false;
    const char *id = platform_id + 6;
    size_t digits = 0;
    for (; id[digits]; ++digits)
        if (id[digits] < '0' || id[digits] > '9') return false;
    if (digits != 17) return false;
    // TODO(steam): `proof` is the session ticket from ISteamUser::GetAuthSessionTicket.
    // Validate it with ISteamUserAuth/AuthenticateUserTicket/v1 (Web API, publisher
    // key) and check that the SteamID it returns is `id`; refuse on VAC/publisher bans.
    // TODO(steam): the persona name from ISteamUser/GetPlayerSummaries/v2 takes
    // precedence over `claimed_name`.
    // TODO(steam): friends (ISteamUser/GetFriendList/v1) feed the social service's
    // "friends online" list.
    (void)proof;
    (void)proof_size;
    return fill(platform_id, claimed_name, "steam", out);
}
const account_provider ACCOUNT_PROVIDER_STEAM = {"steam", steam_resolve};

bool accounts_resolve(const char *platform_id, const char *proof, size_t proof_size, const char *claimed_name,
                      account_identity *out) {
    static const account_provider *const PROVIDERS[] = {&ACCOUNT_PROVIDER_LOCAL, &ACCOUNT_PROVIDER_STEAM};
    if (!platform_id) return false;
    const char *colon = strchr(platform_id, ':');
    if (!colon) return false;
    size_t scheme_length = (size_t)(colon - platform_id);
    for (size_t i = 0; i < sizeof(PROVIDERS) / sizeof(PROVIDERS[0]); ++i) {
        const account_provider *p = PROVIDERS[i];
        if (strlen(p->scheme) == scheme_length && strncmp(p->scheme, platform_id, scheme_length) == 0)
            return p->resolve(platform_id, proof, proof_size, claimed_name, out);
    }
    return false;
}
