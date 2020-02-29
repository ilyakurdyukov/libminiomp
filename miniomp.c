/*
 * Minimal implementation of the OpenMP runtime library.
 * Copyright (C) 2020 Ilya Kurdyukov
 *
 * contains modified parts of libgomp:
 *
 * Copyright (C) 2005-2018 Free Software Foundation, Inc.
 * Contributed by Richard Henderson <rth@redhat.com>.
 */

/*
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * Under Section 7 of GPL version 3, you are granted additional
 * permissions described in the GCC Runtime Library Exception, version
 * 3.1, as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License and
 * a copy of the GCC Runtime Library Exception along with this program;
 * see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#ifdef _WIN32
#define EXPORT
#else
#define EXPORT __attribute__((visibility("hidden")))
#endif

#define LOG0(...) { }
#define LOG(...) { fprintf(stderr, __VA_ARGS__); fflush(stderr); }

#ifndef DYN_ARRAYS
#define DYN_ARRAYS 0
#endif

#ifndef MAX_THREADS
#define MAX_THREADS 16
#endif

// need for ULL loops with negative increment ending at zero
// and some other extreme cases
#ifndef OVERFLOW_CHECKS
#define OVERFLOW_CHECKS 1
#endif

typedef char bool;
#define false 0
#define true 1

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define THREAD_CALLBACK(name) DWORD WINAPI name(LPVOID param)
#define THREAD_RET return 0;

#define THREAD_FIELDS HANDLE handle;
#define THREAD_INIT(t) t.impl.handle = NULL;
#define THREAD_CREATE(t, fn) { DWORD tid; t.impl.handle = CreateThread(NULL, 0, fn, (void*)&t, 0, &tid); }
#define THREAD_JOIN(t) if (t.impl.handle) { WaitForSingleObject(t.impl.handle, INFINITE); CloseHandle(t.impl.handle); }
#else
#include <pthread.h>
#define THREAD_CALLBACK(name) void* name(void* param)
#define THREAD_RET return NULL;

#define THREAD_FIELDS int err; pthread_t pthread;
#define THREAD_INIT(t) t.impl.err = -1;
#define THREAD_CREATE(t, fn) t.impl.err = pthread_create(&t.impl.pthread, NULL, fn, (void*)&t);
#define THREAD_JOIN(t) if (!t.impl.err) pthread_join(t.impl.pthread, NULL);
#endif

typedef unsigned long long gomp_ull;

typedef struct {
	void (*fn)(void*);
	void *data;
	unsigned nthreads;
	volatile int wait; // waiting threads
	volatile char lock;
	char mode;
	union { volatile gomp_ull next_ull; volatile long next; };
	union { gomp_ull end_ull; long end; };
	union { gomp_ull chunk_ull; long chunk; };
} gomp_work_t;

typedef struct {
	int team_id;
	gomp_work_t *work_share;
	struct { THREAD_FIELDS } impl;
} gomp_thread_t;

static int nthreads_var = 1, num_procs = 1;

static gomp_work_t gomp_work_default = { NULL, NULL, 1, -1, 0, 0, { 0 }, { 0 }, { 0 } };
static gomp_thread_t gomp_thread_default = { 0, &gomp_work_default, { 0 } };

static inline void gomp_init_num_threads() {
#ifdef _WIN32
	DWORD_PTR procMask, sysMask;
	int count = 0;
	if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask)) {
		if (procMask) do count++; while (procMask &= procMask - 1);
	}
#else
	int count = sysconf(_SC_NPROCESSORS_ONLN);
#endif
	if (count < 1) count = 1;
	num_procs = nthreads_var = count;
}

#ifdef _WIN32
typedef CRITICAL_SECTION gomp_mutex_t;
static gomp_mutex_t default_lock;
static gomp_mutex_t barrier_lock;
#define MUTEX_INIT(lock) InitializeCriticalSection(lock);
#define MUTEX_FREE(lock) DeleteCriticalSection(lock);

static void gomp_mutex_lock(gomp_mutex_t *lock) {
	EnterCriticalSection(lock);
}

static void gomp_mutex_unlock(gomp_mutex_t *lock) {
	LeaveCriticalSection(lock);
}
#else
typedef pthread_mutex_t gomp_mutex_t;
static gomp_mutex_t default_lock = PTHREAD_MUTEX_INITIALIZER;
static gomp_mutex_t barrier_lock = PTHREAD_MUTEX_INITIALIZER;
// #define MUTEX_INIT(lock) pthread_mutex_init(lock, NULL)
// #define MUTEX_FREE(lock) pthread_mutex_destroy(lock)

static void gomp_mutex_lock(gomp_mutex_t *lock) {
	pthread_mutex_lock(lock);
}

static void gomp_mutex_unlock(gomp_mutex_t *lock) {
	pthread_mutex_unlock(lock);
}
#endif

#if 1 && defined(_WIN32)
static DWORD omp_tls = TLS_OUT_OF_INDEXES;
#define TLS_INIT omp_tls = TlsAlloc();
#define TLS_FREE if (omp_tls != TLS_OUT_OF_INDEXES) TlsFree(omp_tls);

static inline gomp_thread_t *miniomp_gettls() {
	void *data = NULL;
	if (omp_tls != TLS_OUT_OF_INDEXES) data = TlsGetValue(omp_tls);
	return data ? (gomp_thread_t*)data : &gomp_thread_default;
}

static inline void miniomp_settls(gomp_thread_t *data) {
	if (omp_tls != TLS_OUT_OF_INDEXES) TlsSetValue(omp_tls, data);
}

#define TLS_SET(x) miniomp_settls(x);
#define TLS_GET miniomp_gettls()
#else

static __thread gomp_thread_t *gomp_ptr = &gomp_thread_default;

#define TLS_SET(x) gomp_ptr = x;
#define TLS_GET gomp_ptr
#endif

__attribute__((constructor))
static void miniomp_init() {
	// LOG("miniomp_init()\n");
#ifdef TLS_INIT
	TLS_INIT
#endif
#ifdef MUTEX_INIT
	MUTEX_INIT(&default_lock)
	MUTEX_INIT(&barrier_lock)
#endif
	gomp_init_num_threads();
}

__attribute__((destructor))
static void miniomp_deinit() {
	// LOG("miniomp_deinit()\n");
#ifdef MUTEX_FREE
	MUTEX_FREE(&barrier_lock)
	MUTEX_FREE(&default_lock)
#endif
#ifdef TLS_FREE
	TLS_FREE
#endif
}

static inline gomp_thread_t* gomp_thread() { return TLS_GET; }
EXPORT int omp_get_thread_num() { return gomp_thread()->team_id; }
EXPORT int omp_get_num_procs() { return num_procs; }
EXPORT int omp_get_max_threads() { return nthreads_var; }
EXPORT int omp_get_num_threads() { return gomp_thread()->work_share->nthreads; }
EXPORT void omp_set_num_threads(int n) { nthreads_var = n > 0 ? n : 1; }

#define DYN_NEXT(name, type, next, chunk1, end1) \
static bool gomp_iter_##name(gomp_work_t *ws, type *pstart, type *pend) { \
	char mode = ws->mode; \
	type end = ws->end1, nend, chunk = ws->chunk1, start; \
	if (!OVERFLOW_CHECKS || mode & 1) { \
		start = __sync_fetch_and_add(&ws->next, chunk); \
		if (!(mode & 2)) { \
			if (start >= end) return false; \
			nend = start + chunk; \
			if (nend > end) nend = end; \
		} else { \
			if (start <= end) return false; \
			nend = start + chunk; \
			if (nend < end) nend = end; \
		} \
	} else { \
		start = ws->next; \
		for (;;) { \
			type left = end - start, tmp; \
			if (!left) return false; \
			if (mode & 2) { if (chunk < left) chunk = left; } \
			else if (chunk > left) chunk = left;  \
			nend = start + chunk; \
			tmp = __sync_val_compare_and_swap(&ws->next, start, nend); \
			if (tmp == start) break; \
			start = tmp; \
		} \
	} \
	*pstart = start; *pend = nend; return true; \
}
DYN_NEXT(dynamic_next, long, next, chunk, end)
DYN_NEXT(ull_dynamic_next, gomp_ull, next_ull, chunk_ull, end_ull)
#undef DYN_NEXT

static THREAD_CALLBACK(gomp_threadfunc) {
	gomp_thread_t *thr = (gomp_thread_t*)param;
	gomp_work_t *ws = thr->work_share;
	TLS_SET(thr)
	ws->fn(ws->data);
	THREAD_RET
}

#if DYN_ARRAYS
#define THREADS_DEFINE
#define THREADS_ALLOC gomp_thread_t threads[nthreads];
#else // static array allocation
#define THREADS_DEFINE gomp_thread_t threads[MAX_THREADS];
#define THREADS_ALLOC
#endif

#define TEAM_INIT(ws) \
	LOG0("parallel: fn = %p, data = %p, nthreads = %u, flags = %u\n", (void*)(intptr_t)fn, data, nthreads, flags) \
	THREADS_DEFINE \
	ws.fn = fn; ws.data = data; \
	if (nthreads <= 0) nthreads = nthreads_var; \
	if (nthreads > MAX_THREADS) nthreads = MAX_THREADS; \
	ws.nthreads = nthreads; ws.wait = -1; \
	THREADS_ALLOC \
	for (unsigned i = 0; i < nthreads; i++) { \
		THREAD_INIT(threads[i]) \
		threads[i].team_id = i; \
		threads[i].work_share = &ws; \
	} \
	TLS_SET(threads)

#define TEAM_START(type, invoke) \
	for (unsigned i = 1; i < nthreads; i++) THREAD_CREATE(threads[i], type##_threadfunc) \
	invoke; \
	for (unsigned i = 1; i < nthreads; i++) THREAD_JOIN(threads[i]) \
	TLS_SET(&gomp_thread_default)

#define LOOP_START(INIT) unsigned nthreads = ws->nthreads; \
	{ char l; do l = __sync_val_compare_and_swap(&ws->lock, 0, 1); \
	while (l == 1); if (!l) { LOOP_##INIT((*ws)) ws->lock = 2; } }

#define LOOP_INIT(ws) ws.mode = incr >= 0 ? 0 : 2; \
	LOG0("loop_start: start = %li, end = %li, incr = %li, chunk = %li\n", start, end, incr, chunk) \
	end = (incr >= 0 && start > end) || (incr < 0 && start < end) ? start : end; \
	ws.next = start; ws.end = end; ws.chunk = chunk *= incr; \
	if (OVERFLOW_CHECKS) { chunk = (~0UL >> 1) - chunk * (nthreads + 1); \
		ws.mode |= incr >= 0 ? end <= chunk : end >= chunk; }

#define LOOP_ULL_INIT(ws) ws.mode = up ? 0 : 2; \
	LOG0("loop_ull_start: up = %i, start = %llu, end = %llu, incr = %lli, chunk = %llu\n", up, start, end, incr, chunk) \
	end = (up && start > end) || (!up && start < end) ? start : end; \
	ws.next_ull = start; ws.end_ull = end; ws.chunk_ull = chunk *= incr; \
	if (OVERFLOW_CHECKS) { chunk = -1 - chunk * (nthreads + 1); \
		ws.mode |= up ? end <= chunk : end >= chunk; }

// up ? end <= fe00 : end >= 0200-1

#define TEAM_ARGS void (*fn)(void*), void *data, unsigned nthreads
#define LOOP_ARGS(t) t start, t end, t incr, t chunk

EXPORT void GOMP_parallel(TEAM_ARGS, unsigned flags) {
	(void)flags;
	gomp_work_t ws;
	TEAM_INIT(ws) ws.lock = 0;
	TEAM_START(gomp, fn(data))
}

EXPORT void GOMP_parallel_loop_dynamic(TEAM_ARGS, LOOP_ARGS(long), unsigned flags) {
	(void)flags;
	gomp_work_t ws;
	TEAM_INIT(ws) LOOP_INIT(ws) ws.lock = 2;
	TEAM_START(gomp, fn(data))
}

EXPORT bool GOMP_loop_dynamic_start(LOOP_ARGS(long), long *istart, long *iend) {
	gomp_thread_t *thr = gomp_thread();
	gomp_work_t *ws = thr->work_share;
	LOOP_START(INIT)
	return gomp_iter_dynamic_next(ws, istart, iend);
}

EXPORT bool GOMP_loop_ull_dynamic_start(bool up, LOOP_ARGS(gomp_ull), gomp_ull *istart, gomp_ull *iend) {
	gomp_thread_t *thr = gomp_thread();
	gomp_work_t *ws = thr->work_share;
	LOOP_START(ULL_INIT)
	return gomp_iter_ull_dynamic_next(ws, istart, iend);
}

EXPORT bool GOMP_loop_dynamic_next(long *istart, long *iend) {
	// LOG("loop_dynamic_next: istart = %p, iend = %p\n", istart, iend);
	return gomp_iter_dynamic_next(gomp_thread()->work_share, istart, iend);
}

EXPORT bool GOMP_loop_ull_dynamic_next(gomp_ull *istart, gomp_ull *iend) {
	// LOG("loop_ull_dynamic_next: istart = %p, iend = %p\n", istart, iend);
	return gomp_iter_ull_dynamic_next(gomp_thread()->work_share, istart, iend);
}

EXPORT void GOMP_loop_end_nowait() {
	// LOG("loop_end_nowait\n");
}

static void miniomp_barrier(int loop_end) {
	gomp_thread_t *thr = gomp_thread();
	gomp_work_t *ws = thr->work_share;
	int i, nthreads = ws->nthreads;
	if (nthreads == 1) {
		if (loop_end) ws->lock = 0;
		return;
	}
	// LOG("barrier: team_id = %i\n", thr->team_id);
	do i = __sync_val_compare_and_swap(&ws->wait, -1, 0); while (!i || i < -1);
	if (i == -1) gomp_mutex_lock(&barrier_lock);
	i = __sync_add_and_fetch(&ws->wait, 1);
	if (i < nthreads) { gomp_mutex_lock(&barrier_lock); ws->wait++; }
	else {
		if (loop_end) ws->lock = 0;
		ws->wait = -nthreads;
	}
	gomp_mutex_unlock(&barrier_lock);
}

EXPORT void GOMP_barrier() { miniomp_barrier(0); }
EXPORT void GOMP_loop_end() { miniomp_barrier(1); }

#define M1(fn, copy) extern __typeof(fn) copy __attribute__((alias(#fn)));
M1(GOMP_parallel_loop_dynamic, GOMP_parallel_loop_guided)
M1(GOMP_parallel_loop_dynamic, GOMP_parallel_loop_nonmonotonic_dynamic)
M1(GOMP_parallel_loop_dynamic, GOMP_parallel_loop_nonmonotonic_guided)
M1(GOMP_loop_dynamic_start, GOMP_loop_guided_start)
M1(GOMP_loop_dynamic_next, GOMP_loop_guided_next)
M1(GOMP_loop_dynamic_start, GOMP_loop_nonmonotonic_dynamic_start)
M1(GOMP_loop_dynamic_next, GOMP_loop_nonmonotonic_dynamic_next)
M1(GOMP_loop_dynamic_start, GOMP_loop_nonmonotonic_guided_start)
M1(GOMP_loop_dynamic_next, GOMP_loop_nonmonotonic_guided_next)
M1(GOMP_loop_ull_dynamic_start, GOMP_loop_ull_guided_start)
M1(GOMP_loop_ull_dynamic_next, GOMP_loop_ull_guided_next)
M1(GOMP_loop_ull_dynamic_start, GOMP_loop_ull_nonmonotonic_dynamic_start)
M1(GOMP_loop_ull_dynamic_next, GOMP_loop_ull_nonmonotonic_dynamic_next)
M1(GOMP_loop_ull_dynamic_start, GOMP_loop_ull_nonmonotonic_guided_start)
M1(GOMP_loop_ull_dynamic_next, GOMP_loop_ull_nonmonotonic_guided_next)
#undef M1

EXPORT void GOMP_critical_start() { gomp_mutex_lock(&default_lock); }
EXPORT void GOMP_critical_end() { gomp_mutex_unlock(&default_lock); }

#ifndef CLANG_KMP
#ifdef __clang__
#define CLANG_KMP 1
#else
#define CLANG_KMP 0
#endif
#endif

#if CLANG_KMP
typedef void (*kmpc_micro)(int32_t *gtid, int32_t *tid, ...);
typedef struct ident {
  int32_t reserved_1, flags, reserved_2, reserved_3;
  char const *psource;
} kmp_ident;
typedef int32_t kmp_critical_name[8];
enum sched_type {
  kmp_sch_static_chunked = 33,
  kmp_sch_static = 34,
  kmp_sch_dynamic_chunked = 35,
  kmp_sch_guided_chunked = 36
};

typedef struct {
	gomp_work_t ws;
	int32_t gtid, argc;
	union { gomp_ull incr_ull; long incr; };
} kmp_work_t;

static struct {
	int32_t nthreads;
	kmp_work_t *task;
} kmp_global[1] = { { 0 } };

static int32_t __kmp_entry_gtid() {
	return 0;
}

EXPORT int32_t __kmpc_global_thread_num(kmp_ident *loc) {
	(void)loc;
	return __kmp_entry_gtid();
}

EXPORT void __kmpc_push_num_threads(kmp_ident *loc, int32_t gtid, int32_t nthreads) {
	(void)loc;
	kmp_global[gtid].nthreads = nthreads;
}

static void kmp_invoke(gomp_thread_t *thr, kmp_work_t *task) {
	int tid = thr->team_id, argc = task->argc;
	void **argv = (void**)task->ws.data;

#define M1(i) , argv[i]
#define M2(i) M1(i) M1(i+1) M1(i+2) M1(i+3)
#define M0(i, tid) case i: (*(kmpc_micro)task->ws.fn)(&task->gtid, &tid); return;
	switch (argc) { 
		M0(0, tid)
		M0(1, tid M1(0))
		M0(2, tid M1(0) M1(1))
		M0(3, tid M1(0) M1(1) M1(2))
		M0(4, tid M2(0))
		M0(5, tid M2(0) M1(4))
		M0(6, tid M2(0) M1(4) M1(5))
		M0(7, tid M2(0) M1(4) M1(5) M1(6))
		M0(8, tid M2(0) M2(4))
		M0(9, tid M2(0) M2(4) M1(8))
		M0(10, tid M2(0) M2(4) M1(8) M1(9))
		M0(11, tid M2(0) M2(4) M1(8) M1(9) M1(10))
		M0(12, tid M2(0) M2(4) M2(8))
		M0(13, tid M2(0) M2(4) M2(8) M1(12))
		M0(14, tid M2(0) M2(4) M2(8) M1(12) M1(13))
		M0(15, tid M2(0) M2(4) M2(8) M1(12) M1(13) M1(14))
 }
#undef M0
#undef M1
#undef M2
}

static THREAD_CALLBACK(kmp_threadfunc) {
	gomp_thread_t *thr = (gomp_thread_t*)param;
	TLS_SET(thr)
	kmp_invoke(thr, (kmp_work_t*)thr->work_share);
	THREAD_RET
}

#include "stdarg.h"

EXPORT void __kmpc_fork_call(kmp_ident *loc, int32_t argc, kmpc_micro microtask, ...) {
	int i, gtid = __kmp_entry_gtid();
	unsigned nthreads = kmp_global[gtid].nthreads;
	kmp_work_t task; void *argv[15];
	(void)loc;
	// LOG("fork_call: argc = %i, fn = %p\n", argc, (void*)(intptr_t)microtask);
	if (argc > 15) return;
	kmp_global[gtid].nthreads = 0;
	kmp_global[gtid].task = &task;
	task.gtid = gtid;
	task.argc = argc;
	{
		va_list ap;
		va_start(ap, microtask);
		for (i = 0; i < argc; i++) argv[i] = va_arg(ap, void*);
		va_end(ap);
	}

	{
		void (*fn)(void*) = (void (*)(void*))microtask;
		void *data = argv;
		TEAM_INIT(task.ws) task.ws.lock = 0;
		TEAM_START(kmp, kmp_invoke(threads, &task))
	}
}

EXPORT void __kmpc_barrier(kmp_ident *loc, int32_t gtid) {
	(void)loc; (void)gtid;
	miniomp_barrier(1);
}

EXPORT void __kmpc_critical(kmp_ident *loc, int32_t gtid, kmp_critical_name *crit) {
	(void)loc; (void)gtid; (void)crit;
	gomp_mutex_lock(&default_lock);
}

EXPORT void __kmpc_end_critical(kmp_ident *loc, int32_t gtid, kmp_critical_name *crit) {
	(void)loc; (void)gtid; (void)crit;
	gomp_mutex_unlock(&default_lock);
}

#define LOOP_INIT_KMP(ws) LOOP_INIT(ws) task->incr = incr;
#define LOOP_ULL_INIT_KMP(ws) LOOP_ULL_INIT(ws) task->incr_ull = incr;

#define KMP_GETTASK_THR (void)gtid; gomp_thread_t *thr = gomp_thread(); \
	gomp_work_t *ws = thr->work_share; kmp_work_t *task = (kmp_work_t*)ws;

#if 1
#define KMP_GETTASK \
	kmp_work_t *task = kmp_global[gtid].task; \
	gomp_work_t *ws = &task->ws;
#else
#define KMP_GETTASK KMP_GETTASK_THR
#endif

#define KMP_INIT_FUNC(name, ST, T) \
EXPORT void __kmpc_dispatch_init_##name(kmp_ident *loc, int32_t gtid, \
	int32_t sched, T lb, T ub, ST st, ST chunk1)

KMP_INIT_FUNC(4, int32_t, int32_t) {
	long start = lb, end = ub + st, incr = st, chunk = chunk1;
	KMP_GETTASK
	(void)loc; (void)sched;
	LOOP_START(INIT_KMP)
}

KMP_INIT_FUNC(4u, int32_t, uint32_t) {
	long s = -1UL << 31;
	long start = lb + s, end = ub + st + s, incr = st, chunk = chunk1;
	KMP_GETTASK
	(void)loc; (void)sched;
	LOOP_START(INIT_KMP)
}

KMP_INIT_FUNC(8, int64_t, int64_t) {
	bool up = st >= 0;
	gomp_ull s = 1ULL << 63;
	gomp_ull start = lb + s, end = ub + st + s, incr = st, chunk = chunk1;
	KMP_GETTASK
	(void)loc; (void)sched;
	LOOP_START(ULL_INIT_KMP)
}

KMP_INIT_FUNC(8u, int64_t, uint64_t) {
	bool up = st >= 0;
	gomp_ull start = lb, end = ub + st, incr = st, chunk = chunk1;
	KMP_GETTASK
	(void)loc; (void)sched;
	LOOP_START(ULL_INIT_KMP)
}

#define KMP_NEXT_FUNC(name, ST, T) \
EXPORT int __kmpc_dispatch_next_##name(kmp_ident *loc, int32_t gtid, \
		int32_t *p_last, T *p_lb, T *p_ub, ST *p_st)

KMP_NEXT_FUNC(4, int32_t, int32_t) {
	long start, end, incr;
	KMP_GETTASK
	(void)loc;
	if (!gomp_iter_dynamic_next(ws, &start, &end)) return 0;
	incr = task->incr;
	if (p_st) *p_st = incr;
	*p_lb = start;
	*p_ub = end - incr;
	if (p_last) *p_last = end == ws->end;
	return 1;
}

KMP_NEXT_FUNC(4u, int32_t, uint32_t) {
	long start, end, incr, s = -1UL << 31;
	KMP_GETTASK
	(void)loc;
	if (!gomp_iter_dynamic_next(ws, &start, &end)) return 0;
	incr = task->incr;
	if (p_st) *p_st = incr;
	*p_lb = start - s;
	*p_ub = end - (incr + s);
	if (p_last) *p_last = end == ws->end;
	return 1;
}

KMP_NEXT_FUNC(8, int64_t, int64_t) {
	gomp_ull start, end, incr, s = 1ULL << 63;
	KMP_GETTASK
	(void)loc;
	if (!gomp_iter_ull_dynamic_next(ws, &start, &end)) return 0;
	incr = task->incr_ull;
	if (p_st) *p_st = incr;
	*p_lb = start - s;
	*p_ub = end - (incr + s);
	if (p_last) *p_last = end == ws->end_ull;
	return 1;
}

KMP_NEXT_FUNC(8u, int64_t, uint64_t) {
	gomp_ull start, end, incr;
	KMP_GETTASK
	(void)loc;
	if (!gomp_iter_ull_dynamic_next(ws, &start, &end)) return 0;
	incr = task->incr_ull;
	if (p_st) *p_st = incr;
	*p_lb = start;
	*p_ub = end - incr;
	if (p_last) *p_last = end == ws->end_ull;
	return 1;
}

#define KMP_STATIC_FUNC(name, ST, T) \
EXPORT void __kmpc_for_static_init_##name(kmp_ident *loc, \
		int32_t gtid, int32_t sched, int32_t *p_last, \
		T *p_lb, T *p_ub, ST *p_st, ST incr, ST chunk)

#define M1(name, ST, T, UT) KMP_STATIC_FUNC(name, ST, T) { \
	KMP_GETTASK_THR \
	unsigned tid = thr->team_id, nth = ws->nthreads; \
	T lb = *p_lb, ub = *p_ub; ST ci; UT count, div, mod; \
	if (!tid) LOG0("static_init: sched = %i, chunk = %i\n", sched, (int)chunk); \
	(void)loc; (void)sched; (void)task; \
	if (incr > 0 ? lb > ub : lb < ub) { if (p_last) *p_last = 0; return; } \
	if (chunk < 1) chunk = 1; ci = chunk * incr; \
	count = (incr > 0 ? (UT)(ub - lb) / ci : (UT)(lb - ub) / -ci) + 1; \
	*p_st = count * chunk; \
	if (count <= nth) { \
		if (tid < count) { *p_lb = lb += tid * ci; lb += ci - incr; \
			*p_ub = incr > 0 ? lb <= ub ? lb : ub : lb >= ub ? lb : ub; \
		} else *p_lb = ub + incr; \
		if (p_last) *p_last = tid == count - 1; \
		return; \
	} \
	div = count / nth; mod = count % nth; \
	*p_lb = lb += ci * (tid * div + (tid < mod ? tid : mod)); \
	lb += div * ci - (tid < mod ? 0 : ci) + ci - incr; \
	*p_ub = incr > 0 ? lb <= ub ? lb : ub : lb >= ub ? lb : ub; \
	if (p_last) *p_last = tid == nth - 1; \
}

M1(4, int32_t, int32_t, uint32_t)
M1(4u, int32_t, uint32_t, uint32_t)
M1(8, int64_t, int64_t, uint64_t)
M1(8u, int64_t, uint64_t, uint64_t)
#undef M1

EXPORT void __kmpc_for_static_fini(kmp_ident *loc, int32_t gtid) {
	(void)loc; (void)gtid;
}
#endif
