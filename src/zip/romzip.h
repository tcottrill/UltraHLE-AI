// ROM images inside .zip archives (uses miniz, copied from the Tempest
// port's platform/windows). The ROM is the first archive entry named
// *.z64 / *.v64 / *.n64 / *.rom / *.bin / *.u64, or else the first file.

#ifndef ROMZIP_H
#define ROMZIP_H

#ifdef __cplusplus
extern "C" {
#endif

// 1 if the file name ends in .zip
int   romzip_iszip(const char *fname);

// Reads the first 'bytes' bytes of the ROM (header, byte-order magic) without
// decompressing the whole image, and the ROM's uncompressed size.
// Returns 0 on success.
int   romzip_peek(const char *zipname,void *buf,int bytes,int *romsize);

// Decompresses the whole ROM into a malloc'd buffer (free() it) and sets
// *romsize. Returns NULL on failure.
void *romzip_load(const char *zipname,int *romsize);

#ifdef __cplusplus
}
#endif

#endif
