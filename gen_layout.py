"""Generate the Grail Tab slot layout and panel layouts from the game's Excel tables.

Writes grail_layout.h: one home slot per grail item (same eligibility rule as grail.py:
spawnable and not disabled), packed into 10x10 pages, one page per shared stash tab.

Filed items sit at these positions, so changing the packing later strands them in their
old cells. layout_v1.tsv freezes the 1.0 layout: generation fails if any slot in it would
move. Items a future patch adds belong in free cells of their section, or on new pages
after the last one; never move an existing slot.
"""
import csv
import json
import os
import sys
from pathlib import Path

# The game's extracted data/global/excel folder (set D2R_EXCEL to point elsewhere).
EXCEL = Path(os.environ.get("D2R_EXCEL", Path.home() / "Projects/d2-rack-log/solver/d2r_data/data/global/excel"))
NAMES = EXCEL.parent.parent / "local/lng/strings/item-names.json"
OUT = Path(__file__).resolve().parent / "grail_layout.h"
LOCK = OUT.parent / "layout_v1.tsv"
W = H = 10

UNIQUE_SECTIONS = [
    ("Helms", {"helm", "circ", "pelt", "phlm", "head"}),
    ("Body Armor, Belts", {"tors", "belt"}),
    ("Shields", {"shie", "ashd"}),
    ("Gloves, Boots, Jewelry", {"glov", "boot", "ring", "amul", "scha", "mcha", "lcha", "jewl"}),
    ("Axes", {"axe", "taxe"}),
    ("Swords", {"swor"}),
    ("Daggers, Javelins, Spears", {"knif", "tkni", "jave", "ajav", "spea", "aspe"}),
    ("Maces, Hammers, Scepters", {"mace", "club", "hamm", "scep"}),
    ("Polearms", {"pole"}),
    ("Bows, Crossbows", {"bow", "abow", "xbow"}),
    ("Staves, Wands, Orbs, Claws", {"staf", "wand", "orb", "grim", "h2h", "h2h2"}),
]


def rows(name):
    with open(EXCEL / name, encoding="latin-1") as f:
        return [r for r in csv.DictReader(f, delimiter="\t") if r.get("*ID", "").strip().isdigit()]


def bases():
    out = {}
    for name in ("armor.txt", "weapons.txt", "misc.txt"):
        with open(EXCEL / name, encoding="latin-1") as f:
            for r in csv.DictReader(f, delimiter="\t"):
                if r.get("code"):
                    out[r["code"]] = (int(r["invwidth"] or 1), int(r["invheight"] or 1), r["type"])
    return out


class Pager:
    """First-fit packing into W x H pages, reading order."""

    def __init__(self):
        self.pages = []            # list of (title, used[H][W])

    def new_page(self, title):
        self.pages.append((title, [[False] * W for _ in range(H)]))

    def place(self, w, h, first_page):
        for p in range(first_page, len(self.pages)):
            used = self.pages[p][1]
            for y in range(H - h + 1):
                for x in range(W - w + 1):
                    if all(not used[y + j][x + i] for j in range(h) for i in range(w)):
                        for j in range(h):
                            for i in range(w):
                                used[y + j][x + i] = True
                        return p, x, y
        return None


def build():
    names = {e["Key"]: e["enUS"] for e in json.load(open(NAMES, encoding="utf-8-sig"))}
    base = bases()
    slots = []                     # (kind, row, page, x, y, w, h, name, key)
    pager = Pager()

    def put(kind, row, code, label, first_page):
        w, h, _ = base[code]
        spot = pager.place(w, h, first_page)
        if spot is None:
            pager.new_page(pager.pages[-1][0])
            spot = pager.place(w, h, len(pager.pages) - 1)
        p, x, y = spot
        slots.append((kind, row, p, x, y, w, h, names.get(label, label), label))

    uniques = [r for r in rows("UniqueItems.txt") if r["spawnable"] == "1" and r["disabled"] != "1"]
    placed = set()
    for title, types in UNIQUE_SECTIONS:
        group = [r for r in uniques if base[r["code"]][2] in types]
        # Tallest first packs into tidy rows; table order (normal, exceptional, elite) breaks ties.
        group.sort(key=lambda r: (-base[r["code"]][1], -base[r["code"]][0]))
        if not group:
            continue
        pager.new_page("Uniques: " + title)
        start = len(pager.pages) - 1
        for r in group:
            put(0, int(r["*ID"]), r["code"], r["index"], start)
            placed.add(r["*ID"])
    left = [r for r in uniques if r["*ID"] not in placed]
    if left:
        print("unsorted uniques:", [(r["index"], base[r["code"]][2]) for r in left], file=sys.stderr)
        pager.new_page("Uniques: Other")
        for r in left:
            put(0, int(r["*ID"]), r["code"], r["index"], len(pager.pages) - 1)
    unique_pages = len(pager.pages)

    # Sets: each set's pieces together on one page, in table order. A set that doesn't
    # fit in what's left of the current page starts the next one.
    sets = [r for r in rows("SetItems.txt") if r["spawnable"] == "1" and r["disabled"] != "1"]
    for r in sets:
        if r["item"] not in base:
            print("set item with unknown base:", r["index"], r["item"], file=sys.stderr)
    sets = [r for r in sets if r["item"] in base]
    groups = {}
    for r in sets:
        groups.setdefault(r["set"], []).append(r)
    pager.new_page("Sets")
    for pieces in groups.values():
        page = len(pager.pages) - 1
        trial = Pager()
        trial.pages = [(pager.pages[page][0], [row[:] for row in pager.pages[page][1]])]
        if any(trial.place(*base[r["item"]][:2], 0) is None for r in pieces):
            pager.new_page("Sets")
            page += 1
        for r in pieces:
            put(1, int(r["*ID"]), r["item"], r["index"], page)
    return slots, pager, unique_pages


SLOTS, PAGER, UNIQUE_PAGES = build()
PAGES = PAGER.pages

def check_lock():
    """Every slot in the frozen layout must still be where it was."""
    if not LOCK.exists():
        return
    now = {(k, r): (p, x, y) for k, r, p, x, y, *_ in SLOTS}
    moved = []
    for line in LOCK.read_text().splitlines()[1:]:
        k, r, p, x, y, name = line.split("\t")
        if now.get((int(k), int(r))) != (int(p), int(x), int(y)):
            moved.append(f"{name}: was page {int(p) + 1} ({x},{y}), now {now.get((int(k), int(r)))}")
    if moved:
        sys.exit("layout_v1.tsv: these slots would move and strand filed items:\n  " + "\n  ".join(moved))


def write_lock():
    rows = ["kind\trow\tpage\tx\ty\tname"] + [f"{k}\t{r}\t{p}\t{x}\t{y}\t{n}" for k, r, p, x, y, _w, _h, n, _key in SLOTS]
    LOCK.write_text("\n".join(rows) + "\n")


if __name__ == "__main__":
    if "--write-lock" in sys.argv:
        write_lock()
        sys.exit(f"wrote {LOCK.name}")
    check_lock()
    out = ["// Generated by gen_layout.py from the game's Excel tables. Do not edit.", "#pragma once", "#include <cstdint>", "",
           f"inline constexpr int GridWidth = {W}, GridHeight = {H};",
           f"inline constexpr int PageCount = {len(PAGES)}, UniquePageCount = {UNIQUE_PAGES};", "",
           "// kind 0 unique, 1 set. name is the English display name, key the table's index (internal name).",
           "struct Slot { uint8_t kind; uint16_t row; uint8_t page, x, y, w, h; const char* name; const char* key; };",
           "inline constexpr Slot Slots[] = {"]
    for kind, row, p, x, y, w, h, name, key in SLOTS:
        out.append(f"\t{{ {kind}, {row}, {p}, {x}, {y}, {w}, {h}, {json.dumps(name)}, {json.dumps(key)} }},")
    out += ["};", "", "// Inventory size of every base item, for outlining items that don't belong.",
            "struct BaseSize { const char code[5]; uint8_t w, h; };", "inline constexpr BaseSize BaseSizes[] = {"]
    for code, (w, h, _type) in sorted(bases().items()):
        out.append(f"\t{{ {json.dumps(code)}, {w}, {h} }},")
    out += ["};", "", "inline constexpr const char* PageTitles[] = {"]
    for title, _ in PAGES:
        out.append(f"\t{json.dumps(title)},")
    out.append("};")
    OUT.write_text("\n".join(out) + "\n")
    used = sum(s[5] * s[6] for s in SLOTS)
    print(f"{len(SLOTS)} slots ({sum(1 for s in SLOTS if s[0] == 0)} unique, {sum(1 for s in SLOTS if s[0] == 1)} set), "
          f"{len(PAGES)} pages ({UNIQUE_PAGES} unique), fill {used / (len(PAGES) * W * H):.0%}")
    for i, (title, used_grid) in enumerate(PAGES):
        print(f"{i + 1:2} {title:42} {sum(map(sum, used_grid)):3}/100")
