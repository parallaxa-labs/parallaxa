/*
 * stage-cmdq.c — fixed 128-entry command ring (any thread -> graphics thread).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Design:
 *   - One pthread mutex guards the ring and the statistics. Critical sections
 *     only copy structs; callbacks always run with the lock released.
 *   - Overflow policy of stage_cmdq_push(): drop the OLDEST command and count
 *     it (newest intent wins). stage_cmdq_try_push() refuses instead.
 *   - Drain is per target: matching commands are removed in FIFO order and
 *     the remaining ones are compacted in place, keeping their order.
 *   - Commands that nobody drains (target not ticking) expire after max age.
 *     Expiry runs in every drain and, so that a queue nobody drains cannot
 *     stay full forever, also in push when the ring is full.
 */
#include <pthread.h>
#include <string.h>

#include "stage-atomic.h"
#include "stage-core.h"

/* Commands copied out per lock acquisition (bounded stack use, ~8 KB). */
#define DRAIN_BATCH 16
/* Upper bound of batches per drain call, so a producer that keeps pushing
 * for the same target cannot pin the graphics thread. */
#define DRAIN_MAX_ROUNDS (STAGE_CMDQ_CAPACITY / DRAIN_BATCH + 1)

/* The ring is zero-initialised (.bss). The lock and the default max age live
 * outside it: a non-zero initialiser on the struct would store the whole ring
 * (~64 KB) in the DLL image. */
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t q_max_age_ms = STAGE_CMDQ_DEFAULT_MAX_AGE_MS; /* guarded by q_lock */

static struct {
	struct stage_cmd ring[STAGE_CMDQ_CAPACITY];
	uint64_t stamp_ms[STAGE_CMDQ_CAPACITY];
	size_t head;  /* index of the oldest entry */
	size_t count; /* entries in use            */
	struct stage_cmdq_stats st;
} q; /* guarded by q_lock */

static volatile uint64_t next_id;
static volatile uint64_t panic_gen;

static inline size_t ring_index(size_t offset)
{
	return (q.head + offset) % STAGE_CMDQ_CAPACITY;
}

uint64_t stage_cmdq_next_id(void)
{
	return stage_atomic_inc64(&next_id);
}

static size_t take_locked(const char *uuid, uint32_t mask, struct stage_cmd *out, size_t cap,
			  bool *more, uint64_t now);

static bool push_impl(const struct stage_cmd *c, bool may_drop, uint64_t *out_id)
{
	if (!c || (unsigned)c->type >= (unsigned)STAGE_CMD_TYPE_COUNT) {
		pthread_mutex_lock(&q_lock);
		q.st.rejected++;
		pthread_mutex_unlock(&q_lock);
		return false;
	}

	/* Resolve and stamp outside the queue lock. */
	struct stage_cmd tmp = *c;
	tmp.scene_uuid[STAGE_SCENE_UUID_MAX - 1] = '\0';
	bool resolved = tmp.scene_uuid[0] != '\0' || stage_current_scene_uuid(tmp.scene_uuid);
	if (tmp.id == 0)
		tmp.id = stage_cmdq_next_id();
	uint64_t now = stage_core_now_ms();

	pthread_mutex_lock(&q_lock);
	if (!resolved) {
		q.st.rejected++;
		pthread_mutex_unlock(&q_lock);
		return false;
	}
	if (q.count == STAGE_CMDQ_CAPACITY) {
		/* Full: first discard expired commands (mask 0 takes nothing, the
		 * pass only expires and compacts). */
		bool more;
		take_locked(NULL, 0, NULL, 0, &more, now);
	}
	if (q.count == STAGE_CMDQ_CAPACITY) {
		if (!may_drop) {
			q.st.rejected++;
			pthread_mutex_unlock(&q_lock);
			return false;
		}
		q.head = (q.head + 1) % STAGE_CMDQ_CAPACITY; /* drop oldest */
		q.count--;
		q.st.dropped++;
	}
	size_t i = ring_index(q.count);
	q.ring[i] = tmp;
	q.stamp_ms[i] = now;
	q.count++;
	q.st.pushed++;
	if (q.count > q.st.high_water)
		q.st.high_water = q.count;
	pthread_mutex_unlock(&q_lock);
	if (out_id)
		*out_id = tmp.id;
	return true;
}

bool stage_cmdq_push(const struct stage_cmd *c)
{
	return push_impl(c, true, NULL);
}

bool stage_cmdq_try_push(const struct stage_cmd *c)
{
	return push_impl(c, false, NULL);
}

bool stage_cmdq_push_id(const struct stage_cmd *c, bool may_drop, uint64_t *out_id)
{
	if (out_id)
		*out_id = 0;
	return push_impl(c, may_drop, out_id);
}

static inline bool cmd_matches(const struct stage_cmd *c, const char *uuid, uint32_t mask)
{
	return (STAGE_CMD_MASK(c->type) & mask) && (!uuid || strcmp(c->scene_uuid, uuid) == 0);
}

/*
 * One pass over the ring under the lock: expired entries are discarded,
 * matching entries are moved to out (up to cap), all others are compacted
 * towards the head in their original order. *more is set when matching
 * entries had to stay because out was full.
 */
static size_t take_locked(const char *uuid, uint32_t mask, struct stage_cmd *out, size_t cap,
			  bool *more, uint64_t now)
{
	size_t n = 0, kept = 0;
	*more = false;
	for (size_t r = 0; r < q.count; r++) {
		size_t ri = ring_index(r);
		/* `now` is read before the lock, so a command pushed meanwhile can
		 * be stamped later than now: never treat that as old. */
		uint64_t stamp = q.stamp_ms[ri];
		if (q_max_age_ms && now > stamp && now - stamp > q_max_age_ms) {
			q.st.expired++;
			continue;
		}
		if (cmd_matches(&q.ring[ri], uuid, mask)) {
			if (n < cap) {
				out[n++] = q.ring[ri];
				continue;
			}
			*more = true;
		}
		size_t wi = ring_index(kept);
		if (wi != ri) {
			q.ring[wi] = q.ring[ri];
			q.stamp_ms[wi] = q.stamp_ms[ri];
		}
		kept++;
	}
	q.count = kept;
	q.st.drained += n;
	return n;
}

size_t stage_cmdq_drain(const char *scene_uuid, void (*cb)(const struct stage_cmd *cmd, void *ud),
			void *ud)
{
	return stage_cmdq_drain_types(scene_uuid, STAGE_CMD_MASK_ALL, cb, ud);
}

size_t stage_cmdq_drain_types(const char *scene_uuid, uint32_t type_mask,
			      void (*cb)(const struct stage_cmd *cmd, void *ud), void *ud)
{
	if (!cb)
		return 0;

	struct stage_cmd batch[DRAIN_BATCH];
	size_t total = 0;
	for (int round = 0; round < DRAIN_MAX_ROUNDS; round++) {
		bool more = false;
		uint64_t now = stage_core_now_ms();

		pthread_mutex_lock(&q_lock);
		size_t n = q.count ? take_locked(scene_uuid, type_mask, batch, DRAIN_BATCH, &more, now) : 0;
		if (!n)
			more = false;
		pthread_mutex_unlock(&q_lock);

		for (size_t i = 0; i < n; i++)
			cb(&batch[i], ud);
		total += n;
		if (!more)
			break;
	}
	return total;
}

size_t stage_cmdq_purge(const char *scene_uuid)
{
	return stage_cmdq_purge_types(scene_uuid, STAGE_CMD_MASK_ALL);
}

size_t stage_cmdq_purge_types(const char *scene_uuid, uint32_t type_mask)
{
	size_t removed = 0, kept = 0;
	pthread_mutex_lock(&q_lock);
	for (size_t r = 0; r < q.count; r++) {
		size_t ri = ring_index(r);
		if (cmd_matches(&q.ring[ri], scene_uuid, type_mask)) {
			removed++;
			continue;
		}
		size_t wi = ring_index(kept);
		if (wi != ri) {
			q.ring[wi] = q.ring[ri];
			q.stamp_ms[wi] = q.stamp_ms[ri];
		}
		kept++;
	}
	q.count = kept;
	q.st.purged += removed;
	pthread_mutex_unlock(&q_lock);
	return removed;
}

void stage_cmdq_set_max_age_ms(uint32_t ms)
{
	pthread_mutex_lock(&q_lock);
	q_max_age_ms = ms;
	pthread_mutex_unlock(&q_lock);
}

void stage_cmdq_get_stats(struct stage_cmdq_stats *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&q_lock);
	*out = q.st;
	out->depth = q.count;
	pthread_mutex_unlock(&q_lock);
}

void stage_panic_trigger(void)
{
	stage_cmdq_purge(NULL);
	stage_atomic_inc64(&panic_gen);
}

uint64_t stage_panic_generation(void)
{
	return stage_atomic_load64(&panic_gen);
}

/* Internal (stage-core.c): reset at module unload. */
void stage_cmdq_shutdown_internal(void)
{
	pthread_mutex_lock(&q_lock);
	q.head = 0;
	q.count = 0;
	pthread_mutex_unlock(&q_lock);
}
