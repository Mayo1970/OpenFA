#!/usr/bin/env python3
"""
fa_edit.py - a standalone level editor for OpenFA (.W02 maps).

    py tools/fa_edit.py [file.W02] [--gdata GData]      (Windows: use `py`)
    python3 tools/fa_edit.py [file.W02] [--gdata GData]

Edits levels in terrain *classes* (solid / one-way / climb / hazard) and places
entities (player start, enemies, bosses, pickups, lifts). Save runs the same
auto-tiler rayimport.py uses, so a hand-built level looks like an imported one.
Load the result with:  fa_slice --custom1 file.W02

The palette and canvas show the real GIUNGLA tile art and entity sprites,
decoded from Welt1.W01 and GData/Scripts (pure Python, no Pillow). If GData is
not found it falls back to flat colours.

Controls
  left drag        paint the selected brush / place an entity
  right click      erase: terrain cell -> empty, or delete the nearest entity
  mouse wheel      scroll;  Ctrl+wheel / +/-  zoom
"""
import argparse
import os
import re
import sys
import tkinter as tk
from tkinter import filedialog, messagebox, simpledialog, ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fa_w02lib as W
import fa_w01lib as A

CLASS_COLOR = {
    W.CL_EMPTY: "", W.CL_SOLID: "#6b8e23", W.CL_ONEWAY: "#b5651d",
    W.CL_CLIMB: "#2e8b57", W.CL_HAZARD: "#1f6f9c",
}
TERRAIN_BRUSHES = [("Erase", W.CL_EMPTY), ("Solid", W.CL_SOLID),
                   ("One-way", W.CL_ONEWAY), ("Climb", W.CL_CLIMB),
                   ("Hazard", W.CL_HAZARD)]
# representative tile for each class: (atlas frame, tile index)
CLASS_TILE = {
    W.CL_SOLID: (W.GRASS_ATLAS, W.GRASS_MID[1]),
    W.CL_ONEWAY: (W.ONEWAY_ATLAS, W.ONEWAY_TILES[1]),
    W.CL_CLIMB: (W.VINE_ATLAS, W.VINE_TILES[3]),
    W.CL_HAZARD: (W.WATER_ATLAS, W.WATER_TILES[7]),
}


def find_gdata(start, w02_path=None):
    cands = [start, os.path.join(start, ".."), os.getcwd()]
    if w02_path:
        # maps live in GData/Maps/, so the .W02's parent dir is GData
        d = os.path.dirname(os.path.abspath(w02_path))
        cands = [os.path.dirname(d), d] + cands
    for d in cands:
        if d and os.path.isfile(os.path.join(d, "Maps", "Welt1.W01")):
            return os.path.abspath(d)
    return os.path.abspath(start)


class Art:
    """Decoded tile atlas + entity sprites, cached per on-screen size."""

    def __init__(self, gdata):
        self.ok = False
        self.why = ""
        self.gdata = gdata
        self.atlas = {}          # frame -> (w, h, px u16 list)
        self.obj_src = {}        # obj_nr -> (w01 path, frame)
        self._cache = {}         # key -> tk.PhotoImage
        sheet_path = os.path.join(gdata, "Maps", "Welt1.W01")
        try:
            if not os.path.isfile(sheet_path):
                raise FileNotFoundError(sheet_path)
            sheet = A.W01.open(sheet_path)
            for fr in range(min(4, sheet.count())):
                w, h, px = sheet.decode(fr)
                self.atlas[fr] = (w, h, px)
            self._scan_scripts(os.path.join(gdata, "Scripts"))
            self.ok = True
            print("art: loaded %d atlas frames, %d entity sprites from %s"
                  % (len(self.atlas), len(self.obj_src), gdata))
        except Exception as ex:                       # noqa: BLE001
            self.why = "%s: %s" % (type(ex).__name__, ex)
            print("art DISABLED -", self.why)

    def _scan_scripts(self, sdir):
        for fn in os.listdir(sdir):
            if not fn.lower().endswith((".jrs", ".txt")):
                continue
            try:
                t = open(os.path.join(sdir, fn), encoding="latin-1").read()
            except OSError:
                continue
            if 'SCRIPT_TYP' not in t or '"AOM"' not in t:
                continue
            mo = re.search(r'ObjNr\s*=\s*(-?\d+)', t)
            mf = re.search(r'FileName\s*=\s*"([^"]+)"', t)
            ms = re.search(r'FileAnimStart\s*=\s*(\d+)', t)
            if not (mo and mf):
                continue
            rel = re.sub(r"[\\/]+", "/", mf.group(1)).lstrip("/")
            if rel.lower().startswith("gdata/"):
                rel = rel[6:]
            self.obj_src[int(mo.group(1))] = (
                os.path.join(self.gdata, rel), int(ms.group(1)) if ms else 0)

    # -- public --------------------------------------------------
    def atlas_photo(self, frame, idx, size, key=None):
        """One 32x32 atlas tile, scaled, cached. `key` colour -> transparent."""
        ck = ("a", frame, idx, size, key)
        if ck in self._cache:
            return self._cache[ck]
        if frame not in self.atlas:
            return None
        w, h, px = self.atlas[frame]
        sx, sy = (idx % 20) * 32, (idx // 20) * 32
        sub = [px[(sy + y) * w + sx + x] if 0 <= sy + y < h and 0 <= sx + x < w else 0
               for y in range(32) for x in range(32)]
        rgba = A.rgba_from_565(sub, 32, 32, key=key)
        img = _photo(A.scale_rgba(rgba, 32, 32, size, size), size, size)
        if img is not None:
            self._cache[ck] = img
        return img

    def tile_photo(self, cls, size):
        fr, idx = CLASS_TILE.get(cls, (0, 0))
        return self.atlas_photo(fr, idx, size)

    def deco_photo(self, atlas, tile, size):
        return self.atlas_photo(atlas, tile, size, key=0x0000)

    def entity_photo(self, obj_nr, size):
        key = ("e", obj_nr, size)
        if key in self._cache:
            return self._cache[key]
        src = self.obj_src.get(obj_nr)
        img = None
        if src and os.path.isfile(src[0]):
            try:
                sh = A.W01.open(src[0])
                fr = min(src[1], sh.count() - 1)
                w, h, px = sh.decode(fr)
                rgba = A.rgba_from_565(px, w, h, key=0x0000)
                m = max(w, h) or 1
                tw, th = max(1, w * size // m), max(1, h * size // m)
                img = _photo(A.scale_rgba(rgba, w, h, tw, th), tw, th)
            except Exception:                        # noqa: BLE001
                img = None
        self._cache[key] = img
        return img

    def clear_size_cache(self):
        self._cache.clear()


_PHOTO_BG = (32, 48, 58)          # canvas background - alpha is composited onto it


def _photo(rgba, w, h):
    """Build a PhotoImage with .put() - works on every Tk 8.6, no PNG needed.
    Transparent pixels are flattened onto the canvas background colour."""
    try:
        br, bg, bb = _PHOTO_BG
        rows = []
        for y in range(h):
            cells = []
            base = y * w * 4
            for x in range(w):
                o = base + x * 4
                a = rgba[o + 3]
                if a == 255:
                    cells.append("#%02x%02x%02x" % (rgba[o], rgba[o + 1], rgba[o + 2]))
                elif a == 0:
                    cells.append("#%02x%02x%02x" % (br, bg, bb))
                else:
                    cells.append("#%02x%02x%02x" % (
                        (rgba[o] * a + br * (255 - a)) // 255,
                        (rgba[o + 1] * a + bg * (255 - a)) // 255,
                        (rgba[o + 2] * a + bb * (255 - a)) // 255))
            rows.append("{" + " ".join(cells) + "}")
        img = tk.PhotoImage(width=w, height=h)
        img.put(" ".join(rows))
        return img
    except Exception as ex:                           # noqa: BLE001
        print("photo build failed:", ex)
        return None


class Editor(tk.Tk):
    def __init__(self, path, gdata):
        super().__init__()
        self.title("OpenFA level editor")
        self.geometry("1240x780")
        self.gdata = find_gdata(gdata, path)
        self.template = self._find_template()
        self.art = Art(self.gdata)
        self.level = None
        self.path = None
        self.cell = 20
        self.brush = ("terrain", W.CL_SOLID)
        self.dirty = False
        self.cell_item = {}          # (x, y) -> canvas id
        self.ent_items = []

        self._build_ui()
        if not self.art.ok:
            messagebox.showwarning(
                "Tile art unavailable",
                "Showing flat colours instead of tiles/sprites.\n\n"
                "Reason: %s\n\nGData tried: %s\n\n"
                "Run from the folder that holds GData, or pass --gdata DIR."
                % (self.art.why or "GData/Maps/Welt1.W01 not found", self.gdata))
        if path and os.path.isfile(path):
            self._open(path)
        elif self.template:
            self.level = W.Level.blank(self.template, 60, 34)
            self._refresh_title()
            self._redraw()
        else:
            messagebox.showwarning("No template",
                                   "No GData/Maps/*.W02 found. Open a .W02 or pass --gdata.")

    def _find_template(self):
        for n in ("Welt1E.W02", "Welt2E.W02", "Welt1.W02"):
            p = os.path.join(self.gdata, "Maps", n)
            if os.path.isfile(p):
                return p
        return None

    # -- UI ----------------------------------------------------
    def _build_ui(self):
        m = tk.Menu(self)
        fm = tk.Menu(m, tearoff=0)
        for lbl, cmd, acc in (("New...", self._new, "Ctrl+N"),
                              ("Open...", self._open_dialog, "Ctrl+O"),
                              ("Save", self._save, "Ctrl+S"),
                              ("Save As...", self._save_as, "")):
            fm.add_command(label=lbl, command=cmd, accelerator=acc)
        fm.add_separator()
        fm.add_command(label="Quit", command=self._quit)
        m.add_cascade(label="File", menu=fm)
        self.config(menu=m)
        self.bind("<Control-n>", lambda e: self._new())
        self.bind("<Control-o>", lambda e: self._open_dialog())
        self.bind("<Control-s>", lambda e: self._save())
        self.bind("<plus>", lambda e: self._zoom(1))
        self.bind("<minus>", lambda e: self._zoom(-1))
        self.protocol("WM_DELETE_WINDOW", self._quit)

        left = tk.Frame(self, width=230)
        left.pack(side=tk.LEFT, fill=tk.Y)
        left.pack_propagate(False)

        tk.Label(left, text="Terrain", font=("", 9, "bold")).pack(anchor="w", padx=6, pady=(6, 2))
        self.brush_var = tk.StringVar(value="terrain:%d" % W.CL_SOLID)
        self._terrain_btns = []
        for label, cl in TERRAIN_BRUSHES:
            rb = tk.Radiobutton(left, text=" " + label, value="terrain:%d" % cl,
                                variable=self.brush_var, anchor="w", compound="left",
                                command=self._pick_brush)
            rb.pack(fill=tk.X, padx=6)
            self._terrain_btns.append((rb, cl))

        nb = ttk.Notebook(left)
        nb.pack(fill=tk.BOTH, expand=True, padx=4, pady=(10, 4))

        ef = tk.Frame(nb)
        nb.add(ef, text="Entities")
        self.tree = ttk.Treeview(ef, show="tree", selectmode="browse")
        self.tree.pack(fill=tk.BOTH, expand=True)
        self.tree_rows = {}
        for label, nr, dg in W.ENTITY_KINDS:
            if nr < 0:
                self.tree.insert("", tk.END, text=label.strip("- "), tags=("hdr",))
                continue
            iid = self.tree.insert("", tk.END, text=" " + label)
            self.tree_rows[iid] = (nr, dg, label)
        self.tree.tag_configure("hdr", foreground="#888")
        self.tree.bind("<<TreeviewSelect>>", self._pick_entity)

        df = tk.Frame(nb)
        nb.add(df, text="Decor")
        self.dtree = ttk.Treeview(df, show="tree", selectmode="browse")
        self.dtree.pack(fill=tk.BOTH, expand=True)
        self.dtree_rows = {}
        for label, plane, atlas, tile in W.DECOR_KINDS:
            if plane < 0:
                self.dtree.insert("", tk.END, text=label.strip("- "), tags=("hdr",))
                continue
            iid = self.dtree.insert("", tk.END, text=" " + label)
            self.dtree_rows[iid] = (plane, atlas, tile, label)
        self.dtree.tag_configure("hdr", foreground="#888")
        self.dtree.bind("<<TreeviewSelect>>", self._pick_decor)

        self.status = tk.Label(self, anchor="w", relief=tk.SUNKEN)
        self.status.pack(side=tk.BOTTOM, fill=tk.X)

        cf = tk.Frame(self)
        cf.pack(side=tk.RIGHT, fill=tk.BOTH, expand=True)
        self.hbar = tk.Scrollbar(cf, orient=tk.HORIZONTAL)
        self.hbar.pack(side=tk.BOTTOM, fill=tk.X)
        self.vbar = tk.Scrollbar(cf)
        self.vbar.pack(side=tk.RIGHT, fill=tk.Y)
        self.canvas = tk.Canvas(cf, bg="#20303a", xscrollcommand=self.hbar.set,
                                yscrollcommand=self.vbar.set)
        self.canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self.hbar.config(command=self.canvas.xview)
        self.vbar.config(command=self.canvas.yview)
        self.canvas.bind("<Button-1>", self._on_paint)
        self.canvas.bind("<B1-Motion>", self._on_paint)
        self.canvas.bind("<Button-3>", self._on_erase)
        self.canvas.bind("<Motion>", self._on_hover)
        self.canvas.bind("<MouseWheel>", lambda e: self.canvas.yview_scroll(-1 if e.delta > 0 else 1, "units"))
        self.canvas.bind("<Shift-MouseWheel>", lambda e: self.canvas.xview_scroll(-1 if e.delta > 0 else 1, "units"))
        self.canvas.bind("<Control-MouseWheel>", lambda e: self._zoom(1 if e.delta > 0 else -1))

        self._refresh_palette_icons()

    def _refresh_palette_icons(self):
        if not self.art.ok:
            return
        for rb, cl in self._terrain_btns:
            img = self.art.tile_photo(cl, 20) if cl != W.CL_EMPTY else None
            rb.config(image=img if img else "")
            rb._img = img
        for iid, row in list(self.tree_rows.items()):
            img = self.art.entity_photo(row[0], 22)
            self.tree.item(iid, image=img if img else "")
            self.tree_rows[iid] = (row[0], row[1], row[2], img)
        for iid, row in list(self.dtree_rows.items()):
            img = self.art.deco_photo(row[1], row[2], 22)
            self.dtree.item(iid, image=img if img else "")
            self.dtree_rows[iid] = (row[0], row[1], row[2], row[3], img)

    # -- brush ------------------------------------------------
    def _pick_brush(self):
        self.brush = ("terrain", int(self.brush_var.get().split(":")[1]))
        for i in self.tree.selection():
            self.tree.selection_remove(i)
        for i in self.dtree.selection():
            self.dtree.selection_remove(i)

    def _pick_entity(self, _e):
        sel = self.tree.selection()
        if not sel or sel[0] not in self.tree_rows:
            return
        row = self.tree_rows[sel[0]]
        self.brush = ("entity", row[0], row[1], row[2])
        self.brush_var.set("")
        for i in self.dtree.selection():
            self.dtree.selection_remove(i)

    def _pick_decor(self, _e):
        sel = self.dtree.selection()
        if not sel or sel[0] not in self.dtree_rows:
            return
        plane, atlas, tile, label = self.dtree_rows[sel[0]][:4]
        self.brush = ("decor", plane, atlas, tile, label)
        self.brush_var.set("")
        for i in self.tree.selection():
            self.tree.selection_remove(i)

    # -- file ops -------------------------------------------
    def _new(self):
        if not self.template:
            messagebox.showerror("No template", "Need a GData template .W02.")
            return
        w = simpledialog.askinteger("New level", "Width in cells:", initialvalue=60,
                                    minvalue=8, maxvalue=1000, parent=self)
        h = w and simpledialog.askinteger("New level", "Height in cells:", initialvalue=34,
                                          minvalue=8, maxvalue=400, parent=self)
        if not (w and h):
            return
        self.level = W.Level.blank(self.template, w, h)
        self.path, self.dirty = None, False
        self._refresh_title()
        self._redraw()

    def _open_dialog(self):
        p = filedialog.askopenfilename(filetypes=[("WESTKA pool", "*.W02 *.w02"), ("All", "*.*")])
        if p:
            self._open(p)

    def _open(self, p):
        try:
            self.level = W.Level.load(p)
        except Exception as ex:                       # noqa: BLE001
            messagebox.showerror("Open failed", str(ex))
            return
        self.path, self.dirty = p, False
        self._refresh_title()
        self._redraw()

    def _save(self):
        if not self.level:
            return
        self._write(self.path) if self.path else self._save_as()

    def _save_as(self):
        p = filedialog.asksaveasfilename(defaultextension=".W02",
                                         filetypes=[("WESTKA pool", "*.W02")])
        if p:
            self.path = p
            self._write(p)

    def _write(self, p):
        try:
            c = self.level.save(p)
        except Exception as ex:                       # noqa: BLE001
            messagebox.showerror("Save failed", str(ex))
            return
        self.dirty = False
        self._refresh_title()
        self._set_status("saved %s  (solid %d one-way %d climb %d hazard %d, %d entities)"
                         % (os.path.basename(p), c[W.CL_SOLID], c[W.CL_ONEWAY],
                            c[W.CL_CLIMB], c[W.CL_HAZARD], len(self.level.entities)))

    def _quit(self):
        if self.dirty and not messagebox.askokcancel("Unsaved changes",
                                                     "Discard changes and quit?"):
            return
        self.destroy()

    # -- canvas -----------------------------------------------
    def _cell_at(self, evt):
        return (int(self.canvas.canvasx(evt.x) // self.cell),
                int(self.canvas.canvasy(evt.y) // self.cell))

    def _redraw(self):
        c = self.canvas
        c.delete("all")
        self.cell_item.clear()
        self.ent_items.clear()
        if not self.level:
            return
        lv, s = self.level, self.cell
        c.config(scrollregion=(0, 0, lv.gw * s, lv.gh * s))
        todo = {(x, y) for y in range(lv.gh) for x in range(lv.gw)
                if lv.cls[y][x] != W.CL_EMPTY}
        todo |= set(lv.decor)
        for x, y in todo:
            self._paint_cell(x, y)
        if s >= 12:
            for x in range(lv.gw + 1):
                c.create_line(x * s, 0, x * s, lv.gh * s, fill="#2b3d48", tags="grid")
            for y in range(lv.gh + 1):
                c.create_line(0, y * s, lv.gw * s, y * s, fill="#2b3d48", tags="grid")
        for e in lv.entities:
            self._draw_entity(e)
        self._set_status("%d x %d cells%s" % (lv.gw, lv.gh,
                         "" if self.art.ok else
                         "   [flat colours - art off: %s]" % (self.art.why or "GData not found")))

    def _paint_cell(self, x, y):
        for it in self.cell_item.pop((x, y), []):
            self.canvas.delete(it)
        s = self.cell
        ox, oy = x * s, y * s
        ids = []
        deco = self.level.decor.get((x, y))
        cl = self.level.cls[y][x]

        def blit(img, col):
            if img:
                return self.canvas.create_image(ox, oy, image=img, anchor="nw", tags="cell")
            return self.canvas.create_rectangle(ox, oy, ox + s, oy + s,
                                                fill=col, outline="", tags="cell")

        if deco and deco[0] < W.FA_PLANE_TERRAIN:            # behind terrain
            ids.append(blit(self.art.deco_photo(deco[1], deco[2], s) if self.art.ok else None,
                            "#3a5a40"))
        if cl != W.CL_EMPTY:
            ids.append(blit(self.art.tile_photo(cl, s) if self.art.ok else None,
                            CLASS_COLOR[cl]))
        if deco and deco[0] > W.FA_PLANE_TERRAIN:            # in front of terrain
            ids.append(blit(self.art.deco_photo(deco[1], deco[2], s) if self.art.ok else None,
                            "#3a5a40"))
        if ids:
            self.cell_item[(x, y)] = ids
        if self.canvas.find_withtag("grid"):
            self.canvas.tag_raise("grid")
        if self.canvas.find_withtag("ent"):
            self.canvas.tag_raise("ent")

    def _draw_entity(self, e):
        s = self.cell
        px, py = e["x"] * s / 32.0, e["y"] * s / 32.0
        nr = e["obj_nr"]
        img = self.art.entity_photo(nr, max(16, int(s * 1.4))) if self.art.ok else None
        if img:
            self.ent_items.append(self.canvas.create_image(px, py, image=img, tags="ent"))
        col = ("#ffd000" if nr == 1000 else "#e23b3b" if e["dg"] == 3
               else "#39c0d0" if e["dg"] == 1 else "#c0c0c0")
        if not img:
            r = max(4, s // 3)
            self.ent_items.append(self.canvas.create_oval(px - r, py - r, px + r, py + r,
                                                          fill=col, outline="black", tags="ent"))
        label = "START" if nr == 1000 else str(nr)
        self.ent_items.append(self.canvas.create_text(px, py - s * 0.8, text=label,
                                                      fill=col, font=("", 7), tags="ent"))

    def _on_paint(self, evt):
        if not self.level:
            return
        x, y = self._cell_at(evt)
        if not (0 <= x < self.level.gw and 0 <= y < self.level.gh):
            return
        if self.brush[0] == "terrain":
            if self.level.cls[y][x] != self.brush[1]:
                self.level.cls[y][x] = self.brush[1]
                self._paint_cell(x, y)
                self._mark_dirty()
        elif self.brush[0] == "decor":
            _, plane, atlas, tile, _l = self.brush
            if self.level.decor.get((x, y)) != (plane, atlas, tile):
                self.level.decor[(x, y)] = (plane, atlas, tile)
                self._paint_cell(x, y)
                self._mark_dirty()
        elif self.brush[0] == "entity" and evt.type == tk.EventType.ButtonPress:
            _, nr, dg, _l = self.brush
            if nr == 1000:
                self.level.entities = [e for e in self.level.entities if e["obj_nr"] != 1000]
            self.level.entities.append({"obj_nr": nr, "x": x * 32 + 16,
                                        "y": y * 32 + 16, "dg": dg, "raw": None})
            self._redraw_entities()
            self._mark_dirty()

    def _on_erase(self, evt):
        if not self.level:
            return
        x, y = self._cell_at(evt)
        wx, wy = x * 32 + 16, y * 32 + 16
        near = [e for e in self.level.entities
                if abs(e["x"] - wx) <= 24 and abs(e["y"] - wy) <= 24]
        onmap = 0 <= x < self.level.gw and 0 <= y < self.level.gh
        if near:
            self.level.entities.remove(near[0])
            self._redraw_entities()
        elif onmap and (x, y) in self.level.decor:
            del self.level.decor[(x, y)]
            self._paint_cell(x, y)
        elif onmap and self.level.cls[y][x] != W.CL_EMPTY:
            self.level.cls[y][x] = W.CL_EMPTY
            self._paint_cell(x, y)
        else:
            return
        self._mark_dirty()

    def _redraw_entities(self):
        for it in self.ent_items:
            self.canvas.delete(it)
        self.ent_items.clear()
        for e in self.level.entities:
            self._draw_entity(e)

    def _on_hover(self, evt):
        if not self.level:
            return
        x, y = self._cell_at(evt)
        if 0 <= x < self.level.gw and 0 <= y < self.level.gh:
            b = (W.CLASS_NAME[self.brush[1]] if self.brush[0] == "terrain"
                 else self.brush[-1])
            self._set_status("cell (%d, %d)  px (%d, %d)   brush: %s"
                             % (x, y, x * 32, y * 32, b))

    def _zoom(self, d):
        self.cell = max(8, min(44, self.cell + d * 2))
        self.art.clear_size_cache()
        self._redraw()

    # -- misc ------------------------------------------------
    def _mark_dirty(self):
        if not self.dirty:
            self.dirty = True
            self._refresh_title()

    def _refresh_title(self):
        n = os.path.basename(self.path) if self.path else "(new level)"
        self.title("%s%s - OpenFA level editor" % ("*" if self.dirty else "", n))

    def _set_status(self, msg):
        self.status.config(text="  " + msg)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?", help="a .W02 to open")
    ap.add_argument("--gdata", default="GData", help="GData dir (art + New template)")
    a = ap.parse_args(argv)
    Editor(a.file, a.gdata).mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
