// Who a player is, across platforms: the crossplay seam.
//
// A client presents a platform identity ("steam:76561198000000000",
// "local:alice") and the name it would like shown. A provider for the scheme
// verifies the claim and produces the account: a 64-bit id derived from the
// platform identity, so the same person is the same account on every server and
// every visit, whatever the platform. The local provider is the development mock
// and accepts anything well-formed. The Steam provider is a stub with the calls a
// real one makes marked; until they are made it verifies the format only.
//
// Accounts are resolved the same way on a zone server (to know who joined) and in
// the social service (to keep the record), so neither has to ask the other.
#ifndef TP_ACCOUNTS_H
#define TP_ACCOUNTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ACCOUNT_NAME_CAPACITY 16      // display names: 15 characters
#define ACCOUNT_PLATFORM_CAPACITY 48  // "scheme:identifier"

struct account_identity {
    uint64_t account;
    char name[ACCOUNT_NAME_CAPACITY];
    char platform[ACCOUNT_PLATFORM_CAPACITY];
    const char *provider;        // the scheme that vouched
};

struct account_provider {
    const char *scheme;
    // Verifies `platform_id` (and whatever the client attached as proof, e.g. a
    // Steam auth ticket) and fills the identity. False refuses the player.
    bool (*resolve)(const char *platform_id, const char *proof, size_t proof_size, const char *claimed_name,
                    account_identity *out);
};

// Picks the provider by scheme; an unknown scheme is refused.
bool accounts_resolve(const char *platform_id, const char *proof, size_t proof_size, const char *claimed_name,
                      account_identity *out);
// The account id a platform identity maps to.
uint64_t account_id_for(const char *platform_id);
// Trims and sanitises a display name into `out`; false when nothing usable is left.
bool account_clean_name(const char *name, char out[ACCOUNT_NAME_CAPACITY]);

extern const account_provider ACCOUNT_PROVIDER_LOCAL;
extern const account_provider ACCOUNT_PROVIDER_STEAM;

#endif
