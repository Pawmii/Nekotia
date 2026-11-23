# Nekotia build: C# kernel → (nekoc) → C++ → (clang) → ELF → (Limine + xorriso) → ISO → QEMU
#
#   make            kernel ELF (build/nekotia.elf)
#   make run-host   run the kernel as a Linux process (no QEMU needed)
#   make iso        bootable hybrid BIOS/UEFI ISO (needs limine + xorriso)
#   make run        boot the ISO in QEMU (BIOS), serial on this terminal
#   make run-uefi   same with OVMF firmware
#   make debug      QEMU paused with a gdb stub on :1234
#   make test       boot in QEMU (BIOS + UEFI), run the self tests, fail on any failure
#   make user       user programs (user/NAME → build/user/bin/NAME → /boot/bin/NAME on the ISO)
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
QEMU_FLAGS ?= -M q35 -cpu qemu64,+smep,+smap -m 256M -smp 4 -serial stdio -no-reboot -no-shutdown
# make debug builds an -O0 kernel in build/debug for faithful C# line stepping in gdb
HEAP_SIZE  ?= 4194304  # initial kernel heap; it grows on demand

# ---------------------------------------------------------------- sources

CS_SOURCES  := $(shell find sdk kernel -type f \( -name '*.cs' -o -name '*.neko' \) -not -path '*/bin/*' -not -path '*/obj/*')
NEKOC_SRCS  := $(shell find tools/nekoc -type f \( -name '*.cs' -o -name '*.csproj' \) -not -path '*/bin/*' -not -path '*/obj/*')
RT_HEADERS  := $(wildcard runtime/include/neko/*.hpp)
CORE_SRCS   := $(wildcard runtime/src/core/*.cpp)
KERNEL_SRCS := $(CORE_SRCS) $(wildcard runtime/src/freestanding/*.cpp) $(wildcard runtime/src/arch/x86_64/*.cpp)
KERNEL_ASM  := $(wildcard runtime/src/arch/x86_64/*.S) $(wildcard runtime/src/core/*.S)
HOST_SRCS   := $(CORE_SRCS) $(wildcard runtime/src/host/*.cpp)
CORE_ASM    := $(wildcard runtime/src/core/*.S)
FONT        := runtime/assets/fonts/ter-v16n.psf
GEN_CPP     := $(GEN)/nekotia.gen.cpp

# ---------------------------------------------------------------- flags

OPT        ?= -O2

COMMON_FLAGS := -std=c++20 $(OPT) -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
                -Wno-unused-but-set-variable -Wno-parentheses-equality -fwrapv \
                -Iruntime/include -I$(GEN) -MMD -MP

KERNEL_FLAGS := $(COMMON_FLAGS) --target=x86_64-unknown-none-elf -ffreestanding -fno-exceptions -fno-rtti \
                -fno-stack-protector -fno-stack-check -fno-pic -fno-pie -fno-threadsafe-statics \
                -fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone -mcmodel=kernel \
                -mno-mmx -mno-avx -fno-math-errno \
                -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fno-optimize-sibling-calls \
                -fno-delete-null-pointer-checks \
                -Iruntime/third_party/limine -DNEKO_HOSTED=0 -DNEKO_HEAP_SIZE=$(HEAP_SIZE)
KERNEL_LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 --gc-sections -T runtime/src/arch/x86_64/linker.ld

HOST_SAN   ?= -fsanitize=address,undefined -fno-omit-frame-pointer
HOST_FLAGS := $(COMMON_FLAGS) -O1 -DNEKO_HOSTED=1 $(HOST_SAN)

KERNEL_OBJS := $(patsubst %,$(BUILD)/kernel/%.o,$(KERNEL_SRCS) $(KERNEL_ASM)) $(BUILD)/kernel/gen.o
HOST_OBJS   := $(patsubst %,$(BUILD)/host/%.o,$(HOST_SRCS) $(CORE_ASM)) $(BUILD)/host/gen.o

.PHONY: all kernel gen check host run-host iso run run-uefi debug test doctor tools clean vscode-ext user

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
	@for p in $(USER_PROGRAMS); do $(NEKOC) check --config user/$$p/nekotia.json || exit 1; done

# ---------------------------------------------------------------- kernel ELF

kernel: $(BUILD)/nekotia.elf

# Two link passes: the second one adds the symbol/line table used by the stop screen.
$(BUILD)/nekotia.elf: $(KERNEL_OBJS) runtime/src/arch/x86_64/linker.ld scripts/gen-symbols.py
	$(LD) $(KERNEL_LDFLAGS) $(KERNEL_OBJS) -o $(BUILD)/nekotia.stage1.elf
	python3 -I scripts/gen-symbols.py $(BUILD)/nekotia.stage1.elf $(BUILD)/kernel/symbols.S
	$(CXX) --target=x86_64-unknown-none-elf -c $(BUILD)/kernel/symbols.S -o $(BUILD)/kernel/symbols.o
	$(LD) $(KERNEL_LDFLAGS) $(KERNEL_OBJS) $(BUILD)/kernel/symbols.o -o $@
	python3 -I scripts/gen-symbols.py --verify $(BUILD)/nekotia.stage1.elf $@
	@echo "kernel: $@ ($$(du -h $@ | cut -f1))"

$(BUILD)/kernel/%.cpp.o: %.cpp $(GEN_CPP)
	@mkdir -p $(dir $@)
	$(CXX) $(KERNEL_FLAGS) -c $< -o $@

$(BUILD)/kernel/%.S.o: %.S $(FONT)
	@mkdir -p $(dir $@)
	$(CXX) --target=x86_64-unknown-none-elf -c $< -o $@

$(BUILD)/kernel/gen.o: $(GEN_CPP)
	@mkdir -p $(dir $@)
	$(CXX) $(KERNEL_FLAGS) -c $< -o $@

# ---------------------------------------------------------------- user programs
#
# Every directory user/NAME with a nekotia.json ("target": "user") is a program: nekoc turns its C# into
# build/user/NAME/gen/*.cpp, which is linked with the user runtime (runtime/user/) into a static ELF at
# 0x400000. The ISO carries them in /boot/bin and limine.conf hands them to the kernel as modules.

USER_PROGRAMS := $(sort $(patsubst user/%/nekotia.json,%,$(wildcard user/*/nekotia.json)))
USER_SDK      := $(shell find sdk/Nekotia.User -name '*.cs') $(wildcard sdk/Nekotia.Sdk/*.cs)
USER_RT_SRCS  := runtime/user/src/user.cpp runtime/src/core/math.cpp runtime/src/freestanding/libc.cpp runtime/user/src/crt0.S
USER_FLAGS := -std=c++20 $(OPT) -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
              -Wno-unused-but-set-variable -Wno-parentheses-equality -fwrapv -MMD -MP \
              -Iruntime/user/include -Iruntime/include --target=x86_64-unknown-none-elf -ffreestanding \
              -fno-exceptions -fno-rtti -fno-stack-protector -fno-stack-check -fno-pic -fno-pie \
              -fno-threadsafe-statics -fno-asynchronous-unwind-tables -fno-unwind-tables -mno-mmx -mno-avx \
              -fno-math-errno -fno-omit-frame-pointer -fno-delete-null-pointer-checks -DNEKO_HOSTED=0
USER_LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 --gc-sections -T runtime/user/linker.ld
USER_RT_OBJS := $(patsubst %,$(BUILD)/user/rt/%.o,$(USER_RT_SRCS))
USER_BINS    := $(patsubst %,$(BUILD)/user/bin/%,$(USER_PROGRAMS))

user: $(USER_BINS)

$(BUILD)/user/rt/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(USER_FLAGS) -c $< -o $@

$(BUILD)/user/rt/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CXX) --target=x86_64-unknown-none-elf -c $< -o $@

# $(call user-program,NAME)
define user-program
build/user/$(1)/gen/nekotia.gen.cpp: $(NEKOC_DLL) $(wildcard user/$(1)/*.cs user/$(1)/*.neko) user/$(1)/nekotia.json $(USER_SDK)
	$(NEKOC) build --config user/$(1)/nekotia.json
	@touch $$@

$(BUILD)/user/$(1)/gen.o: build/user/$(1)/gen/nekotia.gen.cpp
	@mkdir -p $$(dir $$@)
	$(CXX) $(USER_FLAGS) -c $$< -o $$@

$(BUILD)/user/bin/$(1): $(BUILD)/user/$(1)/gen.o $(USER_RT_OBJS) runtime/user/linker.ld
	@mkdir -p $$(dir $$@)
	$(LD) $(USER_LDFLAGS) $(USER_RT_OBJS) $(BUILD)/user/$(1)/gen.o -o $$@
endef
$(foreach p,$(USER_PROGRAMS),$(eval $(call user-program,$(p))))

# boot/limine.conf + one module_path line per program (appended to the last entry).
$(BUILD)/limine.conf: boot/limine.conf Makefile $(wildcard user/*/nekotia.json)
	@mkdir -p $(BUILD)
	{ cat boot/limine.conf; for p in $(USER_PROGRAMS); do echo "    module_path: boot():/boot/bin/$$p"; done; } > $@

# ---------------------------------------------------------------- host build

host: $(BUILD)/host/nekotia-host

$(BUILD)/host/nekotia-host: $(HOST_OBJS)
	$(CXX) $(HOST_SAN) $^ -o $@

$(BUILD)/host/%.cpp.o: %.cpp $(GEN_CPP)
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_FLAGS) -c $< -o $@

$(BUILD)/host/%.S.o: %.S $(FONT)
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@

$(BUILD)/host/gen.o: $(GEN_CPP)
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_FLAGS) -c $< -o $@

run-host: $(BUILD)/host/nekotia-host
	NEKO_HOST_FB=$(BUILD)/host/framebuffer.ppm $<

# ---------------------------------------------------------------- ISO + QEMU

iso: $(BUILD)/nekotia.iso

# $(call make-iso,<limine.conf>,<output.iso>,<staging dir>)
define make-iso
	@test -n "$(LIMINE_DIR)" || { echo "Limine not found: install it (scripts/setup-arch.sh) or run scripts/fetch-limine.sh"; exit 1; }
	rm -rf $(3)
	mkdir -p $(3)/boot/limine $(3)/EFI/BOOT
	cp $(BUILD)/nekotia.elf $(3)/boot/
	mkdir -p $(3)/boot/bin
	$(if $(USER_BINS),cp $(USER_BINS) $(3)/boot/bin/)
	cp $(1) $(3)/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(LIMINE_DIR)/limine-uefi-cd.bin $(3)/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(3)/EFI/BOOT/
	$(XORRISO) -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 \
		-boot-info-table -hfsplus -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label $(3) -o $(2) 2>/dev/null
	$(LIMINE) bios-install $(2) 2>/dev/null
	@echo "iso: $(2)"
endef

$(BUILD)/nekotia.iso: $(BUILD)/nekotia.elf $(BUILD)/limine.conf $(USER_BINS)
	$(call make-iso,$(BUILD)/limine.conf,$@,$(BUILD)/iso_root)

# Same kernel, but the command line says `test`: the kernel runs its self tests and leaves QEMU
# through the isa-debug-exit device (exit status 1 = all passed, 3 = failures).
$(BUILD)/nekotia-test.iso: $(BUILD)/nekotia.elf $(BUILD)/limine.conf $(USER_BINS)
	sed 's/^\( *cmdline:\).*/\1 test/' $(BUILD)/limine.conf > $(BUILD)/limine-test.conf
	$(call make-iso,$(BUILD)/limine-test.conf,$@,$(BUILD)/iso_root_test)

TEST_FLAGS ?= -M q35 -cpu qemu64,+smep,+smap -m 256M -smp 4 -display none -serial stdio -no-reboot \
              -device isa-debug-exit,iobase=0xf4,iosize=0x04
TEST_TIMEOUT ?= 60

test: $(BUILD)/nekotia-test.iso
	@echo "== QEMU (BIOS)"; timeout $(TEST_TIMEOUT) $(QEMU) $(TEST_FLAGS) -cdrom $< > $(BUILD)/test-bios.log 2>&1; \
	  status=$$?; sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' $(BUILD)/test-bios.log | grep -aE '^(  (ok|FAIL)|selftest)'; \
	  if [ $$status -ne 1 ]; then echo "test (BIOS): FAILED (status $$status, log: $(BUILD)/test-bios.log)"; exit 1; fi
	@if [ -n "$(OVMF)" ]; then echo "== QEMU (UEFI)"; timeout $(TEST_TIMEOUT) $(QEMU) $(TEST_FLAGS) -bios $(OVMF) -cdrom $< > $(BUILD)/test-uefi.log 2>&1; \
	  status=$$?; sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' $(BUILD)/test-uefi.log | grep -aE '^selftest'; \
	  if [ $$status -ne 1 ]; then echo "test (UEFI): FAILED (status $$status, log: $(BUILD)/test-uefi.log)"; exit 1; fi; fi
	@echo "test: all passed (BIOS$(if $(OVMF), + UEFI))"

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
