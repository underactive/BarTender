// firmware/test/config_store/test_wifi_lru.c
//
// Host-side unit-tests for the pure LRU array logic in wifi_lru.h.
// No ESP-IDF / NVS headers needed: config_store.h only includes <stdbool.h>,
// <stdint.h>, <stddef.h>; wifi_lru.h adds <string.h>.
//
// Build:  see Makefile (or build.sh) in this directory.
// Run:    ./runtests  =>  exits 0 on all-pass, nonzero on any failure.

#include "wifi_lru.h"
#include "config_store.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Minimal assert framework
// ---------------------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond, msg)                                               \
    do {                                                                \
        if (cond) {                                                     \
            g_pass++;                                                   \
        } else {                                                        \
            g_fail++;                                                   \
            fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        }                                                               \
    } while (0)

#define ASSERT_STR_EQ(a, b, msg) ASSERT(strcmp((a),(b)) == 0, msg)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a minimal, valid wifi_creds_t with count == 0.
static wifi_creds_t make_empty(void)
{
    wifi_creds_t w;
    memset(&w, 0, sizeof w);
    w.magic          = CFG_WIFI_BLOB_MAGIC;
    w.struct_version = 1;
    w.count          = 0;
    return w;
}

// Build a wifi_entry_t with the given ssid and pass.
static wifi_entry_t make_entry(const char *ssid, const char *pass)
{
    wifi_entry_t e;
    memset(&e, 0, sizeof e);
    strncpy(e.ssid, ssid, CFG_SSID_MAX - 1);
    strncpy(e.pass, pass, CFG_PASS_MAX - 1);
    return e;
}

// Convenience: call wifi_lru_upsert with CFG_WIFI_MAX_ENTRIES.
static void upsert(wifi_creds_t *w, const char *ssid, const char *pass)
{
    wifi_entry_t ne = make_entry(ssid, pass);
    wifi_lru_upsert(w, &ne, CFG_WIFI_MAX_ENTRIES);
}

// ---------------------------------------------------------------------------
// TEST 1: Insert into empty list, then fill to capacity (append order)
// ---------------------------------------------------------------------------
static void test_insert_empty_and_fill(void)
{
    printf("[TEST] insert empty then fill to capacity\n");
    wifi_creds_t w = make_empty();

    // Insert first entry into an empty list.
    upsert(&w, "Net1", "pass1");
    ASSERT(w.count == 1, "count == 1 after first insert");
    ASSERT_STR_EQ(w.e[0].ssid, "Net1", "e[0].ssid == Net1");
    ASSERT_STR_EQ(w.e[0].pass, "pass1", "e[0].pass == pass1");
    // Tail slot should be zeroed.
    ASSERT(w.e[1].ssid[0] == '\0', "e[1] zeroed after first insert");

    // Fill to capacity one by one; newest always at e[0].
    upsert(&w, "Net2", "pass2");
    ASSERT(w.count == 2, "count == 2");
    ASSERT_STR_EQ(w.e[0].ssid, "Net2", "e[0] == Net2");
    ASSERT_STR_EQ(w.e[1].ssid, "Net1", "e[1] == Net1");

    upsert(&w, "Net3", "pass3");
    ASSERT(w.count == 3, "count == 3");
    ASSERT_STR_EQ(w.e[0].ssid, "Net3", "e[0] == Net3");

    upsert(&w, "Net4", "pass4");
    ASSERT(w.count == 4, "count == 4");
    ASSERT_STR_EQ(w.e[0].ssid, "Net4", "e[0] == Net4");

    upsert(&w, "Net5", "pass5");
    ASSERT(w.count == CFG_WIFI_MAX_ENTRIES, "count == CFG_WIFI_MAX_ENTRIES (5)");
    ASSERT_STR_EQ(w.e[0].ssid, "Net5", "e[0] == Net5 (MRU)");
    ASSERT_STR_EQ(w.e[4].ssid, "Net1", "e[4] == Net1 (LRU)");
    // Magic refreshed.
    ASSERT(w.magic == CFG_WIFI_BLOB_MAGIC, "magic retained");
}

// ---------------------------------------------------------------------------
// TEST 2: Duplicate SSID promotes to MRU and does NOT grow count
// ---------------------------------------------------------------------------
static void test_duplicate_promotes_no_count_growth(void)
{
    printf("[TEST] duplicate SSID promotes to MRU, count unchanged\n");
    wifi_creds_t w = make_empty();
    upsert(&w, "Net1", "p1");
    upsert(&w, "Net2", "p2");
    upsert(&w, "Net3", "p3");
    // State: [Net3, Net2, Net1], count=3

    upsert(&w, "Net1", "p1-new");   // duplicate; should bubble to front
    ASSERT(w.count == 3, "count unchanged on duplicate");
    ASSERT_STR_EQ(w.e[0].ssid, "Net1", "Net1 promoted to e[0]");
    ASSERT_STR_EQ(w.e[0].pass, "p1-new", "password updated on promote");
    ASSERT_STR_EQ(w.e[1].ssid, "Net3", "Net3 shifted to e[1]");
    ASSERT_STR_EQ(w.e[2].ssid, "Net2", "Net2 shifted to e[2]");

    // Duplicate of already-MRU entry: count still stable.
    upsert(&w, "Net1", "p1-again");
    ASSERT(w.count == 3, "count still 3 after re-promoting MRU");
    ASSERT_STR_EQ(w.e[0].ssid, "Net1", "Net1 still e[0]");
    ASSERT_STR_EQ(w.e[0].pass, "p1-again", "password updated again");
}

// ---------------------------------------------------------------------------
// TEST 3: Insert when full evicts LRU; new entry becomes MRU
// ---------------------------------------------------------------------------
static void test_full_evicts_lru(void)
{
    printf("[TEST] insert when full evicts LRU\n");
    wifi_creds_t w = make_empty();
    // Fill to capacity: MRU order e[0..4] = Net5..Net1
    upsert(&w, "Net1", "p1");
    upsert(&w, "Net2", "p2");
    upsert(&w, "Net3", "p3");
    upsert(&w, "Net4", "p4");
    upsert(&w, "Net5", "p5");
    ASSERT(w.count == 5, "count == 5 at capacity");
    // LRU is Net1 at e[4].
    ASSERT_STR_EQ(w.e[4].ssid, "Net1", "LRU is Net1 before evict");

    // Insert new; Net1 (LRU) must be evicted.
    upsert(&w, "Net6", "p6");
    ASSERT(w.count == 5, "count remains 5 after eviction");
    ASSERT_STR_EQ(w.e[0].ssid, "Net6", "Net6 is new MRU at e[0]");
    ASSERT_STR_EQ(w.e[4].ssid, "Net2", "Net2 is now LRU at e[4]");
    // Net1 must not appear anywhere.
    for (int i = 0; i < 5; i++)
        ASSERT(strcmp(w.e[i].ssid, "Net1") != 0, "Net1 fully evicted");
    // No stale data in tail (past count) — count==5==max, no tail slots.

    // Evict once more: Net2 should be evicted.
    upsert(&w, "Net7", "p7");
    ASSERT(w.count == 5, "count remains 5 after second eviction");
    ASSERT_STR_EQ(w.e[0].ssid, "Net7", "Net7 is MRU");
    for (int i = 0; i < 5; i++)
        ASSERT(strcmp(w.e[i].ssid, "Net2") != 0, "Net2 fully evicted");
}

// ---------------------------------------------------------------------------
// TEST 4: Update (upsert duplicate) overwrites password and promotes
// ---------------------------------------------------------------------------
static void test_update_overwrites_password_and_promotes(void)
{
    printf("[TEST] update existing entry: password overwritten + promoted to MRU\n");
    wifi_creds_t w = make_empty();
    upsert(&w, "Home",   "secret1");
    upsert(&w, "Office", "work123");
    upsert(&w, "Cafe",   "coffee");
    // State: [Cafe, Office, Home], count=3

    // Update the entry deepest in the list (LRU = "Home").
    upsert(&w, "Home", "new-secret");
    ASSERT(w.count == 3, "count unchanged after update of LRU entry");
    ASSERT_STR_EQ(w.e[0].ssid, "Home",   "Home promoted to MRU");
    ASSERT_STR_EQ(w.e[0].pass, "new-secret", "password overwritten");
    ASSERT_STR_EQ(w.e[1].ssid, "Cafe",   "Cafe at e[1]");
    ASSERT_STR_EQ(w.e[2].ssid, "Office", "Office at e[2]");

    // Update the middle entry.
    upsert(&w, "Cafe", "latte");
    ASSERT(w.count == 3, "count unchanged after update of middle entry");
    ASSERT_STR_EQ(w.e[0].ssid, "Cafe", "Cafe promoted to MRU");
    ASSERT_STR_EQ(w.e[0].pass, "latte", "Cafe password updated");
    ASSERT_STR_EQ(w.e[1].ssid, "Home",   "Home now at e[1]");
    ASSERT_STR_EQ(w.e[2].ssid, "Office", "Office still at e[2]");
}

// ---------------------------------------------------------------------------
// TEST 5: Count never exceeds max; single and full boundary
// ---------------------------------------------------------------------------
static void test_count_boundary(void)
{
    printf("[TEST] count never exceeds CFG_WIFI_MAX_ENTRIES\n");
    wifi_creds_t w = make_empty();

    // Insert CFG_WIFI_MAX_ENTRIES + 3 unique networks.
    for (int i = 0; i < CFG_WIFI_MAX_ENTRIES + 3; i++) {
        char ssid[16], pass[16];
        snprintf(ssid, sizeof ssid, "Net%02d", i);
        snprintf(pass, sizeof pass, "pass%02d", i);
        upsert(&w, ssid, pass);
        uint8_t expected = (uint8_t)(i + 1 < CFG_WIFI_MAX_ENTRIES
                                     ? i + 1
                                     : CFG_WIFI_MAX_ENTRIES);
        ASSERT(w.count == expected, "count never exceeds max");
    }
    ASSERT(w.count == CFG_WIFI_MAX_ENTRIES, "final count == max");

    // Single-entry boundary: fresh list, one insert.
    wifi_creds_t w2 = make_empty();
    upsert(&w2, "Solo", "alone");
    ASSERT(w2.count == 1, "single-insert count == 1");
    ASSERT_STR_EQ(w2.e[0].ssid, "Solo", "solo entry at e[0]");
    // Tail slots must be zero.
    for (int i = 1; i < CFG_WIFI_MAX_ENTRIES; i++)
        ASSERT(w2.e[i].ssid[0] == '\0', "tail slots zeroed for single-entry list");

    // Full-boundary: insert duplicate when full, count must not exceed max.
    upsert(&w, w.e[CFG_WIFI_MAX_ENTRIES-1].ssid, "dup-pass");
    ASSERT(w.count == CFG_WIFI_MAX_ENTRIES, "count stays at max on dup-when-full");
}

// ---------------------------------------------------------------------------
// TEST 6: wifi_lru_promote (used by config_store_wifi_promote path)
// ---------------------------------------------------------------------------
static void test_lru_promote(void)
{
    printf("[TEST] wifi_lru_promote: moves entry to MRU, preserves password\n");
    wifi_creds_t w = make_empty();
    upsert(&w, "A", "pa");
    upsert(&w, "B", "pb");
    upsert(&w, "C", "pc");
    upsert(&w, "D", "pd");
    // State: [D, C, B, A], count=4

    // Promote the LRU (A at index 3).
    wifi_lru_promote(&w, 3);
    ASSERT_STR_EQ(w.e[0].ssid, "A", "A promoted to MRU");
    ASSERT_STR_EQ(w.e[0].pass, "pa", "password preserved on promote");
    ASSERT_STR_EQ(w.e[1].ssid, "D", "D shifted to e[1]");
    ASSERT_STR_EQ(w.e[2].ssid, "C", "C shifted to e[2]");
    ASSERT_STR_EQ(w.e[3].ssid, "B", "B shifted to e[3]");
    ASSERT(w.count == 4, "count unchanged after promote");

    // Promote middle element.
    wifi_lru_promote(&w, 2);  // "C"
    ASSERT_STR_EQ(w.e[0].ssid, "C", "C now at MRU after second promote");
    ASSERT_STR_EQ(w.e[1].ssid, "A", "A now at e[1]");
    ASSERT_STR_EQ(w.e[2].ssid, "D", "D now at e[2]");
    ASSERT_STR_EQ(w.e[3].ssid, "B", "B still at e[3]");
}

// ---------------------------------------------------------------------------
// TEST 7: wifi_lru_find returns correct index or -1
// ---------------------------------------------------------------------------
static void test_lru_find(void)
{
    printf("[TEST] wifi_lru_find: correct index and absent case\n");
    wifi_creds_t w = make_empty();
    upsert(&w, "Alpha", "pa");
    upsert(&w, "Beta",  "pb");
    upsert(&w, "Gamma", "pc");
    // State: [Gamma, Beta, Alpha], count=3

    ASSERT(wifi_lru_find(&w, "Gamma") == 0, "Gamma found at 0");
    ASSERT(wifi_lru_find(&w, "Beta")  == 1, "Beta found at 1");
    ASSERT(wifi_lru_find(&w, "Alpha") == 2, "Alpha found at 2");
    ASSERT(wifi_lru_find(&w, "Delta") == -1, "absent returns -1");
    ASSERT(wifi_lru_find(&w, "")      == -1, "empty string absent");
}

// ---------------------------------------------------------------------------
// TEST 8: Tail slots are always zeroed after any upsert
// ---------------------------------------------------------------------------
static void test_tail_zeroed(void)
{
    printf("[TEST] tail slots zeroed after each upsert\n");
    wifi_creds_t w = make_empty();

    // Manually poison the tail to detect if upsert forgets to zero it.
    memset(&w.e[1], 0xFF, sizeof w.e[1]);
    memset(&w.e[2], 0xFF, sizeof w.e[2]);

    upsert(&w, "OnlyOne", "pass");
    ASSERT(w.count == 1, "count == 1");
    // All slots past count must be zero.
    for (int i = 1; i < CFG_WIFI_MAX_ENTRIES; i++) {
        ASSERT(w.e[i].ssid[0] == '\0', "tail ssid zeroed");
        ASSERT(w.e[i].pass[0] == '\0', "tail pass zeroed");
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(void)
{
    test_insert_empty_and_fill();
    test_duplicate_promotes_no_count_growth();
    test_full_evicts_lru();
    test_update_overwrites_password_and_promotes();
    test_count_boundary();
    test_lru_promote();
    test_lru_find();
    test_tail_zeroed();

    printf("\n--- RESULTS: %d passed, %d failed ---\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
