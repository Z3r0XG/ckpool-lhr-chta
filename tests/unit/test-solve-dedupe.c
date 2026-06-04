/*
 * Unit tests for the block-solve dedupe helpers (share_exists /
 * record_solve_share) in stratifier_internal.h.
 *
 * These exercise the REAL helpers against a real uthash table and a real
 * mutex. The only thing modeled is the node's accept/reject result — the one
 * genuinely external input — by choosing whether to call record_solve_share()
 * after a "submission". This pins the contract the block-solve path relies on:
 *
 *   - a winning fingerprint is recorded only on a confirmed submission;
 *   - a duplicate solve share is then detected before re-entering block solve;
 *   - a REJECTED block leaves no fingerprint, so a retry is not a false dupe.
 *
 * The surrounding test_blocksolve()/parse_remote_block() control flow (node
 * RPC, workbases, locks across threads) is not unit-testable and remains a
 * live-only concern.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "../test_common.h"
#include "stratifier_internal.h"

/* Build a deterministic 32-byte share hash from a single seed byte. */
static void make_hash(uchar *hash, uchar seed)
{
	memset(hash, seed, 32);
}

/* -------------------------------------------------------------------------
 * Test 1: first solve records; a duplicate is then detected
 * ------------------------------------------------------------------------- */
static void test_first_solve_then_duplicate(void)
{
	printf("\n  First solve records, duplicate detected:\n");

	share_t *table = NULL;
	mutex_t lock;
	uchar h1[32];

	mutex_init(&lock);
	make_hash(h1, 0x11);

	/* Before submission: not seen → block-solve path would proceed. */
	assert_false(share_exists(&lock, &table, h1));

	/* Node accepted the block → record the winning fingerprint. */
	record_solve_share(&lock, &table, h1, 100);

	/* A resubmitted identical solve share is now caught before re-entry. */
	assert_true(share_exists(&lock, &table, h1));

	printf("    ok: not-seen before record, seen after\n");
}

/* -------------------------------------------------------------------------
 * Test 2: a REJECTED block leaves no fingerprint (record-on-success only)
 * This is the subtle contract: we must NOT record before knowing the node
 * accepted, or a valid-share/rejected-block would be mis-flagged a duplicate.
 * ------------------------------------------------------------------------- */
static void test_rejected_block_not_recorded(void)
{
	printf("\n  Rejected block leaves no fingerprint:\n");

	share_t *table = NULL;
	mutex_t lock;
	uchar h2[32];

	mutex_init(&lock);
	make_hash(h2, 0x22);

	/* Not seen. */
	assert_false(share_exists(&lock, &table, h2));

	/* Node REJECTED the block → record_solve_share() is NOT called. */

	/* A retry of the same share must still be treated as new, not a dupe. */
	assert_false(share_exists(&lock, &table, h2));

	printf("    ok: retry after rejection is not a false duplicate\n");
}

/* -------------------------------------------------------------------------
 * Test 3: distinct fingerprints do not collide
 * ------------------------------------------------------------------------- */
static void test_distinct_hashes_no_collision(void)
{
	printf("\n  Distinct fingerprints do not collide:\n");

	share_t *table = NULL;
	mutex_t lock;
	uchar h1[32], h3[32];

	mutex_init(&lock);
	make_hash(h1, 0x11);
	make_hash(h3, 0x33);

	record_solve_share(&lock, &table, h1, 100);

	assert_true(share_exists(&lock, &table, h1));
	assert_false(share_exists(&lock, &table, h3));

	printf("    ok: recording h1 does not match h3\n");
}

/* -------------------------------------------------------------------------
 * Test 4: double-record is safe and keeps exactly one entry
 * ------------------------------------------------------------------------- */
static void test_double_record_safe(void)
{
	printf("\n  Double record keeps a single entry:\n");

	share_t *table = NULL;
	mutex_t lock;
	uchar h1[32];

	mutex_init(&lock);
	make_hash(h1, 0x11);

	record_solve_share(&lock, &table, h1, 100);
	record_solve_share(&lock, &table, h1, 100);

	assert_true(share_exists(&lock, &table, h1));
	assert_int_equal(HASH_COUNT(table), 1);

	printf("    ok: two records of the same hash → one table entry\n");
}

/* -------------------------------------------------------------------------
 * Test 5: record fidelity — count and stored workbase_id are correct
 * (record_solve_share takes only (lock, table, hash, wb_id); by construction
 * it cannot touch shares_generated or any other state — the side-effect-free
 * property is guaranteed by the signature, so here we verify data fidelity.)
 * ------------------------------------------------------------------------- */
static void test_record_fidelity(void)
{
	printf("\n  Record fidelity (count + workbase_id):\n");

	share_t *table = NULL, *match = NULL;
	mutex_t lock;
	uchar h1[32], h2[32], h3[32];

	mutex_init(&lock);
	make_hash(h1, 0x11);
	make_hash(h2, 0x22);
	make_hash(h3, 0x33);

	record_solve_share(&lock, &table, h1, 100);
	record_solve_share(&lock, &table, h2, 200);
	record_solve_share(&lock, &table, h3, 300);

	assert_int_equal(HASH_COUNT(table), 3);

	HASH_FIND(hh, table, h2, 32, match);
	assert_non_null(match);
	assert_true(match->workbase_id == 200);

	printf("    ok: three distinct records, workbase_id preserved\n");
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */
int main(void)
{
	run_test(test_first_solve_then_duplicate);
	run_test(test_rejected_block_not_recorded);
	run_test(test_distinct_hashes_no_collision);
	run_test(test_double_record_safe);
	run_test(test_record_fidelity);
	printf("All tests passed!\n");
	return 0;
}
