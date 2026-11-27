#include "cuvs_page_recovery.h"
#include "cuvs_page_overlay.h"

#include <assert.h>
#include <string.h>

static void
check_lineage(void)
{
    const CuvsPageRecoveryHead base = {10, 512, 0};
    const CuvsPageRecoveryHead middle = {20, 640, 128};
    const CuvsPageRecoveryHead current = {30, 768, 256};
    const CuvsPageRecoveryHead wrong_folded = {20, 640, 127};
    const CuvsPageRecoveryHead reused_nonce = {30, 640, 128};
    const CuvsPageRecoveryHead divergent = {40, 768, 256};
    const CuvsPageRecoveryHead future = {50, 896, 384};

    assert(cuvs_page_recovery_lineage(&base, &current));
    assert(cuvs_page_recovery_lineage(&middle, &current));
    assert(cuvs_page_recovery_lineage(&current, &current));
    assert(!cuvs_page_recovery_lineage(&wrong_folded, &current));
    assert(!cuvs_page_recovery_lineage(&reused_nonce, &current));
    assert(!cuvs_page_recovery_lineage(&divergent, &current));
    assert(!cuvs_page_recovery_lineage(&future, &current));
}

static void
check_namespaces(void)
{
    CuvsPageOverlayBudget budget = {.limit = 1024};
    CuvsPageOverlayRequest pending_request = {.from = 2, .positions = 1};
    CuvsPageSearchRequest head = {.n_slots = 4, .folded = 2, .tail = 3};
    CuvsCmdFrame cmd = {.dim = 1, .k = 4, .metric = CUVS_METRIC_L2};
    CuvsPageOverlayChunk *proxy, *pending;
    CuvsPageOverlayResult rows[4] = {{.slot = 0, .distance = 9}, {.slot = 1, .distance = 100}};
    float proxy_data[] = {9, 8};
    float pending_data = 12;
    float query = 10;
    int count = 2;

    assert(cuvs_page_overlay_alloc(&budget, &(CuvsPageOverlayRequest) {0},
                                   sizeof(proxy_data), &proxy) == CUVS_STATUS_OK);
    memcpy(proxy->marks, proxy_data, sizeof(proxy_data));
    assert(cuvs_page_overlay_alloc(&budget, &pending_request,
                                   sizeof(CuvsPagePosition) + sizeof(pending_data), &pending) == CUVS_STATUS_OK);
    pending->marks[0] = (CuvsPagePosition) {.state = CUVS_PAGE_POSITION_VECTOR, .block = 7};
    memcpy(pending->marks + 1, &pending_data, sizeof(pending_data));

    cuvs_page_proxy_score(proxy, 2, 4, &query, &cmd, rows, &count);
    cuvs_page_overlay_score(pending, 3, &head, &query, &cmd, rows, &count);
    assert(count == 4);
    assert(rows[0].slot == 2 && rows[0].distance == 1 && rows[0].block == 0);
    assert(rows[1].slot == 3 && rows[1].distance == 4 && rows[1].block == 0);
    assert(rows[2].slot == 4 && rows[2].distance == 4 && rows[2].block == 7);
    assert(rows[3].slot == 0 && rows[3].distance == 9 && rows[3].block == 0);

    cuvs_page_overlay_release(proxy);
    cuvs_page_overlay_release(pending);
    assert(budget.used == 0);
}

static void
check_checkpoint_reconciliation(void)
{
    const CuvsPageRecoveryHead current = {40, 896, 384};
    const CuvsPageRecoveryHead ancestor = {10, 512, 0};
    const CuvsPageRecoveryHead foreign = {20, 640, 128};
    const CuvsPageRecoveryHead future = {30, 1024, 512};
    const CuvsPageRecoveryHead ancestor_conflict = {10, 640, 128};
    const CuvsPageRecoveryHead current_conflict = {40, 768, 256};
    CuvsPageCheckpointRef listed = {.head = foreign, .build_xid = 100, .device = 2, .inode = 7,
                                   .change_sec = 0x100000002, .change_nsec = 85, .manifest_crc = 200};
    CuvsPageCheckpointRef replacement = listed;

    assert(cuvs_page_checkpoint_action(&current, &current, &current) == CUVS_PAGE_CHECKPOINT_KEEP);
    assert(cuvs_page_checkpoint_action(&current_conflict, &current, NULL) == CUVS_PAGE_CHECKPOINT_KEEP);
    assert(cuvs_page_checkpoint_action(&ancestor_conflict, &current, &ancestor) == CUVS_PAGE_CHECKPOINT_KEEP);
    assert(cuvs_page_checkpoint_action(&ancestor, &current, &ancestor) == CUVS_PAGE_CHECKPOINT_PRUNE_ANCESTOR);
    assert(cuvs_page_checkpoint_action(&foreign, &current, NULL) == CUVS_PAGE_CHECKPOINT_PRUNE_FOREIGN);
    assert(cuvs_page_checkpoint_action(&future, &current, NULL) == CUVS_PAGE_CHECKPOINT_PRUNE_FOREIGN);

    assert(cuvs_page_checkpoint_can_prune(CUVS_PAGE_CHECKPOINT_PRUNE_FOREIGN, false));
    assert(!cuvs_page_checkpoint_can_prune(CUVS_PAGE_CHECKPOINT_PRUNE_ANCESTOR, false));
    assert(cuvs_page_checkpoint_can_prune(CUVS_PAGE_CHECKPOINT_PRUNE_ANCESTOR, true));
    assert(!cuvs_page_checkpoint_can_prune(CUVS_PAGE_CHECKPOINT_KEEP, true));

    assert(cuvs_page_checkpoint_xid_in_window(100, 101));
    assert(cuvs_page_checkpoint_xid_in_window(UINT64_C(0xffffffff), UINT64_C(0x100000004)));
    assert(!cuvs_page_checkpoint_xid_in_window(0, 100));
    assert(!cuvs_page_checkpoint_xid_in_window(2, 100));
    assert(!cuvs_page_checkpoint_xid_in_window(101, 100));
    assert(!cuvs_page_checkpoint_xid_in_window(100, 100));
    assert(!cuvs_page_checkpoint_xid_in_window(100, UINT64_C(0x80000064)));

    assert(cuvs_page_checkpoint_ref_matches(&listed, &replacement));
    ++replacement.inode;
    assert(!cuvs_page_checkpoint_ref_matches(&listed, &replacement));
    replacement = listed;
    ++replacement.change_nsec;
    assert(!cuvs_page_checkpoint_ref_matches(&listed, &replacement));
    replacement = listed;
    ++replacement.manifest_crc;
    assert(!cuvs_page_checkpoint_ref_matches(&listed, &replacement));
}

static void
check_recovery_offer(void)
{
    const CuvsPageRecoveryHead source = {9, 512, 0};
    const CuvsPageRecoveryHead target = {10, 640, 128};
    const CuvsPageRecoveryAuthority authority = {300, 1, 2, 3, 4};
    const CuvsPageRecoveryRequest request = {
        .system_identifier = 5, .boot_id = 6, .index_nonce = 7, .base_nonce = 9,
        .target = target, .source = source,
    };
    const CuvsPageRecoveryOffer ready = {
        .daemon_id = 101, .challenge = 123, .db_oid = 2, .index_oid = 8, .system_identifier = 5,
        .boot_id = 6, .index_nonce = 7, .base_nonce = 9, .target = target,
        .source = source, .authority = authority,
    };
    CuvsPageRecoveryOffer expired = ready;

    assert(cuvs_page_recovery_offer_matches(&ready, &request, &authority, 2, 8));
    ++expired.boot_id;
    assert(!cuvs_page_recovery_offer_matches(&expired, &request, &authority, 2, 8));
    expired = ready;
    ++expired.target.graph_nonce;
    assert(!cuvs_page_recovery_offer_matches(&expired, &request, &authority, 2, 8));
    expired = ready;
    ++expired.source.folded;
    assert(!cuvs_page_recovery_offer_matches(&expired, &request, &authority, 2, 8));
    expired = ready;
    ++expired.authority.rel_number;
    assert(!cuvs_page_recovery_offer_matches(&expired, &request, &authority, 2, 8));
    expired = ready;
    ++expired.authority.wal_lsn;
    assert(!cuvs_page_recovery_offer_matches(&expired, &request, &authority, 2, 8));
}

static void
check_manifest_protection(void)
{
    CuvsPageManifest manifest = {.version = CUVS_PAGE_MANIFEST_VERSION, .head = {7, 512, 0}};

    assert(cuvs_page_manifest_boot_usable(&manifest, 123));
    manifest.flags = CUVS_PAGE_MANIFEST_RECOVERY_PLACEHOLDERS;
    assert(!cuvs_page_manifest_protection_valid(&manifest));
    manifest.placeholder_boot = 123;
    assert(cuvs_page_manifest_boot_usable(&manifest, 123));
    assert(!cuvs_page_manifest_boot_usable(&manifest, 124));
    manifest.physical_dead_slots = 513;
    assert(!cuvs_page_manifest_boot_usable(&manifest, 123));
    manifest.physical_dead_slots = 31;
    assert(cuvs_page_manifest_boot_usable(&manifest, 123));
    manifest.version = 1;
    assert(!cuvs_page_manifest_boot_usable(&manifest, 123));
}

static void
check_checkpoint_name_allowlist(void)
{
    assert(cuvs_page_checkpoint_name_nonce("000000000000a1b2") == 0xa1b2);
    assert(cuvs_page_checkpoint_name_nonce("ffffffffffffffff") == UINT64_MAX);
    assert(!cuvs_page_checkpoint_name_nonce("0000000000000000"));
    assert(!cuvs_page_checkpoint_name_nonce("0000000000000a1b2"));
    assert(!cuvs_page_checkpoint_name_nonce("000000000000A1B2"));
    assert(!cuvs_page_checkpoint_name_nonce("000000000000a1b2.bak"));
    assert(!cuvs_page_checkpoint_name_nonce(".save-abc"));
}

int
main(void)
{
    check_lineage();
    check_namespaces();
    check_checkpoint_reconciliation();
    check_recovery_offer();
    check_manifest_protection();
    check_checkpoint_name_allowlist();
    return 0;
}
