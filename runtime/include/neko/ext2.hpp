// ext2 / ext3 / ext4, read-only (fs/ext2.cpp): mount a Linux partition into the VFS.
#pragma once

#include <neko/types.hpp>

namespace neko::ext2 {

// Mounts the volume on block device `device` (a name such as "vdc") at the empty directory `dir`.
// 0 or an Error (kErrNotSupported: not ext2/3/4, or features it cannot read: compression, inline data…).
int64_t mount(const char* device, const char* dir);
inline int64_t mount(const uint8_t* device, const uint8_t* dir) {
    return mount(reinterpret_cast<const char*>(device), reinterpret_cast<const char*>(dir));
}
bool probe(const char* device);  // an ext2/3/4 superblock?
inline bool probe(const uint8_t* device) { return probe(reinterpret_cast<const char*>(device)); }

}  // namespace neko::ext2
