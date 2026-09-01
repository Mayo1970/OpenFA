/*
 * sim_replay.c - headless deterministic driver for the RRR-34 loop.
 *
 * A replay file is plain text, one frame per line:
 *     <frame_dt_ns> <input_mask>
 * Lines starting with '#' are ignored.
 *
 * Usage:
 *   sim_replay run <file>              run a replay, print tick count + hash
 *   sim_replay record <seed> <frames>  write a replay to stdout
 *
 * Two `run` passes over the same file must print the same hash, on any
 * platform. This is the tool RRR-102 will use to check the port against the
 * oracle once the real simulation is in.
 */
#include "fa/fa_loop.h"
#include "../tests/refsim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

typedef struct { refsim s; } driver;

static void d_sim(uint64_t tick, const void *input, void *user)
{
    driver *d = (driver *)user;
    refsim_step(&d->s, *(const uint32_t *)input, tick);
}

static int cmd_run(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }

    driver d; refsim_init(&d.s);
    fa_loop lp; fa_loop_init(&lp, d_sim, NULL, &d);

    char line[256];
    uint64_t frames = 0, clamped = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned long long dt; unsigned mask;
        if (sscanf(line, "%llu %u", &dt, &mask) != 2) continue;
        fa_loop_frame(&lp, (uint64_t)dt, &mask);
        frames++;
        clamped += (unsigned)lp.last_clamped;
    }
    fclose(f);

    printf("frames=%" PRIu64 " ticks=%" PRIu64 " clamped=%" PRIu64
           " hash=0x%08x\n",
           frames, lp.sim_tick, clamped, d.s.hash);
    return 0;
}

static int cmd_record(uint32_t seed, long frames)
{
    printf("# fresh-adventures replay  seed=0x%08x frames=%ld\n", seed, frames);
    uint32_t s = seed ? seed : 1u;
    for (long i = 0; i < frames; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        uint64_t dt = 4000000ull + (s % 32000001ull);   /* 4..36 ms */
        uint32_t mask = (s >> 8) & 0x7u;
        printf("%" PRIu64 " %u\n", dt, mask);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "run") == 0)
        return cmd_run(argv[2]);
    if (argc >= 4 && strcmp(argv[1], "record") == 0)
        return cmd_record((uint32_t)strtoul(argv[2], NULL, 0), strtol(argv[3], NULL, 0));

    fprintf(stderr,
        "usage:\n"
        "  %s run <file>\n"
        "  %s record <seed> <frames>\n", argv[0], argv[0]);
    return 2;
}
