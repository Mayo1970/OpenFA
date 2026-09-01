/*
 * fa_res.h - resource lifetime and streaming policy (RRR-38)
 *
 * Parity / design basis: ENGINE-ARCH section 12, RRR-8 (boot load order) and
 * RRR-18 (the decoded .W01 working-set measurement).
 *
 * The original preloads ~119 `.W01` AOM animations at boot into DirectDraw
 * surfaces and never frees them (PL / RRR-8). RRR-18 measured the decoded
 * cost: 170.3 MiB for all 192 `.W01`, and ROBOTER.W01 alone is 17.3 MiB -
 * more than the 16 MiB floor. A straight boot-set port therefore cannot run
 * on a constrained target. ENGINE-ARCH section 12 leaves the choice to this
 * module; RRR-38 decides: STREAM PER LEVEL. The engine loads a level's
 * resources when the level starts and evicts them when it ends. Desktop, with
 * memory to spare, may set a large budget so nothing is evicted mid-run - the
 * same code path, a bigger number.
 *
 * This is an explicit-lifetime cache, no implicit garbage collection:
 *
 *   fa_res_acquire   get a resource by key. A cache hit bumps its ref count
 *                    and its use order. A miss calls the caller's loader,
 *                    inserts the result pinned (ref count 1), then evicts
 *                    unreferenced entries (least-recently-used first) to get
 *                    back under budget. A load is never failed for budget:
 *                    if the working set still exceeds budget after evicting
 *                    everything evictable, the entry is kept and an overflow
 *                    is logged.
 *   fa_res_release   drop one reference. At zero the entry becomes evictable
 *                    but is NOT freed yet.
 *   fa_res_evict     free one unreferenced entry now. Fails if it is pinned.
 *   fa_res_trim      evict LRU unreferenced entries until at or under budget.
 *
 * Use order is a monotonic sequence counter, never a wall clock, so eviction
 * is deterministic and replay-safe (ENGINE-ARCH goal 3).
 *
 * Per-level budgeting: fa_res_begin_level / fa_res_end_level bracket a level.
 * end_level logs that level's peak working set against its budget. The peak
 * is the true transient high-water mark: it is sampled after each insert,
 * before the follow-up eviction.
 */
#ifndef FA_RES_H
#define FA_RES_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The smallest working set any target is assumed to provide (ENGINE-ARCH
 * section 12). A per-level budget must be at least this; fa_res_cache_init
 * raises a smaller budget up to the floor and counts it. */
#define FA_RES_FLOOR_BYTES   (16u * 1024u * 1024u)

/* Frees a resource's data. `data` and `loader_ctx` are exactly what the
 * loader returned / was given. */
typedef void (*fa_res_free_fn)(void *data, void *loader_ctx);

/*
 * Loads one resource. On success return 0 and set *data (non-NULL), *bytes
 * (the working-set cost to charge against the budget) and *free_fn (may be
 * NULL if `data` needs no freeing). On failure return -1; fa_res_acquire
 * then fails too and inserts nothing.
 */
typedef int (*fa_res_load_fn)(const char *key, void *loader_ctx,
                              void **data, size_t *bytes,
                              fa_res_free_fn *free_fn);

typedef struct fa_res_entry {
    char          *key;
    void          *data;
    size_t         bytes;
    int            refs;
    uint64_t       last_use;     /* monotonic sequence, not time */
    fa_res_free_fn free_fn;
    void          *loader_ctx;
} fa_res_entry;

typedef struct fa_res_cache {
    fa_res_entry *e;
    size_t        n, cap;
    size_t        budget;
    uint64_t      seq;

    size_t        live_bytes;    /* sum of bytes over resident entries */
    size_t        peak_bytes;    /* high-water mark since init            */

    char          level[64];
    size_t        level_peak;    /* high-water mark since begin_level     */
    int           in_level;

    /* counters, for fa_res_log_stats */
    uint64_t      loads, hits, evictions, overflows;
    uint64_t      budget_raised; /* times init clamped a sub-floor budget */
} fa_res_cache;

/* Initialise an empty cache. `budget_bytes` 0 means FA_RES_FLOOR_BYTES; a
 * value below the floor is raised to it (and budget_raised incremented).
 * Returns 0, or -1 on a bad arg. */
int    fa_res_cache_init(fa_res_cache *c, size_t budget_bytes);

/* Free every resident resource (calling each free_fn) and zero the cache. If
 * `log` is non-NULL, any still-referenced entry is reported as a leak.
 * Returns the number of entries that were still referenced. */
int    fa_res_cache_shutdown(fa_res_cache *c, FILE *log);

/* Change the budget. Below the floor is raised to it. A lower budget does not
 * evict on its own - call fa_res_trim. */
void   fa_res_cache_set_budget(fa_res_cache *c, size_t budget_bytes);

/*
 * Acquire a resource. On a hit: ref++, use order updated, *out_data set,
 * return 0. On a miss: `load` is called; on its failure return -1 and insert
 * nothing; on success insert pinned, trim to budget, return 0. `out_data`
 * may be NULL. `load` may be NULL, in which case a miss just returns -1.
 */
int    fa_res_acquire(fa_res_cache *c, const char *key,
                      fa_res_load_fn load, void *loader_ctx, void **out_data);

/* Drop one reference. Returns 0, or -1 if the key is absent or already at
 * zero references. */
int    fa_res_release(fa_res_cache *c, const char *key);

/* Free one unreferenced entry now. Returns 0, or -1 if it is absent or
 * pinned. */
int    fa_res_evict(fa_res_cache *c, const char *key);

/* Evict LRU unreferenced entries until live_bytes <= budget (or none are
 * left). Returns the number of bytes freed. */
size_t fa_res_trim(fa_res_cache *c);

/* Evict every unreferenced entry now, regardless of budget. Use this at level
 * teardown to drop the outgoing level's resources before the next one loads.
 * Returns the number of bytes freed. */
size_t fa_res_evict_unused(fa_res_cache *c);

/* --- per-level bracketing --------------------------------------- */

/* Start a level. Records its name and resets the per-level peak to the
 * current live working set. Optionally sets a level budget (0 keeps the
 * current one). */
void   fa_res_begin_level(fa_res_cache *c, const char *name, size_t budget_bytes);

/* End the current level. If `log` is non-NULL, writes one line:
 *   res: level <name> peak <KiB> / budget <KiB>  (loads H hits E evict O over)
 * Does not evict - the caller decides whether to fa_res_trim or carry
 * resources into the next level. */
void   fa_res_end_level(fa_res_cache *c, FILE *log);

/* --- introspection -------------------------------------------- */

size_t fa_res_live_bytes(const fa_res_cache *c);
size_t fa_res_peak_bytes(const fa_res_cache *c);
size_t fa_res_count(const fa_res_cache *c);
int    fa_res_is_loaded(const fa_res_cache *c, const char *key);
int    fa_res_refs(const fa_res_cache *c, const char *key);
void   fa_res_log_stats(const fa_res_cache *c, FILE *log);

#ifdef __cplusplus
}
#endif

#endif /* FA_RES_H */
