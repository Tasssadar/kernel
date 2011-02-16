/*
 * BFQ, or Budget Fair Queueing, disk scheduler.
 *
 * Based on ideas and code from CFQ:
 * Copyright (C) 2003 Jens Axboe <axboe@kernel.dk>
 *
 * Copyright (C) 2008 Fabio Checconi <fabio@gandalf.sssup.it>
 *		      Paolo Valente <paolo.valente@unimore.it>
 *
 * BFQ is a proportional share disk scheduling algorithm based on CFQ,
 * that uses the B-WF2Q+ internal scheduler to assign budgets (i.e.,
 * slices in the service domain) to the tasks accessing the disk.  It
 * has been introduced in [1], where the interested reader can find an
 * accurate description of the algorithm, the guarantees it provides
 * and their formal proofs.  With respect to the algorithm presented
 * in the paper, this implementation adds a timeout to limit the maximum
 * time a queue can spend to complete its assigned budget, and a
 * hierarchical extension, based on H-WF2Q+.
 *
 * B-WF2Q+ is based on WF2Q+, that is described in [2], together with
 * H-WF2Q+, while the augmented tree used to implement B-WF2Q+ with O(log N)
 * complexity derives from the one introduced with EEVDF in [3].
 *
 * [1] P. Valente and F. Checconi, ``High Throughput Disk Scheduling
 *     with Deterministic Guarantees on Bandwidth Distribution,'' to be
 *     published.
 *
 *     http://algo.ing.unimo.it/people/paolo/disk_sched/bfq.pdf
 *
 * [2] Jon C.R. Bennett and H. Zhang, ``Hierarchical Packet Fair Queueing
 *     Algorithms,'' IEEE/ACM Transactions on Networking, 5(5):675-689,
 *     Oct 1997.
 *
 *     http://www.cs.cmu.edu/~hzhang/papers/TON-97-Oct.ps.gz
 *
 * [3] I. Stoica and H. Abdel-Wahab, ``Earliest Eligible Virtual Deadline
 *     First: A Flexible and Accurate Mechanism for Proportional Share
 *     Resource Allocation,'' technical report.
 *
 *     http://www.cs.berkeley.edu/~istoica/papers/eevdf-tr-95.pdf
 */
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/cgroup.h>
#include <linux/elevator.h>
#include <linux/rbtree.h>
#include <linux/ioprio.h>
#include "bfq.h"

/* Max nr of dispatches in one round of service. */
static const int bfq_quantum = 4;

/* Expiration time of each request (jiffies). */
static const int bfq_fifo_expire[2] = { HZ / 4, HZ / 8 };

/* Maximum backwards seek, in KiB. */
static const int bfq_back_max = 16 * 1024;

/* Penalty of a backwards seek. */
static const int bfq_back_penalty = 2;

/* Idling period duration (jiffies). */
static int bfq_slice_idle = HZ / 125;

/* Default maximum budget values (sectors). */
static const int bfq_max_budget = 16 * 1024;
static const int bfq_max_budget_async_rq = 4;

/* Default timeout values (jiffies), approximating CFQ defaults. */
static const int bfq_timeout_sync = HZ / 8;
static int bfq_timeout_async = HZ / 25;

struct kmem_cache *bfq_pool;
struct kmem_cache *bfq_ioc_pool;

static DEFINE_PER_CPU(unsigned long, bfq_ioc_count);
static struct completion *bfq_ioc_gone;
static DEFINE_SPINLOCK(bfq_ioc_gone_lock);

/* Below this threshold (in ms), we consider thinktime immediate. */
#define BFQ_MIN_TT		2

/* hw_tag detection: parallel requests threshold and min samples needed. */
#define BFQ_HW_QUEUE_THRESHOLD	4
#define BFQ_HW_QUEUE_SAMPLES	32

/* Budget feedback step. */
#define BFQ_BUDGET_STEP		128

/* Min samples used for peak rate estimation (for autotuning). */
#define BFQ_PEAK_RATE_SAMPLES	32

/* Shift used for peak rate fixed precision calculations. */
#define BFQ_RATE_SHIFT		16

#define BFQ_SERVICE_TREE_INIT	((struct bfq_service_tree)		\
				{ RB_ROOT, RB_ROOT, NULL, NULL, 0, 0 })

#define RQ_CIC(rq)		\
	((struct cfq_io_context *) (rq)->elevator_private)
#define RQ_BFQQ(rq)		((rq)->elevator_private2)

#include "bfq-ioc.c"
#include "bfq-sched.c"
#include "bfq-cgroup.c"

static inline int bfq_class_idle(struct bfq_queue *bfqq)
{
	return bfqq->entity.ioprio_class == IOPRIO_CLASS_IDLE;
}

static inline int bfq_sample_valid(int samples)
{
	return samples > 80;
}

/*
 * We regard a request as SYNC, if either it's a read or has the SYNC bit
 * set (in which case it could also be a direct WRITE).
 */
static inline int bfq_bio_sync(struct bio *bio)
{
	if (bio_data_dir(bio) == READ || bio_rw_flagged(bio, BIO_RW_SYNCIO))
		return 1;

	return 0;
}

/*
 * Scheduler run of queue, if there are requests pending and no one in the
 * driver that will restart queueing.
 */
static inline void bfq_schedule_dispatch(struct bfq_data *bfqd)
{
	if (bfqd->queued != 0) {
		bfq_log(bfqd, "schedule dispatch");
		kblockd_schedule_work(bfqd->queue, &bfqd->unplug_work);
	}
}

static inline int bfq_queue_empty(struct request_queue *q)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;

	return bfqd->queued == 0;
}

/*
 * Lifted from AS - choose which of rq1 and rq2 that is best served now.
 * We choose the request that is closesr to the head right now.  Distance
 * behind the head is penalized and only allowed to a certain extent.
 */
static struct request *bfq_choose_req(struct bfq_data *bfqd,
				      struct request *rq1,
				      struct request *rq2)
{
	sector_t last, s1, s2, d1 = 0, d2 = 0;
	unsigned long back_max;
#define BFQ_RQ1_WRAP	0x01 /* request 1 wraps */
#define BFQ_RQ2_WRAP	0x02 /* request 2 wraps */
	unsigned wrap = 0; /* bit mask: requests behind the disk head? */

	if (rq1 == NULL || rq1 == rq2)
		return rq2;
	if (rq2 == NULL)
		return rq1;

	if (rq_is_sync(rq1) && !rq_is_sync(rq2))
		return rq1;
	else if (rq_is_sync(rq2) && !rq_is_sync(rq1))
		return rq2;
	if (rq_is_meta(rq1) && !rq_is_meta(rq2))
		return rq1;
	else if (rq_is_meta(rq2) && !rq_is_meta(rq1))
		return rq2;

	s1 = blk_rq_pos(rq1);
	s2 = blk_rq_pos(rq2);

	last = bfqd->last_position;

	/*
	 * by definition, 1KiB is 2 sectors
	 */
	back_max = bfqd->bfq_back_max * 2;

	/*
	 * Strict one way elevator _except_ in the case where we allow
	 * short backward seeks which are biased as twice the cost of a
	 * similar forward seek.
	 */
	if (s1 >= last)
		d1 = s1 - last;
	else if (s1 + back_max >= last)
		d1 = (last - s1) * bfqd->bfq_back_penalty;
	else
		wrap |= BFQ_RQ1_WRAP;

	if (s2 >= last)
		d2 = s2 - last;
	else if (s2 + back_max >= last)
		d2 = (last - s2) * bfqd->bfq_back_penalty;
	else
		wrap |= BFQ_RQ2_WRAP;

	/* Found required data */

	/*
	 * By doing switch() on the bit mask "wrap" we avoid having to
	 * check two variables for all permutations: --> faster!
	 */
	switch (wrap) {
	case 0: /* common case for CFQ: rq1 and rq2 not wrapped */
		if (d1 < d2)
			return rq1;
		else if (d2 < d1)
			return rq2;
		else {
			if (s1 >= s2)
				return rq1;
			else
				return rq2;
		}

	case BFQ_RQ2_WRAP:
		return rq1;
	case BFQ_RQ1_WRAP:
		return rq2;
	case (BFQ_RQ1_WRAP|BFQ_RQ2_WRAP): /* both rqs wrapped */
	default:
		/*
		 * Since both rqs are wrapped,
		 * start with the one that's further behind head
		 * (--> only *one* back seek required),
		 * since back seek takes more time than forward.
		 */
		if (s1 <= s2)
			return rq1;
		else
			return rq2;
	}
}

static struct request *bfq_find_next_rq(struct bfq_data *bfqd,
					struct bfq_queue *bfqq,
					struct request *last)
{
	struct rb_node *rbnext = rb_next(&last->rb_node);
	struct rb_node *rbprev = rb_prev(&last->rb_node);
	struct request *next = NULL, *prev = NULL;

	BUG_ON(RB_EMPTY_NODE(&last->rb_node));

	if (rbprev != NULL)
		prev = rb_entry_rq(rbprev);

	if (rbnext != NULL)
		next = rb_entry_rq(rbnext);
	else {
		rbnext = rb_first(&bfqq->sort_list);
		if (rbnext && rbnext != &last->rb_node)
			next = rb_entry_rq(rbnext);
	}

	return bfq_choose_req(bfqd, next, prev);
}

static void bfq_del_rq_rb(struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq);
	struct bfq_data *bfqd = bfqq->bfqd;
	const int sync = rq_is_sync(rq);

	BUG_ON(bfqq->queued[sync] == 0);
	bfqq->queued[sync]--;
	bfqd->queued--;

	elv_rb_del(&bfqq->sort_list, rq);

	if (bfq_bfqq_busy(bfqq) && bfqq != bfqd->active_queue &&
	    RB_EMPTY_ROOT(&bfqq->sort_list))
		bfq_del_bfqq_busy(bfqd, bfqq, 1);
}

/**
 * bfq_updated_next_req - update the queue after a new next_rq selection.
 * @bfqd: the device data the queue belongs to.
 * @bfqq: the queue to update.
 *
 * Whenever the first request of a queue changes we try to allocate it
 * enough service (if it has grown), or to anticipate its finish time
 * (if it has shrinked), to reduce the time it has to wait, still taking
 * into account the queue budget.  We try to avoid the queue having not
 * enough service allocated for its first request, thus having to go
 * through two dispatch rounds to actually dispatch the request.
 */
static void bfq_updated_next_req(struct bfq_data *bfqd,
				 struct bfq_queue *bfqq)
{
	struct bfq_entity *entity = &bfqq->entity;
	struct bfq_service_tree *st = bfq_entity_service_tree(entity);
	struct request *next_rq = bfqq->next_rq;
	bfq_service_t new_budget;

	if (next_rq == NULL)
		return;

	if (bfqq == bfqd->active_queue)
		/*
		 * In order not to break guarantees, budgets cannot be
		 * changed after an entity has been selected.
		 */
		return;

	BUG_ON(entity->tree != &st->active);
	BUG_ON(entity == entity->sched_data->active_entity);

	new_budget = max_t(bfq_service_t, bfqq->max_budget, blk_rq_sectors(next_rq));
	entity->budget = new_budget;
	bfq_log_bfqq(bfqd, bfqq, "budget=%lu", new_budget);
	bfq_activate_bfqq(bfqd, bfqq);
}

static void bfq_add_rq_rb(struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq);
	struct bfq_entity *entity = &bfqq->entity;
	struct bfq_data *bfqd = bfqq->bfqd;
	struct request *__alias, *next_rq;

	bfqq->queued[rq_is_sync(rq)]++;
	bfqd->queued++;

	/*
	 * Looks a little odd, but the first insert might return an alias,
	 * if that happens, put the alias on the dispatch list.
	 */
	while ((__alias = elv_rb_add(&bfqq->sort_list, rq)) != NULL)
		bfq_dispatch_insert(bfqd->queue, __alias);

	/*
	 * check if this request is a better next-serve candidate
	 */
	next_rq = bfq_choose_req(bfqd, bfqq->next_rq, rq);
	BUG_ON(next_rq == NULL);
	bfqq->next_rq = next_rq;

	if (!bfq_bfqq_busy(bfqq)) {
		entity->budget = max_t(bfq_service_t, bfqq->max_budget,
				                   blk_rq_sectors(next_rq));
		bfq_add_bfqq_busy(bfqd, bfqq);
	} else
		bfq_updated_next_req(bfqd, bfqq);
}

static void bfq_reposition_rq_rb(struct bfq_queue *bfqq, struct request *rq)
{
	elv_rb_del(&bfqq->sort_list, rq);
	bfqq->queued[rq_is_sync(rq)]--;
	bfqq->bfqd->queued--;
	bfq_add_rq_rb(rq);
}

static struct request *bfq_find_rq_fmerge(struct bfq_data *bfqd,
					  struct bio *bio)
{
	struct task_struct *tsk = current;
	struct cfq_io_context *cic;
	struct bfq_queue *bfqq;

	cic = bfq_cic_lookup(bfqd, tsk->io_context);
	if (cic == NULL)
		return NULL;

	bfqq = cic_to_bfqq(cic, bfq_bio_sync(bio));
	if (bfqq != NULL) {
		sector_t sector = bio->bi_sector + bio_sectors(bio);

		return elv_rb_find(&bfqq->sort_list, sector);
	}

	return NULL;
}

static void bfq_activate_request(struct request_queue *q, struct request *rq)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;

	bfqd->rq_in_driver[rq_is_sync(rq)]++;
	bfqd->last_position = blk_rq_pos(rq) + blk_rq_sectors(rq);
}

static void bfq_deactivate_request(struct request_queue *q, struct request *rq)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
        const int sync = rq_is_sync(rq);

        WARN_ON(!bfqd->rq_in_driver[sync]);
        bfqd->rq_in_driver[sync]--;
}

static void bfq_remove_request(struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq);
	struct bfq_data *bfqd = bfqq->bfqd;

	if (bfqq->next_rq == rq) {
		bfqq->next_rq = bfq_find_next_rq(bfqd, bfqq, rq);
		bfq_updated_next_req(bfqd, bfqq);
	}

	list_del_init(&rq->queuelist);
	bfq_del_rq_rb(rq);

	if (rq_is_meta(rq)) {
		WARN_ON(bfqq->meta_pending == 0);
		bfqq->meta_pending--;
	}
}

static int bfq_merge(struct request_queue *q, struct request **req,
		     struct bio *bio)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct request *__rq;

	__rq = bfq_find_rq_fmerge(bfqd, bio);
	if (__rq != NULL && elv_rq_merge_ok(__rq, bio)) {
		*req = __rq;
		return ELEVATOR_FRONT_MERGE;
	}

	return ELEVATOR_NO_MERGE;
}

static void bfq_merged_request(struct request_queue *q, struct request *req,
			       int type)
{
	if (type == ELEVATOR_FRONT_MERGE) {
		struct bfq_queue *bfqq = RQ_BFQQ(req);

		bfq_reposition_rq_rb(bfqq, req);
	}
}

static void bfq_merged_requests(struct request_queue *q, struct request *rq,
				struct request *next)
{
	/*
	 * reposition in fifo if next is older than rq
	 */
	if (!list_empty(&rq->queuelist) && !list_empty(&next->queuelist) &&
	    time_before(rq_fifo_time(next), rq_fifo_time(rq))) {
		list_move(&rq->queuelist, &next->queuelist);
		rq_set_fifo_time(rq, rq_fifo_time(next));
	}

	bfq_remove_request(next);
}

static int bfq_allow_merge(struct request_queue *q, struct request *rq,
			   struct bio *bio)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct cfq_io_context *cic;
	struct bfq_queue *bfqq;

	/* Disallow merge of a sync bio into an async request. */
	if (bfq_bio_sync(bio) && !rq_is_sync(rq))
		return 0;

	/*
	 * Lookup the bfqq that this bio will be queued with. Allow
	 * merge only if rq is queued there.
	 */
	cic = bfq_cic_lookup(bfqd, current->io_context);
	if (cic == NULL)
		return 0;

	bfqq = cic_to_bfqq(cic, bfq_bio_sync(bio));
	if (bfqq == RQ_BFQQ(rq))
		return 1;

	return 0;
}

static void __bfq_set_active_queue(struct bfq_data *bfqd,
				   struct bfq_queue *bfqq)
{
	if (bfqq != NULL) {
		bfq_mark_bfqq_must_alloc(bfqq);
		bfq_mark_bfqq_budget_new(bfqq);
		bfq_clear_bfqq_fifo_expire(bfqq);

		bfqq->budgets_assigned = (bfqq->budgets_assigned*7 + 256) / 8;

		bfq_log_bfqq(bfqd, bfqq, "active");
	}

	bfqd->active_queue = bfqq;
}

/*
 * Get and set a new active queue for service.
 */
static struct bfq_queue *bfq_set_active_queue(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq;

	bfqq = bfq_get_next_queue(bfqd);
	__bfq_set_active_queue(bfqd, bfqq);
	return bfqq;
}

#define CIC_SEEKY(cic) ((cic)->seek_mean > (8 * 1024))

static void bfq_arm_slice_timer(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq = bfqd->active_queue;
	struct cfq_io_context *cic;
	unsigned long sl;

	WARN_ON(!RB_EMPTY_ROOT(&bfqq->sort_list));

	/* Idling is disabled, either manually or by past process history. */
	if (bfqd->bfq_slice_idle == 0 || !bfq_bfqq_idle_window(bfqq))
		return;

	/* Tasks have exited, don't wait. */
	cic = bfqd->active_cic;
	if (cic == NULL || atomic_read(&cic->ioc->nr_tasks) == 0)
		return;

	bfq_mark_bfqq_wait_request(bfqq);

	/*
	 * we don't want to idle for seeks, but we do want to allow
	 * fair distribution of slice time for a process doing back-to-back
	 * seeks. so allow a little bit of time for him to submit a new rq
	 */
	sl = bfqd->bfq_slice_idle;
	if (bfq_sample_valid(cic->seek_samples) && CIC_SEEKY(cic))
		sl = min(sl, msecs_to_jiffies(BFQ_MIN_TT));

	bfqd->last_idling_start = ktime_get();
	mod_timer(&bfqd->idle_slice_timer, jiffies + sl);
	bfq_log(bfqd, "arm idle: %lu", sl);
}

static void bfq_set_budget_timeout(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq = bfqd->active_queue;

	bfqd->last_budget_start = ktime_get();

	bfq_clear_bfqq_budget_new(bfqq);
	bfqq->budget_timeout = jiffies +
		bfqd->bfq_timeout[!!bfq_bfqq_sync(bfqq)];
}

/*
 * Move request from internal lists to the request queue dispatch list.
 */
static void bfq_dispatch_insert(struct request_queue *q, struct request *rq)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct bfq_queue *bfqq = RQ_BFQQ(rq);

	bfqq->next_rq = bfq_find_next_rq(bfqd, bfqq, rq);
	bfq_remove_request(rq);
	bfqq->dispatched++;
	elv_dispatch_sort(q, rq);

	if (bfq_bfqq_sync(bfqq))
		bfqd->sync_flight++;
}

/*
 * return expired entry, or NULL to just start from scratch in rbtree
 */
static struct request *bfq_check_fifo(struct bfq_queue *bfqq)
{
	struct request *rq = NULL;

	if (bfq_bfqq_fifo_expire(bfqq))
		return NULL;

	bfq_mark_bfqq_fifo_expire(bfqq);

	if (list_empty(&bfqq->fifo))
		return NULL;

	rq = rq_entry_fifo(bfqq->fifo.next);
	if (time_before(jiffies, rq_fifo_time(rq)))
		return NULL;

	return rq;
}

static inline bfq_service_t bfq_bfqq_budget_left(struct bfq_queue *bfqq)
{
	struct bfq_entity *entity = &bfqq->entity;
	return entity->budget - entity->service;
}

static void __bfq_bfqq_expire(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	BUG_ON(bfqq != bfqd->active_queue);

	__bfq_bfqd_reset_active(bfqd);

	if (RB_EMPTY_ROOT(&bfqq->sort_list))
		bfq_del_bfqq_busy(bfqd, bfqq, 1);
	else
		bfq_activate_bfqq(bfqd, bfqq);
}

/**
 * bfq_default_budget - return the default budget for @bfqq on @bfqd.
 * @bfqd: the device descriptor.
 * @bfqq: the queue to consider.
 *
 * We use 3/4 of the @bfqd maximum budget as the default value
 * for the max_budget field of the queues.  This lets the feedback
 * mechanism to start from some middle ground, then the behavior
 * of the task will drive the heuristics towards high values, if
 * it behaves as a greedy sequential reader, or towards small values
 * if it shows a more intermittent behavior.
 */
static bfq_service_t bfq_default_budget(struct bfq_data *bfqd,
					struct bfq_queue *bfqq)
{
	bfq_service_t budget;

	/*
	 * When we need an estimate of the peak rate we need to avoid
	 * to give budgets that are too short due to previous measurements.
	 * So, in the first 10 assignments use a ``safe'' budget value.
	 */
	if (bfqq->budgets_assigned < 194 && bfqd->bfq_user_max_budget == 0)
		budget = bfq_max_budget;
	else
		budget = bfqd->bfq_max_budget;

	return budget - budget / 4;
}

static inline bfq_service_t bfq_min_budget(struct bfq_data *bfqd,
					   struct bfq_queue *bfqq)
{
	return bfqd->bfq_max_budget / 2;
}

/**
 * __bfq_bfqq_recalc_budget - try to adapt the budget to the @bfqq behavior.
 * @bfqd: device data.
 * @bfqq: queue to update.
 * @reason: reason for expiration.
 *
 * Handle the feedback on @bfqq budget.  This is driven by the following
 * principles:
 *   - async queues get always the maximum budget value (their ability to
 *     dispatch is limited by @bfqd->bfq_max_budget_async_rq).
 *   - If @bfqq has been too idle we decrease its budget, as it is likely
 *     to be more interested in latency than in throughput.
 *   - If @bfqq took too much to consume its budget it is likely to be
 *     seeky, so reset the budget to the default, in order to have all
 *     the seeky queues to be charged for the same service, trying to
 *     achieve fairness at least in the time domain among them.
 *   - If @bfqq exhausted its budget treat it as a greedy reader, in
 *     order to run it at full speed.
 *   - If @bfqq expired due to lack of requests leave its budget untouched.
 */
static void __bfq_bfqq_recalc_budget(struct bfq_data *bfqd,
				     struct bfq_queue *bfqq,
				     enum bfqq_expiration reason)
{
	struct request *next_rq;
	bfq_service_t budget, min_budget;

	budget = bfqq->max_budget;
	min_budget = bfq_min_budget(bfqd, bfqq);

	BUG_ON(bfqq != bfqd->active_queue);

	if (bfq_bfqq_sync(bfqq)) {
		switch (reason) {
		case BFQ_BFQQ_TOO_IDLE:
			if (budget > min_budget + BFQ_BUDGET_STEP)
				budget -= BFQ_BUDGET_STEP;
			else
				budget = min_budget;
			break;
		case BFQ_BFQQ_BUDGET_TIMEOUT:
			budget = bfq_default_budget(bfqd, bfqq);
			break;
		case BFQ_BFQQ_BUDGET_EXHAUSTED:
			budget = min(budget + 8 * BFQ_BUDGET_STEP,
				     bfqd->bfq_max_budget);
			break;
		case BFQ_BFQQ_NO_MORE_REQUESTS:
		default:
			return;
		}
	} else
		budget = bfqd->bfq_max_budget;

	bfqq->max_budget = budget;

	if (bfqq->budgets_assigned >= 194 && bfqd->bfq_user_max_budget == 0 &&
	    bfqq->max_budget > bfqd->bfq_max_budget)
		bfqq->max_budget = bfqd->bfq_max_budget;

	/*
	 * Make sure that we have enough budget for the next request.
	 * Since the finish time of the bfqq must be kept in sync with
	 * the budget, be sure to call __bfq_bfqq_expire() after the
	 * update.
	 */
	next_rq = bfqq->next_rq;
	if (next_rq != NULL)
		bfqq->entity.budget = max(bfqq->max_budget,
					  (bfq_service_t)blk_rq_sectors(next_rq));
	bfq_log_bfqq(bfqd, bfqq, "budget=%lu (%d)", bfqq->entity.budget,
		     bfq_bfqq_sync(bfqq));
}

static bfq_service_t bfq_calc_max_budget(u64 peak_rate, u64 timeout)
{
	bfq_service_t max_budget;

	/*
	 * The max_budget calculated when autotuning is equal to the
	 * amount of sectors transfered in 0.75 * timeout_sync at the
	 * estimated peak rate.
	 */
	max_budget = (bfq_service_t)(peak_rate * 1000 *
				     timeout >> BFQ_RATE_SHIFT);
	max_budget -= max_budget / 4;

	return max_budget;
}

static int bfq_update_peak_rate(struct bfq_data *bfqd, struct bfq_queue *bfqq,
				int compensate)
{
	u64 bw, usecs, expected, timeout;
	ktime_t delta;
	int update = 0;

	if (!bfq_bfqq_sync(bfqq) || bfq_bfqq_budget_new(bfqq))
		return 0;

	delta = compensate ? bfqd->last_idling_start : ktime_get();
	delta = ktime_sub(delta, bfqd->last_budget_start);
	usecs = ktime_to_us(delta);

	/* Don't trust short/unrealistic values. */
	if (usecs < 100 || usecs >= LONG_MAX)
		return 0;

	/*
	 * Calculate the bandwidth for the last slice.  We use a 64 bit
	 * value to store the peak rate, in sectors per usec in fixed
	 * point math.  We do so to have enough precision in the estimate
	 * and to avoid overflows.
	 */
	bw = (u64)bfqq->entity.service << BFQ_RATE_SHIFT;
	do_div(bw, (unsigned long)usecs);

	timeout = jiffies_to_msecs(bfqd->bfq_timeout[SYNC]);

	/*
	 * Use only long (> 20ms) intervals to filter out spikes for
	 * the peak rate estimation.
	 */
	if (usecs > 20000) {
		if (bw > bfqd->peak_rate) {
			bfqd->peak_rate = bw;
			update = 1;
			bfq_log(bfqd, "peak_rate=%llu", bw);
		}

		update |= bfqd->peak_rate_samples == BFQ_PEAK_RATE_SAMPLES - 1;

		if (bfqd->peak_rate_samples < BFQ_PEAK_RATE_SAMPLES)
			bfqd->peak_rate_samples++;

		if (bfqd->peak_rate_samples == BFQ_PEAK_RATE_SAMPLES &&
		    update && bfqd->bfq_user_max_budget == 0) {
			bfqd->bfq_max_budget =
				bfq_calc_max_budget(bfqd->peak_rate, timeout);
			bfq_log(bfqd, "max_budget=%lu", bfqd->bfq_max_budget);
		}
	}

	/*
	 * A process is considered ``slow'' (i.e., seeky, so that we
	 * cannot treat it fairly in the service domain, as it would
	 * slow down too much the other processes) if, when a slice
	 * ends for whatever reason, it has received service at a
	 * rate that would not be high enough to complete the budget
	 * before the budget timeout expiration.
	 */
	expected = bw * 1000 * timeout >> BFQ_RATE_SHIFT;

	return expected > bfqq->entity.budget;
}

/*
 * bfq_bfqq_expire - expire a queue.
 * @bfqd: device owning the queue.
 * @bfqq: the queue to expire.
 * @compensate: if true, compensate for the time spent idling.
 * @reason: the reason causing the expiration.
 *
 * The behavior is the following: when a queue expires because it has
 * been idling for too much we sync its finish time with the service
 * received and decrease its budget.  If @bfqq expires due to budget
 * exhaustion we increase its budget and sync its finish time.
 * If @bfqq expires due to budget timeout we do not sync its finish time
 * to avoid seeky queues to take too much disk time; instead we charge
 * it the maximum budget value.  Using the max budget value for all the
 * queues that expire due to budget timeout has the effect of using the
 * WF2Q+ scheduler to assign timeslices to those queues, without violating
 * the service domain guarantees for well-behaved queues.
 */
static void bfq_bfqq_expire(struct bfq_data *bfqd,
			    struct bfq_queue *bfqq,
			    int compensate,
			    enum bfqq_expiration reason)
{
	int slow;

	slow = bfq_update_peak_rate(bfqd, bfqq, compensate);

	/*
	 * Treat slow (i.e., seeky) traffic as timed out, to not favor
	 * it over sequential traffic (a seeky queue consumes less budget,
	 * so it would receive smaller timestamps wrt a sequential one
	 * when an idling timer fires).
	 */
	if (slow && reason == BFQ_BFQQ_TOO_IDLE)
		reason = BFQ_BFQQ_BUDGET_TIMEOUT;

	if (reason == BFQ_BFQQ_BUDGET_TIMEOUT || !bfq_bfqq_sync(bfqq))
		bfq_bfqq_charge_full_budget(bfqq);

	bfq_log_bfqq(bfqd, bfqq, "expire (%d, %d)", reason, slow);

	__bfq_bfqq_recalc_budget(bfqd, bfqq, reason);
	__bfq_bfqq_expire(bfqd, bfqq);
}

static int bfq_bfqq_budget_timeout(struct bfq_queue *bfqq)
{
	if (bfq_bfqq_budget_new(bfqq))
		return 0;

	if (time_before(jiffies, bfqq->budget_timeout))
		return 0;

	return 1;
}

/*
 * Select a queue for service.  If we have a current active queue,
 * check whether to continue servicing it, or retrieve and set a new one.
 */
static struct bfq_queue *bfq_select_queue(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq;
	struct request *next_rq;
	enum bfqq_expiration reason = BFQ_BFQQ_BUDGET_TIMEOUT;

	bfqq = bfqd->active_queue;
	if (bfqq == NULL)
		goto new_queue;

	if (bfq_bfqq_budget_timeout(bfqq)) {
		bfq_bfqq_charge_full_budget(bfqq);
		goto expire;
	}

	next_rq = bfqq->next_rq;
	/*
	 * If bfqq has requests queued and it has enough budget left to
	 * serve them, keep the queue, otherwise expire it.
	 */
	if (next_rq != NULL) {
		if (blk_rq_sectors(next_rq) > bfq_bfqq_budget_left(bfqq)) {
			reason = BFQ_BFQQ_BUDGET_EXHAUSTED;
			goto expire;
		} else
			goto keep_queue;
	}

	/*
	 * No requests pending.  If the active queue still has requests in
	 * flight or is idling for a new request, allow either of these
	 * conditions to happen (or time out) before selecting a new queue.
	 */
	if (timer_pending(&bfqd->idle_slice_timer) ||
	    (bfqq->dispatched != 0 && bfq_bfqq_idle_window(bfqq))) {
		bfqq = NULL;
		goto keep_queue;
	}

	reason = BFQ_BFQQ_NO_MORE_REQUESTS;
expire:
	bfq_bfqq_expire(bfqd, bfqq, 0, reason);
new_queue:
	bfqq = bfq_set_active_queue(bfqd);
keep_queue:
	return bfqq;
}

/*
 * Dispatch some requests from bfqq, moving them to the request queue
 * dispatch list.
 */
static int __bfq_dispatch_requests(struct bfq_data *bfqd,
				   struct bfq_queue *bfqq,
				   int max_dispatch)
{
	int dispatched = 0;

	BUG_ON(RB_EMPTY_ROOT(&bfqq->sort_list));

	do {
		struct request *rq;

		/* Follow expired path, else get first next available. */
		rq = bfq_check_fifo(bfqq);
		if (rq == NULL)
			rq = bfqq->next_rq;

		if (blk_rq_sectors(rq) > bfq_bfqq_budget_left(bfqq)) {
			/*
			 * Expire the queue for budget exhaustion, and
			 * make sure that the next act_budget is enough
			 * to serve the next request, even if it comes
			 * from the fifo expired path.
			 */
			bfqq->next_rq = rq;
			goto expire;
		}

		/* Finally, insert request into driver dispatch list. */
		bfq_bfqq_served(bfqq, blk_rq_sectors(rq));
		bfq_dispatch_insert(bfqd->queue, rq);

		dispatched++;

		if (bfqd->active_cic == NULL) {
			atomic_long_inc(&RQ_CIC(rq)->ioc->refcount);
			bfqd->active_cic = RQ_CIC(rq);
		}

		if (RB_EMPTY_ROOT(&bfqq->sort_list))
			break;
	} while (dispatched < max_dispatch);

	if (bfqd->busy_queues > 1 && ((!bfq_bfqq_sync(bfqq) &&
	    dispatched >= bfqd->bfq_max_budget_async_rq) ||
	    bfq_class_idle(bfqq)))
		goto expire;

	return dispatched;

expire:
	bfq_bfqq_expire(bfqd, bfqq, 0, BFQ_BFQQ_BUDGET_EXHAUSTED);
	return dispatched;
}

static int __bfq_forced_dispatch_bfqq(struct bfq_queue *bfqq)
{
	int dispatched = 0;

	while (bfqq->next_rq != NULL) {
		bfq_dispatch_insert(bfqq->bfqd->queue, bfqq->next_rq);
		dispatched++;
	}

	BUG_ON(!list_empty(&bfqq->fifo));
	return dispatched;
}

/*
 * Drain our current requests.  Used for barriers and when switching
 * io schedulers on-the-fly.
 */
static int bfq_forced_dispatch(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq, *n;
	struct bfq_service_tree *st;
	int dispatched = 0;

	bfqq = bfqd->active_queue;
	if (bfqq != NULL)
		__bfq_bfqq_expire(bfqd, bfqq);

	/*
	 * Loop through classes, and be careful to leave the scheduler
	 * in a consistent state, as feedback mechanisms and vtime
	 * updates cannot be disabled during the process.
	 */
	list_for_each_entry_safe(bfqq, n, &bfqd->active_list, bfqq_list) {
		st = bfq_entity_service_tree(&bfqq->entity);

		dispatched += __bfq_forced_dispatch_bfqq(bfqq);
		bfqq->max_budget = bfq_default_budget(bfqd, bfqq);

		bfq_forget_idle(st);
	}

	BUG_ON(bfqd->busy_queues != 0);

	return dispatched;
}

static int bfq_dispatch_requests(struct request_queue *q, int force)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct bfq_queue *bfqq;
	int dispatched;

	if (bfqd->busy_queues == 0)
		return 0;

	if (unlikely(force))
		return bfq_forced_dispatch(bfqd);

	dispatched = 0;
	while ((bfqq = bfq_select_queue(bfqd)) != NULL) {
		int max_dispatch;

		max_dispatch = bfqd->bfq_quantum;
		if (bfq_class_idle(bfqq))
			max_dispatch = 1;

		if (!bfq_bfqq_sync(bfqq))
			max_dispatch = bfqd->bfq_max_budget_async_rq;

		if (bfqq->dispatched >= max_dispatch) {
			if (bfqd->busy_queues > 1)
				break;
			if (bfqq->dispatched >= 4 * max_dispatch)
				break;
		}

	        if (bfq_bfqq_idle_window(bfqq) && bfqd->rq_in_driver[BLK_RW_ASYNC])
	                return 0;

		if (bfqd->sync_flight != 0 && !bfq_bfqq_sync(bfqq))
			break;

		bfq_clear_bfqq_wait_request(bfqq);
		BUG_ON(timer_pending(&bfqd->idle_slice_timer));

		dispatched += __bfq_dispatch_requests(bfqd, bfqq, max_dispatch);
	}

	bfq_log(bfqd, "dispatched=%d", dispatched);
	return dispatched;
}

/*
 * Task holds one reference to the queue, dropped when task exits.  Each rq
 * in-flight on this queue also holds a reference, dropped when rq is freed.
 *
 * Queue lock must be held here.
 */
static void bfq_put_queue(struct bfq_queue *bfqq)
{
	struct bfq_data *bfqd = bfqq->bfqd;

	BUG_ON(atomic_read(&bfqq->ref) <= 0);

	if (!atomic_dec_and_test(&bfqq->ref))
		return;

	BUG_ON(rb_first(&bfqq->sort_list) != NULL);
	BUG_ON(bfqq->allocated[READ] + bfqq->allocated[WRITE] != 0);
	BUG_ON(bfqq->entity.tree != NULL);
	BUG_ON(bfq_bfqq_busy(bfqq));
	BUG_ON(bfqd->active_queue == bfqq);

	bfq_log_bfqq(bfqd, bfqq, "freed");

	kmem_cache_free(bfq_pool, bfqq);
}

static void bfq_exit_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	if (bfqq == bfqd->active_queue) {
		__bfq_bfqq_expire(bfqd, bfqq);
		bfq_schedule_dispatch(bfqd);
	}

	bfq_put_queue(bfqq);
}

/*
 * Update the entity prio values; note that the new values will not
 * be used until the next (re)activation.
 */
static void bfq_init_prio_data(struct bfq_queue *bfqq, struct io_context *ioc)
{
	struct task_struct *tsk = current;
	int ioprio_class;

	if (!bfq_bfqq_prio_changed(bfqq))
		return;

	ioprio_class = IOPRIO_PRIO_CLASS(ioc->ioprio);
	switch (ioprio_class) {
	default:
		printk(KERN_ERR "bfq: bad prio %x\n", ioprio_class);
	case IOPRIO_CLASS_NONE:
		/*
		 * no prio set, inherit CPU scheduling settings
		 */
		bfqq->entity.new_ioprio = task_nice_ioprio(tsk);
		bfqq->entity.new_ioprio_class = task_nice_ioclass(tsk);
		break;
	case IOPRIO_CLASS_RT:
		bfqq->entity.new_ioprio = task_ioprio(ioc);
		bfqq->entity.new_ioprio_class = IOPRIO_CLASS_RT;
		break;
	case IOPRIO_CLASS_BE:
		bfqq->entity.new_ioprio = task_ioprio(ioc);
		bfqq->entity.new_ioprio_class = IOPRIO_CLASS_BE;
		break;
	case IOPRIO_CLASS_IDLE:
		bfqq->entity.new_ioprio_class = IOPRIO_CLASS_IDLE;
		bfqq->entity.new_ioprio = 7;
		bfq_clear_bfqq_idle_window(bfqq);
		break;
	}

	bfqq->entity.ioprio_changed = 1;

	/*
	 * keep track of original prio settings in case we have to temporarily
	 * elevate the priority of this queue
	 */
	bfqq->org_ioprio = bfqq->entity.new_ioprio;
	bfqq->org_ioprio_class = bfqq->entity.new_ioprio_class;
	bfq_clear_bfqq_prio_changed(bfqq);
}

static void bfq_changed_ioprio(struct io_context *ioc,
			       struct cfq_io_context *cic)
{
	struct bfq_data *bfqd;
	struct bfq_queue *bfqq, *new_bfqq;
	struct bfq_group *bfqg;
	unsigned long uninitialized_var(flags);

	bfqd = bfq_get_bfqd_locked(&cic->key, &flags);
	if (unlikely(bfqd == NULL))
		return;

	bfqq = cic->cfqq[ASYNC];
	if (bfqq != NULL) {
		bfqg = container_of(bfqq->entity.sched_data, struct bfq_group,
				    sched_data);
		new_bfqq = bfq_get_queue(bfqd, bfqg, ASYNC, cic->ioc,
					 GFP_ATOMIC);
		if (new_bfqq != NULL) {
			cic->cfqq[ASYNC] = new_bfqq;
			bfq_put_queue(bfqq);
		}
	}

	bfqq = cic->cfqq[SYNC];
	if (bfqq != NULL)
		bfq_mark_bfqq_prio_changed(bfqq);

	bfq_put_bfqd_unlock(bfqd, &flags);
}

static struct bfq_queue *bfq_find_alloc_queue(struct bfq_data *bfqd,
					      struct bfq_group *bfqg,
					      int is_sync,
					      struct io_context *ioc,
					      gfp_t gfp_mask)
{
	struct bfq_queue *bfqq, *new_bfqq = NULL;
	struct cfq_io_context *cic;

retry:
	cic = bfq_cic_lookup(bfqd, ioc);
	/* cic always exists here */
	bfqq = cic_to_bfqq(cic, is_sync);

	if (bfqq == NULL) {
		if (new_bfqq != NULL) {
			bfqq = new_bfqq;
			new_bfqq = NULL;
		} else if (gfp_mask & __GFP_WAIT) {
			/*
			 * Inform the allocator of the fact that we will
			 * just repeat this allocation if it fails, to allow
			 * the allocator to do whatever it needs to attempt to
			 * free memory.
			 */
			spin_unlock_irq(bfqd->queue->queue_lock);
			new_bfqq = kmem_cache_alloc_node(bfq_pool,
					gfp_mask | __GFP_NOFAIL | __GFP_ZERO,
					bfqd->queue->node);
			spin_lock_irq(bfqd->queue->queue_lock);
			goto retry;
		} else {
			bfqq = kmem_cache_alloc_node(bfq_pool,
					gfp_mask | __GFP_ZERO,
					bfqd->queue->node);
			if (bfqq == NULL)
				goto out;
		}

		RB_CLEAR_NODE(&bfqq->entity.rb_node);
		INIT_LIST_HEAD(&bfqq->fifo);

		atomic_set(&bfqq->ref, 0);
		bfqq->bfqd = bfqd;

		bfq_mark_bfqq_prio_changed(bfqq);

		bfq_init_prio_data(bfqq, ioc);
		bfq_init_entity(&bfqq->entity, bfqg);

		if (is_sync) {
			if (!bfq_class_idle(bfqq))
				bfq_mark_bfqq_idle_window(bfqq);
			bfq_mark_bfqq_sync(bfqq);
		}
		bfqq->max_budget = bfq_default_budget(bfqd, bfqq);
		bfqq->pid = current->pid;

		bfq_log_bfqq(bfqd, bfqq, "allocated");
	}

	if (new_bfqq != NULL)
		kmem_cache_free(bfq_pool, new_bfqq);

out:
	WARN_ON((gfp_mask & __GFP_WAIT) && bfqq == NULL);
	return bfqq;
}

static struct bfq_queue **bfq_async_queue_prio(struct bfq_data *bfqd,
					       struct bfq_group *bfqg,
					       int ioprio_class, int ioprio)
{
	switch (ioprio_class) {
	case IOPRIO_CLASS_RT:
		return &bfqg->async_bfqq[0][ioprio];
	case IOPRIO_CLASS_BE:
		return &bfqg->async_bfqq[1][ioprio];
	case IOPRIO_CLASS_IDLE:
		return &bfqg->async_idle_bfqq;
	default:
		BUG();
	}
}

static struct bfq_queue *bfq_get_queue(struct bfq_data *bfqd,
				       struct bfq_group *bfqg, int is_sync,
				       struct io_context *ioc, gfp_t gfp_mask)
{
	const int ioprio = task_ioprio(ioc);
	const int ioprio_class = task_ioprio_class(ioc);
	struct bfq_queue **async_bfqq = NULL;
	struct bfq_queue *bfqq = NULL;

	if (!is_sync) {
		async_bfqq = bfq_async_queue_prio(bfqd, bfqg, ioprio_class,
						  ioprio);
		bfqq = *async_bfqq;
	}

	if (bfqq == NULL) {
		bfqq = bfq_find_alloc_queue(bfqd, bfqg, is_sync, ioc, gfp_mask);
		if (bfqq == NULL)
			return NULL;
	}

	/*
	 * pin the queue now that it's allocated, scheduler exit will prune it
	 */
	if (!is_sync && *async_bfqq == NULL) {
		atomic_inc(&bfqq->ref);
		*async_bfqq = bfqq;
	}

	atomic_inc(&bfqq->ref);
	return bfqq;
}

static void bfq_update_io_thinktime(struct bfq_data *bfqd,
				    struct cfq_io_context *cic)
{
	unsigned long elapsed = jiffies - cic->last_end_request;
	unsigned long ttime = min(elapsed, 2UL * bfqd->bfq_slice_idle);

	cic->ttime_samples = (7*cic->ttime_samples + 256) / 8;
	cic->ttime_total = (7*cic->ttime_total + 256*ttime) / 8;
	cic->ttime_mean = (cic->ttime_total + 128) / cic->ttime_samples;
}

static void bfq_update_io_seektime(struct bfq_data *bfqd,
				   struct bfq_queue *bfqq,
				   struct cfq_io_context *cic,
				   struct request *rq)
{
	sector_t sdist;
	u64 total;

	if (cic->last_request_pos < blk_rq_pos(rq))
		sdist = blk_rq_pos(rq) - cic->last_request_pos;
	else
		sdist = cic->last_request_pos - blk_rq_pos(rq);

	/*
	 * Don't allow the seek distance to get too large from the
	 * odd fragment, pagein, etc.
	 */
	if (cic->seek_samples == 0) /* first request, not really a seek */
		sdist = 0;
	else if (cic->seek_samples <= 60) /* second&third seek */
		sdist = min(sdist, (cic->seek_mean * 4) + 2*1024*1024);
	else
		sdist = min(sdist, (cic->seek_mean * 4)	+ 2*1024*64);

	cic->seek_samples = (7*cic->seek_samples + 256) / 8;
	cic->seek_total = (7*cic->seek_total + (u64)256*sdist) / 8;
	total = cic->seek_total + (cic->seek_samples/2);
	do_div(total, cic->seek_samples);
	cic->seek_mean = (sector_t)total;

	bfq_log_bfqq(bfqd, bfqq, "dist=%lu mean=%lu", sdist, cic->seek_mean);
}

/*
 * Disable idle window if the process thinks too long or seeks so much that
 * it doesn't matter.
 */
static void bfq_update_idle_window(struct bfq_data *bfqd,
				   struct bfq_queue *bfqq,
				   struct cfq_io_context *cic)
{
	int enable_idle;

	/* Don't idle for async or idle io prio class. */
	if (!bfq_bfqq_sync(bfqq) || bfq_class_idle(bfqq))
		return;

	enable_idle = bfq_bfqq_idle_window(bfqq);

	if (atomic_read(&cic->ioc->nr_tasks) == 0 || bfqd->bfq_slice_idle == 0 ||
		(!bfqd->bfq_desktop && bfqd->hw_tag && CIC_SEEKY(cic)))
		enable_idle = 0;
	else if (bfq_sample_valid(cic->ttime_samples)) {
		unsigned int slice_idle = bfqd->bfq_slice_idle;
		if (bfq_sample_valid(cic->seek_samples) && CIC_SEEKY(cic))
			slice_idle = msecs_to_jiffies(BFQ_MIN_TT);
		if (cic->ttime_mean > slice_idle)
			enable_idle = 0;
		else
			enable_idle = 1;
	}

	if (enable_idle)
		bfq_mark_bfqq_idle_window(bfqq);
	else
		bfq_clear_bfqq_idle_window(bfqq);

	bfq_log_bfqq(bfqd, bfqq, "idle_window=%d (%d)",
		     enable_idle, CIC_SEEKY(cic));
}

/*
 * Called when a new fs request (rq) is added to bfqq.  Check if there's
 * something we should do about it.
 */
static void bfq_rq_enqueued(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			    struct request *rq)
{
	struct cfq_io_context *cic = RQ_CIC(rq);

	if (rq_is_meta(rq))
		bfqq->meta_pending++;

	bfq_update_io_thinktime(bfqd, cic);
	bfq_update_io_seektime(bfqd, bfqq, cic, rq);
	bfq_update_idle_window(bfqd, bfqq, cic);

	cic->last_request_pos = blk_rq_pos(rq) + blk_rq_sectors(rq);

	if (bfqq == bfqd->active_queue && bfq_bfqq_wait_request(bfqq)) {
		/*
		 * If we are waiting for a request for this queue, let it rip
		 * immediately and flag that we must not expire this queue
		 * just now.
		 */
		bfq_clear_bfqq_wait_request(bfqq);
		del_timer(&bfqd->idle_slice_timer);
		__blk_run_queue(bfqd->queue);
	}
}

static void bfq_insert_request(struct request_queue *q, struct request *rq)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct bfq_queue *bfqq = RQ_BFQQ(rq);

	bfq_init_prio_data(bfqq, RQ_CIC(rq)->ioc);

	bfq_add_rq_rb(rq);

	rq_set_fifo_time(rq, jiffies + bfqd->bfq_fifo_expire[rq_is_sync(rq)]);
	list_add_tail(&rq->queuelist, &bfqq->fifo);

	bfq_rq_enqueued(bfqd, bfqq, rq);
}

static void bfq_update_hw_tag(struct bfq_data *bfqd)
{
	bfqd->max_rq_in_driver = max(bfqd->max_rq_in_driver,
				     rq_in_driver(bfqd));

	/*
	 * This sample is valid if the number of outstanding requests
	 * is large enough to allow a queueing behavior.  Note that the
	 * sum is not exact, as it's not taking into account deactivated
	 * requests.
	 */
	if (rq_in_driver(bfqd) + bfqd->queued < BFQ_HW_QUEUE_THRESHOLD)
		return;

	if (bfqd->hw_tag_samples++ < BFQ_HW_QUEUE_SAMPLES)
		return;

	bfqd->hw_tag = bfqd->max_rq_in_driver > BFQ_HW_QUEUE_THRESHOLD;
	bfqd->max_rq_in_driver = 0;
	bfqd->hw_tag_samples = 0;
}

static void bfq_completed_request(struct request_queue *q, struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq);
	struct bfq_data *bfqd = bfqq->bfqd;
	const int sync = rq_is_sync(rq);

	bfq_log_bfqq(bfqd, bfqq, "complete");

	bfq_update_hw_tag(bfqd);

	WARN_ON(!bfqd->rq_in_driver[sync]);
	WARN_ON(!bfqq->dispatched);
	bfqd->rq_in_driver[sync]--;
	bfqq->dispatched--;

	if (bfq_bfqq_sync(bfqq))
		bfqd->sync_flight--;

	if (sync)
		RQ_CIC(rq)->last_end_request = jiffies;

	/*
	 * If this is the active queue, check if it needs to be expired,
	 * or if we want to idle in case it has no pending requests.
	 */
	if (bfqd->active_queue == bfqq) {
		if (bfq_bfqq_budget_new(bfqq))
			bfq_set_budget_timeout(bfqd);

		if (bfq_bfqq_budget_timeout(bfqq))
			bfq_bfqq_expire(bfqd, bfqq, 0, BFQ_BFQQ_BUDGET_TIMEOUT);
		else if (sync && rq_in_driver(bfqd) == 0 &&
			 RB_EMPTY_ROOT(&bfqq->sort_list))
			bfq_arm_slice_timer(bfqd);
	}

	if (!rq_in_driver(bfqd))
		bfq_schedule_dispatch(bfqd);
}

/*
 * We temporarily boost lower priority queues if they are holding fs exclusive
 * resources.  They are boosted to normal prio (CLASS_BE/4).
 */
static void bfq_prio_boost(struct bfq_queue *bfqq)
{
	if (has_fs_excl()) {
		/*
		 * boost idle prio on transactions that would lock out other
		 * users of the filesystem
		 */
		if (bfq_class_idle(bfqq))
			bfqq->entity.new_ioprio_class = IOPRIO_CLASS_BE;
		if (bfqq->entity.new_ioprio > IOPRIO_NORM)
			bfqq->entity.new_ioprio = IOPRIO_NORM;
	} else {
		/*
		 * check if we need to unboost the queue
		 */
		if (bfqq->entity.new_ioprio_class != bfqq->org_ioprio_class)
			bfqq->entity.new_ioprio_class = bfqq->org_ioprio_class;
		if (bfqq->entity.new_ioprio != bfqq->org_ioprio)
			bfqq->entity.new_ioprio = bfqq->org_ioprio;
	}
}

static inline int __bfq_may_queue(struct bfq_queue *bfqq)
{
	if (bfq_bfqq_wait_request(bfqq) && bfq_bfqq_must_alloc(bfqq)) {
		bfq_clear_bfqq_must_alloc(bfqq);
		return ELV_MQUEUE_MUST;
	}

	return ELV_MQUEUE_MAY;
}

static int bfq_may_queue(struct request_queue *q, int rw)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct task_struct *tsk = current;
	struct cfq_io_context *cic;
	struct bfq_queue *bfqq;

	/*
	 * Don't force setup of a queue from here, as a call to may_queue
	 * does not necessarily imply that a request actually will be queued.
	 * so just lookup a possibly existing queue, or return 'may queue'
	 * if that fails.
	 */
	cic = bfq_cic_lookup(bfqd, tsk->io_context);
	if (cic == NULL)
		return ELV_MQUEUE_MAY;

	bfqq = cic_to_bfqq(cic, rw & REQ_RW_SYNC);
	if (bfqq != NULL) {
		bfq_init_prio_data(bfqq, cic->ioc);
		bfq_prio_boost(bfqq);

		return __bfq_may_queue(bfqq);
	}

	return ELV_MQUEUE_MAY;
}

/*
 * queue lock held here
 */
static void bfq_put_request(struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq);

	if (bfqq != NULL) {
		const int rw = rq_data_dir(rq);

		BUG_ON(!bfqq->allocated[rw]);
		bfqq->allocated[rw]--;

		put_io_context(RQ_CIC(rq)->ioc);

		rq->elevator_private = NULL;
		rq->elevator_private2 = NULL;

		bfq_put_queue(bfqq);
	}
}

/*
 * Allocate bfq data structures associated with this request.
 */
static int bfq_set_request(struct request_queue *q, struct request *rq,
			   gfp_t gfp_mask)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct cfq_io_context *cic;
	const int rw = rq_data_dir(rq);
	const int is_sync = rq_is_sync(rq);
	struct bfq_queue *bfqq;
	struct bfq_group *bfqg;
	unsigned long flags;

	might_sleep_if(gfp_mask & __GFP_WAIT);

	cic = bfq_get_io_context(bfqd, gfp_mask);

	spin_lock_irqsave(q->queue_lock, flags);

	if (cic == NULL)
		goto queue_fail;

	bfqg = bfq_cic_update_cgroup(cic);

	bfqq = cic_to_bfqq(cic, is_sync);
	if (bfqq == NULL) {
		bfqq = bfq_get_queue(bfqd, bfqg, is_sync, cic->ioc, gfp_mask);
		if (bfqq == NULL)
			goto queue_fail;

		cic_set_bfqq(cic, bfqq, is_sync);
	}

	bfqq->allocated[rw]++;
	atomic_inc(&bfqq->ref);

	spin_unlock_irqrestore(q->queue_lock, flags);

	rq->elevator_private = cic;
	rq->elevator_private2 = bfqq;

	return 0;

queue_fail:
	if (cic != NULL)
		put_io_context(cic->ioc);

	bfq_schedule_dispatch(bfqd);
	spin_unlock_irqrestore(q->queue_lock, flags);

	return 1;
}

static void bfq_kick_queue(struct work_struct *work)
{
	struct bfq_data *bfqd =
		container_of(work, struct bfq_data, unplug_work);
	struct request_queue *q = bfqd->queue;
	unsigned long flags;

	spin_lock_irqsave(q->queue_lock, flags);
	__blk_run_queue(bfqd->queue);
	spin_unlock_irqrestore(q->queue_lock, flags);
}

/*
 * Timer running if the active_queue is currently idling inside its time slice
 */
static void bfq_idle_slice_timer(unsigned long data)
{
	struct bfq_data *bfqd = (struct bfq_data *)data;
	struct bfq_queue *bfqq;
	unsigned long flags;
	enum bfqq_expiration reason;

	bfq_log(bfqd, "slice_timer expired");

	spin_lock_irqsave(bfqd->queue->queue_lock, flags);

	bfqq = bfqd->active_queue;
	/*
	 * Theoretical race here: active_queue can be NULL or different
	 * from the queue that was idling if the timer handler spins on
	 * the queue_lock and a new request arrives for the current
	 * queue and there is a full dispatch cycle that changes the
	 * active_queue.  This can hardly happen, but in the worst case
	 * we just expire a queue too early.
	 */
	if (bfqq != NULL) {
		reason = BFQ_BFQQ_TOO_IDLE;
		if (bfq_bfqq_budget_timeout(bfqq))
			reason = BFQ_BFQQ_BUDGET_TIMEOUT;

		bfq_bfqq_expire(bfqd, bfqq, 1, reason);
	}

	bfq_schedule_dispatch(bfqd);

	spin_unlock_irqrestore(bfqd->queue->queue_lock, flags);
}

static void bfq_shutdown_timer_wq(struct bfq_data *bfqd)
{
	del_timer_sync(&bfqd->idle_slice_timer);
	cancel_work_sync(&bfqd->unplug_work);
}

static inline void __bfq_put_async_bfqq(struct bfq_data *bfqd,
					struct bfq_queue **bfqq_ptr)
{
	struct bfq_group *root_group = bfqd->root_group;
	struct bfq_queue *bfqq = *bfqq_ptr;

	if (bfqq != NULL) {
		bfq_bfqq_move(bfqd, bfqq, &bfqq->entity, root_group);
		bfq_put_queue(bfqq);
		*bfqq_ptr = NULL;
	}
}

/*
 * Release all the bfqg references to its async queues.  If we are
 * deallocating the group these queues may still contain requests, so
 * we reparent them to the root cgroup (i.e., the only one that will
 * exist for sure untill all the requests on a device are gone).
 */
static void bfq_put_async_queues(struct bfq_data *bfqd, struct bfq_group *bfqg)
{
	int i, j;

	for (i = 0; i < 2; i++)
		for (j = 0; j < IOPRIO_BE_NR; j++)
			__bfq_put_async_bfqq(bfqd, &bfqg->async_bfqq[i][j]);

	__bfq_put_async_bfqq(bfqd, &bfqg->async_idle_bfqq);
}

static void bfq_exit_queue(struct elevator_queue *e)
{
	struct bfq_data *bfqd = e->elevator_data;
	struct request_queue *q = bfqd->queue;
	struct bfq_queue *bfqq, *n;
	struct cfq_io_context *cic;

	bfq_shutdown_timer_wq(bfqd);

	spin_lock_irq(q->queue_lock);

	while (!list_empty(&bfqd->cic_list)) {
		cic = list_entry(bfqd->cic_list.next, struct cfq_io_context,
				 queue_list);
		__bfq_exit_single_io_context(bfqd, cic);
	}

	BUG_ON(bfqd->active_queue != NULL);
	list_for_each_entry_safe(bfqq, n, &bfqd->idle_list, bfqq_list)
		bfq_deactivate_bfqq(bfqd, bfqq, 0);

	bfq_disconnect_groups(bfqd);
	spin_unlock_irq(q->queue_lock);

	bfq_shutdown_timer_wq(bfqd);

	/* Wait for cic->key accessors to exit their grace periods. */
	synchronize_rcu();

	BUG_ON(timer_pending(&bfqd->idle_slice_timer));

	bfq_free_root_group(bfqd);
	kfree(bfqd);
}

static void *bfq_init_queue(struct request_queue *q)
{
	struct bfq_group *bfqg;
	struct bfq_data *bfqd;

	bfqd = kmalloc_node(sizeof(*bfqd), GFP_KERNEL | __GFP_ZERO, q->node);
	if (bfqd == NULL)
		return NULL;

	INIT_LIST_HEAD(&bfqd->cic_list);

	bfqd->queue = q;

	bfqg = bfq_alloc_root_group(bfqd, q->node);
	if (bfqg == NULL) {
		kfree(bfqd);
		return NULL;
	}

	bfqd->root_group = bfqg;

	init_timer(&bfqd->idle_slice_timer);
	bfqd->idle_slice_timer.function = bfq_idle_slice_timer;
	bfqd->idle_slice_timer.data = (unsigned long)bfqd;

	INIT_WORK(&bfqd->unplug_work, bfq_kick_queue);

	INIT_LIST_HEAD(&bfqd->active_list);
	INIT_LIST_HEAD(&bfqd->idle_list);

	bfqd->hw_tag = 1;

	bfqd->bfq_max_budget = bfq_max_budget;

	bfqd->bfq_quantum = bfq_quantum;
	bfqd->bfq_fifo_expire[0] = bfq_fifo_expire[0];
	bfqd->bfq_fifo_expire[1] = bfq_fifo_expire[1];
	bfqd->bfq_back_max = bfq_back_max;
	bfqd->bfq_back_penalty = bfq_back_penalty;
	bfqd->bfq_slice_idle = bfq_slice_idle;
	bfqd->bfq_max_budget_async_rq = bfq_max_budget_async_rq;
	bfqd->bfq_timeout[ASYNC] = bfq_timeout_async;
	bfqd->bfq_timeout[SYNC] = bfq_timeout_sync;
	bfqd->bfq_desktop = 1;

	return bfqd;
}

static void bfq_slab_kill(void)
{
	if (bfq_pool != NULL)
		kmem_cache_destroy(bfq_pool);
	if (bfq_ioc_pool != NULL)
		kmem_cache_destroy(bfq_ioc_pool);
}

static int __init bfq_slab_setup(void)
{
	bfq_pool = KMEM_CACHE(bfq_queue, 0);
	if (bfq_pool == NULL)
		goto fail;

	bfq_ioc_pool = kmem_cache_create("bfq_io_context",
					 sizeof(struct cfq_io_context),
					 __alignof__(struct cfq_io_context),
					 0, NULL);
	if (bfq_ioc_pool == NULL)
		goto fail;

	return 0;
fail:
	bfq_slab_kill();
	return -ENOMEM;
}

static ssize_t bfq_var_show(unsigned int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t bfq_var_store(unsigned int *var, const char *page, size_t count)
{
	char *p = (char *)page;

	*var = simple_strtoul(p, &p, 10);
	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)				\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct bfq_data *bfqd = e->elevator_data;			\
	unsigned int __data = __VAR;					\
	if (__CONV)							\
		__data = jiffies_to_msecs(__data);			\
	return bfq_var_show(__data, (page));				\
}
SHOW_FUNCTION(bfq_quantum_show, bfqd->bfq_quantum, 0);
SHOW_FUNCTION(bfq_fifo_expire_sync_show, bfqd->bfq_fifo_expire[1], 1);
SHOW_FUNCTION(bfq_fifo_expire_async_show, bfqd->bfq_fifo_expire[0], 1);
SHOW_FUNCTION(bfq_back_seek_max_show, bfqd->bfq_back_max, 0);
SHOW_FUNCTION(bfq_back_seek_penalty_show, bfqd->bfq_back_penalty, 0);
SHOW_FUNCTION(bfq_slice_idle_show, bfqd->bfq_slice_idle, 1);
SHOW_FUNCTION(bfq_max_budget_show, bfqd->bfq_user_max_budget, 0);
SHOW_FUNCTION(bfq_max_budget_async_rq_show, bfqd->bfq_max_budget_async_rq, 0);
SHOW_FUNCTION(bfq_timeout_sync_show, bfqd->bfq_timeout[SYNC], 1);
SHOW_FUNCTION(bfq_timeout_async_show, bfqd->bfq_timeout[ASYNC], 1);
SHOW_FUNCTION(bfq_desktop_show, bfqd->bfq_desktop, 0);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t								\
__FUNC(struct elevator_queue *e, const char *page, size_t count)	\
{									\
	struct bfq_data *bfqd = e->elevator_data;			\
	unsigned int __data;						\
	int ret = bfq_var_store(&__data, (page), count);		\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	if (__CONV)							\
		*(__PTR) = msecs_to_jiffies(__data);			\
	else								\
		*(__PTR) = __data;					\
	return ret;							\
}
STORE_FUNCTION(bfq_quantum_store, &bfqd->bfq_quantum, 1, INT_MAX, 0);
STORE_FUNCTION(bfq_fifo_expire_sync_store, &bfqd->bfq_fifo_expire[1], 1,
		INT_MAX, 1);
STORE_FUNCTION(bfq_fifo_expire_async_store, &bfqd->bfq_fifo_expire[0], 1,
		INT_MAX, 1);
STORE_FUNCTION(bfq_back_seek_max_store, &bfqd->bfq_back_max, 0, INT_MAX, 0);
STORE_FUNCTION(bfq_back_seek_penalty_store, &bfqd->bfq_back_penalty, 1,
		INT_MAX, 0);
STORE_FUNCTION(bfq_slice_idle_store, &bfqd->bfq_slice_idle, 0, INT_MAX, 1);
STORE_FUNCTION(bfq_max_budget_async_rq_store, &bfqd->bfq_max_budget_async_rq,
		1, INT_MAX, 0);
STORE_FUNCTION(bfq_timeout_async_store, &bfqd->bfq_timeout[ASYNC], 0,
		INT_MAX, 1);
STORE_FUNCTION(bfq_desktop_store, &bfqd->bfq_desktop, 0, 1, 0);
#undef STORE_FUNCTION

static inline bfq_service_t bfq_estimated_max_budget(struct bfq_data *bfqd)
{
	u64 timeout = jiffies_to_msecs(bfqd->bfq_timeout[SYNC]);

	if (bfqd->peak_rate_samples >= BFQ_PEAK_RATE_SAMPLES)
		return bfq_calc_max_budget(bfqd->peak_rate, timeout);
	else
		return bfq_max_budget;
}

static ssize_t bfq_max_budget_store(struct elevator_queue *e,
				    const char *page, size_t count)
{
	struct bfq_data *bfqd = e->elevator_data;
	unsigned int __data;
	int ret = bfq_var_store(&__data, (page), count);

	if (__data == 0)
		bfqd->bfq_max_budget = bfq_estimated_max_budget(bfqd);
	else {
		if (__data > INT_MAX)
			__data = INT_MAX;
		bfqd->bfq_max_budget = __data;
	}

	bfqd->bfq_user_max_budget = __data;

	return ret;
}

static ssize_t bfq_timeout_sync_store(struct elevator_queue *e,
				      const char *page, size_t count)
{
	struct bfq_data *bfqd = e->elevator_data;
	unsigned int __data;
	int ret = bfq_var_store(&__data, (page), count);

	if (__data < 1)
		__data = 1;
	else if (__data > INT_MAX)
		__data = INT_MAX;

	bfqd->bfq_timeout[SYNC] = msecs_to_jiffies(__data);
	if (bfqd->bfq_user_max_budget == 0)
		bfqd->bfq_max_budget = bfq_estimated_max_budget(bfqd);

	return ret;
}

#define BFQ_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, bfq_##name##_show, bfq_##name##_store)

static struct elv_fs_entry bfq_attrs[] = {
	BFQ_ATTR(quantum),
	BFQ_ATTR(fifo_expire_sync),
	BFQ_ATTR(fifo_expire_async),
	BFQ_ATTR(back_seek_max),
	BFQ_ATTR(back_seek_penalty),
	BFQ_ATTR(slice_idle),
	BFQ_ATTR(max_budget),
	BFQ_ATTR(max_budget_async_rq),
	BFQ_ATTR(timeout_sync),
	BFQ_ATTR(timeout_async),
	BFQ_ATTR(desktop),
	__ATTR_NULL
};

static struct elevator_type iosched_bfq = {
	.ops = {
		.elevator_merge_fn = 		bfq_merge,
		.elevator_merged_fn =		bfq_merged_request,
		.elevator_merge_req_fn =	bfq_merged_requests,
		.elevator_allow_merge_fn =	bfq_allow_merge,
		.elevator_dispatch_fn =		bfq_dispatch_requests,
		.elevator_add_req_fn =		bfq_insert_request,
		.elevator_activate_req_fn =	bfq_activate_request,
		.elevator_deactivate_req_fn =	bfq_deactivate_request,
		.elevator_queue_empty_fn =	bfq_queue_empty,
		.elevator_completed_req_fn =	bfq_completed_request,
		.elevator_former_req_fn =	elv_rb_former_request,
		.elevator_latter_req_fn =	elv_rb_latter_request,
		.elevator_set_req_fn =		bfq_set_request,
		.elevator_put_req_fn =		bfq_put_request,
		.elevator_may_queue_fn =	bfq_may_queue,
		.elevator_init_fn =		bfq_init_queue,
		.elevator_exit_fn =		bfq_exit_queue,
		.trim =				bfq_free_io_context,
	},
	.elevator_attrs =	bfq_attrs,
	.elevator_name =	"bfq",
	.elevator_owner =	THIS_MODULE,
};

static int __init bfq_init(void)
{
	/*
	 * can be 0 on HZ < 1000 setups
	 */
	if (bfq_slice_idle == 0)
		bfq_slice_idle = 1;

	if (bfq_timeout_async == 0)
		bfq_timeout_async = 1;

	if (bfq_slab_setup())
		return -ENOMEM;

	elv_register(&iosched_bfq);

	return 0;
}

static void __exit bfq_exit(void)
{
	DECLARE_COMPLETION_ONSTACK(all_gone);
	elv_unregister(&iosched_bfq);
	bfq_ioc_gone = &all_gone;
	/* bfq_ioc_gone's update must be visible before reading bfq_ioc_count */
	smp_wmb();
	if (elv_ioc_count_read(bfq_ioc_count) != 0)
		wait_for_completion(&all_gone);
	bfq_slab_kill();
}

module_init(bfq_init);
module_exit(bfq_exit);

MODULE_AUTHOR("Fabio Checconi, Paolo Valente");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Budget Fair Queueing IO scheduler");
