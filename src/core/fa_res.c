/*
 * fa_res.c - resource lifetime and streaming policy (RRR-38)
 * See include/fa/fa_res.h for the model and the RRR-8 / RRR-18 basis.
 */
#include "fa/fa_res.h"

#include <stdlib.h>
#include <string.h>

#define INIT_CAP 16

static fa_res_entry *find(const fa_res_cache *c, const char *key)
{
    for (size_t i = 0; i < c->n; i++)
        if (strcmp(c->e[i].key, key) == 0)
            return &c->e[i];
    return NULL;
}

static void free_entry(fa_res_entry *en)
{
    if (en->free_fn)
        en->free_fn(en->data, en->loader_ctx);
    free(en->key);
    en->key = NULL;
    en->data = NULL;
    en->free_fn = NULL;
}

/* Remove entry at index `idx`, adjusting live_bytes. Does not free the data
 * (caller has already, or is moving it). */
static void remove_at(fa_res_cache *c, size_t idx)
{
    c->live_bytes -= c->e[idx].bytes;
    for (size_t i = idx + 1; i < c->n; i++)
        c->e[i - 1] = c->e[i];
    c->n--;
}

static void bump_peak(fa_res_cache *c)
{
    if (c->live_bytes > c->peak_bytes)
        c->peak_bytes = c->live_bytes;
    if (c->in_level && c->live_bytes > c->level_peak)
        c->level_peak = c->live_bytes;
}

int fa_res_cache_init(fa_res_cache *c, size_t budget_bytes)
{
    if (!c)
        return -1;
    memset(c, 0, sizeof(*c));
    c->e = (fa_res_entry *)calloc(INIT_CAP, sizeof(fa_res_entry));
    if (!c->e)
        return -1;
    c->cap = INIT_CAP;
    if (budget_bytes == 0)
        budget_bytes = FA_RES_FLOOR_BYTES;
    if (budget_bytes < FA_RES_FLOOR_BYTES) {
        budget_bytes = FA_RES_FLOOR_BYTES;
        c->budget_raised++;
    }
    c->budget = budget_bytes;
    return 0;
}

int fa_res_cache_shutdown(fa_res_cache *c, FILE *log)
{
    if (!c || !c->e)
        return 0;
    int leaked = 0;
    for (size_t i = 0; i < c->n; i++) {
        if (c->e[i].refs > 0) {
            leaked++;
            if (log)
                fprintf(log, "res: LEAK on shutdown: %s still has %d ref(s)\n",
                        c->e[i].key, c->e[i].refs);
        }
        free_entry(&c->e[i]);
    }
    free(c->e);
    memset(c, 0, sizeof(*c));
    return leaked;
}

void fa_res_cache_set_budget(fa_res_cache *c, size_t budget_bytes)
{
    if (!c)
        return;
    if (budget_bytes == 0)
        budget_bytes = FA_RES_FLOOR_BYTES;
    if (budget_bytes < FA_RES_FLOOR_BYTES) {
        budget_bytes = FA_RES_FLOOR_BYTES;
        c->budget_raised++;
    }
    c->budget = budget_bytes;
}

size_t fa_res_trim(fa_res_cache *c)
{
    if (!c)
        return 0;
    size_t freed = 0;
    while (c->live_bytes > c->budget) {
        /* least-recently-used unreferenced entry */
        size_t best = c->n;
        uint64_t best_use = UINT64_MAX;
        for (size_t i = 0; i < c->n; i++) {
            if (c->e[i].refs == 0 && c->e[i].last_use < best_use) {
                best_use = c->e[i].last_use;
                best = i;
            }
        }
        if (best == c->n)
            break;                 /* nothing evictable */
        freed += c->e[best].bytes;
        free_entry(&c->e[best]);
        remove_at(c, best);
        c->evictions++;
    }
    return freed;
}

size_t fa_res_evict_unused(fa_res_cache *c)
{
    if (!c)
        return 0;
    size_t freed = 0;
    for (size_t i = 0; i < c->n; ) {
        if (c->e[i].refs == 0) {
            freed += c->e[i].bytes;
            free_entry(&c->e[i]);
            remove_at(c, i);
            c->evictions++;
        } else {
            i++;
        }
    }
    return freed;
}

int fa_res_acquire(fa_res_cache *c, const char *key,
                   fa_res_load_fn load, void *loader_ctx, void **out_data)
{
    if (!c || !key)
        return -1;

    fa_res_entry *en = find(c, key);
    if (en) {
        en->refs++;
        en->last_use = ++c->seq;
        c->hits++;
        if (out_data)
            *out_data = en->data;
        return 0;
    }

    if (!load)
        return -1;

    void *data = NULL;
    size_t bytes = 0;
    fa_res_free_fn free_fn = NULL;
    if (load(key, loader_ctx, &data, &bytes, &free_fn) != 0 || data == NULL)
        return -1;

    if (c->n == c->cap) {
        size_t nc = c->cap * 2;
        fa_res_entry *ne = (fa_res_entry *)realloc(c->e, nc * sizeof(*ne));
        if (!ne) {
            if (free_fn)
                free_fn(data, loader_ctx);
            return -1;
        }
        c->e = ne;
        c->cap = nc;
    }

    char *kd = (char *)malloc(strlen(key) + 1);
    if (!kd) {
        if (free_fn)
            free_fn(data, loader_ctx);
        return -1;
    }
    strcpy(kd, key);

    fa_res_entry *slot = &c->e[c->n++];
    slot->key = kd;
    slot->data = data;
    slot->bytes = bytes;
    slot->refs = 1;
    slot->last_use = ++c->seq;
    slot->free_fn = free_fn;
    slot->loader_ctx = loader_ctx;

    c->live_bytes += bytes;
    c->loads++;
    bump_peak(c);                  /* sample the transient high-water mark */

    fa_res_trim(c);

    if (c->live_bytes > c->budget)
        c->overflows++;

    if (out_data)
        *out_data = data;
    return 0;
}

int fa_res_release(fa_res_cache *c, const char *key)
{
    if (!c || !key)
        return -1;
    fa_res_entry *en = find(c, key);
    if (!en || en->refs <= 0)
        return -1;
    en->refs--;
    return 0;
}

int fa_res_evict(fa_res_cache *c, const char *key)
{
    if (!c || !key)
        return -1;
    for (size_t i = 0; i < c->n; i++) {
        if (strcmp(c->e[i].key, key) == 0) {
            if (c->e[i].refs > 0)
                return -1;
            free_entry(&c->e[i]);
            remove_at(c, i);
            c->evictions++;
            return 0;
        }
    }
    return -1;
}

void fa_res_begin_level(fa_res_cache *c, const char *name, size_t budget_bytes)
{
    if (!c)
        return;
    snprintf(c->level, sizeof(c->level), "%s", name ? name : "?");
    c->in_level = 1;
    c->level_peak = c->live_bytes;
    if (budget_bytes != 0)
        fa_res_cache_set_budget(c, budget_bytes);
}

void fa_res_end_level(fa_res_cache *c, FILE *log)
{
    if (!c)
        return;
    if (log) {
        fprintf(log,
                "res: level %s peak %zu KiB / budget %zu KiB "
                "(loads %llu hits %llu evict %llu over %llu)\n",
                c->level,
                (size_t)((c->level_peak + 1023) / 1024),
                (size_t)((c->budget + 1023) / 1024),
                (unsigned long long)c->loads,
                (unsigned long long)c->hits,
                (unsigned long long)c->evictions,
                (unsigned long long)c->overflows);
    }
    c->in_level = 0;
}

size_t fa_res_live_bytes(const fa_res_cache *c) { return c ? c->live_bytes : 0; }
size_t fa_res_peak_bytes(const fa_res_cache *c) { return c ? c->peak_bytes : 0; }
size_t fa_res_count(const fa_res_cache *c)      { return c ? c->n : 0; }

int fa_res_is_loaded(const fa_res_cache *c, const char *key)
{
    return (c && key && find(c, key)) ? 1 : 0;
}

int fa_res_refs(const fa_res_cache *c, const char *key)
{
    if (!c || !key)
        return -1;
    fa_res_entry *en = find(c, key);
    return en ? en->refs : -1;
}

void fa_res_log_stats(const fa_res_cache *c, FILE *log)
{
    if (!c || !log)
        return;
    fprintf(log,
            "res: %zu resident, live %zu KiB, peak %zu KiB, budget %zu KiB, "
            "loads %llu hits %llu evict %llu over %llu raised %llu\n",
            c->n,
            (size_t)((c->live_bytes + 1023) / 1024),
            (size_t)((c->peak_bytes + 1023) / 1024),
            (size_t)((c->budget + 1023) / 1024),
            (unsigned long long)c->loads,
            (unsigned long long)c->hits,
            (unsigned long long)c->evictions,
            (unsigned long long)c->overflows,
            (unsigned long long)c->budget_raised);
}
