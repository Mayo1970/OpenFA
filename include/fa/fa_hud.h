/*
 * fa_hud.h - the in-level status display (RRR-51 AC5)
 *
 * The exe loads these sheets at level start (0x4083f0..0x4086d0) and redraws
 * them every frame in hud_draw (0x4089c0):
 *   Interface/Ingame/InterfaceItaly.w01  the framed HUD panel (1 frame, 0,0)
 *   Interface/Ingame/Energy.w01          the health bar, 5 frames 0..4
 *   Interface/Ingame/Schuss.w01          the snowball gauge, 10 frames 0..9
 *   Interface/Ingame/SchussD.w01         the same gauge, "dirty" snowballs
 *   Interface/Ingame/Item1..6.w01        the 6 recipe pieces, frame 0 dim /
 *                                        frame 1 lit
 *   Interface/Ingame/Actors.w01          the active-kid portrait, 2 frames
 *   Interface/Ingame/Schrift.w01         a 10-frame digit sheet (0..9) for
 *                                        the score
 * Every sprite blits at its own .W01 table-A origin, so the layout comes from
 * the art. There is NO lives counter in the exe HUD - health is the only
 * survival meter (0x45F014, max 100; 20 per hit; game over at 0, RRR-53).
 *
 * Score comes from 0x45ED2C (+100 per enemy / +100..+10000 per pickup, AC4).
 * Health from 0x45F014, mapped to an Energy frame by the exe thresholds
 * (>80 -> 4, >60 -> 3, >40 -> 2, >20 -> 1, >0 -> 0). Ammo from 0x45ED34,
 * Schuss frame = ammo - 1.
 */
#ifndef FA_HUD_H
#define FA_HUD_H

#ifdef __cplusplus
extern "C" {
#endif

struct fa_surface;

typedef struct fa_hud fa_hud;

/* Load the HUD sheets from `gdata_dir`/Animation/Interface/Ingame/. Returns a
 * heap object (free with fa_hud_free) or NULL when the Energy sheet is absent
 * (no GData). Missing optional sheets are simply not drawn. */
fa_hud *fa_hud_load(const char *gdata_dir);

void fa_hud_free(fa_hud *h);

/*
 * Draw one HUD frame over `dst` (the 800x600 framebuffer, after the scene).
 *   score      the running score (0x45ED2C)
 *   health     0..100 (0x45F014); <= 0 hides the Energy bar
 *   ammo       0..10 snowballs left (0x45ED34); <= 0 hides the gauge
 *   dirty      non-zero selects the SchussD art (collect_dirtyballs, ObjNr 60)
 *   items      6 flags: items[i] != 0 shows recipe piece i+1 as collected
 *   character  0 penguin / 1 Milchschnitte - selects the Actors frame
 */
void fa_hud_render(const fa_hud *h, const struct fa_surface *dst,
                   int score, int health, int ammo, int dirty,
                   const int items[6], int character);

#ifdef __cplusplus
}
#endif

#endif /* FA_HUD_H */
