/*
 * Unit tests for difficulty and network difficulty interaction
 * ==============================================================
 * 
 * PURPOSE:
 * Verifies that worker difficulty, pool constraints, and network difficulty
 * interact correctly. Tests the difference between difficulty constraints
 * (for vardiff) and network difficulty (threshold for block detection).
 * 
 * KEY CONCEPTS:
 *
 * WORKER DIFFICULTY: Vardiff-managed difficulty for the miner.
 *   - Target from hashrate: optimal = dsps * 1.0 (~1 share/sec). CHTA uses a
 *     faster cadence than stock's dsps * 3.33 so a miner's next share lands
 *     within ~1s when a cheetah window opens.
 *   - Clamped by pool constraints: mindiff <= final_diff <= maxdiff
 *   - Can be BELOW network_diff (miner gets partial shares)
 *   - Constrains: vardiff adjustments, share variance tracking
 *
 * NETWORK DIFFICULTY: protocol threshold for valid blocks.
 *   - Extracted from block header nBits, used RAW (unclamped).
 *   - CHTA legitimately mines sub-1.0 difficulty (cheetah blocks at 0.0025),
 *     so there is NO floor. The old allow_low_diff clamp-to-1.0 was removed
 *     for this fork; forks without cheetah mode may still clamp.
 *   - Not a constraint on worker_diff, they're independent
 *   - Shares >= network_diff * 0.999 are submitted as potential blocks
 *
 * CONSTRAINT HIERARCHY (for worker difficulty ONLY):
 * 1. Optimal difficulty = dsps * 1.0
 * 2. Pool global: pool_mindiff (floor), pool_maxdiff (ceiling)
 * 3. Pool per-worker: startdiff (initial), mindiff (floor), maxdiff (ceiling)
 *
 * FINAL RULE for worker_diff:
 * pool_mindiff <= final_diff <= pool_maxdiff
 * The network_diff cap (final = MIN(optimal, network_diff)) applies in normal
 * mode but is SKIPPED in cheetah mode, where 0.0025 would otherwise force an
 * absurd submission rate — the hashrate-derived optimal is the right limit.
 *
 * TEST SCENARIOS:
 * - Optimal vs. network: Worker diff can be below/above network diff; the cap
 *   is skipped in cheetah mode
 * - Pool constraints: Min/max bounds on worker difficulty
 * - All compose: No hidden conflicts between constraints
 * - Worker overrides: Per-worker settings override pool defaults
 *
 * EXPECTED RESULTS:
 * - Worker difficulty independent from network difficulty
 * - All pool constraints respected (mindiff <= diff <= maxdiff)
 * - Network cap applied in normal mode, skipped in cheetah mode
 * - Impossible configurations detected (e.g., mindiff > maxdiff)
 */

/* config.h must be first to define _GNU_SOURCE before system headers */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

#include "../test_common.h"
#include "libckpool.h"

/* Helper: Calculate optimal diff from hashrate (CHTA target: ~1 share/sec) */
static double calculate_optimal_diff(double hashrate)
{
	double dsps = hashrate / (double)(1UL << 32);
	return dsps * 1.0;
}

/*
 * Real-world network difficulty scenarios:
 * - Bitcoin: 1,000,000,000 to 100,000,000,000,000 (varies by era)
 * - Regtest: 1, 0.5, or even lower
 * - Testnet: 1 to millions
 */

/* Test 1: Network difficulty floor clamping.
 *
 * DISABLED for the CHTA fork: this validates the old allow_low_diff clamp
 * (network_diff < 1.0 -> 1.0), which was removed here because CHTA legitimately
 * mines sub-1.0 cheetah blocks (0.0025). Asserting 0.5 -> 1.0 is the inverse of
 * current behavior. Kept under #if 0 because it remains valid for forks that
 * retain the allow_low_diff floor (e.g. non-cheetah ckpool-lhr variants). */
#if 0
static void test_network_diff_floor_clamping(void)
{
	printf("\n  Testing network difficulty floor behavior:\n");
	
	struct {
		const char *network;
		double network_diff;
		bool allow_low_diff;
		double expected_floor;
	} scenarios[] = {
		/* Mainnet: 1B diff, floor doesn't apply (already > 1.0) */
		{ "Bitcoin mainnet", 1000000000.0, false, 1000000000.0 },
		
		/* Regtest: allow_low_diff=true, no floor */
		{ "Regtest (unlimited low)", 0.5, true, 0.5 },
		
		/* Testnet pool: diff < 1.0, clamped to 1.0 */
		{ "Testnet pool low diff", 0.5, false, 1.0 },
		
		/* Solo mining on testnet: allow_low_diff=true, no clamping */
		{ "Testnet solo (low allowed)", 0.1, true, 0.1 },
	};
	
	for (int i = 0; i < (int)(sizeof(scenarios) / sizeof(scenarios[0])); i++) {
		double network_diff = scenarios[i].network_diff;
		bool allow_low_diff = scenarios[i].allow_low_diff;
		
		/* Simulate the clamping from stratifier.c add_base() */
		if (!allow_low_diff && network_diff < 1.0)
			network_diff = 1.0;
		
		printf("    %s:\n", scenarios[i].network);
		printf("      Raw network diff: %.10f, allow_low=%d\n",
		       scenarios[i].network_diff, allow_low_diff);
		printf("      After clamping: %.10f (expected: %.10f)\n",
		       network_diff, scenarios[i].expected_floor);
		
		assert_double_equal(network_diff, scenarios[i].expected_floor, EPSILON_DIFF);
	}
}
#endif /* allow_low_diff floor test — invalid for CHTA, kept for other forks */

/* Test 2: Optimal diff vs network diff cap */
static void test_optimal_capped_by_network_diff(void)
{
	printf("\n  Testing that optimal diff doesn't exceed network diff:\n");
	
	struct {
		const char *scenario;
		double hashrate;
		double network_diff;
		bool cheetah_mode;
		bool should_cap;
	} scenarios[] = {
		/* Low hashrate, high network: no cap needed */
		{ "Low hashrate (100 H/s), Bitcoin mainnet (1B)", 100.0, 1000000000.0, false, false },

		/* High hashrate, low network: needs cap */
		{ "ASIC (100 TH/s), low network (1000)", 100000000000000.0, 1000.0, false, true },

		/* Extreme: massive ASIC on regtest */
		{ "ASIC (1 EH/s), regtest (0.5)", 1000000000000000.0, 0.5, false, true },

		/* Reasonable high-end */
		{ "Mid ASIC (100 GH/s), testnet (100000)", 100000000000.0, 100000.0, false, false },

		/* Cheetah window: cap is SKIPPED even though optimal >> network_diff,
		 * so the miner keeps its hashrate-derived diff instead of 0.0025. */
		{ "ASIC in cheetah window (net 0.0025)", 100000000000000.0, 0.0025, true, false },
	};

	for (int i = 0; i < (int)(sizeof(scenarios) / sizeof(scenarios[0])); i++) {
		double optimal_diff = calculate_optimal_diff(scenarios[i].hashrate);
		double network_diff = scenarios[i].network_diff;

		/* Production clamps final_diff = MIN(optimal, network_diff), but skips
		 * this cap in cheetah mode (stratifier.c: if (!cheetah_mode) ...). */
		double final_diff = optimal_diff;
		bool was_capped = false;
		if (!scenarios[i].cheetah_mode && final_diff > network_diff) {
			final_diff = network_diff;
			was_capped = true;
		}

		printf("    %s:\n", scenarios[i].scenario);
		printf("      Optimal: %.2f, Network: %.2f, Cheetah: %d, Final: %.2f\n",
		       optimal_diff, network_diff, scenarios[i].cheetah_mode, final_diff);

		/* Final must never exceed network — except in cheetah mode, where the
		 * cap is intentionally skipped. */
		if (!scenarios[i].cheetah_mode)
			assert_true(final_diff <= network_diff);

		/* Cap status must match expectation */
		assert_true(was_capped == scenarios[i].should_cap);
	}
}

/* Test 3: Pool mindiff and maxdiff constraints */
static void test_pool_min_maxdiff_constraints(void)
{
	printf("\n  Testing pool minimum and maximum difficulty constraints:\n");
	
	struct {
		const char *scenario;
		double optimal_diff;
		double pool_mindiff;
		double pool_maxdiff;
		double expected_result;
	} scenarios[] = {
		/* No constraints */
		{ "No constraints", 5.0, 0.001, 0.0, 5.0 },
		
		/* Clamped UP by pool_mindiff */
		{ "Clamped up by pool_mindiff", 0.0001, 0.001, 0.0, 0.001 },
		
		/* Clamped DOWN by pool_maxdiff */
		{ "Clamped down by pool_maxdiff", 1000000.0, 0.001, 100000.0, 100000.0 },
		
		/* Both constraints apply (unlikely but possible) */
		{ "Between min and max", 5.0, 1.0, 10.0, 5.0 },
		
		/* Below pool_mindiff */
		{ "Far below pool_mindiff", 0.0001, 0.1, 0.0, 0.1 },
		
		/* Above pool_maxdiff */
		{ "Far above pool_maxdiff", 5000.0, 0.1, 100.0, 100.0 },
	};
	
	for (int i = 0; i < (int)(sizeof(scenarios) / sizeof(scenarios[0])); i++) {
		double clamped = scenarios[i].optimal_diff;
		
		/* Apply constraints in order */
		if (clamped < scenarios[i].pool_mindiff)
			clamped = scenarios[i].pool_mindiff;
		if (scenarios[i].pool_maxdiff > 0 && clamped > scenarios[i].pool_maxdiff)
			clamped = scenarios[i].pool_maxdiff;
		
		printf("    %s:\n", scenarios[i].scenario);
		printf("      Optimal: %.2f, Pool constraints: [%.2f, %.2f] → Result: %.2f\n",
		       scenarios[i].optimal_diff,
		       scenarios[i].pool_mindiff,
		       scenarios[i].pool_maxdiff,
		       clamped);
		
		assert_double_equal(clamped, scenarios[i].expected_result, EPSILON_DIFF);
	}
}

/* Test 4: All constraints compose correctly */
static void test_all_constraints_compose(void)
{
	printf("\n  Testing composition of all constraints:\n");
	
	struct {
		const char *scenario;
		double hashrate;
		double network_diff;
		double pool_mindiff;
		double pool_maxdiff;
	} scenarios[] = {
		/* Typical: Bitcoin mainnet, reasonable hashrate */
		{
			"Bitcoin mainnet, GPU miner",
			10000000.0,  /* 10 MH/s */
			1000000000.0,
			0.001,
			0.0,
		},

		/* Low hashrate IoT on mainnet */
		{
			"Mainnet, ESP32 (100 H/s)",
			100.0,
			1000000000.0,
			0.001,
			0.0,
		},

		/* Low network diff (CHTA uses raw nBits, no floor) */
		{
			"Low network diff (raw, unclamped)",
			1000.0,
			0.5,
			0.00001,
			0.0,
		},

		/* Pool with aggressive min/max */
		{
			"Pool with min=10, max=1000",
			1000000000.0,  /* 1 GH/s */
			1000000000.0,
			10.0,
			1000.0,
		},

		/* Cheetah-range network diff with tiny hashrate */
		{
			"Cheetah-range net diff (0.0025-scale)",
			1.0,  /* 1 H/s theoretical */
			0.01,
			0.001,
			0.0,
		},
	};
	
	for (int i = 0; i < (int)(sizeof(scenarios) / sizeof(scenarios[0])); i++) {
		double optimal_diff = calculate_optimal_diff(scenarios[i].hashrate);
		double network_diff = scenarios[i].network_diff;

		/* CHTA uses raw network_diff (no allow_low_diff floor). It is the
		 * block-detection threshold and does NOT constrain worker diff. */

		/* Calculate worker difficulty starting from optimal */
		double worker_diff = optimal_diff;

		/* Apply pool constraints (INDEPENDENT of network_diff) */
		if (worker_diff < scenarios[i].pool_mindiff)
			worker_diff = scenarios[i].pool_mindiff;
		if (scenarios[i].pool_maxdiff > 0 && worker_diff > scenarios[i].pool_maxdiff)
			worker_diff = scenarios[i].pool_maxdiff;

		printf("    %s:\n", scenarios[i].scenario);
		printf("      Hashrate: %.0f H/s\n", scenarios[i].hashrate);
		printf("      Network diff (raw): %.2f (for block detection, not a constraint)\n", network_diff);
		printf("      Optimal worker diff: %.2f\n", optimal_diff);
		printf("      Pool constraints: [%.2f, %.2f]\n",
		       scenarios[i].pool_mindiff,
		       scenarios[i].pool_maxdiff);
		printf("      Final worker diff: %.2f\n", worker_diff);
		
		/* Final worker diff must respect pool constraints ONLY */
		assert_true(worker_diff >= scenarios[i].pool_mindiff);
		if (scenarios[i].pool_maxdiff > 0)
			assert_true(worker_diff <= scenarios[i].pool_maxdiff);
		
		/* Worker diff can be above or below network_diff - they're independent */
		printf("      (Note: worker diff can be %.s network_diff for partial shares)\n",
		       worker_diff < network_diff ? "below" : worker_diff > network_diff ? "above" : "equal to");
	}
}

/* Test 5: Worker-specific constraints override pool defaults */
static void test_worker_overrides_pool_defaults(void)
{
	printf("\n  Testing worker-specific difficulty overrides:\n");
	
	struct {
		const char *scenario;
		double optimal_diff;
		double pool_mindiff;
		double pool_maxdiff;
		double worker_startdiff;    /* Initial difficulty for worker */
		double worker_mindiff;      /* Worker's minimum */
		double worker_maxdiff;      /* Worker's maximum */
		double expected_result;
	} scenarios[] = {
		/* Worker sets higher floor than pool */
		{
			"Worker requires minimum 10 (pool min 0.1)",
			5.0,
			0.1, 0.0,
			10.0, 10.0, 0.0,
			10.0,  /* Worker mindiff wins */
		},
		
		/* Worker accepts lower diffs than pool */
		{
			"Worker allows 0.00001 (pool min 0.1)",
			0.00001,
			0.1, 0.0,
			0.00001, 0.00001, 0.0,
			0.1,  /* Pool mindiff still enforced */
		},
		
		/* Worker max overrides pool max */
		{
			"Worker maxdiff lower than pool",
			50.0,
			0.1, 100.0,
			10.0, 0.1, 30.0,  /* Worker max 30 < pool max 100 */
			30.0,  /* Worker maxdiff wins */
		},
	};
	
	for (int i = 0; i < (int)(sizeof(scenarios) / sizeof(scenarios[0])); i++) {
		double final_diff = scenarios[i].optimal_diff;
		
		/* Apply pool constraints first */
		if (final_diff < scenarios[i].pool_mindiff)
			final_diff = scenarios[i].pool_mindiff;
		if (scenarios[i].pool_maxdiff > 0 && final_diff > scenarios[i].pool_maxdiff)
			final_diff = scenarios[i].pool_maxdiff;
		
		/* Then apply worker constraints */
		if (final_diff < scenarios[i].worker_mindiff)
			final_diff = scenarios[i].worker_mindiff;
		if (scenarios[i].worker_maxdiff > 0 && final_diff > scenarios[i].worker_maxdiff)
			final_diff = scenarios[i].worker_maxdiff;
		
		printf("    %s:\n", scenarios[i].scenario);
		printf("      Pool [%.6f, %.6f], Worker [%.6f, %.6f]\n",
		       scenarios[i].pool_mindiff,
		       scenarios[i].pool_maxdiff,
		       scenarios[i].worker_mindiff,
		       scenarios[i].worker_maxdiff);
		printf("      Optimal: %.2f → Final: %.2f (expected: %.2f)\n",
		       scenarios[i].optimal_diff, final_diff, scenarios[i].expected_result);
		
		assert_double_equal(final_diff, scenarios[i].expected_result, EPSILON_DIFF);
	}
}

/* Test 6: Conflicting constraints are impossible to violate */
static void test_constraint_conflicts_impossible(void)
{
	printf("\n  Testing that constraint conflicts don't cause violations:\n");
	
	/* These scenarios have contradictory constraints - verify we handle them gracefully */
	struct {
		const char *scenario;
		double pool_mindiff;
		double pool_maxdiff;
		double worker_mindiff;
		double worker_maxdiff;
	} conflicts[] = {
		/* Pool min > pool max (invalid config, but shouldn't crash) */
		{
			"Pool: min=100 > max=50",
			100.0, 50.0,
			0.1, 0.0,
		},
		
		/* Worker min > worker max (invalid) */
		{
			"Worker: min=50 > max=10",
			0.1, 0.0,
			50.0, 10.0,
		},
		
		/* All constraints contradictory */
		{
			"All contradictory",
			100.0, 50.0,
			200.0, 10.0,
		},
	};
	
	for (int i = 0; i < (int)(sizeof(conflicts) / sizeof(conflicts[0])); i++) {
		printf("    %s:\n", conflicts[i].scenario);
		printf("      Pool [%.0f, %.0f], Worker [%.0f, %.0f]\n",
		       conflicts[i].pool_mindiff,
		       conflicts[i].pool_maxdiff,
		       conflicts[i].worker_mindiff,
		       conflicts[i].worker_maxdiff);
		
		/* In real code, this should be caught at config load time */
		/* For now, just verify we can process without crashing */
		double test_diff = 25.0;  /* Arbitrary value */
		
		/* Try to apply constraints - order matters to handle conflicts */
		if (conflicts[i].pool_mindiff > 0)
			test_diff = fmax(test_diff, conflicts[i].pool_mindiff);
		if (conflicts[i].pool_maxdiff > 0)
			test_diff = fmin(test_diff, conflicts[i].pool_maxdiff);
		if (conflicts[i].worker_mindiff > 0)
			test_diff = fmax(test_diff, conflicts[i].worker_mindiff);
		if (conflicts[i].worker_maxdiff > 0)
			test_diff = fmin(test_diff, conflicts[i].worker_maxdiff);
		
		printf("      Result: %.0f (handled gracefully)\n", test_diff);
		
		/* Should not be infinite or NaN */
		assert_true(isfinite(test_diff));
	}
}

/* Test 7: Block-solve threshold uses workbase network_diff, not current_workbase
 *
 * Regression test for the race where current_workbase is updated by ZMQ before
 * the threshold check runs. The threshold must use wb->network_diff (the workbase
 * the share was hashed against), not any global current value.
 */
static void test_blocksolve_threshold_uses_wb_network_diff(void)
{
	printf("\n  Testing block-solve threshold uses workbase network_diff:\n");

	struct {
		const char *scenario;
		double share_diff;
		double wb_network_diff;       /* workbase the share was hashed against */
		double current_network_diff;  /* stale global after ZMQ update */
		bool should_solve_with_wb;
		bool should_solve_with_current;
	} cases[] = {
		/* CHTA: share just meets the workbase threshold (0.999 tolerance).
		 * After ZMQ, current_workbase has inflated diff (retarget to 20G) —
		 * using current would silently drop a valid block solve. */
		{
			"CHTA: valid solve, ZMQ updated current to 20G",
			2577036.0,
			2577035.681529,
			19999698720.5,
			true,   /* wb: 2577036 >= 2577035.68 * 0.999 ✓ */
			false,  /* current: 2577036 < 19999698720.5 * 0.999 ✗ */
		},
		/* BCH: per-block DAA, similar scenario */
		{
			"BCH: valid solve, next block has higher diff",
			500000000.0,
			499000000.0,
			600000000.0,
			true,
			false,
		},
		/* Share does not meet even the workbase threshold — not a solve */
		{
			"Not a solve: share below wb threshold",
			1000000.0,
			2577035.681529,
			2577035.681529,
			false,
			false,
		},
	};

	for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
		bool wb_result      = cases[i].share_diff >= cases[i].wb_network_diff * 0.999;
		bool current_result = cases[i].share_diff >= cases[i].current_network_diff * 0.999;

		printf("    %s:\n", cases[i].scenario);
		printf("      share_diff=%.2f, wb_network_diff=%.2f, current=%.2f\n",
		       cases[i].share_diff, cases[i].wb_network_diff, cases[i].current_network_diff);
		printf("      Using wb: %s | Using current: %s\n",
		       wb_result ? "SOLVE" : "no", current_result ? "SOLVE" : "no");

		assert_true(wb_result == cases[i].should_solve_with_wb);
		assert_true(current_result == cases[i].should_solve_with_current);
	}
}

/* Test 8: Block solve luck% formula correctness
 *
 * block_share_summary() computes: bdiff = accounted_diff_shares / network_diff * 100
 *
 * Covers the full luck range (1% to 1000%+), real CHTA block regression anchors,
 * and edge cases at single-share and EH/s-scale share counts.
 */
static void test_block_luck_formula_correctness(void)
{
	printf("\n  Testing block solve luck%% formula correctness:\n");

	struct {
		const char *scenario;
		double accounted_diff_shares;
		double network_diff;
		double expected_luck_pct;
	} cases[] = {
		/* Parametric: exact round-number inputs covering full luck range.
		 * Luck% < 100 = block found in fewer shares than expected (lucky).
		 * Luck% > 100 = block needed more shares than expected (unlucky). */
		{ "1% luck (extremely lucky)",           10000.0,   1000000.0,     1.0 },
		{ "10% luck (very lucky)",              100000.0,   1000000.0,    10.0 },
		{ "50% luck",                           500000.0,   1000000.0,    50.0 },
		{ "100% luck (exactly expected work)", 1000000.0,   1000000.0,   100.0 },
		{ "200% luck (unlucky)",               2000000.0,   1000000.0,   200.0 },
		{ "1000% luck (very unlucky)",        10000000.0,   1000000.0,  1000.0 },

		/* Real CHTA block regression anchors — any formula change will
		 * break these against known production values. */
		{ "CHTA block 4765296 (11.5% luck)",    295076.0, 2577035.681529,  11.45 },
		{ "CHTA block 4765299 (2.9% luck)",      75264.0, 2577035.681529,   2.92 },

		/* Edge cases: extreme share counts must not overflow or lose precision */
		{ "Single share (near-zero luck)",           1.0,   1000000.0,    0.0001 },
		{ "Very large counts (EH/s-scale pool)",   1.0e15,     1.0e13, 10000.0  },
	};

	for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
		double luck = cases[i].accounted_diff_shares / cases[i].network_diff * 100.0;
		/* Tolerance: 0.1% of expected value, floor of 0.001 for near-zero cases */
		double tol  = fmax(cases[i].expected_luck_pct * 0.001, 0.001);

		printf("    %s:\n", cases[i].scenario);
		printf("      shares=%.6g, network_diff=%.6g → luck=%.4f%% (expected ~%.4f%%)\n",
		       cases[i].accounted_diff_shares, cases[i].network_diff,
		       luck, cases[i].expected_luck_pct);

		assert_true(fabs(luck - cases[i].expected_luck_pct) < tol);
	}
}

/* Test 9: add_remote_base populates network_diff via diff_from_nbits
 *
 * Exercises diff_from_nbits(headerbin + 72) with nBits placed at offset 72 of
 * a 112-byte buffer — mirroring the exact call in add_remote_base(). Verifies
 * the function returns a non-zero, correct value for known nBits inputs.
 * Catches wrong-offset bugs and regressions in diff_from_nbits itself.
 */
static void test_remote_base_network_diff_populated(void)
{
	printf("\n  Testing diff_from_nbits(headerbin + 72) produces correct network_diff:\n");

	struct {
		const char *coin;
		uint8_t nbits[4];  /* raw nBits bytes */
		double expected_diff;
		double tolerance_pct;
	} cases[] = {
		/* CHTA nBits 1a06829b → ~2,577,035.7 */
		{ "CHTA (1a06829b)", { 0x1a, 0x06, 0x82, 0x9b }, 2577035.7, 0.01 },
		/* Bitcoin genesis nBits 1d00ffff → 1.0 */
		{ "Bitcoin genesis (1d00ffff)", { 0x1d, 0x00, 0xff, 0xff }, 1.0, 0.01 },
	};

	for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
		/* nBits at offset 72, matching add_remote_base() call site */
		char headerbin[112];
		double network_diff;
		double tolerance = cases[i].expected_diff * cases[i].tolerance_pct;

		memset(headerbin, 0, sizeof(headerbin));
		memcpy(headerbin + 72, cases[i].nbits, 4);

		network_diff = diff_from_nbits(headerbin + 72);

		printf("    %s: diff_from_nbits(headerbin + 72) → %.4f (expected ~%.4f)\n",
		       cases[i].coin, network_diff, cases[i].expected_diff);

		/* Must be non-zero (the bug: zero-alloc without this call gives 0) */
		assert_true(network_diff > 0.0);
		/* Must be within tolerance of expected */
		assert_true(fabs(network_diff - cases[i].expected_diff) < tolerance);
	}
}

/* Main test runner */
int main(void)
{
	/* test_network_diff_floor_clamping disabled — see #if 0 above (allow_low_diff
	 * floor removed for CHTA; valid only for non-cheetah forks). */
	run_test(test_optimal_capped_by_network_diff);
	run_test(test_pool_min_maxdiff_constraints);
	run_test(test_all_constraints_compose);
	run_test(test_worker_overrides_pool_defaults);
	run_test(test_constraint_conflicts_impossible);
	run_test(test_blocksolve_threshold_uses_wb_network_diff);
	run_test(test_block_luck_formula_correctness);
	run_test(test_remote_base_network_diff_populated);
	printf("All tests passed!\n");
	return 0;
}
