/*
 * map_probe.c - dump a .W02 level's MapInfo header and grid stats (RRR-42).
 *
 *   map_probe <file.W02>            chunk 0 MapInfo + first cells
 *   map_probe <file.W02> --cells N  also print the first N non-empty cells
 */
#include "fa/fa_w02.h"
#include "fa/fa_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: map_probe <file.W02> [--cells N]\n"); return 2; }
    long ncells = 0;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--cells") && i + 1 < argc) ncells = strtol(argv[++i], NULL, 0);

    fa_w02 pool;
    if (fa_w02_open_file(&pool, argv[1]) != 0) {
        fprintf(stderr, "cannot open pool %s\n", argv[1]); return 1;
    }
    printf("pool: %d chunk(s), item_kind %u, sig %s\n",
           fa_w02_count(&pool), pool.item_kind, pool.sig_ok ? "ok" : "BAD");

    fa_map m;
    if (fa_map_load_w02(&m, &pool) != 0) {
        fprintf(stderr, "chunk 0 is not a MapInfo level\n");
        fa_w02_close(&pool); return 1;
    }

    const fa_map_info *in = &m.info;
    printf("grid       %d x %d tiles\n", in->grid_w, in->grid_h);
    printf("tile       %d x %d px\n", in->tile_w, in->tile_h);
    printf("world      %d x %d px\n", m.world_w, m.world_h);
    printf("grid bytes %u  (= w*h*%u, %s)\n", in->grid_bytes, FA_MAP_CELL_STRIDE,
           in->grid_bytes == (uint32_t)in->grid_w * in->grid_h * FA_MAP_CELL_STRIDE
           ? "ok" : "MISMATCH");
    printf("tail bytes %u  (object / entity data, RRR-50)\n", m.tail_size);
    printf("planes     %d   atlas cols %d\n", in->plane_count, in->atlas_cols);
    printf("bg layers  {%d,%d,%d,%d,%d}  ([0..3] tile atlases, [4] backdrop)\n",
           in->bg_layer[0], in->bg_layer[1], in->bg_layer[2],
           in->bg_layer[3], in->bg_layer[4]);
    printf("u278       {%u,%u,%u,%u,%u,%u,%u,%u,%u}  u296 %u  (UNKNOWN)\n",
           in->u278[0], in->u278[1], in->u278[2], in->u278[3], in->u278[4],
           in->u278[5], in->u278[6], in->u278[7], in->u278[8], in->u296);

    printf("MapInfo raw (348 bytes):\n");
    for (int i = 0; i < (int)FA_MAP_INFO_SIZE; i += 16) {
        printf("  %03x:", i);
        for (int j = 0; j < 16 && i + j < (int)FA_MAP_INFO_SIZE; j++)
            printf(" %02x", in->raw[i + j]);
        printf("\n");
    }

    if (ncells > 0) {
        printf("first %ld non-empty cells (plane: tile/code/atlas/flip):\n",
               ncells);
        long shown = 0;
        for (int cy = 0; cy < in->grid_h && shown < ncells; cy++)
            for (int cx = 0; cx < in->grid_w && shown < ncells; cx++) {
                if (!fa_map_cell_occupied(&m, cx, cy)) continue;
                printf("  (%3d,%3d):", cx, cy);
                for (int pl = 0; pl < FA_MAP_PLANES; pl++) {
                    fa_map_entry e = fa_map_cell_entry(&m, cx, cy, pl);
                    if (fa_map_entry_empty(e)) continue;
                    printf(" [p%d t%d c%d a%d%s%s%s]", pl,
                           fa_map_entry_tile(e), fa_map_entry_code(e),
                           fa_map_entry_atlas(e),
                           fa_map_entry_flip_x(e) ? " fx" : "",
                           fa_map_entry_flip_y(e) ? " fy" : "",
                           fa_map_entry_solid(e) ? " SOLID" : "");
                }
                printf("\n");
                shown++;
            }
        if (shown == 0) printf("  (grid is entirely empty)\n");
    }

    fa_map_free(&m);
    fa_w02_close(&pool);
    return 0;
}
