/* puff_stream.c — streaming DEFLATE state machine
 * Based on puff.c by Mark Adler (zlib/libpng license).
 * Adapted for embedded streaming: resumable via puff_stream_read().
 */
//en: Active only in the FOTA FW build (-DWITH_LORA_FOTA) or the standalone flasher
//en: (-DFOTA_FLASHER_BUILD). In stock builds (where the recursive build_src_filter
//en: picks this file up) it stays empty and --gc-sections removes it.
//sk: Aktívne len vo FOTA FW builde (-DWITH_LORA_FOTA) alebo standalone flasheri
//sk: (-DFOTA_FLASHER_BUILD). V stock buildoch (kde to zoberie rekurzívny
//sk: build_src_filter) ostáva prázdne a --gc-sections to odstráni.
#if defined(WITH_LORA_FOTA) || defined(FOTA_FLASHER_BUILD)

#include "puff_stream.h"
#include <string.h>

/* ─── DEFLATE tables (RFC 1951) ─────────────────────────────────────────── */

static const uint16_t lens[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t lext[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t dists[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t dext[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* ─── Bit reader ─────────────────────────────────────────────────────────── */

static uint32_t ps_bits(puff_stream_t* s, int n) {
    if (n == 0) return 0;
    while (s->bitcnt < n) {
        if (s->src_pos >= s->src_size) { s->error = PS_ERR_TRUNCATED; return 0; }
        s->bitbuf |= (uint32_t)s->src[s->src_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    uint32_t val = s->bitbuf & ((1u << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt  -= n;
    return val;
}

static void ps_byte_align(puff_stream_t* s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
}

static uint16_t ps_read_le16(puff_stream_t* s) {
    if (s->src_pos + 1 >= s->src_size) { s->error = PS_ERR_TRUNCATED; return 0; }
    uint16_t v = (uint16_t)s->src[s->src_pos] | ((uint16_t)s->src[s->src_pos+1] << 8);
    s->src_pos += 2;
    return v;
}

/* ─── Ring buffer ────────────────────────────────────────────────────────── */

static inline void ps_ring_put(puff_stream_t* s, uint8_t b) {
    s->ring[s->out_pos & (PS_RING_SIZE - 1)] = b;
    s->out_pos++;
}

static inline uint8_t ps_ring_back(puff_stream_t* s, uint16_t dist) {
    return s->ring[(s->out_pos - dist) & (PS_RING_SIZE - 1)];
}

/* ─── Huffman table construction (from puff.c construct()) ──────────────── */

static int ps_construct_l(ps_lhuff_t* h, const uint8_t* lengths, int n) {
    short offs[PS_MAX_BITS + 1];
    int len, sym, left;
    for (len = 0; len <= PS_MAX_BITS; len++) h->count[len] = 0;
    for (sym = 0; sym < n; sym++) if (lengths[sym] <= PS_MAX_BITS) h->count[lengths[sym]]++;
    if (h->count[0] == n) return 0;
    left = 1;
    for (len = 1; len <= PS_MAX_BITS; len++) {
        left <<= 1; left -= h->count[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (len = 1; len < PS_MAX_BITS; len++) offs[len+1] = offs[len] + h->count[len];
    for (sym = 0; sym < n && sym < PS_MAX_LSYM; sym++)
        if (lengths[sym] != 0) h->symbol[offs[lengths[sym]]++] = (short)sym;
    return left;
}

static int ps_construct_d(ps_dhuff_t* h, const uint8_t* lengths, int n) {
    short offs[PS_MAX_BITS + 1];
    int len, sym, left;
    for (len = 0; len <= PS_MAX_BITS; len++) h->count[len] = 0;
    for (sym = 0; sym < n; sym++) if (lengths[sym] <= PS_MAX_BITS) h->count[lengths[sym]]++;
    if (h->count[0] == n) return 0;
    left = 1;
    for (len = 1; len <= PS_MAX_BITS; len++) {
        left <<= 1; left -= h->count[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (len = 1; len < PS_MAX_BITS; len++) offs[len+1] = offs[len] + h->count[len];
    for (sym = 0; sym < n && sym < PS_MAX_DSYM; sym++)
        if (lengths[sym] != 0) h->symbol[offs[lengths[sym]]++] = (short)sym;
    return left;
}

/* ─── Huffman decode (from puff.c decode()) ─────────────────────────────── */

static int ps_decode_l(puff_stream_t* s, const ps_lhuff_t* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= PS_MAX_BITS; len++) {
        code |= (int)ps_bits(s, 1);
        if (s->error) return -1;
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->error = PS_ERR_INVALID;
    return -1;
}

static int ps_decode_d(puff_stream_t* s, const ps_dhuff_t* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= PS_MAX_BITS; len++) {
        code |= (int)ps_bits(s, 1);
        if (s->error) return -1;
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->error = PS_ERR_INVALID;
    return -1;
}

/* ─── Fixed Huffman tables (DEFLATE spec §3.2.6) ────────────────────────── */

static void ps_build_fixed(puff_stream_t* s) {
    uint8_t lengths[288];
    int i;
    for (i=0;   i<144; i++) lengths[i] = 8;
    for (i=144; i<256; i++) lengths[i] = 9;
    for (i=256; i<280; i++) lengths[i] = 7;
    for (i=280; i<288; i++) lengths[i] = 8;
    if (ps_construct_l(&s->len_tbl, lengths, 288) < 0) { s->error = PS_ERR_INVALID; return; }
    for (i=0; i<30; i++) lengths[i] = 5;
    if (ps_construct_d(&s->dist_tbl, lengths, 30) < 0) { s->error = PS_ERR_INVALID; }
}

/* ─── Dynamic Huffman tables (DEFLATE spec §3.2.7) ──────────────────────── */

static void ps_build_dynamic(puff_stream_t* s) {
    static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int hlit  = (int)ps_bits(s, 5) + 257;
    int hdist = (int)ps_bits(s, 5) + 1;
    int hclen = (int)ps_bits(s, 4) + 4;
    if (s->error) return;

    uint8_t clens[19];
    memset(clens, 0, sizeof(clens));
    for (int i = 0; i < hclen; i++) clens[order[i]] = (uint8_t)ps_bits(s, 3);
    if (s->error) return;

    /* build code-length Huffman table */
    ps_lhuff_t clcode;
    if (ps_construct_l(&clcode, clens, 19) < 0) { s->error = PS_ERR_INVALID; return; }

    /* read hlit + hdist code lengths using clcode */
    uint8_t lengths[PS_MAX_LSYM + PS_MAX_DSYM];
    int total = hlit + hdist;
    int i = 0;
    while (i < total) {
        int sym = ps_decode_l(s, &clcode);
        if (s->error) return;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            int rep = (int)ps_bits(s, 2) + 3;
            if (i == 0) { s->error = PS_ERR_INVALID; return; }
            uint8_t prev = lengths[i-1];
            while (rep-- && i < total) lengths[i++] = prev;
        } else if (sym == 17) {
            int rep = (int)ps_bits(s, 3) + 3;
            while (rep-- && i < total) lengths[i++] = 0;
        } else { /* sym == 18 */
            int rep = (int)ps_bits(s, 7) + 11;
            while (rep-- && i < total) lengths[i++] = 0;
        }
        if (s->error) return;
    }
    if (ps_construct_l(&s->len_tbl,            lengths,       hlit)  < 0) { s->error = PS_ERR_INVALID; return; }
    if (ps_construct_d(&s->dist_tbl, lengths + hlit, hdist) < 0) { s->error = PS_ERR_INVALID; }
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

void puff_stream_init(puff_stream_t* s, const uint8_t* src, uint32_t src_size) {
    memset(s, 0, sizeof(*s));
    s->src      = src;
    s->src_size = src_size;
    s->state    = PS_BLOCK_HEADER;
}

uint32_t puff_stream_read(puff_stream_t* s, uint8_t* out, uint32_t n) {
    uint32_t written = 0;

    while (written < n && s->state != PS_DONE && !s->error) {
        switch (s->state) {

        case PS_BLOCK_HEADER: {
            s->last_block = (uint8_t)ps_bits(s, 1);
            int btype     = (int)ps_bits(s, 2);
            if (s->error) break;
            if (btype == 0) {
                ps_byte_align(s);
                s->stored_len = ps_read_le16(s);
                uint16_t nlen = ps_read_le16(s);
                if ((uint16_t)(~nlen & 0xFFFFu) != s->stored_len) { s->error = PS_ERR_INVALID; break; }
                s->state = PS_STORED_DATA;
            } else if (btype == 1) {
                ps_build_fixed(s);
                if (!s->error) s->state = PS_CODES;
            } else if (btype == 2) {
                ps_build_dynamic(s);
                if (!s->error) s->state = PS_CODES;
            } else {
                s->error = PS_ERR_INVALID;
            }
            break;
        }

        case PS_STORED_DATA: {
            if (s->src_pos >= s->src_size) { s->error = PS_ERR_TRUNCATED; break; }
            uint8_t b = s->src[s->src_pos++];
            ps_ring_put(s, b);
            out[written++] = b;
            if (--s->stored_len == 0)
                s->state = s->last_block ? PS_DONE : PS_BLOCK_HEADER;
            break;
        }

        case PS_CODES: {
            int sym = ps_decode_l(s, &s->len_tbl);
            if (s->error) break;
            if (sym < 256) {
                ps_ring_put(s, (uint8_t)sym);
                out[written++] = (uint8_t)sym;
            } else if (sym == 256) {
                s->state = s->last_block ? PS_DONE : PS_BLOCK_HEADER;
            } else {
                int lidx = sym - 257;
                if (lidx < 0 || lidx >= 29) { s->error = PS_ERR_INVALID; break; }
                s->copy_len  = lens[lidx] + (uint16_t)ps_bits(s, lext[lidx]);
                int dsym = ps_decode_d(s, &s->dist_tbl);
                if (s->error) break;
                if (dsym < 0 || dsym >= 30) { s->error = PS_ERR_INVALID; break; }
                uint32_t dist = dists[dsym] + ps_bits(s, dext[dsym]);
                if (dist > PS_RING_SIZE || dist > s->out_pos) { s->error = PS_ERR_DIST_TOO_FAR; break; }
                s->copy_dist = (uint16_t)dist;
                s->state = PS_COPY;
            }
            break;
        }

        case PS_COPY: {
            uint8_t b = ps_ring_back(s, s->copy_dist);
            ps_ring_put(s, b);
            out[written++] = b;
            if (--s->copy_len == 0) s->state = PS_CODES;
            break;
        }

        default:
            s->error = PS_ERR_INVALID;
            break;
        }
    }
    return written;
}

#endif  /* WITH_LORA_FOTA || FOTA_FLASHER_BUILD */
