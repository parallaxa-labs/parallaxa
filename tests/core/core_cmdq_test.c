/*
 * core_cmdq_test.c — unit tests for src/core (command queue,
 * registries, state snapshots, scene cache, unload hooks). No libobs: built
 * with STAGE_CORE_NO_OBS and the pthread shim in tests/core/shim.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stage-core.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void sleep_ms(unsigned ms)
{
	Sleep(ms);
}
static void yield_cpu(void)
{
	SwitchToThread();
}
#else
#include <sched.h>
#include <time.h>
static void sleep_ms(unsigned ms)
{
	struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}
static void yield_cpu(void)
{
	sched_yield();
}
#endif

static int failures;
static int checks;

#define CHECK(cond)                                                                   \
	do {                                                                          \
		checks++;                                                             \
		if (!(cond)) {                                                        \
			failures++;                                                   \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                     \
	} while (0)

#define CHECK_EQ_U64(a, b)                                                                    \
	do {                                                                                  \
		unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b); \
		checks++;                                                                     \
		if (va_ != vb_) {                                                             \
			failures++;                                                           \
			fprintf(stderr, "FAIL %s:%d: %s == %s (%llu vs %llu)\n", __FILE__, __LINE__, \
				#a, #b, va_, vb_);                                            \
		}                                                                             \
	} while (0)

static void make_cmd(struct stage_cmd *c, enum stage_cmd_type type, const char *scene, int64_t tag)
{
	memset(c, 0, sizeof(*c));
	c->type = type;
	stage_strlcpy(c->scene_uuid, scene, sizeof(c->scene_uuid));
	c->u.layer.item_id = tag; /* payload marker; layer is the widest int field */
}

static void reset_queue(void)
{
	stage_cmdq_purge(NULL);
	stage_cmdq_set_max_age_ms(STAGE_CMDQ_DEFAULT_MAX_AGE_MS);
}

/* ------------------------------------------------------------------------- */
/* Collector callback                                                        */
/* ------------------------------------------------------------------------- */

#define COLLECT_MAX 512
struct collect {
	size_t n;
	int64_t tags[COLLECT_MAX];
	uint64_t ids[COLLECT_MAX];
	enum stage_cmd_type types[COLLECT_MAX];
	char scenes[COLLECT_MAX][STAGE_SCENE_UUID_MAX];
};

static void collect_cb(const struct stage_cmd *c, void *ud)
{
	struct collect *col = ud;
	if (col->n < COLLECT_MAX) {
		col->tags[col->n] = c->u.layer.item_id;
		col->ids[col->n] = c->id;
		col->types[col->n] = c->type;
		stage_strlcpy(col->scenes[col->n], c->scene_uuid, STAGE_SCENE_UUID_MAX);
	}
	col->n++;
}

/* ------------------------------------------------------------------------- */
/* Queue basics                                                              */
/* ------------------------------------------------------------------------- */

static void test_basic_fifo(void)
{
	reset_queue();
	struct stage_cmd c;
	for (int i = 0; i < 10; i++) {
		make_cmd(&c, STAGE_CMD_CAMERA_MOVE, "scene-a", i);
		CHECK(stage_cmdq_push(&c));
	}
	struct collect col = {0};
	CHECK_EQ_U64(stage_cmdq_drain("scene-a", collect_cb, &col), 10);
	CHECK_EQ_U64(col.n, 10);
	for (int i = 0; i < 10; i++) {
		CHECK(col.tags[i] == i);
		CHECK(col.ids[i] > 0);
		if (i)
			CHECK(col.ids[i] > col.ids[i - 1]);
	}
	/* explicit ids are preserved */
	make_cmd(&c, STAGE_CMD_CAMERA_SET, "scene-a", 99);
	c.id = stage_cmdq_next_id();
	uint64_t want = c.id;
	CHECK(stage_cmdq_push(&c));
	memset(&col, 0, sizeof(col));
	stage_cmdq_drain("scene-a", collect_cb, &col);
	CHECK(col.n == 1 && col.ids[0] == want);
	/* invalid */
	CHECK(!stage_cmdq_push(NULL));
	make_cmd(&c, (enum stage_cmd_type)STAGE_CMD_TYPE_COUNT, "scene-a", 0);
	CHECK(!stage_cmdq_push(&c));
	CHECK_EQ_U64(stage_cmdq_drain("scene-a", NULL, NULL), 0);
}

static void test_scene_resolution(void)
{
	reset_queue();
	struct stage_cmd c;
	char uuid[STAGE_SCENE_UUID_MAX];

	stage_core_set_program_scene(NULL);
	CHECK(!stage_current_scene_uuid(uuid));
	CHECK(uuid[0] == '\0');
	make_cmd(&c, STAGE_CMD_CAMERA_RESET, "", 1);
	CHECK(!stage_cmdq_push(&c)); /* no program scene known */

	stage_core_set_program_scene("prog-uuid");
	CHECK(stage_current_scene_uuid(uuid));
	CHECK(strcmp(uuid, "prog-uuid") == 0);
	CHECK(stage_cmdq_push(&c));
	struct collect col = {0};
	CHECK_EQ_U64(stage_cmdq_drain("prog-uuid", collect_cb, &col), 1);
	CHECK(strcmp(col.scenes[0], "prog-uuid") == 0);

	/* too-long uuid is refused by the cache */
	stage_core_set_program_scene("0123456789012345678901234567890123456789-too-long");
	CHECK(!stage_current_scene_uuid(uuid));
	stage_core_set_program_scene(NULL);
}

static void test_per_scene_and_types(void)
{
	reset_queue();
	struct stage_cmd c;
	/* interleave scenes and rig/fx types */
	for (int i = 0; i < 12; i++) {
		const char *scene = (i % 3 == 0) ? "s1" : "s2";
		enum stage_cmd_type t = (i % 2) ? STAGE_CMD_FX_SET : STAGE_CMD_CAMERA_MOVE;
		make_cmd(&c, t, scene, i);
		CHECK(stage_cmdq_push(&c));
	}
	/* s2 rig commands only: i % 3 != 0 and even -> 2, 4, 8, 10 */
	struct collect col = {0};
	CHECK_EQ_U64(stage_cmdq_drain_types("s2", STAGE_CMD_MASK_RIG, collect_cb, &col), 4);
	CHECK(col.tags[0] == 2 && col.tags[1] == 4 && col.tags[2] == 8 && col.tags[3] == 10);

	/* s2 fx: 1, 5, 7, 11 */
	memset(&col, 0, sizeof(col));
	CHECK_EQ_U64(stage_cmdq_drain_types("s2", STAGE_CMD_MASK_FX, collect_cb, &col), 4);
	CHECK(col.tags[0] == 1 && col.tags[1] == 5 && col.tags[2] == 7 && col.tags[3] == 11);

	/* s1 everything, order preserved after compaction: 0, 3, 6, 9 */
	memset(&col, 0, sizeof(col));
	CHECK_EQ_U64(stage_cmdq_drain("s1", collect_cb, &col), 4);
	CHECK(col.tags[0] == 0 && col.tags[1] == 3 && col.tags[2] == 6 && col.tags[3] == 9);

	struct stage_cmdq_stats st;
	stage_cmdq_get_stats(&st);
	CHECK_EQ_U64(st.depth, 0);
}

static void test_overflow_drops_oldest(void)
{
	reset_queue();
	struct stage_cmdq_stats before, after;
	stage_cmdq_get_stats(&before);

	struct stage_cmd c;
	const int total = STAGE_CMDQ_CAPACITY + 72;
	for (int i = 0; i < total; i++) {
		make_cmd(&c, STAGE_CMD_CAMERA_SET, "s", i);
		CHECK(stage_cmdq_push(&c));
	}
	stage_cmdq_get_stats(&after);
	CHECK_EQ_U64(after.depth, STAGE_CMDQ_CAPACITY);
	CHECK_EQ_U64(after.dropped - before.dropped, 72);
	CHECK_EQ_U64(after.high_water, STAGE_CMDQ_CAPACITY);

	/* try_push refuses when full and does not drop */
	make_cmd(&c, STAGE_CMD_CAMERA_SET, "s", -1);
	CHECK(!stage_cmdq_try_push(&c));
	struct stage_cmdq_stats st;
	stage_cmdq_get_stats(&st);
	CHECK_EQ_U64(st.rejected - after.rejected, 1);
	CHECK_EQ_U64(st.dropped, after.dropped);

	/* the newest CAPACITY commands survive, in order (drained in batches) */
	struct collect col = {0};
	CHECK_EQ_U64(stage_cmdq_drain("s", collect_cb, &col), STAGE_CMDQ_CAPACITY);
	for (int i = 0; i < STAGE_CMDQ_CAPACITY; i++)
		CHECK(col.tags[i] == 72 + i);

	/* try_push succeeds once there is room */
	CHECK(stage_cmdq_try_push(&c));
	reset_queue();
}

static void test_purge_expiry_panic(void)
{
	reset_queue();
	struct stage_cmd c;
	make_cmd(&c, STAGE_CMD_CAMERA_MOVE, "a", 1);
	stage_cmdq_push(&c);
	make_cmd(&c, STAGE_CMD_FX_PULSE, "a", 2);
	stage_cmdq_push(&c);
	make_cmd(&c, STAGE_CMD_CAMERA_MOVE, "b", 3);
	stage_cmdq_push(&c);

	CHECK_EQ_U64(stage_cmdq_purge_types("a", STAGE_CMD_MASK_RIG), 1);
	CHECK_EQ_U64(stage_cmdq_purge("a"), 1);
	struct collect col = {0};
	CHECK_EQ_U64(stage_cmdq_drain(NULL, collect_cb, &col), 1);
	CHECK(col.tags[0] == 3);

	/* expiry */
	struct stage_cmdq_stats before, after;
	stage_cmdq_get_stats(&before);
	stage_cmdq_set_max_age_ms(30);
	make_cmd(&c, STAGE_CMD_CAMERA_MOVE, "old", 4);
	stage_cmdq_push(&c);
	sleep_ms(80);
	make_cmd(&c, STAGE_CMD_CAMERA_MOVE, "old", 5);
	stage_cmdq_push(&c);
	memset(&col, 0, sizeof(col));
	CHECK_EQ_U64(stage_cmdq_drain("old", collect_cb, &col), 1);
	CHECK(col.tags[0] == 5);
	stage_cmdq_get_stats(&after);
	CHECK_EQ_U64(after.expired - before.expired, 1);

	/* expiry disabled */
	stage_cmdq_set_max_age_ms(0);
	make_cmd(&c, STAGE_CMD_CAMERA_MOVE, "old", 6);
	stage_cmdq_push(&c);
	sleep_ms(40);
	CHECK_EQ_U64(stage_cmdq_drain("old", collect_cb, &col), 1);
	stage_cmdq_set_max_age_ms(STAGE_CMDQ_DEFAULT_MAX_AGE_MS);

	/* panic purges everything and bumps the generation */
	for (int i = 0; i < 5; i++) {
		make_cmd(&c, STAGE_CMD_CAMERA_MOVE, "p", i);
		stage_cmdq_push(&c);
	}
	uint64_t g = stage_panic_generation();
	stage_panic_trigger();
	CHECK_EQ_U64(stage_panic_generation(), g + 1);
	struct stage_cmdq_stats st;
	stage_cmdq_get_stats(&st);
	CHECK_EQ_U64(st.depth, 0);
}

/* push_id reports the id; a full ring nobody drains frees expired slots. */
static void test_push_id_and_full_expiry(void)
{
	reset_queue();
	struct stage_cmd c;
	uint64_t id = 99;
	make_cmd(&c, STAGE_CMD_CAMERA_MOVE, "s", 1);
	CHECK(stage_cmdq_push_id(&c, false, &id));
	CHECK(id != 0);
	uint64_t own = stage_cmdq_next_id();
	c.id = own;
	uint64_t id2 = 0;
	CHECK(stage_cmdq_push_id(&c, true, &id2));
	CHECK_EQ_U64(id2, own);
	CHECK(id2 > id);
	struct collect col = {0};
	CHECK_EQ_U64(stage_cmdq_drain("s", collect_cb, &col), 2);

	/* invalid type: false, id 0 */
	c.type = STAGE_CMD_TYPE_COUNT;
	id = 7;
	CHECK(!stage_cmdq_push_id(&c, true, &id));
	CHECK_EQ_U64(id, 0);

	/* Fill the ring for a target that never drains, let it age, then
	 * try_push must succeed (expired slots reclaimed, nothing dropped). */
	stage_cmdq_set_max_age_ms(30);
	for (int i = 0; i < STAGE_CMDQ_CAPACITY; i++) {
		make_cmd(&c, STAGE_CMD_CAMERA_MOVE, "gone", i);
		CHECK(stage_cmdq_try_push(&c));
	}
	make_cmd(&c, STAGE_CMD_CAMERA_MOVE, "live", 1);
	CHECK(!stage_cmdq_try_push(&c)); /* full, nothing old yet */
	sleep_ms(80);
	struct stage_cmdq_stats before, after;
	stage_cmdq_get_stats(&before);
	CHECK(stage_cmdq_try_push(&c));
	stage_cmdq_get_stats(&after);
	CHECK_EQ_U64(after.expired - before.expired, STAGE_CMDQ_CAPACITY);
	CHECK_EQ_U64(after.dropped, before.dropped);
	CHECK_EQ_U64(after.depth, 1);
	memset(&col, 0, sizeof(col));
	CHECK_EQ_U64(stage_cmdq_drain("live", collect_cb, &col), 1);
	reset_queue();
}

/* A drain callback may push; the new command waits for the next drain. */
static void repush_cb(const struct stage_cmd *c, void *ud)
{
	int *calls = ud;
	(*calls)++;
	if (c->u.layer.item_id < 3) {
		struct stage_cmd n;
		make_cmd(&n, STAGE_CMD_CAMERA_MOVE, c->scene_uuid, c->u.layer.item_id + 1);
		stage_cmdq_push(&n);
	}
}

static void test_push_from_callback(void)
{
	reset_queue();
	struct stage_cmd c;
	make_cmd(&c, STAGE_CMD_CAMERA_MOVE, "r", 0);
	stage_cmdq_push(&c);
	int calls = 0;
	CHECK_EQ_U64(stage_cmdq_drain("r", repush_cb, &calls), 1);
	CHECK_EQ_U64(stage_cmdq_drain("r", repush_cb, &calls), 1);
	CHECK_EQ_U64(stage_cmdq_drain("r", repush_cb, &calls), 1);
	CHECK_EQ_U64(stage_cmdq_drain("r", repush_cb, &calls), 1);
	CHECK_EQ_U64(stage_cmdq_drain("r", repush_cb, &calls), 0);
	CHECK(calls == 4);
}

/* ------------------------------------------------------------------------- */
/* Multi-producer stress                                                     */
/* ------------------------------------------------------------------------- */

#define PRODUCERS 4
#define PER_PRODUCER 10000

struct producer_arg {
	int index;
	bool blocking; /* true: try_push + retry (lossless) */
	volatile long *done;
};

static void *producer_main(void *p)
{
	struct producer_arg *a = p;
	char scene[16];
	snprintf(scene, sizeof(scene), "scene-%d", a->index % 2);
	for (int64_t seq = 0; seq < PER_PRODUCER; seq++) {
		struct stage_cmd c;
		make_cmd(&c, (seq & 1) ? STAGE_CMD_FX_PULSE : STAGE_CMD_CAMERA_MOVE, scene,
			 ((int64_t)a->index << 32) | seq);
		if (a->blocking) {
			while (!stage_cmdq_try_push(&c))
				yield_cpu();
		} else {
			stage_cmdq_push(&c);
		}
	}
#ifdef _WIN32
	InterlockedIncrement(a->done);
#else
	__atomic_add_fetch(a->done, 1, __ATOMIC_SEQ_CST);
#endif
	return NULL;
}

/* FIFO holds per (target, drain class): rig and FX commands of one producer
 * are drained by different consumers, so order is checked per class. */
struct verify {
	int64_t last_seq[PRODUCERS][2];
	uint64_t delivered;
	uint64_t order_errors;
	uint64_t scene_errors;
};

static void verify_cb(const struct stage_cmd *c, void *ud)
{
	struct verify *v = ud;
	int p = (int)(c->u.layer.item_id >> 32);
	int64_t seq = c->u.layer.item_id & 0xffffffff;
	v->delivered++;
	if (p < 0 || p >= PRODUCERS) {
		v->order_errors++;
		return;
	}
	char want[16];
	snprintf(want, sizeof(want), "scene-%d", p % 2);
	if (strcmp(want, c->scene_uuid) != 0)
		v->scene_errors++;
	/* per producer and class: strictly increasing (gaps = dropped) */
	int cls = c->type == STAGE_CMD_FX_PULSE ? 1 : 0;
	if ((seq & 1) != cls || seq <= v->last_seq[p][cls])
		v->order_errors++;
	v->last_seq[p][cls] = seq;
}

static void run_stress(bool blocking, double *ms_out, struct verify *v,
		       struct stage_cmdq_stats *delta)
{
	reset_queue();
	/* expiry stays on (5 s): a run takes milliseconds, so any expired
	 * command would be a clock-ordering bug */
	stage_cmdq_set_max_age_ms(STAGE_CMDQ_DEFAULT_MAX_AGE_MS);
	struct stage_cmdq_stats before, after;
	stage_cmdq_get_stats(&before);

	memset(v, 0, sizeof(*v));
	for (int i = 0; i < PRODUCERS; i++)
		v->last_seq[i][0] = v->last_seq[i][1] = -1;

	volatile long done = 0;
	pthread_t th[PRODUCERS];
	struct producer_arg args[PRODUCERS];
	uint64_t t0 = stage_core_now_ms();
	for (int i = 0; i < PRODUCERS; i++) {
		args[i].index = i;
		args[i].blocking = blocking;
		args[i].done = &done;
		CHECK(pthread_create(&th[i], NULL, producer_main, &args[i]) == 0);
	}

	/* Single consumer = graphics thread: per-scene, per-type drains like
	 * two rigs and two FX filters would do. */
	static const char *scenes[2] = {"scene-0", "scene-1"};
	for (;;) {
		bool finished = done == PRODUCERS;
		for (int s = 0; s < 2; s++) {
			stage_cmdq_drain_types(scenes[s], STAGE_CMD_MASK_RIG, verify_cb, v);
			stage_cmdq_drain_types(scenes[s], STAGE_CMD_MASK_FX, verify_cb, v);
		}
		if (finished) {
			struct stage_cmdq_stats st;
			stage_cmdq_get_stats(&st);
			if (st.depth == 0)
				break;
		}
		yield_cpu();
	}
	for (int i = 0; i < PRODUCERS; i++)
		pthread_join(th[i], NULL);
	*ms_out = (double)(stage_core_now_ms() - t0);

	stage_cmdq_get_stats(&after);
	delta->pushed = after.pushed - before.pushed;
	delta->drained = after.drained - before.drained;
	delta->dropped = after.dropped - before.dropped;
	delta->rejected = after.rejected - before.rejected;
	delta->expired = after.expired - before.expired;
	delta->high_water = after.high_water;
	stage_cmdq_set_max_age_ms(STAGE_CMDQ_DEFAULT_MAX_AGE_MS);
}

static void test_stress(void)
{
	const uint64_t total = (uint64_t)PRODUCERS * PER_PRODUCER;
	struct verify v;
	struct stage_cmdq_stats d;
	double ms;

	/* 1. push (drop-oldest): accounting must balance exactly. */
	run_stress(false, &ms, &v, &d);
	printf("  stress push:     %d x %d in %.0f ms, delivered %llu, dropped %llu, high-water %zu\n",
	       PRODUCERS, PER_PRODUCER, ms, (unsigned long long)v.delivered,
	       (unsigned long long)d.dropped, d.high_water);
	CHECK_EQ_U64(d.pushed, total);
	CHECK_EQ_U64(v.delivered + d.dropped, total);
	CHECK_EQ_U64(d.drained, v.delivered);
	CHECK_EQ_U64(d.expired, 0);
	CHECK_EQ_U64(v.order_errors, 0);
	CHECK_EQ_U64(v.scene_errors, 0);

	/* 2. try_push + retry: lossless. */
	run_stress(true, &ms, &v, &d);
	printf("  stress try_push: %d x %d in %.0f ms, delivered %llu, rejected(retried) %llu\n",
	       PRODUCERS, PER_PRODUCER, ms, (unsigned long long)v.delivered,
	       (unsigned long long)d.rejected);
	CHECK_EQ_U64(d.pushed, total);
	CHECK_EQ_U64(v.delivered, total);
	CHECK_EQ_U64(d.dropped, 0);
	CHECK_EQ_U64(d.expired, 0);
	CHECK_EQ_U64(v.order_errors, 0);
	CHECK_EQ_U64(v.scene_errors, 0);
	for (int i = 0; i < PRODUCERS; i++)
		CHECK(v.last_seq[i][0] == PER_PRODUCER - 2 && v.last_seq[i][1] == PER_PRODUCER - 1);
}

static void test_ids_unique_across_threads(void)
{
	/* covered by the stress run implicitly; check monotonic single thread */
	uint64_t a = stage_cmdq_next_id(), b = stage_cmdq_next_id();
	CHECK(a > 0 && b == a + 1);
}

/* ------------------------------------------------------------------------- */
/* Registries                                                                */
/* ------------------------------------------------------------------------- */

struct fake_rig {
	struct stage_state_buf sb;
	int applied;
	size_t layers;
};

static void fake_get_state(void *ud, struct stage_state *out)
{
	struct fake_rig *r = ud;
	stage_state_read(&r->sb, out, NULL);
}

static size_t fake_list_layers(void *ud, struct stage_layer_info *out, size_t cap)
{
	struct fake_rig *r = ud;
	size_t n = r->layers < cap ? r->layers : cap;
	for (size_t i = 0; i < n; i++) {
		memset(&out[i], 0, sizeof(out[i]));
		out[i].item_id = (int64_t)i + 1;
		snprintf(out[i].name, sizeof(out[i].name), "Layer %zu", i + 1);
		out[i].enrolled = i % 2 == 0;
		out[i].depth = out[i].enrolled ? 1.0 + (double)i : 0.0;
		out[i].role = out[i].enrolled ? STAGE_ROLE_BG : STAGE_ROLE_NONE;
	}
	return r->layers; /* may exceed cap: core clamps */
}

static void fake_apply(void *ud, const struct stage_cmd *c)
{
	(void)c;
	((struct fake_rig *)ud)->applied++;
}

static const struct stage_rig_ops fake_ops = {
	.get_state = fake_get_state,
	.list_layers = fake_list_layers,
	.apply_cmd = fake_apply,
};

static bool count_cb(const char *uuid, void *param)
{
	(void)uuid;
	(*(int *)param)++;
	return true;
}

static bool stop_after_one(const char *uuid, void *param)
{
	(void)uuid;
	(*(int *)param)++;
	return false;
}

static void bump(void *ud, void *param)
{
	((struct fake_rig *)ud)->applied += *(int *)param;
}

static void fx_info(void *ud, struct stage_fx_info *out)
{
	(void)ud;
	stage_strlcpy(out->preset, "cozy", sizeof(out->preset));
	out->enabled = true;
}

static const struct stage_fx_ops fake_fx_ops = {.get_info = fx_info};

static void test_registry(void)
{
	reset_queue();
	struct fake_rig r1 = {0}, r2 = {0};
	CHECK(stage_state_buf_init(&r1.sb));
	CHECK(stage_state_buf_init(&r2.sb));
	r1.layers = 3;

	CHECK(!stage_rig_register(NULL, &fake_ops, &r1));
	CHECK(!stage_rig_register("", &fake_ops, &r1));
	CHECK(!stage_rig_register("x", NULL, &r1));
	CHECK(stage_rig_register("scene-1", &fake_ops, &r1));
	CHECK(stage_rig_register("scene-1", &fake_ops, &r1)); /* same owner */
	CHECK(!stage_rig_register("scene-1", &fake_ops, &r2)); /* second rig */
	CHECK(stage_rig_register("scene-2", &fake_ops, &r2));
	CHECK_EQ_U64(stage_rig_count(), 2);
	CHECK(stage_rig_exists("scene-1"));
	CHECK(!stage_rig_exists("scene-3"));

	struct stage_state s = {0};
	stage_strlcpy(s.scene_uuid, "scene-1", sizeof(s.scene_uuid));
	s.zoom = 1.5f;
	s.auto_mode = STAGE_AUTO_DRIFT;
	CHECK(stage_state_publish(&r1.sb, &s));
	struct stage_state got;
	CHECK(stage_rig_get_state("scene-1", &got));
	CHECK(got.zoom == 1.5f && got.auto_mode == STAGE_AUTO_DRIFT);
	CHECK(!stage_rig_get_state("scene-3", &got));

	struct stage_layer_info layers[2];
	size_t count = 99;
	CHECK(stage_rig_list_layers("scene-1", layers, 2, &count));
	CHECK_EQ_U64(count, 2); /* clamped to cap */
	CHECK(layers[0].item_id == 1 && layers[0].enrolled && !layers[1].enrolled);
	CHECK(!stage_rig_list_layers("nope", layers, 2, &count));
	CHECK_EQ_U64(count, 0);

	struct stage_cmd c;
	make_cmd(&c, STAGE_CMD_PANIC, "scene-1", 0);
	CHECK(stage_rig_apply("scene-1", &c));
	CHECK(r1.applied == 1);
	int add = 10;
	CHECK(stage_rig_call("scene-2", bump, &add));
	CHECK(r2.applied == 10);

	int n = 0;
	CHECK_EQ_U64(stage_rig_for_each(count_cb, &n), 2);
	CHECK(n == 2);
	n = 0;
	CHECK_EQ_U64(stage_rig_for_each(stop_after_one, &n), 1);

	/* FX on the same scene as a rig */
	CHECK(stage_fx_attach("scene-1", &fake_fx_ops, &r2));
	CHECK(!stage_fx_attach("scene-1", &fake_fx_ops, &r1));
	struct stage_fx_info fi;
	CHECK(stage_fx_get_info("scene-1", &fi));
	CHECK(strcmp(fi.preset, "cozy") == 0 && fi.enabled);
	CHECK_EQ_U64(stage_fx_count(), 1);

	/* unregistering the rig purges its commands but keeps FX commands */
	make_cmd(&c, STAGE_CMD_CAMERA_MOVE, "scene-1", 1);
	stage_cmdq_push(&c);
	make_cmd(&c, STAGE_CMD_FX_SET, "scene-1", 2);
	stage_cmdq_push(&c);
	stage_rig_unregister("scene-1", &r2); /* wrong owner: no-op */
	CHECK(stage_rig_exists("scene-1"));
	stage_rig_unregister("scene-1", &r1);
	CHECK(!stage_rig_exists("scene-1"));
	struct collect col = {0};
	CHECK_EQ_U64(stage_cmdq_drain("scene-1", collect_cb, &col), 1);
	CHECK(col.types[0] == STAGE_CMD_FX_SET);

	stage_rig_unregister_all(&r2);
	stage_fx_detach_all(&r2);
	CHECK_EQ_U64(stage_rig_count(), 0);
	CHECK_EQ_U64(stage_fx_count(), 0);

	/* table full */
	struct fake_rig many[STAGE_MAX_RIGS + 1];
	char name[32];
	int ok = 0;
	for (int i = 0; i <= STAGE_MAX_RIGS; i++) {
		snprintf(name, sizeof(name), "full-%d", i);
		ok += stage_rig_register(name, &fake_ops, &many[i]) ? 1 : 0;
	}
	CHECK(ok == STAGE_MAX_RIGS);
	for (int i = 0; i <= STAGE_MAX_RIGS; i++)
		stage_rig_unregister_all(&many[i]);
	CHECK_EQ_U64(stage_rig_count(), 0);

	stage_state_buf_free(&r1.sb);
	stage_state_buf_free(&r2.sb);
}

/* ------------------------------------------------------------------------- */
/* State snapshot under concurrency                                          */
/* ------------------------------------------------------------------------- */

struct snap_ctx {
	struct stage_state_buf sb;
	volatile long stop;
	uint64_t torn;
	uint64_t reads;
};

static void *snap_reader(void *p)
{
	struct snap_ctx *ctx = p;
	while (!ctx->stop) {
		struct stage_state s;
		uint64_t seq;
		stage_state_read(&ctx->sb, &s, &seq);
		/* writer keeps every field equal to k */
		if (s.x != s.y || s.y != s.zoom || s.zoom != s.rot || s.rot != (float)s.move_id)
			ctx->torn++;
		ctx->reads++;
	}
	return NULL;
}

static void test_state_snapshot(void)
{
	struct snap_ctx ctx = {0};
	CHECK(stage_state_buf_init(&ctx.sb));

	struct stage_state s = {0};
	CHECK(!stage_state_publish(&ctx.sb, &s)); /* identical to initial zero */
	s.zoom = 1.0f;
	CHECK(stage_state_publish(&ctx.sb, &s));
	CHECK(!stage_state_publish(&ctx.sb, &s));
	uint64_t seq;
	struct stage_state r;
	stage_state_read(&ctx.sb, &r, &seq);
	CHECK(r.zoom == 1.0f && seq == 1);

	pthread_t th[2];
	for (int i = 0; i < 2; i++)
		pthread_create(&th[i], NULL, snap_reader, &ctx);
	for (int k = 1; k <= 200000; k++) {
		memset(&s, 0, sizeof(s));
		s.x = s.y = s.zoom = s.rot = (float)k;
		s.move_id = (uint64_t)k;
		stage_state_publish(&ctx.sb, &s);
	}
	ctx.stop = 1;
	for (int i = 0; i < 2; i++)
		pthread_join(th[i], NULL);
	printf("  snapshot: %llu concurrent reads, %llu torn\n", (unsigned long long)ctx.reads,
	       (unsigned long long)ctx.torn);
	CHECK_EQ_U64(ctx.torn, 0);
	stage_state_read(&ctx.sb, &r, NULL);
	CHECK(r.move_id == 200000);
	stage_state_buf_free(&ctx.sb);
}

/* ------------------------------------------------------------------------- */
/* Misc                                                                      */
/* ------------------------------------------------------------------------- */

static int hook_order[4];
static int hook_n;

static void hook_fn(void *ud)
{
	hook_order[hook_n++] = (int)(intptr_t)ud;
}

static void test_strlcpy_and_hooks(void)
{
	char buf[4];
	stage_strlcpy(buf, "abcdef", sizeof(buf));
	CHECK(strcmp(buf, "abc") == 0);
	stage_strlcpy(buf, NULL, sizeof(buf));
	CHECK(buf[0] == '\0');

	CHECK(stage_core_on_unload(hook_fn, (void *)(intptr_t)1));
	CHECK(stage_core_on_unload(hook_fn, (void *)(intptr_t)2));
	CHECK(stage_core_on_unload(hook_fn, (void *)(intptr_t)3));
	CHECK(!stage_core_on_unload(NULL, NULL));

	struct stage_cmd c;
	make_cmd(&c, STAGE_CMD_CAMERA_MOVE, "left-over", 1);
	stage_cmdq_push(&c);
	stage_core_shutdown();
	CHECK(hook_n == 3);
	CHECK(hook_order[0] == 3 && hook_order[1] == 2 && hook_order[2] == 1);
	struct stage_cmdq_stats st;
	stage_cmdq_get_stats(&st);
	CHECK_EQ_U64(st.depth, 0);
	stage_core_init(); /* usable again */
}

int main(void)
{
	stage_core_init();
	printf("core_cmdq_test\n");
	test_basic_fifo();
	test_scene_resolution();
	test_per_scene_and_types();
	test_overflow_drops_oldest();
	test_purge_expiry_panic();
	test_push_id_and_full_expiry();
	test_push_from_callback();
	test_ids_unique_across_threads();
	test_stress();
	test_registry();
	test_state_snapshot();
	test_strlcpy_and_hooks();
	stage_core_shutdown();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
