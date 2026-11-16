// Minimal "ustar" tar writer: the fire cam streams all samples as one .tar download.
// Header-only and platform-neutral (tested on a PC against Python's tarfile).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace tar {
constexpr int BLOCK = 512;

// Fills a 512-byte header for a regular file.
inline void header(uint8_t out[BLOCK], const char *name, uint32_t size, uint32_t mtime)
{
    memset(out, 0, BLOCK);
    memcpy(out, name, strlen(name) < 99 ? strlen(name) : 99);                       // name
    snprintf((char *)out + 100, 8, "%07o", 0644);         // mode
    snprintf((char *)out + 108, 8, "%07o", 0);            // uid
    snprintf((char *)out + 116, 8, "%07o", 0);            // gid
    snprintf((char *)out + 124, 12, "%011o", (unsigned)size);
    snprintf((char *)out + 136, 12, "%011o", (unsigned)mtime);
    memset(out + 148, ' ', 8);                            // checksum field counts as spaces
    out[156] = '0';                                       // regular file
    memcpy(out + 257, "ustar", 6);                        // magic + NUL
    memcpy(out + 263, "00", 2);                           // version
    unsigned sum = 0;
    for (int i = 0; i < BLOCK; i++) {
        sum += out[i];
    }
    snprintf((char *)out + 148, 8, "%06o", sum);          // 6 digits, NUL, space
    out[155] = ' ';
}

// Bytes of zero padding after `size` bytes of file data.
inline int padding(uint32_t size)
{
    return (BLOCK - size % BLOCK) % BLOCK;
}
}  // namespace tar
