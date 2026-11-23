// FAT32 (fs/fat.cpp): format a block device, mount it into the VFS. Read/write, long file names.
#pragma once

#include <neko/types.hpp>

namespace neko::fat {

// Writes an empty FAT32 file system on block device `device` (a name such as "vda").
// Needs at least 65525 clusters (≥ 33 MiB with 512-byte clusters). 0 or an Error.
int64_t format(const char* device, const char* label);
inline int64_t format(const uint8_t* device, const uint8_t* label) {
    return format(reinterpret_cast<const char*>(device), reinterpret_cast<const char*>(label));
}

// Mounts the FAT32 volume on `device` at the empty directory `dir`. 0 or an Error
// (kErrNotSupported: not FAT32).
int64_t mount(const char* device, const char* dir);
inline int64_t mount(const uint8_t* device, const uint8_t* dir) {
    return mount(reinterpret_cast<const char*>(device), reinterpret_cast<const char*>(dir));
}
bool probe(const char* device);  // looks like FAT32?
inline bool probe(const uint8_t* device) { return probe(reinterpret_cast<const char*>(device)); }

}  // namespace neko::fat
