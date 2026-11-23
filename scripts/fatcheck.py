#!/usr/bin/env python3
"""Independent FAT32 checker for disk images (no dosfstools needed).

    scripts/fatcheck.py IMAGE            check, print the tree, exit 1 on any error
    scripts/fatcheck.py IMAGE --cat PATH print one file (path inside the volume, '/'-separated)

Checks: boot sector and its backup, FAT32 layout (>= 65525 clusters), identical FAT copies, every
directory entry (short-name characters, long-name sequence and checksum, '.' and '..'), cluster
chains (valid, no loops, length matching the size, no cluster used twice), lost clusters, and the
FSInfo free-cluster count. Written from the specification, separately from runtime/src/fs/fat.cpp.
"""
import struct
import sys

SHORT_OK = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()-@^_`{}~ ") | set(range(0x80, 0x100))


class Fat32:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.errors = []
        b = self.read(0, 512)
        if b[510:512] != b"\x55\xaa":
            raise SystemExit("no boot signature")
        (self.bps, self.spc, self.reserved, self.nfats, root_entries, total16, _media, fat16) = struct.unpack_from(
            "<HBHBHHBH", b, 11)
        total32, self.fat_sectors = struct.unpack_from("<II", b, 32)
        self.root_cluster, self.fsinfo = struct.unpack_from("<IH", b, 44)
        backup = struct.unpack_from("<H", b, 50)[0]
        if root_entries or fat16 or not self.fat_sectors:
            raise SystemExit("not FAT32")
        self.total = total16 or total32
        self.cb = self.bps * self.spc
        self.fat_offset = self.reserved * self.bps
        self.data_offset = (self.reserved + self.nfats * self.fat_sectors) * self.bps
        self.clusters = (self.total - self.reserved - self.nfats * self.fat_sectors) // self.spc
        if self.clusters < 65525:
            self.err(f"only {self.clusters} clusters: other systems read this as FAT16")
        if backup and self.read(backup * self.bps, 512) != b:
            self.err("backup boot sector differs")
        fats = [self.read(self.fat_offset + i * self.fat_sectors * self.bps, self.fat_sectors * self.bps)
                for i in range(self.nfats)]
        if any(f != fats[0] for f in fats[1:]):
            self.err("FAT copies differ")
        if (self.clusters + 2) * 4 > len(fats[0]):
            self.err(f"the FAT has {len(fats[0]) // 4} entries, {self.clusters + 2} needed for the data area")
            self.clusters = len(fats[0]) // 4 - 2
        self.fat = struct.unpack_from(f"<{self.clusters + 2}I", fats[0])
        self.fat = [x & 0x0FFFFFFF for x in self.fat]
        self.used = {}

    def read(self, offset, n):
        self.f.seek(offset)
        return self.f.read(n)

    def err(self, message):
        self.errors.append(message)

    def chain(self, first, owner):
        out, c = [], first
        while True:
            if not 2 <= c < self.clusters + 2:
                self.err(f"{owner}: bad cluster {c:#x} in chain")
                return out
            if c in self.used:
                self.err(f"{owner}: cluster {c} also belongs to {self.used[c]}")
                return out
            self.used[c] = owner
            out.append(c)
            nxt = self.fat[c]
            if nxt >= 0x0FFFFFF8:
                return out
            if nxt == 0:
                self.err(f"{owner}: chain runs into a free cluster after {c}")
                return out
            c = nxt

    def data(self, chain, size=None):
        raw = b"".join(self.read(self.data_offset + (c - 2) * self.cb, self.cb) for c in chain)
        return raw if size is None else raw[:size]

    def entries(self, raw, where):
        """(name, attr, first cluster, size) for every entry; validates long names."""
        lfn, out = None, []
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = None
                continue
            attr = e[11]
            if attr == 0x0F:
                seq = e[0] & 0x1F
                if e[0] & 0x40:
                    lfn = {"count": seq, "next": seq, "sum": e[13], "parts": {}}
                if lfn is None or seq != lfn["next"] or e[13] != lfn["sum"]:
                    self.err(f"{where}: broken long-name sequence at slot {i // 32}")
                    lfn = None
                    continue
                lfn["parts"][seq] = e[1:11] + e[14:26] + e[28:32]
                lfn["next"] = seq - 1
                continue
            short = e[0:11]
            s = 0
            for ch in short:
                s = (((s & 1) << 7) + (s >> 1) + ch) & 0xFF
            name = None
            if lfn is not None:
                if lfn["next"] != 0 or lfn["sum"] != s:
                    self.err(f"{where}: long name does not match its short entry {short!r}")
                else:
                    units = b"".join(lfn["parts"][k] for k in range(1, lfn["count"] + 1)).decode("utf-16-le")
                    name = units.split("\x00")[0]
                lfn = None
            if not (attr & 0x08) and short[0:1] != b".":
                bad = [ch for ch in short if ch not in SHORT_OK and not (short[0] == 0x05)]
                if bad:
                    self.err(f"{where}: invalid characters in short name {short!r}")
            if name is None:
                base, ext = short[:8].rstrip(b" "), short[8:].rstrip(b" ")
                if e[12] & 0x08:
                    base = base.lower()
                if e[12] & 0x10:
                    ext = ext.lower()
                name = (base + (b"." + ext if ext else b"")).decode("latin-1")
            first = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
            out.append((name, attr, first, struct.unpack_from("<I", e, 28)[0], short))
        return out

    def walk(self, first, path, parent_first, depth=0, listing=None):
        raw = self.data(self.chain(first, path or "/"))
        items = self.entries(raw, path or "/")
        if path:  # '.' and '..' come first
            dots = [x for x in items if x[4][:1] == b"."]
            if len(dots) < 2 or dots[0][4] != b".          " or dots[1][4] != b"..         ":
                self.err(f"{path}: missing '.' / '..'")
            else:
                if dots[0][2] != first:
                    self.err(f"{path}: '.' points to {dots[0][2]}, not {first}")
                want = 0 if parent_first == self.root_cluster else parent_first
                if dots[1][2] != want:
                    self.err(f"{path}: '..' points to {dots[1][2]}, not {want}")
        for name, attr, cl, size, short in items:
            if attr & 0x08 or short[:1] == b".":
                continue
            full = f"{path}/{name}"
            if attr & 0x10:
                listing.append((full + "/", None))
                if depth < 32:
                    self.walk(cl, full, first, depth + 1, listing)
            else:
                listing.append((full, size))
                if size == 0:
                    if cl:
                        self.chain(cl, full)  # allowed, but it must be a valid chain
                    continue
                chain = self.chain(cl, full)
                need = (size + self.cb - 1) // self.cb
                if len(chain) != need:
                    self.err(f"{full}: {len(chain)} clusters for {size} bytes (expected {need})")
        return listing

    def check(self):
        listing = self.walk(self.root_cluster, "", None, listing=[])
        lost = [c for c in range(2, self.clusters + 2) if self.fat[c] != 0 and c not in self.used]
        if lost:
            self.err(f"{len(lost)} lost cluster(s), e.g. {lost[:5]}")
        free = sum(1 for c in range(2, self.clusters + 2) if self.fat[c] == 0)
        info = self.read(self.fsinfo * self.bps, 512)
        if struct.unpack_from("<I", info, 0)[0] == 0x41615252:
            hint = struct.unpack_from("<I", info, 488)[0]
            if hint != 0xFFFFFFFF and hint != free:
                self.err(f"FSInfo says {hint} free clusters, the FAT has {free}")
        return listing, free

    def cat(self, path):
        first = self.root_cluster
        parts = [p for p in path.split("/") if p]
        for i, part in enumerate(parts):
            items = self.entries(self.data(self.chain(first, path)), path)
            match = [x for x in items if x[0] == part]
            if not match:
                raise SystemExit(f"{path}: not found")
            name, attr, cl, size, _ = match[0]
            if i == len(parts) - 1:
                return self.data(self.chain(cl, path), size) if cl else b""
            first = cl
        raise SystemExit("a directory")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    fs = Fat32(sys.argv[1])
    if len(sys.argv) == 4 and sys.argv[2] == "--cat":
        sys.stdout.buffer.write(fs.cat(sys.argv[3]))
        return
    listing, free = fs.check()
    for path, size in listing:
        print(f"  {path:50} {'' if size is None else size}")
    print(f"{fs.clusters} clusters of {fs.cb} B, {free} free, {len(listing)} entries")
    for e in fs.errors:
        print(f"ERROR: {e}")
    print("fatcheck: " + ("ok" if not fs.errors else f"{len(fs.errors)} error(s)"))
    sys.exit(1 if fs.errors else 0)


if __name__ == "__main__":
    main()
