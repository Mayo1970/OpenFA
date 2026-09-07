#!/usr/bin/env python3
"""
fa_w02lib.py - shared WESTKA .W02 level read / write for the OpenFA tools.

A playable level is chunk 0 of a .W02 pool:
    [ 281-byte pool header ][ u32 size | u16 kind | payload ][ u32 ptr table ]
    payload = [ 348-byte MapInfo ][ 8-plane grid ][ entity tail ]

The grid is plane-major, 8 planes, 3 bytes per entry: u8 attr, u16 packed
(little-endian). tile = packed & 0x1FF, code = (packed >> 9) & 0x7F. Terrain
collision lives on plane 2. See include/fa/fa_map.h and include/fa/fa_w02.h.

This module works in terrain *classes* (empty / solid / one-way / climb /
hazard), not raw tiles. `autotile` turns a class grid into drawable GIUNGLA
tiles; `read_w02` turns a shipped grid back into classes. Both rayimport.py
and fa_edit.py build on this.
"""
import struct

# --- terrain classes -----------------------------------------------------
CL_EMPTY, CL_SOLID, CL_ONEWAY, CL_CLIMB, CL_HAZARD = range(5)
CLASS_NAME = {CL_EMPTY: "empty", CL_SOLID: "solid", CL_ONEWAY: "one-way",
              CL_CLIMB: "climb", CL_HAZARD: "hazard"}

# --- .W02 / MapInfo / grid layout -------------------------------------
POOL_HDR = 0x119
POOL_REC = 6
FA_PLANE_TERRAIN = 2
FA_PLANES = 8
FA_ENTRY_EMPTY = b"\xff\xff\xff"
FA_MAP_INFO_SIZE = 348
MI_GRID_W, MI_GRID_H, MI_TILE_W, MI_TILE_H, MI_GRID_BYTES = 274, 276, 270, 272, 306

FA_ATTR_SOLID = 0x20
FA_ATTR_HAZARD = 0x80
FA_ATTR_ATLAS = 0x07
FA_CODE_ONEWAY = 2
FA_CODE_CLIMB = 1

# --- entity tail ------------------------------------------------------
FA_REC_BYTES = 186
REC_OBJNR, REC_X, REC_Y = 0x00, 0x02, 0x04
REC_ACTIVE, REC_BAND, REC_DG, REC_PLANE, REC_COLL = 0x06, 0x17, 0x18, 0x19, 0x1A

# Placeable entities: label -> (ObjNr, DetailGroup). ObjNr / DG verified
# against GData/Scripts/*.jrs. DG 4 = misc, 3 = enemy, 1 = pickup, 0 = lift.
ENTITY_KINDS = [
    ("Player start",      1000, 4),
    ("-- enemies --",       -1, 0),
    ("Snake",                4, 3),
    ("Parrot",               3, 3),
    ("Kong",                 5, 3),
    ("Eagle (Adler)",        6, 3),
    ("Little Yeti",          7, 3),
    ("Snowman",              8, 3),
    ("Little Robot",        11, 3),
    ("Egg Robot",           12, 3),
    ("Flying Robot",        13, 3),
    ("Bear",                15, 3),
    ("Bee (Biene)",         16, 3),
    ("Little Octopus",      17, 3),
    ("-- bosses --",        -1, 0),
    ("Boss: Yeti",           9, 3),
    ("Boss: Gorilla",       10, 3),
    ("Boss: Robot",         14, 3),
    ("Boss: Octopus",       18, 3),
    ("-- pickups --",       -1, 0),
    ("Energy",              51, 1),
    ("Snowballs (ammo)",    52, 1),
    ("Dirty balls",         60, 1),
    ("Ingredient 1",        53, 1),
    ("Ingredient 2",        54, 1),
    ("Ingredient 3",        55, 1),
    ("Ingredient 4",        56, 1),
    ("Ingredient 5",        57, 1),
    ("Ingredient 6",        58, 1),
    ("Ingredient 7",        59, 1),
    ("Collect: Paradiso",   48, 1),
    ("Collect: Pingui",     49, 1),
    ("Collect: Milchschn.", 50, 1),
    ("-- other --",         -1, 0),
    ("Lift / platform",      2, 0),
    ("Crumble platform",   414, 0),
    ("Paradiso (NPC)",      77, 4),
]
OBJ_DG = {nr: dg for (_, nr, dg) in ENTITY_KINDS if nr >= 0}

# Decoration brushes: pure-visual tiles, no collision. (label, plane, atlas
# frame, tile index). plane < 2 draws behind the terrain, plane > 2 in front.
# Tiles picked off the decoded GIUNGLA atlas frames.
DECOR_KINDS = [
    ("-- behind --",     -1, 0, 0),
    ("Tree trunk",         1, 0, 123),
    ("Tree top",           1, 0, 43),
    ("Rock",               1, 1, 237),
    ("Rock wall",          1, 1, 151),
    ("Vine rope",          1, 3, 20),
    ("Wood post (top)",    1, 3, 34),
    ("Wood post (base)",   1, 3, 74),
    ("-- in front --",    -1, 0, 0),
    ("Fern",               3, 3, 132),
    ("Small plant",        3, 3, 51),
    ("Grass tuft",         3, 3, 36),
    ("Grass fringe",       3, 3, 270),
    ("Hanging leaves",     3, 0, 66),
]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


# --- GIUNGLA (Welt1.W01) atlas tile roles -------------------------------
# Read straight off the decoded atlas frames (20 cols x 32 px):
#   frame 0 : grass strip (row 2) + rock fill (rows 6-9)
#   frame 2 : water surface (rows 3-6) + wooden raft (row 12)
#   frame 3 : hanging vine (column 0)
GRASS_ATLAS, GRASS_MID, GRASS_LEFT, GRASS_RIGHT = 0, (41, 42, 43, 44, 45, 46), 40, 47
FILL_ATLAS = 0
FILL_TILES = (129, 130, 131, 132, 133, 149, 150, 151, 152, 153,
              169, 170, 171, 172, 173, 189, 190, 191, 192, 193)
WATER_ATLAS, WATER_TILES = 2, (62, 63, 64, 65, 66, 82, 83, 84, 85, 86,
                               102, 103, 104, 105, 106)
ONEWAY_ATLAS, ONEWAY_TILES = 2, (240, 241, 242)
VINE_ATLAS, VINE_TILES = 3, (20, 40, 60, 80, 100, 120, 140, 160, 180, 200)


def autotile(get_class, gw, gh):
    """
    Build the 8-plane grid (bytes) from a terrain class field. `get_class(x, y)`
    returns one of CL_*. Only plane 2 is written; the rest stay empty.
    Returns (grid_bytes, counts).
    """
    plane_bytes = 3 * gw * gh
    grid = bytearray(FA_ENTRY_EMPTY * (gw * gh * FA_PLANES))
    cls = [[get_class(x, y) for x in range(gw)] for y in range(gh)]

    def put(x, y, attr, packed):
        off = FA_PLANE_TERRAIN * plane_bytes + 3 * (y * gw + x)
        grid[off] = attr
        struct.pack_into("<H", grid, off + 1, packed)

    def solidish(x, y):
        return 0 <= y < gh and 0 <= x < gw and cls[y][x] in (CL_SOLID, CL_ONEWAY)

    def pick(seq, x, y):
        return seq[(x * 7 + y * 3) % len(seq)]

    counts = {CL_SOLID: 0, CL_ONEWAY: 0, CL_CLIMB: 0, CL_HAZARD: 0}
    for y in range(gh):
        for x in range(gw):
            c = cls[y][x]
            if c == CL_EMPTY:
                continue
            counts[c] += 1
            if c == CL_SOLID:
                if not solidish(x, y - 1):
                    left, right = solidish(x - 1, y), solidish(x + 1, y)
                    tile = (GRASS_LEFT if right and not left else
                            GRASS_RIGHT if left and not right else
                            pick(GRASS_MID, x, y))
                    put(x, y, FA_ATTR_SOLID | GRASS_ATLAS, tile)
                else:
                    put(x, y, FA_ATTR_SOLID | FILL_ATLAS, pick(FILL_TILES, x, y))
            elif c == CL_ONEWAY:
                put(x, y, FA_ATTR_SOLID | ONEWAY_ATLAS,
                    pick(ONEWAY_TILES, x, y) | (FA_CODE_ONEWAY << 9))
            elif c == CL_CLIMB:
                put(x, y, VINE_ATLAS,
                    VINE_TILES[y % len(VINE_TILES)] | (FA_CODE_CLIMB << 9))
            elif c == CL_HAZARD:
                put(x, y, FA_ATTR_HAZARD | WATER_ATLAS, pick(WATER_TILES, x, y))
    return bytes(grid), counts


def classify_entry(attr, packed):
    """Reverse of autotile for one plane-2 entry -> a CL_* class."""
    if packed == 0xFFFF:
        return CL_EMPTY
    code = (packed >> 9) & 0x7F
    if code & FA_CODE_CLIMB:
        return CL_CLIMB
    if code & FA_CODE_ONEWAY:
        return CL_ONEWAY
    if attr & FA_ATTR_HAZARD:
        return CL_HAZARD
    if attr & FA_ATTR_SOLID:
        return CL_SOLID
    return CL_SOLID


# --- entity records -------------------------------------------------

def entity_record(obj_nr, x, y, detail_group=None, raw=None):
    """186-byte record. Reuse `raw` (round-trips unknown fields) if given."""
    r = bytearray(raw if raw is not None else bytes(FA_REC_BYTES))
    if detail_group is None:
        detail_group = OBJ_DG.get(obj_nr, 4)
    struct.pack_into("<h", r, REC_OBJNR, obj_nr)
    struct.pack_into("<h", r, REC_X, max(-32768, min(32767, int(x))))
    struct.pack_into("<h", r, REC_Y, max(-32768, min(32767, int(y))))
    if raw is None:
        struct.pack_into("<H", r, REC_ACTIVE, 0x00FF)
        struct.pack_into("<H", r, 0x08, 0x00FF)
        r[REC_BAND] = 0
        r[REC_PLANE] = FA_PLANE_TERRAIN
        r[REC_COLL] = 1
        for o in (0x1F, 0x21, 0x23, 0x25):
            struct.pack_into("<h", r, o, -1)
    r[REC_DG] = detail_group
    return bytes(r)


def build_tail(records):
    hdr = struct.pack("<IH", len(records) * FA_REC_BYTES, len(records)) + b"\x00" * 20
    return hdr + b"".join(records)


# --- whole-file read / write ---------------------------------------

def read_template(path):
    """(pool_header_281, mapinfo_348) from a shipped .W02, for a blank map."""
    b = open(path, "rb").read()
    if u32(b, 0) != 0x115 or len(b) < POOL_HDR:
        raise ValueError("%s is not a .W02 pool" % path)
    n = u16(b, 0x117)
    off = u32(b, len(b) - n * 4)
    if len(b) - off < FA_MAP_INFO_SIZE:
        raise ValueError("template chunk 0 too small")
    return bytearray(b[:POOL_HDR]), bytearray(b[off:off + FA_MAP_INFO_SIZE])


class Level:
    """An editable level: header + mapinfo + class grid + entity list."""

    def __init__(self, header, mapinfo, gw, gh, cls=None, entities=None, decor=None):
        self.header = bytearray(header)
        self.mapinfo = bytearray(mapinfo)
        self.gw, self.gh = gw, gh
        self.cls = cls or [[CL_EMPTY] * gw for _ in range(gh)]
        # entities: list of dicts {obj_nr, x, y, dg, raw}
        self.entities = entities or []
        # decor: {(x, y): (plane, atlas, tile)} - one visual-only tile per cell
        self.decor = decor or {}

    # -- factory ---------------------------------------------------
    @classmethod
    def blank(cls_, template_w02, gw, gh):
        hdr, mi = read_template(template_w02)
        struct.pack_into("<H", mi, MI_GRID_W, gw)
        struct.pack_into("<H", mi, MI_GRID_H, gh)
        struct.pack_into("<I", mi, MI_GRID_BYTES, gw * gh * 24)
        return cls_(hdr, mi, gw, gh)

    @classmethod
    def load(cls_, path):
        b = open(path, "rb").read()
        if u32(b, 0) != 0x115:
            raise ValueError("not a .W02 pool")
        n = u16(b, 0x117)
        off = u32(b, len(b) - n * 4)
        p = b[off:]
        gw, gh = u16(p, MI_GRID_W), u16(p, MI_GRID_H)
        gb = u32(p, MI_GRID_BYTES)
        grid = p[FA_MAP_INFO_SIZE:FA_MAP_INFO_SIZE + gb]
        plane_bytes = 3 * gw * gh
        cls = [[CL_EMPTY] * gw for _ in range(gh)]
        base = FA_PLANE_TERRAIN * plane_bytes
        for y in range(gh):
            for x in range(gw):
                o = base + 3 * (y * gw + x)
                cls[y][x] = classify_entry(grid[o], u16(grid, o + 1))
        # decoration: first non-empty entry on any plane other than 2
        decor = {}
        for pl in (1, 3, 0, 4, 5, 6, 7):
            pb = pl * plane_bytes
            for y in range(gh):
                for x in range(gw):
                    if (x, y) in decor:
                        continue
                    o = pb + 3 * (y * gw + x)
                    pk = u16(grid, o + 1)
                    if pk != 0xFFFF:
                        decor[(x, y)] = (pl, grid[o] & FA_ATTR_ATLAS, pk & 0x1FF)
        ents = []
        tail = p[FA_MAP_INFO_SIZE + gb:]
        if len(tail) >= 26:
            cnt = u16(tail, 4)
            for i in range(cnt):
                r = tail[26 + i * FA_REC_BYTES: 26 + (i + 1) * FA_REC_BYTES]
                if len(r) < FA_REC_BYTES:
                    break
                nr = struct.unpack_from("<h", r, REC_OBJNR)[0]
                ents.append({
                    "obj_nr": nr,
                    "x": struct.unpack_from("<h", r, REC_X)[0],
                    "y": struct.unpack_from("<h", r, REC_Y)[0],
                    "dg": r[REC_DG],
                    "raw": bytes(r),
                })
        return cls_(bytearray(b[:POOL_HDR]), bytearray(p[:FA_MAP_INFO_SIZE]),
                    gw, gh, cls, ents, decor)

    # -- save ----------------------------------------------------
    def save(self, path):
        struct.pack_into("<H", self.mapinfo, MI_GRID_W, self.gw)
        struct.pack_into("<H", self.mapinfo, MI_GRID_H, self.gh)
        struct.pack_into("<I", self.mapinfo, MI_GRID_BYTES, self.gw * self.gh * 24)
        grid, counts = autotile(lambda x, y: self.cls[y][x], self.gw, self.gh)
        grid = bytearray(grid)
        plane_bytes = 3 * self.gw * self.gh
        for (x, y), (pl, atlas, tile) in self.decor.items():
            if not (0 <= x < self.gw and 0 <= y < self.gh) or pl == FA_PLANE_TERRAIN:
                continue
            o = pl * plane_bytes + 3 * (y * self.gw + x)
            grid[o] = atlas & FA_ATTR_ATLAS
            struct.pack_into("<H", grid, o + 1, tile & 0x1FF)
        grid = bytes(grid)

        recs = []
        for e in self.entities:
            raw = e.get("raw")
            # a moved/retyped record keeps its other bytes; a new one has none
            if raw is not None and len(raw) == FA_REC_BYTES:
                recs.append(entity_record(e["obj_nr"], e["x"], e["y"],
                                          e.get("dg"), raw))
            else:
                recs.append(entity_record(e["obj_nr"], e["x"], e["y"], e.get("dg")))

        payload = bytes(self.mapinfo) + grid + build_tail(recs)
        hdr = bytearray(self.header)
        struct.pack_into("<H", hdr, 0x117, 1)
        kind = u16(hdr, 0x115) or 2
        rec = struct.pack("<IH", len(payload), kind)
        blob = bytes(hdr) + rec + payload + struct.pack("<I", POOL_HDR + POOL_REC)
        open(path, "wb").write(blob)
        return counts

    # -- helpers -----------------------------------------------
    @property
    def world_w(self):
        return self.gw * 32

    @property
    def world_h(self):
        return self.gh * 32
