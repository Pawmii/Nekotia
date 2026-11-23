#!/usr/bin/env bash
# Checks that every tool the build needs is installed.
cd "$(dirname "$0")/.."
ok=1
check() {  # name, command, required?
    if command -v "$2" >/dev/null 2>&1; then
        printf '  \e[32m✓\e[0m %-22s %s\n' "$1" "$(command -v "$2")"
    elif [ "$3" = required ]; then
        printf '  \e[31m✗\e[0m %-22s missing (required)\n' "$1"; ok=0
    else
        printf '  \e[33m-\e[0m %-22s missing (optional)\n' "$1"
    fi
}

echo "Nekotia toolchain:"
check "dotnet"              dotnet              required
check "clang++"             clang++             required
check "ld.lld (or ld)"      ld.lld              optional
check "make"                make                required
check "xorriso"             xorriso             required
check "limine"              limine              required
check "qemu-system-x86_64"  qemu-system-x86_64  required
check "gdb"                 gdb                 optional
check "addr2line"           addr2line           optional
check "code (VS Code)"      code                optional

if command -v dotnet >/dev/null && ! dotnet --list-sdks | grep -q '^10\.'; then
    echo "  ✗ .NET SDK 10 is required (found: $(dotnet --list-sdks | tr '\n' ' '))"; ok=0
fi
limine_dir=$(ls -d /usr/share/limine third_party/limine-bin 2>/dev/null | head -1)
if [ -n "$limine_dir" ] && [ -f "$limine_dir/limine-bios-cd.bin" ]; then
    echo "  ✓ Limine binaries        $limine_dir"
else
    echo "  ✗ Limine binaries not found (install limine or run scripts/fetch-limine.sh)"; ok=0
fi
ovmf=$(ls /usr/share/edk2/x64/OVMF.4m.fd /usr/share/edk2-ovmf/x64/OVMF.fd 2>/dev/null | head -1)
[ -n "$ovmf" ] && echo "  ✓ OVMF (UEFI)            $ovmf" || echo "  - OVMF not found (optional, for make run-uefi)"

echo
[ $ok = 1 ] && echo "All required tools are present. Try: make run" || { echo "Some required tools are missing: scripts/setup-arch.sh"; exit 1; }
