"""Find where each .txt column lives in the compiled rows of a grail-tables dump.

    python3 tools/decode_dump.py <grail-tab-tables.txt> <excel folder> <Table> <file.txt>

For every numeric column, lists the byte offsets (u8/u16/u32) whose value matches
the column in every row. Text columns are matched as null-terminated strings.

Results for 3.3.94995 (byte offsets; u16 unless noted):
    UniqueItems (348 bytes): index char[32] 0x02, name string id 0x22, base code 0x28 (4 chars),
        flags 0x2c (u8: bit 0 spawnable, bit 3 disableChronicle), rarity 0x34, lvl 0x38, lvl req 0x3a
    SetItems (460 bytes): index 0x02, name string id 0x24, item code 0x28, flags 0x2c,
        set 0x30 (row in Sets), lvl 0x34, lvl req 0x36, rarity 0x38
    Sets (352 bytes): name string id 0x02
    Items (448 bytes): code 0x80, namestr id 0xfc, invwidth 0x11e (u8), invheight 0x11f (u8),
        type 0x12e (row in ItemTypes)
    ItemTypes (232 bytes): code 0x00 (4 chars), Equiv1 0x04, Equiv2 0x06 (rows; 0 = none)
grail_tab.cpp's RowLayout uses the ones grail-tables needs. Fields stored as row numbers
into another table and flag bits were found by comparing against the other table's row
order and testing each bit, not by this script.
"""
import csv
import struct
import sys


def dump_rows(path, table):
    rows, on = [], False
    for line in open(path, encoding='latin-1'):
        if line.startswith('== '):
            on = line.startswith(f'== {table} (RotW)')
            continue
        if on and ' | ' in line:
            rows.append(bytes.fromhex(line.rsplit(' | ', 1)[1].strip()))
    return rows


def txt_rows(path):
    with open(path, encoding='latin-1') as f:
        rows = [r for r in csv.DictReader(f, delimiter='\t')]
    first = next(iter(rows[0]))
    return [r for r in rows if r[first] != 'Expansion']


def main():
    dump, excel, table, txt = sys.argv[1:5]
    rows = dump_rows(dump, table)
    tx = txt_rows(f'{excel}/{txt}')
    n = min(len(rows), len(tx))
    print(f'{table}: {len(rows)} compiled rows, {len(tx)} txt rows, comparing {n}')
    size = len(rows[0])
    for col in tx[0]:
        if not col or col.startswith('*'):
            continue
        vals = [tx[i][col] for i in range(n)]
        if all(v == '' for v in vals):
            continue
        if all(v.lstrip('-').isdigit() or v == '' for v in vals):
            nums = [int(v) if v else 0 for v in vals]
            if len(set(nums)) < 2:
                continue
            hits = []
            for fmt, w in (('<B', 1), ('<H', 2), ('<h', 2), ('<I', 4), ('<i', 4)):
                for off in range(0, size - w + 1):
                    if all(struct.unpack_from(fmt, rows[i], off)[0] == nums[i] for i in range(n)):
                        hits.append(f'{off:#x}{fmt[1:]}')
            print(f'  {col:24} {", ".join(hits[:6]) or "-"}')
        else:
            hits = []
            for off in range(0, size):
                ok = 0
                for i in range(n):
                    v = vals[i].encode('latin-1')
                    if v and rows[i][off:off + len(v)] == v:
                        ok += 1
                if ok >= n * 0.95:
                    hits.append(f'{off:#x}({ok}/{n})')
            print(f'  {col:24} text {", ".join(hits[:4]) or "-"}')


if __name__ == '__main__':
    main()
