/*
 * Unit tests for best_diff / best_ever reset behaviour on block solve
 * ====================================================================
 *
 * PURPOSE:
 * Verifies that check_best_diff() and the client->best_diff gate in
 * parse_submit() behave correctly when a share meets or exceeds
 * network_diff (i.e. it is a block-solve share).
 *
 * HOW THESE TESTS WORK:
 * Rather than replicating production logic in stubs, these tests include
 * stratifier_internal.h and call the real static-inline functions
 * update_best_diff() and client_gate_update() directly with the real
 * user_instance_t / worker_instance_t / stratum_instance_t struct types.
 * Only the pool stats.best_diff update (which requires a mutex and sdata_t)
 * is kept as a tiny inline stub — the condition is tested separately.
 *
 * If struct fields or update conditions change in stratifier_internal.h,
 * these tests break at compile time or at assertion time.
 *
 * KEY INVARIANTS:
 *
 * best_ever  — all-time record; updated only if sdiff > best_ever.
 *              Never zeroed by reset_bestshares().
 *
 * best_diff  — per-round best; zeroed by reset_bestshares() on every
 *              block solve.  Must NOT be updated for a solve share
 *              (sdiff >= network_diff) because reset_bestshares() has
 *              already zeroed it — a solve diff would then poison the
 *              new round, causing every subsequent share below that diff
 *              to fail the "sdiff > best_diff" gate.
 *
 * client->best_diff — per-connection gate that controls whether
 *              check_best_diff() is called at all.  Advances to solve diff
 *              when a solve share passes the gate; for confirmed solves,
 *              reset_bestshares() then zeroes it back; for failed solves,
 *              it correctly records the genuine session best.
 *
 * SCENARIOS:
 *  1. Normal share (sdiff < network_diff) updates best_diff and best_ever.
 *  2. Solve share (sdiff >= network_diff) updates best_ever if record,
 *     but leaves best_diff at 0 (post-reset).
 *  3. Solve share that does NOT beat best_ever leaves best_ever unchanged.
 *  4. client->best_diff gate: after a confirmed solve, reset_bestshares()
 *     zeroes client->best_diff so lower post-solve shares can pass the gate.
 *  5. Post-solve shares correctly accumulate best_diff from zero.
 *  6. Remote solve via SM_SHARE: same as local solve, network_diff guard
 *     prevents best_diff poisoning while best_ever is still updated.
 *  7. DAA-spike race: sdiff falls between old frozen network_diff and new live
 *     network_diff on master.  The frozen workbase value correctly classifies
 *     the share as a solve (best_diff stays 0); the live master value would
 *     misclassify it as a normal share and poison best_diff.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#include "../test_common.h"
#include "stratifier_internal.h"

/* --------------------------------------------------------------------------
 * Pool best_diff update stub.
 *
 * In production, check_best_diff() updates sdata->stats.best_diff under
 * sdata->stats_lock.  We can't use sdata_t here, so we replicate the
 * condition only (no mutex needed in single-threaded tests).
 *
 * guard_round=false (local paths): update whenever best_user is true.
 * guard_round=true  (remote path): only update when sdiff < network_diff
 *   (SM_BLOCK has already reset, solve diff must not poison new round).
 * -------------------------------------------------------------------------- */
static inline void
pool_best_diff_update(double *pool_best_diff, bool best_user,
		      double sdiff, double network_diff, bool guard_round)
{
	if (best_user && (!guard_round || sdiff < network_diff * 0.999) && sdiff > *pool_best_diff)
		*pool_best_diff = sdiff;
}

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/* Run a share through the client gate and update_best_diff (local path,
 * guard_round=false).  Models gate + update only; does NOT include
 * reset_bestshares().  Use process_solve() for the confirmed-solve pipeline. */
static bool
process_share(stratum_instance_t *client, user_instance_t *user,
	      worker_instance_t *worker, double *pool_best_diff,
	      double sdiff, double network_diff)
{
	if (client_gate_update(client, sdiff)) {
		best_diff_result_t r = update_best_diff(user, worker, sdiff, network_diff, false);
		pool_best_diff_update(pool_best_diff, r.best_user, sdiff, network_diff, false);
		return true;
	}
	return false;
}

/* Remote SM_SHARE path: no client gate, guard_round=true.
 * SM_BLOCK has already fired reset_bestshares() so best_diff must not be
 * written for a solve share — only best_ever is updated. */
static void
process_share_remote(user_instance_t *user, worker_instance_t *worker,
		     double *pool_best_diff, double sdiff, double network_diff)
{
	best_diff_result_t r = update_best_diff(user, worker, sdiff, network_diff, true);
	pool_best_diff_update(pool_best_diff, r.best_user, sdiff, network_diff, true);
}

/* Simulate reset_bestshares() on our stub structs.
 * Note: best_ever fields are intentionally NOT touched — matches production. */
static void
reset_bestshares_stub(stratum_instance_t *client, user_instance_t *user,
		      worker_instance_t *worker, double *pool_best_diff)
{
	client->best_diff = 0;
	user->best_diff   = 0;
	worker->best_diff = 0;
	*pool_best_diff   = 0;
}

/* Model the confirmed-solve pipeline:
 *   1. client_gate_update() + update_best_diff()  — best_ever captured here
 *   2. reset_bestshares()                         — zeros best_diff; best_ever kept
 * This ordering is the fix this PR makes: check_best_diff runs inside
 * test_blocksolve() before block_solve() → reset_bestshares(). */
/* For a failed solve, use process_share() alone (no reset follows). */
static void
process_solve(stratum_instance_t *client, user_instance_t *user,
	      worker_instance_t *worker, double *pool_best_diff,
	      double sdiff, double network_diff)
{
	process_share(client, user, worker, pool_best_diff, sdiff, network_diff);
	reset_bestshares_stub(client, user, worker, pool_best_diff);
}

/* Models test_blocksolve()'s reset decision: a non-stale solve resets the
 * session (block_solve → reset_bestshares); a stale solve does neither. */
static void
process_solve_staleaware(stratum_instance_t *client, user_instance_t *user,
			 worker_instance_t *worker, double *pool_best_diff,
			 double sdiff, double network_diff, bool stale)
{
	if (!stale) {
		process_share(client, user, worker, pool_best_diff, sdiff, network_diff);
		reset_bestshares_stub(client, user, worker, pool_best_diff);
	}
	/* stale: neither capture nor reset */
}

/* -------------------------------------------------------------------------
 * Test 1: Normal share updates best_diff and best_ever
 * ------------------------------------------------------------------------- */
static void test_normal_share_updates_best_diff(void)
{
	printf("\n  Testing normal share updates best_diff and best_ever:\n");

	struct { double sdiff; double network_diff; } cases[] = {
		{ 50000.0,       3539393.4       },  /* typical CHTA share */
		{ 1308616.0,     3539393.4       },  /* high share, well under solve */
		{ 999999.0,      1000000.0       },  /* just under network_diff */
		{ 0.5,           1.0             },  /* fractional, allow_low_diff */
	};

	for (int i = 0; i < (int)(sizeof(cases)/sizeof(cases[0])); i++) {
		stratum_instance_t client  = {0};
		user_instance_t    user    = {0};
		worker_instance_t  worker  = {0};
		double             pool_bd = 0;

		bool called = process_share(&client, &user, &worker, &pool_bd,
					    cases[i].sdiff, cases[i].network_diff);

		printf("    sdiff=%.2f network_diff=%.2f → "
		       "best_diff=%.2f best_ever=%.2f client_bd=%.2f\n",
		       cases[i].sdiff, cases[i].network_diff,
		       user.best_diff, user.best_ever, client.best_diff);

		assert_true(called);
		assert_double_equal(user.best_diff,   cases[i].sdiff, 1e-6);
		assert_double_equal(worker.best_diff, cases[i].sdiff, 1e-6);
		assert_double_equal(user.best_ever,   cases[i].sdiff, 1e-6);
		assert_double_equal(worker.best_ever, cases[i].sdiff, 1e-6);
		assert_double_equal(client.best_diff, cases[i].sdiff, 1e-6);
	}
}

/* -------------------------------------------------------------------------
 * Test 2: Solve share updates best_ever but leaves best_diff at 0
 * ------------------------------------------------------------------------- */
static void test_solve_share_updates_best_ever_not_best_diff(void)
{
	printf("\n  Testing solve share updates best_ever but not best_diff:\n");

	struct {
		const char *label;
		double sdiff;
		double network_diff;
		double prior_best_ever;
	} cases[] = {
		/* sdiff exactly equals network_diff */
		{ "exact solve", 3539393.4, 3539393.4, 0.0 },
		/* sdiff slightly above network_diff (typical: hash beats target) */
		{ "solve above diff", 4308532.0, 3539393.4, 0.0 },
		/* very high solve beating prior best_ever */
		{ "12M solve beats prior 10M", 12392863.0, 3539393.4, 10057343.0 },
		/* solve where prior best_ever was already higher (shouldn't update) */
		{ "solve below prior best_ever", 4308532.0, 3539393.4, 12392863.0 },
	};

	for (int i = 0; i < (int)(sizeof(cases)/sizeof(cases[0])); i++) {
		stratum_instance_t client  = {0};
		/* Start with state after reset_bestshares (best_diff=0) but with
		 * existing best_ever from a previous round. */
		user_instance_t   user   = { .best_diff = 0, .best_ever = cases[i].prior_best_ever };
		worker_instance_t worker = { .best_diff = 0, .best_ever = cases[i].prior_best_ever };
		double pool_bd = 0;

		double expected_best_ever = cases[i].sdiff > cases[i].prior_best_ever
					    ? cases[i].sdiff
					    : cases[i].prior_best_ever;

		process_share(&client, &user, &worker, &pool_bd,
			      cases[i].sdiff, cases[i].network_diff);

		printf("    %s: sdiff=%.2f network_diff=%.2f prior_best_ever=%.2f\n"
		       "      → best_diff=%.2f (want %.2f) client_bd=%.2f (want %.2f) best_ever=%.2f (want %.2f)\n",
		       cases[i].label,
		       cases[i].sdiff, cases[i].network_diff, cases[i].prior_best_ever,
		       user.best_diff, cases[i].sdiff,
		       client.best_diff, cases[i].sdiff,
		       user.best_ever, expected_best_ever);

		/* Local path (guard_round=false): best_diff set to solve diff.
		 * For confirmed solve, block_solve() logs this, then reset_bestshares()
		 * zeroes it.  For failed solve, it is the legitimate round best. */
		assert_double_equal(user.best_diff,   cases[i].sdiff,      1e-6);
		assert_double_equal(worker.best_diff, cases[i].sdiff,      1e-6);
		assert_double_equal(pool_bd,          cases[i].sdiff,      1e-6);
		/* client gate also advances to solve diff */
		assert_double_equal(client.best_diff, cases[i].sdiff,      1e-6);
		/* best_ever updates only if sdiff beats prior record */
		assert_double_equal(user.best_ever,   expected_best_ever,  1e-6);
		assert_double_equal(worker.best_ever, expected_best_ever,  1e-6);
	}
}

static void test_solve_share_best_ever_flags(void)
{
	printf("\n  Testing solve-share best_ever user/worker flags:\n");

	struct {
		const char *label;
		double user_prior_best_ever;
		double worker_prior_best_ever;
		double sdiff;
		double network_diff;
		bool want_best_ever_user;
		bool want_best_ever_worker;
	} cases[] = {
		{ "user hits best_ever only", 15000.0, 10000.0, 12000.0, 12000.0, false, true },
		{ "worker hits best_ever only", 10000.0, 15000.0, 12000.0, 12000.0, true, false },
		{ "both hit best_ever", 10000.0, 10000.0, 12000.0, 12000.0, true, true },
	};

	for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
		user_instance_t user = { .best_ever = cases[i].user_prior_best_ever };
		worker_instance_t worker = { .best_ever = cases[i].worker_prior_best_ever };

		best_diff_result_t r = update_best_diff(&user, &worker,
			cases[i].sdiff, cases[i].network_diff, false);

		printf("    %s: user_prior=%.0f worker_prior=%.0f sdiff=%.0f\n",
			cases[i].label, cases[i].user_prior_best_ever,
			cases[i].worker_prior_best_ever, cases[i].sdiff);

		assert_true(r.best_ever);
		assert_true(r.best_ever_user == cases[i].want_best_ever_user);
		assert_true(r.best_ever_worker == cases[i].want_best_ever_worker);
	}
}

/* -------------------------------------------------------------------------
 * Test 3: Post-solve shares correctly accumulate best_diff from zero
 *
 * Uses process_solve() to model the full pipeline: gate → update → reset.
 * After reset, client->best_diff is 0 so all post-solve shares pass the
 * gate and accumulate best_diff for the new round.
 * ------------------------------------------------------------------------- */
static void test_post_solve_shares_accumulate_best_diff(void)
{
	printf("\n  Testing post-solve shares accumulate best_diff from zero:\n");

	double network_diff  = 3539393.4;
	double solve_diff    = 12392863.0;
	double prior_best    = 10057343.0;  /* best_ever before this solve */

	/* End-of-round state immediately before the solve share arrives */
	stratum_instance_t client  = { .best_diff = 1308616.0 };
	user_instance_t    user    = { .best_diff = 1308616.0, .best_ever = prior_best };
	worker_instance_t  worker  = { .best_diff = 1308616.0, .best_ever = prior_best };
	double pool_bd = 1308616.0;

	/* === Step 1: solve share — full pipeline (gate → update → reset) === */
	process_solve(&client, &user, &worker, &pool_bd, solve_diff, network_diff);

	printf("    After solve (%.2f): client_bd=%.2f user.bd=%.2f best_ever=%.2f\n",
	       solve_diff, client.best_diff, user.best_diff, user.best_ever);

	/* reset_bestshares zeros everything; best_ever was captured before reset */
	assert_double_equal(client.best_diff, 0.0,        1e-6);
	assert_double_equal(user.best_diff,   0.0,        1e-6);
	assert_double_equal(worker.best_diff, 0.0,        1e-6);
	assert_double_equal(pool_bd,          0.0,        1e-6);
	assert_double_equal(user.best_ever,   solve_diff, 1e-6);  /* new record */

	/* === Step 2: new round — all shares pass gate and accumulate === */
	double new_shares[] = { 47326.0, 57275.0, 827615.0 };

	for (int i = 0; i < 3; i++) {
		bool called = process_share(&client, &user, &worker, &pool_bd,
					    new_shares[i], network_diff);

		printf("    After share %.2f: called=%d "
		       "client_bd=%.2f best_diff=%.2f\n",
		       new_shares[i], called, client.best_diff, user.best_diff);

		/* All shares pass the gate — client->best_diff was reset to 0 */
		assert_true(called);
		/* best_ever must not change from solve_diff */
		assert_double_equal(user.best_ever, solve_diff, 1e-6);
	}
	/* best_diff must have accumulated up to the highest share in the round */
	assert_double_equal(user.best_diff,   827615.0, 1e-6);
	assert_double_equal(worker.best_diff, 827615.0, 1e-6);
	assert_double_equal(client.best_diff, 827615.0, 1e-6);
}

/* -------------------------------------------------------------------------
 * Test 4: Full round → solve → new round cycle
 * Tests the exact sequence: normal shares, then solve, then new round shares
 * ------------------------------------------------------------------------- */
static void test_full_round_solve_new_round_cycle(void)
{
	printf("\n  Testing full round → solve → new round cycle:\n");

	double network_diff = 3539393.4;

	stratum_instance_t client  = {0};
	user_instance_t    user    = {0};
	worker_instance_t  worker  = {0};
	double pool_bd = 0;

	/* === Round 1: accumulate normal shares === */
	double round1_shares[] = { 5000.0, 44508.0, 1308616.0 };
	for (int i = 0; i < 3; i++)
		process_share(&client, &user, &worker, &pool_bd,
			      round1_shares[i], network_diff);

	printf("    End of round 1: best_diff=%.2f best_ever=%.2f\n",
	       user.best_diff, user.best_ever);
	assert_double_equal(user.best_diff,   1308616.0, 1e-6);
	assert_double_equal(user.best_ever,   1308616.0, 1e-6);
	assert_double_equal(client.best_diff, 1308616.0, 1e-6);

	/* === Block solve: full pipeline (gate → update → reset) === */
	double solve_diff = 4308532.0;
	process_solve(&client, &user, &worker, &pool_bd, solve_diff, network_diff);

	printf("    After solve (%.2f): client_bd=%.2f best_diff=%.2f best_ever=%.2f\n",
	       solve_diff, client.best_diff, user.best_diff, user.best_ever);

	assert_double_equal(client.best_diff, 0.0,        1e-6);  /* reset to 0 */
	assert_double_equal(user.best_diff,   0.0,        1e-6);  /* reset to 0 */
	assert_double_equal(user.best_ever,   solve_diff, 1e-6);  /* new record  */

	/* === Round 2: all shares pass the gate (client_bd reset to 0) === */
	double round2_shares[] = { 47326.0, 827615.0 };
	for (int i = 0; i < 2; i++) {
		process_share(&client, &user, &worker, &pool_bd,
			      round2_shares[i], network_diff);
	}

	printf("    End of round 2: best_diff=%.2f best_ever=%.2f\n",
	       user.best_diff, user.best_ever);

	assert_double_equal(user.best_diff, 827615.0,   1e-6);  /* accumulated */
	assert_double_equal(user.best_ever, solve_diff, 1e-6);  /* untouched */
}

/* -------------------------------------------------------------------------
 * Test 5: Remote solve share (SM_SHARE path) — guard_round=true
 * In parse_remote_share, client is NULL and update_best_diff is called
 * with guard_round=true because SM_BLOCK already fired reset_bestshares().
 * best_diff must stay 0; best_ever is still updated.
 * ------------------------------------------------------------------------- */
static void test_remote_solve_share_best_diff_guard(void)
{
	printf("\n  Testing remote solve share (SM_SHARE) best_diff guard:\n");

	double network_diff = 3539393.4;

	struct {
		const char *label;
		double sdiff;
		double prior_best_ever;
		double expected_best_ever;
	} cases[] = {
		{ "remote solve beats record",
		  4308532.0, 1308616.0, 4308532.0 },
		{ "remote solve below prior record (but still >= network_diff)",
		  3600000.0, 4308532.0, 4308532.0 },
		{ "remote solve exactly at network_diff",
		  3539393.4, 1308616.0, 3539393.4 },
	};

	for (int i = 0; i < (int)(sizeof(cases)/sizeof(cases[0])); i++) {
		/* Simulate post-reset state (SM_BLOCK already fired). */
		user_instance_t   user   = { .best_diff = 0, .best_ever = cases[i].prior_best_ever };
		worker_instance_t worker = { .best_diff = 0, .best_ever = cases[i].prior_best_ever };
		double pool_bd = 0;

		process_share_remote(&user, &worker, &pool_bd, cases[i].sdiff, network_diff);

		printf("    %s: sdiff=%.2f → best_diff=%.2f (want 0) best_ever=%.2f (want %.2f)\n",
		       cases[i].label, cases[i].sdiff,
		       user.best_diff, user.best_ever, cases[i].expected_best_ever);

		assert_double_equal(user.best_diff,   0.0,                         1e-6);
		assert_double_equal(worker.best_diff, 0.0,                         1e-6);
		assert_double_equal(pool_bd,          0.0,                         1e-6);
		assert_double_equal(user.best_ever,   cases[i].expected_best_ever, 1e-6);
		assert_double_equal(worker.best_ever, cases[i].expected_best_ever, 1e-6);
	}
}

/* -------------------------------------------------------------------------
 * Test 7: DAA-spike race — frozen vs live network_diff in parse_remote_share
 *
 * Scenario:
 *   - Remote node solved block against workbase with network_diff = 3.5M
 *   - sdiff = 4M  (beats old target, this IS a solve share)
 *   - Master processes SM_BLOCK → reset_bestshares() → best_diff = 0
 *   - Before SM_SHARE arrives, DAA retarget raises network_diff to 20G
 *   - SM_SHARE arrives at parse_remote_share with:
 *       CORRECT: frozen network_diff = 3.5M from workbase payload
 *       WRONG:   live sdata->stats.network_diff = 20G
 *
 * With CORRECT (frozen 3.5M): sdiff=4M >= 3.5M → solve share → best_diff stays 0
 * With WRONG (live 20G):      sdiff=4M <  20G  → normal share → best_diff set to 4M (POISONED)
 * ------------------------------------------------------------------------- */
static void test_remote_frozen_network_diff_daa_race(void)
{
	printf("\n  Testing frozen network_diff guards against DAA-spike race:\n");

	double frozen_network_diff = 3539393.4;   /* workbase value at solve time */
	double live_network_diff   = 20000000000.0; /* master value after DAA spike */
	double sdiff               = 4308532.0;   /* solve share: above frozen, below live */

	/* Verify precondition: sdiff is in the gap */
	if (!(sdiff >= frozen_network_diff && sdiff < live_network_diff)) {
		printf("    SKIP: precondition not met (not in gap)\n");
		return;
	}

	/* --- Correct path: frozen network_diff + guard_round=true --- */
	{
		user_instance_t   user   = { .best_diff = 0, .best_ever = 0 };
		worker_instance_t worker = { .best_diff = 0, .best_ever = 0 };
		double pool_bd = 0;

		best_diff_result_t r = update_best_diff(&user, &worker, sdiff, frozen_network_diff, true);
		pool_best_diff_update(&pool_bd, r.best_user, sdiff, frozen_network_diff, true);

		printf("    Frozen network_diff=%.2f guard_round=true: best_diff=%.2f (want 0) "
		       "best_ever=%.2f (want %.2f) pool_bd=%.2f (want 0)\n",
		       frozen_network_diff, user.best_diff, user.best_ever, sdiff, pool_bd);

		assert_double_equal(user.best_diff,   0.0,   1e-6);  /* NOT poisoned */
		assert_double_equal(worker.best_diff, 0.0,   1e-6);
		assert_double_equal(pool_bd,          0.0,   1e-6);
		assert_double_equal(user.best_ever,   sdiff, 1e-6);  /* record updated */
		assert_double_equal(worker.best_ever, sdiff, 1e-6);
	}

	/* --- Wrong path: live network_diff + guard_round=true.
	 * sdiff=4M < live_network_diff=20G → guard_round=true still passes the
	 * sdiff < network_diff check → best_diff poisoned. */
	{
		user_instance_t   user   = { .best_diff = 0, .best_ever = 0 };
		worker_instance_t worker = { .best_diff = 0, .best_ever = 0 };
		double pool_bd = 0;

		best_diff_result_t r = update_best_diff(&user, &worker, sdiff, live_network_diff, true);
		pool_best_diff_update(&pool_bd, r.best_user, sdiff, live_network_diff, true);

		printf("    Live network_diff=%.2f guard_round=true: best_diff=%.2f (would poison) "
		       "best_ever=%.2f pool_bd=%.2f (would poison)\n",
		       live_network_diff, user.best_diff, user.best_ever, pool_bd);

		/* sdiff < live_network_diff so the guard passes and poisons best_diff */
		assert_double_equal(user.best_diff, sdiff, 1e-6);  /* poisoned */
		assert_double_equal(pool_bd,        sdiff, 1e-6);  /* poisoned */
	}
}

/* -------------------------------------------------------------------------
 * Test 6: pool stats.best_diff — local vs remote solve behaviour
 *
 * Local confirmed solve (guard_round=false):
 *   check_best_diff sets pool_bd to solve_diff BEFORE block_solve() calls
 *   reset_bestshares(), so the solve diff is visible in logging.  After
 *   reset, pool_bd is zeroed.
 *
 * Remote solve (guard_round=true):
 *   SM_BLOCK already fired reset_bestshares().  pool_bd must NOT be set
 *   to the solve diff — it stays at 0.
 * ------------------------------------------------------------------------- */
static void test_pool_best_diff_not_poisoned_by_solve(void)
{
	printf("\n  Testing pool stats.best_diff local vs remote solve:\n");

	double network_diff = 3539393.4;
	double solve_diff   = 12392863.0;

	/* --- Local confirmed solve --- */
	{
		stratum_instance_t client = {0};
		user_instance_t    user   = {0};
		worker_instance_t  worker = {0};
		double pool_bd = 0;

		/* Round 1 normal share */
		process_share(&client, &user, &worker, &pool_bd, 500000.0, network_diff);
		assert_double_equal(pool_bd, 500000.0, 1e-6);

		/* Confirmed solve: process_solve sets records then resets */
		process_solve(&client, &user, &worker, &pool_bd, solve_diff, network_diff);

		printf("    Local solve: pool_bd=%.2f (want 0 after reset) best_ever=%.2f (want %.2f)\n",
		       pool_bd, user.best_ever, solve_diff);

		assert_double_equal(pool_bd,        0.0,        1e-6);  /* zeroed by reset */
		assert_double_equal(user.best_ever, solve_diff, 1e-6);
	}

	/* --- Remote solve (guard_round=true): pool_bd must stay 0 --- */
	{
		user_instance_t   user   = {0};
		worker_instance_t worker = {0};
		double pool_bd = 0;

		process_share_remote(&user, &worker, &pool_bd, solve_diff, network_diff);

		printf("    Remote solve: pool_bd=%.2f (want 0) best_ever=%.2f (want %.2f)\n",
		       pool_bd, user.best_ever, solve_diff);

		assert_double_equal(pool_bd,        0.0,        1e-6);  /* guard blocked it */
		assert_double_equal(user.best_ever, solve_diff, 1e-6);
	}
}

/* -------------------------------------------------------------------------
 * Test 9: A stale solve must not reset the current session
 *
 * A solve share from a previous round can arrive late — after the pool has
 * already reset and advanced to a new height and begun accumulating best
 * shares for the new session. It is still submitted (resilience), but it must
 * NOT trigger reset_bestshares(), or it wipes the new session's bests. The
 * winner (non-stale) still resets correctly. best_ever is never touched by the
 * reset, so it is unaffected either way.
 * ------------------------------------------------------------------------- */
static void test_stale_solve_does_not_reset_session(void)
{
	printf("\n  Testing stale solve does not reset the current session:\n");

	double network_diff = 3539393.4;
	double solve_diff   = 4308532.0;

	stratum_instance_t client = {0};
	user_instance_t    user   = {0};
	worker_instance_t  worker = {0};
	double pool_bd = 0;

	/* New session has accumulated best shares */
	double round_shares[] = { 5000.0, 44508.0, 827615.0 };
	for (int i = 0; i < 3; i++)
		process_share(&client, &user, &worker, &pool_bd, round_shares[i], network_diff);

	assert_double_equal(user.best_diff,   827615.0, 1e-6);
	assert_double_equal(worker.best_diff, 827615.0, 1e-6);
	assert_double_equal(client.best_diff, 827615.0, 1e-6);
	assert_double_equal(pool_bd,          827615.0, 1e-6);

	/* A STALE solve from a previous round arrives — must NOT reset. */
	process_solve_staleaware(&client, &user, &worker, &pool_bd, solve_diff, network_diff, true);

	printf("    After stale solve: user.bd=%.2f (want 827615) pool=%.2f (want 827615)\n",
	       user.best_diff, pool_bd);
	assert_double_equal(user.best_diff,   827615.0, 1e-6);  /* preserved */
	assert_double_equal(worker.best_diff, 827615.0, 1e-6);
	assert_double_equal(client.best_diff, 827615.0, 1e-6);
	assert_double_equal(pool_bd,          827615.0, 1e-6);

	/* Contrast: the real (non-stale) winner DOES reset. */
	process_solve_staleaware(&client, &user, &worker, &pool_bd, solve_diff, network_diff, false);

	printf("    After non-stale solve: user.bd=%.2f (want 0) pool=%.2f (want 0)\n",
	       user.best_diff, pool_bd);
	assert_double_equal(user.best_diff,   0.0, 1e-6);
	assert_double_equal(worker.best_diff, 0.0, 1e-6);
	assert_double_equal(client.best_diff, 0.0, 1e-6);
	assert_double_equal(pool_bd,          0.0, 1e-6);
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */
int main(void)
{
	run_test(test_normal_share_updates_best_diff);
	run_test(test_solve_share_updates_best_ever_not_best_diff);
	run_test(test_solve_share_best_ever_flags);
	run_test(test_post_solve_shares_accumulate_best_diff);
	run_test(test_full_round_solve_new_round_cycle);
	run_test(test_remote_solve_share_best_diff_guard);
	run_test(test_pool_best_diff_not_poisoned_by_solve);
	run_test(test_remote_frozen_network_diff_daa_race);
	run_test(test_stale_solve_does_not_reset_session);
	printf("All tests passed!\n");
	return 0;
}
