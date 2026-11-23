#!/usr/bin/env bash
# Installs the Nekotia toolchain on Arch Linux (run in a normal terminal: sudo asks for a password).
set -euo pipefail

packages=(
    dotnet-sdk          # nekoc (Roslyn-based transpiler), C# IDE projects
    clang lld llvm      # C++ compiler, linker, addr2line/objdump
    make
    limine              # bootloader binaries (/usr/share/limine)
    xorriso             # ISO image
    qemu-desktop        # emulator (qemu-system-x86_64)
    edk2-ovmf           # UEFI firmware for `make run-uefi`
    gdb                 # `make debug`
    imagemagick         # optional: view host-mode framebuffer dumps
)

sudo pacman -S --needed "${packages[@]}"
echo
"$(dirname "$0")/doctor.sh"
