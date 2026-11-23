// Boot information handed to the C# [KernelEntry]. Field names match sdk/Nekotia.Sdk/Boot.cs exactly.
#pragma once

#include <neko/types.hpp>

namespace neko {

struct Framebuffer {
    uint32_t* Address;
    uint64_t Width;
    uint64_t Height;
    uint64_t Pitch;
    uint16_t Bpp;
};

struct BootInfo {
    uint64_t HhdmOffset;
    uint64_t TotalMemory;
    uint64_t UsableMemory;
    uint64_t MemoryMapEntries;
    Framebuffer Screen;
    const uint8_t* BootloaderName;
    const uint8_t* BootloaderVersion;
    const uint8_t* CommandLine;
    bool Hosted;
};

}  // namespace neko

// Implemented by nekoc-generated code (build/gen/nekotia.gen.cpp).
extern "C" void neko_kernel_main(neko::BootInfo* info);
