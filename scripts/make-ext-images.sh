#!/bin/sh
# Test volumes for the ext2/3/4 driver (make test): the same files on an ext2 image with 1 KiB blocks (block
# map with double-indirect blocks for big.bin) and an ext4 image with 4 KiB blocks (extents).
# usage: make-ext-images.sh OUT_DIR — writes OUT_DIR/test-ext2.img and OUT_DIR/test-ext4.img (16 MiB each).
set -e
out=$1
if ! command -v mke2fs >/dev/null 2>&1; then
    echo "make-ext-images: no mke2fs (e2fsprogs) — the ext2 self tests are skipped"
    rm -f "$out/test-ext2.img" "$out/test-ext4.img"
    truncate -s 1M "$out/test-ext2.img" "$out/test-ext4.img"
    exit 0
fi
src="$out/ext-src"
rm -rf "$src"
mkdir -p "$src/docs/deep/deeper"
printf 'hello from ext2\n' > "$src/hello.txt"
printf 'Привет из ext4\n' > "$src/docs/deep/deeper/note.txt"
python3 -I -c "import sys; open(sys.argv[1], 'wb').write(bytes(((i * 7) + (i >> 12)) & 255 for i in range(300000)))" "$src/big.bin"
rm -f "$out/test-ext2.img" "$out/test-ext4.img"
truncate -s 16M "$out/test-ext2.img" "$out/test-ext4.img"
mke2fs -q -F -t ext2 -b 1024 -d "$src" -L nekoext2 "$out/test-ext2.img"
# A broken volume's worth of trouble: two more names for the root directory (a loop the driver must not follow).
if command -v debugfs >/dev/null 2>&1; then
    debugfs -w -R "ln / loopA" "$out/test-ext2.img" >/dev/null 2>&1
    debugfs -w -R "ln / loopB" "$out/test-ext2.img" >/dev/null 2>&1
fi
mke2fs -q -F -t ext4 -b 4096 -d "$src" -L nekoext4 "$out/test-ext4.img"
