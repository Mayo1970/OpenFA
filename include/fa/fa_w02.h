/*
 * fa_w02.h - .W02 "MultiFile / Pool" container (RRR-42)
 *
 * Direct port of RRR-19 (W02_GetFileHeader fcn.0042e452, W02_SelectChunk
 * fcn.0042e2ff, W02_LoadChunk fcn.0042e4ec). Layout, little-endian:
 *
 *   [ 281-byte header (0x119) ]
 *   [ chunk 0 ][ chunk 1 ] ... [ chunk N-1 ]          from 0x119
 *   [ pointer table: N x u32 ]                        the last N*4 bytes
 *
 * chunk = [ u32 data_size ][ u16 kind ][ data_size payload bytes ].
 * pointer table entry[i] = file offset of chunk i's payload (record + 6).
 *
 * The shipped game selects chunk 0 of the MAPPOOL as the playable level
 * (JR_FERRERO.exe 0x4118a9 calls LoadMap with a hard-coded index 0);
 * chunks 1..N-1 in every shipped pool are identical 40x30 editor scratch
 * maps and are never loaded.
 */
#ifndef FA_W02_H
#define FA_W02_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FA_W02_HEADER_SIZE   0x119u
#define FA_W02_FORMAT_ID     0x115u
#define FA_W02_FAMILY_POOL   2u
#define FA_W02_REC_SIZE      6u
#define FA_W02_SIGNATURE     "WESTKA MultiFile/Pool File V 1.0"

typedef struct fa_w02 {
    uint8_t *buf;
    size_t   len;
    int      owns_buf;
    int      count;          /* N */
    uint16_t item_kind;
    int      sig_ok;
} fa_w02;

int  fa_w02_open(fa_w02 *o, const void *data, size_t len, int copy);
int  fa_w02_open_file(fa_w02 *o, const char *path);
void fa_w02_close(fa_w02 *o);

int  fa_w02_count(const fa_w02 *o);

/*
 * Point at chunk `i`'s payload. `i` is clamped to [0, N-1] exactly as
 * W02_SelectChunk does. *ptr is set into the pool buffer (valid until close),
 * *size to the payload byte count, *kind (may be NULL) to the record kind.
 * Returns 0, or -1 on a malformed pool / bad args.
 */
int  fa_w02_chunk(const fa_w02 *o, int i, const uint8_t **ptr, uint32_t *size,
                  uint16_t *kind);

#ifdef __cplusplus
}
#endif

#endif /* FA_W02_H */
