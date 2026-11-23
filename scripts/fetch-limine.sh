#!/usr/bin/env bash
# Downloads prebuilt Limine binaries into third_party/limine-bin (when the distro has no limine package).
set -euo pipefail
cd "$(dirname "$0")/.."
branch=${1:-$(git ls-remote --heads https://codeberg.org/Limine/Limine.git 'v*-binary' \
    | sed 's#.*refs/heads/##' | sort -V | tail -1)}
echo "Limine branch: $branch"
rm -rf third_party/limine-bin
git clone --depth=1 --branch="$branch" https://codeberg.org/Limine/Limine.git third_party/limine-bin
make -C third_party/limine-bin
echo "Done. The Makefile picks up third_party/limine-bin automatically; use LIMINE=third_party/limine-bin/limine."
