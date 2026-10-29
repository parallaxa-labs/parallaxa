/*
 * Minimal pthread shim for the core unit tests (no libobs, no w32-pthreads).
 * Only what src/core uses plus thread create/join for the tests.
 * Windows: SRWLOCK + _beginthreadex. Elsewhere: forwards to the real
 * <pthread.h> via #include_next is not portable, so non-Windows builds use the
 * system header directly (see tests/core/CMakeLists.txt).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef STAGE_TEST_PTHREAD_SHIM_H
#define STAGE_TEST_PTHREAD_SHIM_H

#ifndef _WIN32
#error "the pthread shim is for Windows only"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdlib.h>

typedef SRWLOCK pthread_mutex_t;
typedef int pthread_mutexattr_t;
typedef HANDLE pthread_t;
typedef int pthread_attr_t;

#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
	(void)a;
	InitializeSRWLock(m);
	return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *m)
{
	(void)m;
	return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *m)
{
	AcquireSRWLockExclusive(m);
	return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *m)
{
	ReleaseSRWLockExclusive(m);
	return 0;
}

struct stage_shim_thread_start {
	void *(*fn)(void *);
	void *arg;
};

static unsigned __stdcall stage_shim_thread_main(void *p)
{
	struct stage_shim_thread_start s = *(struct stage_shim_thread_start *)p;
	free(p);
	s.fn(s.arg);
	return 0;
}

static inline int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
	(void)a;
	struct stage_shim_thread_start *s = malloc(sizeof(*s));
	if (!s)
		return -1;
	s->fn = fn;
	s->arg = arg;
	uintptr_t h = _beginthreadex(NULL, 0, stage_shim_thread_main, s, 0, NULL);
	if (!h) {
		free(s);
		return -1;
	}
	*t = (HANDLE)h;
	return 0;
}

static inline int pthread_join(pthread_t t, void **ret)
{
	if (ret)
		*ret = NULL;
	WaitForSingleObject(t, INFINITE);
	CloseHandle(t);
	return 0;
}

#endif /* STAGE_TEST_PTHREAD_SHIM_H */
