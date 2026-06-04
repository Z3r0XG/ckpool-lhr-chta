#ifndef STRATIFIER_INTERNAL_H
#define STRATIFIER_INTERNAL_H

#include "libckpool.h"
#include "uthash.h"
#include "utlist.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define UA_TRUNCATE_LEN 64

/* Forward declarations used by these structs */
struct userwb;
typedef struct stratum_instance stratum_instance_t;
typedef struct user_instance user_instance_t;
typedef struct worker_instance worker_instance_t;
typedef struct stratifier_data sdata_t;
typedef struct proxy_base proxy_t;
typedef struct ckpool_instance ckpool_t;

/* Share fingerprint stored in the dedupe hashtable, keyed by share hash. */
struct share {
	UT_hash_handle hh;
	uchar hash[32];
	int64_t workbase_id;
};

typedef struct share share_t;

/* Struct definitions used by stratifier and tests */

/* Combined data from users */
struct user_instance {
	UT_hash_handle hh;
	char username[128];
	int id;
	char *secondaryuserid;
	bool btcaddress;
	bool script;
	bool segwit;

	/* A linked list of all connected instances of this user */
	struct stratum_instance *clients;

	/* A linked list of all connected workers of this user */
	struct worker_instance *worker_instances;

	int workers;
	int remote_workers;
	char txnbin[48];
	int txnlen;
	struct userwb *userwbs; /* Protected by instance lock */

	double best_diff; /* Best share found by this user */
	double best_ever; /* Best share ever found by this user */

	double shares;

	double uadiff; /* Shares not yet accounted for in hashmeter */

	double dsps1; /* Diff shares per second, 1 minute rolling average */
	double dsps15s; /* ... 15 second ... */
	double dsps5; /* ... 5 minute ... */
	double dsps60;/* etc */
	double dsps1440;
	double dsps10080;
	tv_t last_share;
	tv_t last_decay;

	bool authorised; /* Has this username ever been authorised? */
	time_t auth_time;
	time_t failed_authtime; /* Last time this username failed to authorise */
	int auth_backoff; /* How long to reject any auth attempts since last failure */
	bool throttled; /* Have we begun rejecting auth attempts */
};

/* Combined data from workers with the same workername */
struct worker_instance {
	struct user_instance *user_instance;
	char *workername;
	/* last-seen user agent string for this worker (persisted in user JSON) */
	char *useragent;
	char norm_useragent[UA_TRUNCATE_LEN + 1];

	/* Number of stratum instances attached as this one worker */
	int instance_count;

	struct worker_instance *next;
	struct worker_instance *prev;

	double shares;

	double uadiff; /* Shares not yet accounted for in hashmeter */

	double dsps1;
	double dsps15s;
	double dsps5;
	double dsps60;
	double dsps1440;
	double dsps10080;
	tv_t last_share;
	tv_t last_decay;
	time_t start_time;
	time_t last_connect;

	double best_diff; /* Best share found by this worker */
	double best_ever; /* Best share ever found by this worker */
	double mindiff; /* User chosen mindiff */

	bool idle;
	bool notified_idle;
};

/* Per client stratum instance == workers */
struct stratum_instance {
	UT_hash_handle hh;
	int64_t id;

	/* Virtualid used as unique local id for passthrough clients */
	int64_t virtualid;

	struct stratum_instance *recycled_next;
	struct stratum_instance *recycled_prev;

	struct stratum_instance *user_next;
	struct stratum_instance *user_prev;

	struct stratum_instance *node_next;
	struct stratum_instance *node_prev;

	struct stratum_instance *remote_next;
	struct stratum_instance *remote_prev;

	/* Descriptive of ID number and passthrough if any */
	char identity[128];

	/* Reference count for when this instance is used outside of the
	 * instance_lock */
	int ref;

	char enonce1[36]; /* Fit up to 16 byte binary enonce1 */
	uchar enonce1bin[16];
	char enonce1var[20]; /* Fit up to 8 byte binary enonce1var */
	uint64_t enonce1_64;
	int session_id;

	double diff; /* Current diff */
	double old_diff; /* Previous diff */
	int64_t diff_change_job_id; /* Last job_id we changed diff */

	double uadiff; /* Shares not yet accounted for in hashmeter */

	double dsps1; /* Diff shares per second, 1 minute rolling average */
	double dsps15s; /* ... 15 second ... */
	double dsps5; /* ... 5 minute ... */
	double dsps60;/* etc */
	double dsps1440;
	double dsps10080;
	tv_t ldc; /* Last diff change */
	double ssdc; /* Shares since diff change */
	tv_t first_share;
	tv_t last_share;
	tv_t last_decay;
	time_t first_invalid; /* Time of first invalid in run of non stale rejects */
	time_t upstream_invalid; /* As first_invalid but for upstream responses */
	time_t start_time;

	char address[INET6_ADDRSTRLEN];
	bool node; /* Is this a mining node */
	bool subscribed;
	bool authorising; /* In progress, protected by instance_lock */
	bool authorised;
	bool dropped;
	bool idle;
	int reject;	/* Indicator that this client is having a run of rejects
			 * or other problem and should be dropped lazily if
			 * this is set to 2 */

	int latency; /* Latency when on a mining node */

	bool reconnect; /* This client really needs to reconnect */
	time_t reconnect_request; /* The time we sent a reconnect message */

	user_instance_t *user_instance;
	worker_instance_t *worker_instance;

	char *useragent;
	char *workername;
	char *password;
	bool messages; /* Is this a client that understands stratum messages */
	int user_id;
	int server; /* Which server is this instance bound to */

	ckpool_t *ckp;

	time_t last_txns; /* Last time this worker requested txn hashes */
	time_t disconnected_time; /* Time this instance disconnected */

	double suggest_diff; /* Stratum client suggested diff */
	double best_diff; /* Best share found by this instance */
	bool password_diff_set; /* Was diff set via password field? Preferred over stratum suggest */

	sdata_t *sdata; /* Which sdata this client is bound to */
	proxy_t *proxy; /* Proxy this is bound to in proxy mode */
	int proxyid; /* Which proxy id  */
	int subproxyid; /* Which subproxy */

	bool passthrough; /* Is this a passthrough */
	bool trusted; /* Is this a trusted remote server */
	bool remote; /* Is this a remote client on a trusted remote server */
};

/* -------------------------------------------------------------------------
 * Pure best_diff / best_ever update logic.
 *
 * Extracted from check_best_diff() so that unit tests can exercise the same
 * code path against the real struct types without pulling in the full
 * stratifier (sdata_t, mutexes, stratum_send_message).
 * ------------------------------------------------------------------------- */

typedef struct {
	bool best_ever;        /* sdiff is a new best_ever for user or worker */
	bool best_ever_user;   /* sdiff is a new best_ever for this user */
	bool best_ever_worker; /* sdiff is a new best_ever for this worker */
	bool best_worker;      /* sdiff is a new best_diff for this worker */
	bool best_user;        /* sdiff is a new best_diff for this user */
} best_diff_result_t;

/* Update best_ever unconditionally (gated by >), and best_diff when guard_round
 * is false OR sdiff < network_diff * 0.999.
 *
 * guard_round=false (local paths): always update best_diff — the caller owns
 *   the ordering; for confirmed solves check_best_diff runs before
 *   block_solve()→reset_bestshares(), so logging captures the solve diff.
 *
 * guard_round=true (remote SM_SHARE path only): SM_BLOCK fires reset_bestshares()
 *   before SM_SHARE arrives, so the round is already fresh; we must NOT write
 *   the solve diff into best_diff or it poisons the new round. */
static inline best_diff_result_t
update_best_diff(user_instance_t *user, worker_instance_t *worker,
		 double sdiff, double network_diff, bool guard_round)
{
	best_diff_result_t r = {false, false, false, false, false};

	if (sdiff > user->best_ever) {
		user->best_ever = sdiff;
		r.best_ever = true;
		r.best_ever_user = true;
	}
	if (sdiff > worker->best_ever) {
		worker->best_ever = sdiff;
		r.best_ever = true;
		r.best_ever_worker = true;
	}

	if (!guard_round || sdiff < network_diff * 0.999) {
		if (sdiff > worker->best_diff) { worker->best_diff = sdiff; r.best_worker = true; }
		if (sdiff > user->best_diff)   { user->best_diff   = sdiff; r.best_user   = true; }
	}
	return r;
}

/* Per-client gate: returns true when sdiff sets a new session best.
 * Confirmed solves are handled before reset_bestshares() in test_blocksolve()
 * and never reach this path, so no network_diff guard is needed here.
 * The persistent-round guard (sdiff < network_diff * 0.999, matching test_blocksolve's
 * 0.999 tolerance to handle floating-point rounding) lives in update_best_diff()
 * which protects user->best_diff and worker->best_diff written to disk. */
static inline bool
client_gate_update(stratum_instance_t *client, double sdiff)
{
	if (sdiff > client->best_diff) {
		client->best_diff = sdiff;
		return true;
	}
	return false;
}

/* Look up a share fingerprint without recording it. Pure check, no side
 * effects: lets the block-solve path detect a duplicate solve share before
 * resubmitting the block and re-triggering reset_bestshares(). */
static inline bool
share_exists(mutex_t *lock, share_t **table, const uchar *hash)
{
	share_t *match = NULL;

	mutex_lock(lock);
	HASH_FIND(hh, *table, hash, 32, match);
	mutex_unlock(lock);

	return match != NULL;
}

/* Record a solved share's fingerprint so a later duplicate is caught by
 * share_exists(). The block-solve path skips normal share accounting, so the
 * fingerprint is recorded here on a confirmed block submission only. Does not
 * touch shares_generated — the solve share is not counted as a normal share. */
static inline void
record_solve_share(mutex_t *lock, share_t **table, const uchar *hash, const int64_t wb_id)
{
	share_t *share = ckzalloc(sizeof(share_t)), *match = NULL;

	memcpy(share->hash, hash, 32);
	share->workbase_id = wb_id;

	mutex_lock(lock);
	HASH_FIND(hh, *table, hash, 32, match);
	if (likely(!match))
		HASH_ADD(hh, *table, hash, 32, share);
	mutex_unlock(lock);

	if (unlikely(match))
		dealloc(share);
}

#endif /* STRATIFIER_INTERNAL_H */
