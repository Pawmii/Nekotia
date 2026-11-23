# Nekotia build: C# kernel → (nekoc) → C++ → (clang) → ELF → (Limine + xorriso) → ISO → QEMU
#
#   make            kernel ELF (build/nekotia.elf)
#   make run-host   run the kernel as a Linux process (no QEMU needed)
#   make iso        bootable hybrid BIOS/UEFI ISO (needs limine + xorriso)
#   make run        boot the ISO in QEMU (BIOS), serial on this terminal
#   make run-uefi   same with OVMF firmware
#   make debug      QEMU paused with a gdb stub on :1234
#   make check      nekoc diagnostics only
#   make doctor     check that the toolchain is installed

BUILD      ?= build
# nekoc output and the nekoc binary are shared by all build flavours (see nekotia.json "output")
GEN        := build/gen
NEKOC_DIR  := build/tools/nekoc
DEBUG_BUILD := build/debug
NEKOC_DLL  := $(NEKOC_DIR)/nekoc.dll
NEKOC      := dotnet $(NEKOC_DLL)

CXX        ?= clang++
CXX        := $(if $(filter default,$(origin CXX)),clang++,$(CXX))
LD         := $(shell command -v ld.lld 2>/dev/null || echo ld)
QEMU       ?= qemu-system-x86_64
XORRISO    ?= xorriso
LIMINE     ?= limine
LIMINE_DIR ?= $(firstword $(wildcard /usr/share/limine third_party/limine-bin))
OVMF       ?= $(firstword $(wildcard /usr/share/edk2/x64/OVMF.4m.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/ovmf/x64/OVMF.fd /usr/share/OVMF/OVMF_CODE.fd))
QEMU_FLAGS ?= -M q35 -m 256M -serial stdio -no-reboot -no-shutdown
# make debug builds an -O0 kernel in build/debug for faithful C# line stepping in gdb
HEAP_SIZE  ?= 16777216

# ---------------------------------------------------------------- sources

CS_SOURCES  := $(shell find sdk kernel -type f \( -name '*.cs' -o -name '*.neko' \) -not -path '*/bin/*' -not -path '*/obj/*')
NEKOC_SRCS  := $(shell find tools/nekoc -type f \( -name '*.cs' -o -name '*.csproj' \) -not -path '*/bin/*' -not -path '*/obj/*')
RT_HEADERS  := $(wildcard runtime/include/neko/*.hpp)
CORE_SRCS   := $(wildcard runtime/src/core/*.cpp)
KERNEL_SRCS := $(CORE_SRCS) $(wildcard runtime/src/freestanding/*.cpp) $(wildcard runtime/src/arch/x86_64/*.cpp)
KERNEL_ASM  := $(wildcard runtime/src/arch/x86_64/*.S)
HOST_SRCS   := $(CORE_SRCS) $(wildcard runtime/src/host/*.cpp)
GEN_CPP     := $(GEN)/nekotia.gen.cpp

# ---------------------------------------------------------------- flags

OPT        ?= -O2

COMMON_FLAGS := -std=c++20 $(OPT) -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
                -Wno-unused-but-set-variable -Wno-parentheses-equality -fwrapv \
                -Iruntime/include -I$(GEN) -MMD -MP

KERNEL_FLAGS := $(COMMON_FLAGS) --target=x86_64-unknown-none-elf -ffreestanding -fno-exceptions -fno-rtti \
                -fno-stack-protector -fno-stack-check -fno-pic -fno-pie -fno-threadsafe-statics \
                -fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone -mcmodel=kernel \
                -mgeneral-regs-only -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
                -Iruntime/third_party/limine -DNEKO_HOSTED=0 -DNEKO_HEAP_SIZE=$(HEAP_SIZE)
KERNEL_LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 --gc-sections -T runtime/src/arch/x86_64/linker.ld

HOST_SAN   ?= -fsanitize=address,undefined -fno-omit-frame-pointer
HOST_FLAGS := $(COMMON_FLAGS) -O1 -DNEKO_HOSTED=1 $(HOST_SAN)

KERNEL_OBJS := $(patsubst %,$(BUILD)/kernel/%.o,$(KERNEL_SRCS) $(KERNEL_ASM)) $(BUILD)/kernel/gen.o
HOST_OBJS   := $(patsubst %,$(BUILD)/host/%.o,$(HOST_SRCS)) $(BUILD)/host/gen.o

.PHONY: all kernel gen check host run-host iso run run-uefi debug doctor tools clean vscode-ext

all: kernel

# ---------------------------------------------------------------- nekoc (C# → C++)

tools: $(NEKOC_DLL)

$(NEKOC_DLL): $(NEKOC_SRCS)
	dotnet build tools/nekoc/nekoc.csproj -c Release -o $(NEKOC_DIR) --nologo -v quiet -clp:ErrorsOnly
	@touch $@

gen: $(GEN_CPP)

$(GEN_CPP): $(NEKOC_DLL) $(CS_SOURCES) nekotia.json
	$(NEKOC) build

check: $(NEKOC_DLL)
	$(NEKOC) check

# ---------------------------------------------------------------- kernel ELF

kernel: $(BUILD)/nekotia.elf

$(BUILD)/nekotia.elf: $(KERNEL_OBJS) runtime/src/arch/x86_64/linker.ld
	$(LD) $(KERNEL_LDFLAGS) $(KERNEL_OBJS) -o $@
	@echo "kernel: $@ ($$(du -h $@ | cut -f1))"

$(BUILD)/kernel/%.cpp.o: %.cpp $(GEN_CPP)
	@mkdir -p $(dir $@)
	$(CXX) $(KERNEL_FLAGS) -c $< -o $@

$(BUILD)/kernel/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CXX) --target=x86_64-unknown-none-elf -c $< -o $@

$(BUILD)/kernel/gen.o: $(GEN_CPP)
	@mkdir -p $(dir $@)
	$(CXX) $(KERNEL_FLAGS) -c $< -o $@

# ---------------------------------------------------------------- host build

host: $(BUILD)/host/nekotia-host

$(BUILD)/host/nekotia-host: $(HOST_OBJS)
	$(CXX) $(HOST_SAN) $^ -o $@

$(BUILD)/host/%.cpp.o: %.cpp $(GEN_CPP)
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_FLAGS) -c $< -o $@

$(BUILD)/host/gen.o: $(GEN_CPP)
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_FLAGS) -c $< -o $@

run-host: $(BUILD)/host/nekotia-host
	NEKO_HOST_FB=$(BUILD)/host/framebuffer.ppm $<

# ---------------------------------------------------------------- ISO + QEMU

iso: $(BUILD)/nekotia.iso

$(BUILD)/nekotia.iso: $(BUILD)/nekotia.elf boot/limine.conf
	@test -n "$(LIMINE_DIR)" || { echo "Limine not found: install it (scripts/setup-arch.sh) or run scripts/fetch-limine.sh"; exit 1; }
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(BUILD)/nekotia.elf $(BUILD)/iso_root/boot/
	cp boot/limine.conf $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(LIMINE_DIR)/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(BUILD)/iso_root/EFI/BOOT/
	$(XORRISO) -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 \
		-boot-info-table -hfsplus -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label $(BUILD)/iso_root -o $@ 2>/dev/null
	$(LIMINE) bios-install $@ 2>/dev/null
	@echo "iso: $@"

run: $(BUILD)/nekotia.iso
	$(QEMU) $(QEMU_FLAGS) -cdrom $<

run-uefi: $(BUILD)/nekotia.iso
	@test -n "$(OVMF)" || { echo "OVMF firmware not found (install edk2-ovmf)"; exit 1; }
	$(QEMU) $(QEMU_FLAGS) -bios $(OVMF) -cdrom $<

# Separate -O0 build in build/debug so breakpoints on C# lines map 1:1 to code.
debug:
	@$(MAKE) --no-print-directory BUILD=$(DEBUG_BUILD) OPT=-O0 $(DEBUG_BUILD)/nekotia.iso
	@echo "nekotia: starting QEMU paused, gdb server on :1234"
	@( sleep 1; echo "nekotia: QEMU is waiting for gdb on :1234 (symbols: $(DEBUG_BUILD)/nekotia.elf)" ) &
	$(QEMU) $(QEMU_FLAGS) -cdrom $(DEBUG_BUILD)/nekotia.iso -s -S

# ---------------------------------------------------------------- misc

doctor:
	@scripts/doctor.sh

vscode-ext:
	@python3 -I scripts/package-vscode-extension.py
	@echo "install: code --install-extension build/vscode-nekotia-$$(sed -n 's/.*"version": "\(.*\)".*/\1/p' editor/vscode-nekotia/package.json | head -1).vsix"

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
