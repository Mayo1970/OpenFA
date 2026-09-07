#!/usr/bin/env python3
"""
rayimport.py - convert ONE Rayman 1 (PC) jungle level to an OpenFA .W02 map.

    python3 tools/rayimport.py RAYMAN/PCMAP/JUNGLE/RAY1.LEV out.W02
    python3 tools/rayimport.py RAY1.LEV out.W02 --gdata GData --log out.log.txt

The conversion is geometry-first and lossy (see GData/RAYMAN_IMPORT_AND_EDITOR.md):
  - Rayman block types  -> solid / one-way / climb / hazard / empty (plane 2).
    Slopes flatten to full solid squares (OpenFA has no slope collision).
  - Tiles are auto-picked from the GIUNGLA atlas by fa_w02lib.autotile, so the
    result looks consistent with a hand-built level from fa_edit.py.
  - Rayman object types map to OpenFA ObjNr behaviours by their real in-game
    role (verified against the Rayman 1 PS1 decomp and the Ray1Editor
    Events.csv): hostiles -> snake/parrot, Tings -> ammo, cages/medallions ->
    recipe ingredients, Power Ups -> energy, RAY_POS -> player start.
    Decoration, effects, NPCs, signs and platforms are dropped; unknown types
    are logged.

Load the result in the engine with:  fa_slice --custom1 out.W02
Tweak it afterwards in:               python3 tools/fa_edit.py out.W02

Rayman .LEV layout (retail PC, verified byte for byte against the real files):
  u32 ObjectsPointer | u32 TileSetNormalPointer
  u16 Width | u16 Height | 3*256*RGB666 palettes | u8 LastPlan1Palette
  Block[Width*Height], row-major, 6 bytes each:
    u16 tileIndex | u8 blockType | u8 ?a | u8 renderMode | u8 ?b
  ... background + tileset blocks ...
  @ObjectsPointer: u16 count | u16 linkTable[count] | ObjData[count] (132 B) ...
  ObjData: XPosition i32 @0x28, YPosition i32 @0x2C, Type u16 @0x60
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fa_w02lib as W
from fa_w02lib import CL_EMPTY, CL_SOLID, CL_ONEWAY, CL_CLIMB, CL_HAZARD

# --- Rayman .LEV --------------------------------------------------------
RAY_CELL = 16          # Rayman map cell size, px
FA_CELL = 32           # OpenFA tile size, px
LEV_BLOCK = 6
OBJ_REC = 132
OBJ_X, OBJ_Y, OBJ_T = 0x28, 0x2C, 0x60

# Rayman BlockType (PS1 decomp include/common/obj.h) -> our terrain class.
BLOCK_CLASS = {
    0: CL_EMPTY, 1: CL_EMPTY,                       # None, ChangeDirection
    2: CL_SOLID, 3: CL_SOLID, 4: CL_SOLID, 5: CL_SOLID, 6: CL_SOLID, 7: CL_SOLID,
    8: CL_HAZARD,                                   # Damage
    9: CL_SOLID,                                    # Bounce
    10: CL_HAZARD, 13: CL_HAZARD,                   # Water: keep it deadly
    11: CL_EMPTY,                                   # Exit (trigger, no geometry)
    12: CL_CLIMB,                                   # Climb
    14: CL_ONEWAY,                                  # Passthrough
    15: CL_SOLID,                                   # Solid
    16: CL_EMPTY,                                   # Seed
    18: CL_SOLID, 19: CL_SOLID, 20: CL_SOLID, 21: CL_SOLID, 22: CL_SOLID, 23: CL_SOLID,
    24: CL_HAZARD,                                  # Spikes
    25: CL_EMPTY,                                   # Cliff: non-solid marker
    30: CL_SOLID,                                   # Slippery
}
# Slopes (2-7, 18-23) collapse to full 32x32 solid squares - OpenFA has no
# slope collision. Documented mismatch, not a parser bug.

# Rayman ObjType -> intent. Verified against the PS1 decomp + Ray1Editor Events.csv.
RAY_START_TYPES = (99, 206, 21, 23)    # RAY_POS, PIEDS_RAYMAN, PHOTOGRAPHE, RAYMAN
GROUND_ENEMY_TYPES = {
    0, 9, 165,            # Livingstones
    12, 14,               # Hunter / Chasseur
    20,                   # Tentacle Flower
    41, 45, 107,          # Prickly (Ouye)
    65, 100, 40, 122,     # Spider / Mite / Stone dog
    123,                  # Antitoon
    150,                  # Scorpion
    175, 229, 230, 235,   # Punaise
    35, 43, 56, 172, 187, # Stone man / woman
    84, 89, 212,          # Tibetain / Joe / Dark
}
FLY_ENEMY_TYPES = {50, 227, 6, 16}     # Bzzit / Moskito bosses, falling ouye
TING_TYPE = 161                        # blue Ting collectible trail
HEALTH_PICKUP_TYPES = {2, 82, 142, 31} # Power Up, Power Up Big, Oneup
OBJECTIVE_TYPES = {58, 59, 197, 54}    # Cage, Medallion -> recipe ingredient
DECOR_TYPES = {
    3, 4, 5, 11, 19, 44, 83, 143, 208, 13, 151, 94, 95, 21, 30, 42, 124,
    53, 130, 135, 141, 136, 137, 146, 153, 171, 202, 236, 238, 164,
}
PLATFORM_TYPES = {
    1, 7, 8, 16, 17, 22, 25, 26, 27, 28, 29, 34, 49, 63, 67, 68,
    101, 105, 106, 134, 140, 167, 188, 189, 254,
}

OBJ_START = (1000, 4)
OBJ_GROUND_ENEMY = (4, 3)     # snake
OBJ_FLY_ENEMY = (3, 3)        # parrot
OBJ_ENERGY = (51, 1)
OBJ_AMMO = (52, 1)
OBJ_RECIPE = [(53 + i, 1) for i in range(6)]
SPAWN_CLEAR_PX = 56


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def i32(b, o):
    return struct.unpack_from("<i", b, o)[0]


class RayLevel:
    def __init__(self, data):
        self.d = data
        self.unknown_blocks = {}
        self.obj_ptr = u32(data, 0)
        self.w = u16(data, 8)
        self.h = u16(data, 10)
        if not (0 < self.w < 4096 and 0 < self.h < 4096):
            raise ValueError("bad map size %dx%d" % (self.w, self.h))
        self.blocks_off = 12 + 3 * 256 * 3 + 1        # after LastPlan1Palette
        if self.blocks_off + self.w * self.h * LEV_BLOCK > len(data):
            raise ValueError("block array runs past EOF")

    def cell_class(self, cx, cy):
        bt = self.d[self.blocks_off + (cy * self.w + cx) * LEV_BLOCK + 2]
        if bt not in BLOCK_CLASS:
            self.unknown_blocks[bt] = self.unknown_blocks.get(bt, 0) + 1
        return BLOCK_CLASS.get(bt, CL_SOLID)

    def ground_row(self, cx, from_cy=0):
        if not 0 <= cx < self.w:
            return None
        for cy in range(max(0, from_cy), self.h):
            if self.cell_class(cx, cy) in (CL_SOLID, CL_ONEWAY):
                return cy
        return None

    def objects(self):
        d = self.d
        cnt = u16(d, self.obj_ptr)
        base = self.obj_ptr + 2 + cnt * 2
        out = []
        for i in range(cnt):
            o = base + i * OBJ_REC
            if o + OBJ_REC > len(d):
                break
            out.append((i32(d, o + OBJ_X), i32(d, o + OBJ_Y), u16(d, o + OBJ_T)))
        return out


def ent(obj_nr, dg, x, y):
    return {"obj_nr": obj_nr, "x": int(x), "y": int(y), "dg": dg, "raw": None}


def map_objects(objs, lev, gw, gh, log):
    """Return (entities, start_xy) with coordinates in FA pixels."""
    scale = FA_CELL / RAY_CELL
    world_w, world_h = gw * FA_CELL, gh * FA_CELL

    def onmap(x, y):
        return 0 <= x < world_w and 0 <= y < world_h

    cand = [(int(x * scale), int(y * scale), RAY_START_TYPES.index(t))
            for x, y, t in objs
            if t in RAY_START_TYPES and onmap(x * scale, y * scale)]
    start = None
    if cand:
        cand.sort(key=lambda c: (c[2], c[0]))
        start = (max(cand[0][0], FA_CELL), cand[0][1])
    if start:
        src_cy = int(cand[0][1] / FA_CELL)
        gr = lev.ground_row(int(start[0] / FA_CELL), src_cy)
        if gr is None:
            gr = lev.ground_row(int(start[0] / FA_CELL))
        if gr is not None:
            start = (start[0], gr * FA_CELL - 4)

    objectives, tings, health = [], [], []
    ents, dropped, near_spawn = [], {}, 0
    for x, y, t in objs:
        fx, fy = int(x * scale), int(y * scale)
        if t in RAY_START_TYPES or not onmap(fx, fy):
            continue
        is_enemy = t in GROUND_ENEMY_TYPES or t in FLY_ENEMY_TYPES
        if is_enemy:
            clear = 520 if t in FLY_ENEMY_TYPES else SPAWN_CLEAR_PX
            if start and abs(fx - start[0]) < clear and \
               abs(fy - start[1]) < max(clear, 200):
                near_spawn += 1
                continue
            nr, dg = OBJ_FLY_ENEMY if t in FLY_ENEMY_TYPES else OBJ_GROUND_ENEMY
            ents.append(ent(nr, dg, fx, fy))
        elif t in OBJECTIVE_TYPES:
            objectives.append((fx, fy))
        elif t == TING_TYPE:
            tings.append((fx, fy))
        elif t in HEALTH_PICKUP_TYPES:
            health.append((fx, fy))
        elif t in DECOR_TYPES or t in PLATFORM_TYPES:
            pass
        else:
            dropped[t] = dropped.get(t, 0) + 1

    for fx, fy in tings:
        ents.append(ent(OBJ_AMMO[0], OBJ_AMMO[1], fx, fy))
    for fx, fy in health:
        ents.append(ent(OBJ_ENERGY[0], OBJ_ENERGY[1], fx, fy))

    recipe = list(objectives[:6])
    synth = 0
    for i in range(len(recipe), 6):
        gx = int((i + 0.5) / 6 * gw)
        gr = lev.ground_row(gx)
        gy = (gr * FA_CELL - FA_CELL) if gr is not None else world_h // 2
        recipe.append((gx * FA_CELL, gy))
        synth += 1
    for i, (fx, fy) in enumerate(recipe):
        nr, dg = OBJ_RECIPE[i]
        ents.append(ent(nr, dg, fx, fy))
    if synth:
        log.append("synthesized %d recipe ingredient(s): source has only %d "
                   "objective object(s)" % (synth, len(objectives)))

    n_enemy = sum(1 for e in ents if e["obj_nr"] in (3, 4))
    log.append("placed: %d enemies, 6 ingredients (%d real + %d synthesized), "
               "%d ammo (Tings), %d energy"
               % (n_enemy, 6 - synth, synth, len(tings), len(health)))
    if near_spawn:
        log.append("dropped %d hostile(s) inside the spawn-clear radius" % near_spawn)
    if dropped:
        log.append("unmapped object types (count) - review these:")
        for t in sorted(dropped):
            log.append("  type %-3d  x%d" % (t, dropped[t]))

    if start is None:
        start = (FA_CELL * 3, FA_CELL)
        log.append("no Rayman start object found; spawning at %s" % (start,))
    ents.insert(0, ent(OBJ_START[0], OBJ_START[1], *start))
    return ents, start


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lev", help="input Rayman RAY*.LEV")
    ap.add_argument("out", help="output OpenFA .W02")
    ap.add_argument("--gdata", default="GData",
                    help="GData dir holding Maps/Welt1E.W02 (the header template)")
    ap.add_argument("--template", default=None,
                    help="explicit .W02 to copy the header + MapInfo from")
    ap.add_argument("--log", default=None,
                    help="unmapped-object report (default: <out>.log.txt)")
    a = ap.parse_args(argv)
    tmpl = a.template or os.path.join(a.gdata, "Maps", "Welt1E.W02")
    log = []

    lev = RayLevel(open(a.lev, "rb").read())
    gw, gh = lev.w, lev.h
    print("Rayman level: %d x %d cells" % (gw, gh))

    hdr, mapinfo = W.read_template(tmpl)
    cls = [[lev.cell_class(x, y) for x in range(gw)] for y in range(gh)]
    if lev.unknown_blocks:
        log.append("unknown block types (forced solid): "
                   + ", ".join("%d x%d" % kv for kv in sorted(lev.unknown_blocks.items())))

    ents, start = map_objects(lev.objects(), lev, gw, gh, log)
    level = W.Level(hdr, mapinfo, gw, gh, cls, ents)
    counts = level.save(a.out)
    print("terrain cells: solid %d  one-way %d  climb %d  hazard %d"
          % (counts[CL_SOLID], counts[CL_ONEWAY], counts[CL_CLIMB], counts[CL_HAZARD]))
    print("entities: %d placed, start at %s" % (len(ents), start))
    print("wrote %s" % a.out)

    logpath = a.log or (a.out + ".log.txt")
    open(logpath, "w").write("rayimport %s -> %s\n\n%s\n" % (a.lev, a.out, "\n".join(log)))
    print("log: %s" % logpath)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
