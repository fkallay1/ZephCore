/* puff_stream.h — streaming DEFLATE decompressor
 * State-machine version of puff.c for use as HPatchLite read_diff backend.
 * No heap allocation; all state in puff_stream_t (~1.8kB).
 * Input:  raw DEFLATE (Python: zlib.compress(data, wbits=-9))
 * Output: puff_stream_read() returns N decompressed bytes on demand.
 */
#pragma once
#include <stdint.h>

#define PS_RING_SIZE   512   /* max back-reference distance (wbits=9) */
#define PS_MAX_BITS     15
#define PS_MAX_LSYM    288   /* lit/len symbols */
#define PS_MAX_DSYM     30   /* distance symbols */

typedef enum {
    PS_BLOCK_HEADER = 0,
    PS_STORED_DATA,
    PS_CODES,
    PS_COPY,
    PS_DONE,
    PS_ERROR
} ps_state_t;

typedef struct {
    short count[PS_MAX_BITS + 1];
    short symbol[PS_MAX_LSYM];
} ps_lhuff_t;

typedef struct {
    short count[PS_MAX_BITS + 1];
    short symbol[PS_MAX_DSYM];
} ps_dhuff_t;

typedef struct puff_stream {
    const uint8_t*  src;
    uint32_t        src_size;
    uint32_t        src_pos;
    uint32_t        bitbuf;
    int             bitcnt;

    ps_state_t      state;
    uint8_t         last_block;

    uint16_t        stored_len;

    ps_lhuff_t      len_tbl;
    ps_dhuff_t      dist_tbl;

    uint16_t        copy_len;
    uint16_t        copy_dist;

    uint8_t         ring[PS_RING_SIZE];
    uint32_t        out_pos;

    int             error;   /* 0=ok/running, PS_ERR_* on failure */
} puff_stream_t;

#define PS_ERR_TRUNCATED   2
#define PS_ERR_INVALID    -1
#define PS_ERR_DIST_TOO_FAR -2

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise from in-memory raw DEFLATE bytes (no zlib/gzip header). */
void puff_stream_init(puff_stream_t* s, const uint8_t* src, uint32_t src_size);

/* Read up to n decompressed bytes into out.
 * Returns bytes written. 0 = done (s->state==PS_DONE) or error (s->error!=0). */
uint32_t puff_stream_read(puff_stream_t* s, uint8_t* out, uint32_t n);

#ifdef __cplusplus
}
#endif
