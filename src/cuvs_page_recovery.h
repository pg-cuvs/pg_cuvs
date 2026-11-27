#ifndef CUVS_PAGE_RECOVERY_H
#define CUVS_PAGE_RECOVERY_H

#include "cuvs_ipc.h"

#define CUVS_PAGE_MANIFEST_MAGIC UINT64_C(0x4355565350474d31)
#define CUVS_PAGE_MANIFEST_VERSION 2U
#define CUVS_PAGE_MANIFEST_V1_SIZE 104U
#define CUVS_PAGE_MANIFEST_RECOVERY_PLACEHOLDERS 1U

/* An artifact can authorize neither a graph nor its logical slot boundary.
 * This header binds the serialized bytes to a head already proven by PG. */
typedef struct CuvsPageManifest
{
    uint64_t magic;
    uint64_t system_identifier;
    uint64_t index_nonce;
    uint64_t base_nonce;
    uint64_t build_xid;
    CuvsPageRecoveryHead head;
    uint64_t artifact_bytes;
    uint32_t db_oid;
    uint32_t index_oid;
    uint32_t dim;
    uint32_t metric;
    uint32_t version;
    uint32_t artifact_crc;
    uint32_t graph_degree;
    uint32_t flags;
    uint64_t placeholder_boot;
    uint32_t physical_dead_slots;
    uint32_t manifest_crc;
} CuvsPageManifest;

static inline bool
cuvs_page_manifest_protection_valid(const CuvsPageManifest *manifest)
{
    if (manifest->flags & ~CUVS_PAGE_MANIFEST_RECOVERY_PLACEHOLDERS)
        return false;
    if (manifest->flags & CUVS_PAGE_MANIFEST_RECOVERY_PLACEHOLDERS)
        return manifest->placeholder_boot && manifest->physical_dead_slots <= manifest->head.n_slots;
    return !manifest->placeholder_boot && !manifest->physical_dead_slots;
}

static inline bool
cuvs_page_manifest_boot_usable(const CuvsPageManifest *manifest, uint64_t boot_id)
{
    return manifest->version == CUVS_PAGE_MANIFEST_VERSION &&
        cuvs_page_manifest_protection_valid(manifest) &&
        (!manifest->placeholder_boot || manifest->placeholder_boot == boot_id);
}

static inline bool
cuvs_page_recovery_head_matches(const CuvsPageRecoveryHead *a, const CuvsPageRecoveryHead *b)
{
    return a->graph_nonce == b->graph_nonce && a->n_slots == b->n_slots &&
        a->folded == b->folded;
}

static inline bool
cuvs_page_recovery_lineage(const CuvsPageRecoveryHead *source, const CuvsPageRecoveryHead *target)
{
    if (!source->graph_nonce || !target->graph_nonce ||
        source->n_slots < 2 || source->n_slots > target->n_slots ||
        source->folded > target->folded || source->folded > source->n_slots ||
        target->folded > target->n_slots)
        return false;
    if (source->graph_nonce == target->graph_nonce)
        return cuvs_page_recovery_head_matches(source, target);
    return source->n_slots < target->n_slots &&
        target->n_slots - source->n_slots == target->folded - source->folded;
}

static inline bool
cuvs_page_recovery_offer_matches(const CuvsPageRecoveryOffer *offer,
                                 const CuvsPageRecoveryRequest *request,
                                 const CuvsPageRecoveryAuthority *authority,
                                 uint32_t db_oid, uint32_t index_oid)
{
    return offer->daemon_id && offer->challenge && offer->db_oid == db_oid &&
        offer->index_oid == index_oid && offer->boot_id == request->boot_id &&
        offer->system_identifier == request->system_identifier &&
        offer->index_nonce == request->index_nonce && offer->base_nonce == request->base_nonce &&
        cuvs_page_recovery_head_matches(&offer->target, &request->target) &&
        cuvs_page_recovery_head_matches(&offer->source, &request->source) &&
        offer->authority.wal_lsn == authority->wal_lsn &&
        offer->authority.tablespace_oid == authority->tablespace_oid &&
        offer->authority.database_oid == authority->database_oid &&
        offer->authority.rel_number == authority->rel_number &&
        offer->authority.physical_dead_slots == authority->physical_dead_slots;
}

/* Nonces sort independently of L and F. A conflicting record with the same
 * nonce is ambiguous, even when the manifest calls it a different head. */
static inline enum CuvsPageCheckpointAction
cuvs_page_checkpoint_action(const CuvsPageRecoveryHead *candidate,
                           const CuvsPageRecoveryHead *target,
                           const CuvsPageRecoveryHead *wal_record)
{
    if (candidate->graph_nonce == target->graph_nonce)
        return CUVS_PAGE_CHECKPOINT_KEEP;
    if (!wal_record)
        return CUVS_PAGE_CHECKPOINT_PRUNE_FOREIGN;
    if (cuvs_page_recovery_head_matches(candidate, wal_record) &&
        cuvs_page_recovery_lineage(candidate, target))
        return CUVS_PAGE_CHECKPOINT_PRUNE_ANCESTOR;
    return CUVS_PAGE_CHECKPOINT_KEEP;
}

/* The writer emits exactly sixteen lowercase hex digits. Aliases and
 * unrelated names are outside the GC allowlist. */
static inline uint64_t
cuvs_page_checkpoint_name_nonce(const char *name)
{
    uint64_t nonce = 0;

    for (int i = 0; i < 16; ++i)
    {
        unsigned char c = (unsigned char) name[i];
        unsigned digit;

        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else
            return 0;
        nonce = (nonce << 4) | digit;
    }
    return name[16] == '\0' ? nonce : 0;
}

static inline bool
cuvs_page_checkpoint_ref_matches(const CuvsPageCheckpointRef *listed, const CuvsPageCheckpointRef *current)
{
    return cuvs_page_recovery_head_matches(&listed->head, &current->head) &&
        listed->build_xid == current->build_xid && listed->manifest_crc == current->manifest_crc &&
        listed->device == current->device && listed->inode == current->inode &&
        listed->change_sec == current->change_sec && listed->change_nsec == current->change_nsec;
}

static inline bool
cuvs_page_checkpoint_can_prune(enum CuvsPageCheckpointAction action, bool exact_checkpoint)
{
    return action == CUVS_PAGE_CHECKPOINT_PRUNE_FOREIGN ||
        (action == CUVS_PAGE_CHECKPOINT_PRUNE_ANCESTOR && exact_checkpoint);
}

/* PG checks procarray with the low half only after this proves the full
 * XID is past and the low half does not alias another relevant epoch. */
static inline bool
cuvs_page_checkpoint_xid_in_window(uint64_t creator, uint64_t next)
{
    return (uint32_t) creator >= 3 && creator < next &&
        next - creator < (UINT64_C(1) << 31);
}
#endif
