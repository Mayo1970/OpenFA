/*
 * fa_map.c - level (.W02 chunk 0) loader. See fa_map.h.
 */
#include "fa/fa_map.h"
#include "fa/fa_w02.h"

#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int fa_map_parse_info(const uint8_t *chunk, uint32_t chunk_size,
                      fa_map_info *out)
{
    if (!chunk || !out || chunk_size < FA_MAP_INFO_SIZE) return -1;
    memset(out, 0, sizeof *out);
    memcpy(out->raw, chunk, FA_MAP_INFO_SIZE);

    for (int i = 0; i < FA_MAP_BG_LAYERS; i++)
        out->bg_layer[i] = (int)rd16(chunk + 260 + i * 2);
    out->tile_w = (int)rd16(chunk + 270);
    out->tile_h = (int)rd16(chunk + 272);
    out->grid_w = (int)rd16(chunk + 274);
    out->grid_h = (int)rd16(chunk + 276);
    for (int i = 0; i < 9; i++)
        out->u278[i] = rd16(chunk + 278 + i * 2);
    out->u296 = rd16(chunk + 296);
    out->grid_bytes = rd32(chunk + 306);
    out->plane_count = (int)chunk[0x15b];

    if (out->tile_w <= 0 || out->tile_h <= 0 ||
        out->grid_w <= 0 || out->grid_h <= 0)
        return -1;

    uint64_t expect = (uint64_t)out->grid_w * (uint64_t)out->grid_h *
                      FA_MAP_CELL_STRIDE;
    if ((uint64_t)out->grid_bytes != expect) return -1;

    if (out->plane_count <= 0 || out->plane_count > FA_MAP_PLANES)
        out->plane_count = FA_MAP_PLANES;
    /* 0x4328ea: atlas column count = 640 / tile_w (before optional scaling) */
    out->atlas_cols = 640 / out->tile_w;
    if (out->atlas_cols <= 0) out->atlas_cols = 1;

    return 0;
}

int fa_map_load_w02(fa_map *m, const struct fa_w02 *pool)
{
    if (!m || !pool) return -1;
    memset(m, 0, sizeof *m);

    const uint8_t *chunk = NULL;
    uint32_t csize = 0;
    if (fa_w02_chunk(pool, 0, &chunk, &csize, NULL) != 0) return -1;

    if (fa_map_parse_info(chunk, csize, &m->info) != 0) return -1;

    uint32_t need = FA_MAP_INFO_SIZE + m->info.grid_bytes;
    if (csize < need) return -1;

    m->grid = (uint8_t *)malloc(m->info.grid_bytes);
    if (!m->grid) return -1;
    memcpy(m->grid, chunk + FA_MAP_INFO_SIZE, m->info.grid_bytes);

    m->tail_size = csize - need;
    if (m->tail_size) {
        m->tail = (uint8_t *)malloc(m->tail_size);
        if (!m->tail) { free(m->grid); m->grid = NULL; return -1; }
        memcpy(m->tail, chunk + need, m->tail_size);
    }

    m->world_w = m->info.grid_w * m->info.tile_w;
    m->world_h = m->info.grid_h * m->info.tile_h;
    return 0;
}

int fa_map_load_file(fa_map *m, const char *w02_path)
{
    fa_w02 pool;
    if (fa_w02_open_file(&pool, w02_path) != 0) return -1;
    int rc = fa_map_load_w02(m, &pool);
    fa_w02_close(&pool);
    return rc;
}

void fa_map_free(fa_map *m)
{
    if (!m) return;
    free(m->grid);
    free(m->tail);
    memset(m, 0, sizeof *m);
}

int fa_map_plane_count(const fa_map *m)
{
    if (!m) return 0;
    int p = m->info.plane_count;
    return p < 0 ? 0 : (p > FA_MAP_PLANES ? FA_MAP_PLANES : p);
}

/* Byte address of the (plane, cx, cy) entry, or NULL if out of range.
 * Plane-major: grid + plane*(3*w*h) + 3*(cy*w + cx). */
static const uint8_t *entry_ptr(const fa_map *m, int plane, int cx, int cy)
{
    if (!m || !m->grid) return NULL;
    if (plane < 0 || plane >= FA_MAP_PLANES) return NULL;
    if (cx < 0 || cy < 0 || cx >= m->info.grid_w || cy >= m->info.grid_h)
        return NULL;
    size_t plane_stride = (size_t)3 * m->info.grid_w * m->info.grid_h;
    size_t off = (size_t)plane * plane_stride +
                 (size_t)3 * ((size_t)cy * m->info.grid_w + cx);
    if (off + 3 > m->info.grid_bytes) return NULL;
    return m->grid + off;
}

fa_map_entry fa_map_cell_entry(const fa_map *m, int cx, int cy, int plane)
{
    fa_map_entry e = { 0, FA_MAP_ENTRY_EMPTY };
    const uint8_t *p = entry_ptr(m, plane, cx, cy);
    if (!p) return e;
    e.attr   = p[0];
    e.packed = rd16(p + 1);
    return e;
}

int fa_map_cell_occupied(const fa_map *m, int cx, int cy)
{
    if (!m) return 0;
    for (int pl = 0; pl < FA_MAP_PLANES; pl++) {
        const uint8_t *p = entry_ptr(m, pl, cx, cy);
        if (p && rd16(p + 1) != FA_MAP_ENTRY_EMPTY) return 1;
    }
    return 0;
}

int fa_map_ladder_at(const fa_map *m, int world_x, int world_y)
{
    if (!m || !m->grid || m->info.tile_w <= 0 || m->info.tile_h <= 0)
        return 0;
    if (world_x < 0 || world_y < 0 ||
        world_x >= m->world_w || world_y >= m->world_h)
        return 0;
    /* a ladder is a plane-2 grid entry with collision code bit 0 set */
    fa_map_entry e = fa_map_cell_entry(m, world_x / m->info.tile_w,
                                       world_y / m->info.tile_h, 2);
    if (fa_map_entry_empty(e)) return 0;
    return (fa_map_entry_code(e) & 1) != 0;
}

int fa_map_solid_class(const fa_map *m, int world_x, int world_y)
{
    if (!m || !m->grid || m->info.tile_w <= 0 || m->info.tile_h <= 0)
        return 1;
    if (world_x < 0 || world_y < 0 ||
        world_x >= m->world_w || world_y >= m->world_h)
        return 1;                                   /* level edge -> wall */

    fa_map_entry e = fa_map_cell_entry(m, world_x / m->info.tile_w,
                                       world_y / m->info.tile_h, 2);
    if (fa_map_entry_empty(e)) return 0;
    if (e.attr & FA_MAP_ATTR_SUPPRESS) return 0;
    if (fa_map_entry_code(e) & 2) return 2;         /* one-way platform */
    return (e.attr & FA_MAP_ATTR_SOLID) ? 1 : 0;
}

int fa_map_solid_at(const fa_map *m, int plane, int world_x, int world_y)
{
    if (!m || !m->grid || m->info.tile_w <= 0 || m->info.tile_h <= 0)
        return -1;
    if (world_x < 0 || world_y < 0 ||
        world_x >= m->world_w || world_y >= m->world_h)
        return 1;                                   /* out of map -> solid */

    const uint8_t *p = entry_ptr(m, plane, world_x / m->info.tile_w,
                                 world_y / m->info.tile_h);
    if (!p) return 1;
    if (rd16(p + 1) == FA_MAP_ENTRY_EMPTY) return -1;
    if (p[0] & FA_MAP_ATTR_SUPPRESS) return -1;
    return (p[0] & FA_MAP_ATTR_SOLID) ? 1 : 0;
}

uint32_t fa_map_hash(const fa_map *m)
{
    uint32_t h = 2166136261u;
    if (!m) return h;
    const uint8_t *p = m->info.raw;
    for (size_t i = 0; i < FA_MAP_INFO_SIZE; i++) { h ^= p[i]; h *= 16777619u; }
    for (size_t i = 0; i < m->info.grid_bytes; i++) { h ^= m->grid[i]; h *= 16777619u; }
    for (size_t i = 0; i < m->tail_size; i++) { h ^= m->tail[i]; h *= 16777619u; }
    return h;
}
