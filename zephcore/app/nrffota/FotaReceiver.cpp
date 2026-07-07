// =====================================================================
//en: FotaReceiver.cpp — FOTA receiver (MeshCore port of FK_lora-sniffer)
//
//en: Processes the decrypted FOTA payload, stores chunks into the CustomLFS append-log,
//en: after COMPLETE assembles patch.bin and verifies SHA256. Reboot-resilient (meta+bitmap).
//
//sk: FotaReceiver.cpp — FOTA prijímač (MeshCore port z FK_lora-sniffer)
//
//sk: Spracováva dešifrovaný FOTA payload, ukladá chunky do CustomLFS append-logu,
//sk: po COMPLETE zostaví patch.bin a overí SHA256. Reboot-resilient (meta+bitmap).
// =====================================================================
#ifdef WITH_LORA_FOTA
#include "FotaReceiver.h"
#include "FotaFs.h"
#include "FwId.h"             //en: fw_id_trailer (build#, image_size, sha256)
#include "FotaDebug.h"
#if defined(FOTA_MESHCORE_BUILD)
#include <Arduino.h>
#elif defined(FOTA_ZEPHCORE_BUILD)
#include <stdio.h>       //en: sprintf (CLI replies)
#include <stdlib.h>      //en: malloc/free (patch RAM assembly)
#include <string.h>
#endif
#include "FotaCrypto.h"      //en: SHA256 + Ed25519 platform shim (rweather / PSA+Monocypher)

#if defined(FOTA_MESHCORE_BUILD)
//en: Global FotaFS instance — CustomLFS at 0xD4000 (92kB)
CustomLFS FotaFS(FOTA_FS_FLASH_ADDR, FOTA_FS_FLASH_SIZE, FOTA_FS_BLOCK_SIZE);
#else
//en: Global FotaFS instance — thin wrapper over the shared /lfs mount
FotaFsClass FotaFS;
#endif

static bool verify_header_signature(const uint8_t* sig,
                                    const uint8_t* msg, size_t msg_len,
                                    uint8_t key_id) {
    for (int i = 0; i < s_author_count; i++) {
        if (s_authors[i].id == key_id) {
            return fota_ed25519_verify(sig, s_authors[i].pub_key, msg, msg_len);
        }
    }
    FOTA_DEBUG_PRINTLN("[FOTA] UNKNOWN key_id=0x%X", (unsigned)key_id);
    return false;
}

// =====================================================================
//en: RAM state
// =====================================================================
static FotaState   fota;
static uint16_t   s_bitmap_dirty = 0;

//en: Offset table for the assembly step: byte offset of DATA in recv.log for each chunk.
//en: 4B × 1024 = 4KB BSS, acceptable for the nRF52840 (248KB RAM).
//sk: Offset tabuľka pre assembly krok: byte offset DATA v recv.log pre každý chunk.
//sk: 4B × 1024 = 4KB BSS, acceptable pre nRF52840 (248KB RAM).
static uint32_t s_log_data_offset[FOTA_MAX_CHUNKS];

// =====================================================================
//en: Size of the running FW image — from linker symbols (no ld script change,
//en: no post-build). nrf52_common.ld exports __etext (= LMA of .data, i.e. the
//en: end of .text/.exidx in flash), __data_start__/__data_end__ (VMA of .data
//en: in RAM) and __flash_arduino_start (= ORIGIN(FLASH), the real app base of the
//en: active ld). .data has the same size in RAM as its flash LMA image, so:
//en:   image_end = __etext + (__data_end__ - __data_start__)
//en:   fw_size   = image_end - __flash_arduino_start
//en: Matches firmware.bin from the DFU zip (= old_fw_size from the sender). Verified.
//
//en: We take the base from the linker symbol __flash_arduino_start (NOT the hardcoded
//en: APP_FLASH_START) — the symbol always reflects the real link base of the active
//en: ld script (v6=0x26000, v7=0x27000) and is thus a more robust source of truth
//en: than the macro. The APP_FLASH_START/MAX macros remain for compile-time contexts
//en: (the symbol is not a constant expression there). The values are link-time
//en: constants (relocations) — unknown at compile time, but that does not matter
//en: for runtime arithmetic.
//
//sk: Veľkosť bežiaceho FW image — z linker symbolov (žiadna zmena ld scriptu,
//sk: žiadny post-build). nrf52_common.ld exportuje __etext (= LMA .data, t.j.
//sk: koniec .text/.exidx vo flashi), __data_start__/__data_end__ (VMA .data
//sk: v RAM) a __flash_arduino_start (= ORIGIN(FLASH), reálny app base z aktívneho
//sk: ld). .data má v RAM rovnakú veľkosť ako jej flash LMA-obraz, takže:
//sk:   image_end = __etext + (__data_end__ - __data_start__)
//sk:   fw_size   = image_end - __flash_arduino_start
//sk: Zhoduje sa s firmware.bin z DFU zipu (= old_fw_size od sendera). Overené.
//
//sk: Base berieme z linker symbolu __flash_arduino_start (NIE hardcoded
//sk: APP_FLASH_START) — symbol vždy odráža reálny link base aktívneho ld scriptu
//sk: (v6=0x26000, v7=0x27000) a je tak robustnejší zdroj pravdy než makro.
//sk: APP_FLASH_START/MAX makrá ostávajú pre compile-time kontexty (symbol tam
//sk: nie je konštantný výraz). Hodnoty sú link-time konštanty (relokácie) — pri
//sk: kompilácii neznáme, ale to runtime aritmetike nevadí.
// =====================================================================
#if defined(FOTA_MESHCORE_BUILD)
extern "C" {
    extern char __etext;
    extern char __data_start__;
    extern char __data_end__;
    extern char __flash_arduino_start;   //en: = ORIGIN(FLASH) = app base
}

static inline uint32_t fw_image_size(void) {
    uint32_t data_size = (uint32_t)(uintptr_t)&__data_end__
                       - (uint32_t)(uintptr_t)&__data_start__;
    uint32_t image_end = (uint32_t)(uintptr_t)&__etext + data_size;
    return image_end - (uint32_t)(uintptr_t)&__flash_arduino_start;
}
#elif defined(FOTA_ZEPHCORE_BUILD)
//en: Zephyr: __rom_region_start = app base (code_partition, USE_DT_CODE_PARTITION),
//en: __rom_region_end = end of all ROM content (text+rodata+data-load) = zephyr.bin end.
//sk: Zephyr: __rom_region_start = app base (code_partition, USE_DT_CODE_PARTITION),
//sk: __rom_region_end = koniec ROM obsahu (text+rodata+data-load) = koniec zephyr.bin.
extern "C" {
    extern char __rom_region_start[];
    extern char __rom_region_end[];
}

static inline uint32_t fw_image_size(void) {
    return (uint32_t)((uintptr_t)__rom_region_end - (uintptr_t)__rom_region_start);
}
#endif

//en: Real app base from the linker symbol (= ORIGIN(FLASH) of the active ld script):
//en: v6=0x26000, v7=0x27000. This is the source of truth for the device-side SHA — NOT
//en: the APP_FLASH_START macro, which is wrong with a bad/missing board configuration
//en: (e.g. a v7 board without FOTA_SOFTDEVICE_V7) and the hash would be computed over a
//en: different region.
//en: (The flasher is standalone without linker symbols → the macro stays there, see flash_layout.h.)
//sk: Reálny app base z linker symbolu (= ORIGIN(FLASH) aktívneho ld scriptu):
//sk: v6=0x26000, v7=0x27000. Toto je zdroj pravdy pre device-side SHA — NIE makro
//sk: APP_FLASH_START, ktoré je pri zlej/chýbajúcej board konfigurácii (napr. v7
//sk: board bez FOTA_SOFTDEVICE_V7) nesprávne a hash by sa počítal z inej oblasti.
//sk: (Flasher je standalone bez linker symbolov → tam makro ostáva, viď flash_layout.h.)
static inline uint32_t fw_flash_base(void) {
#if defined(FOTA_MESHCORE_BUILD)
    return (uint32_t)(uintptr_t)&__flash_arduino_start;
#else
    return (uint32_t)(uintptr_t)__rom_region_start;
#endif
}

//en: Exported for FotaPatcher / FotaMesh — single source of truth for the app base and
//en: the size of the running FW (from linker symbols, not from the macro).
//sk: Exportované pre FotaPatcher / FotaMesh — jeden zdroj pravdy pre app base a
//sk: veľkosť bežiaceho FW (z linker symbolov, nie z makra).
uint32_t fota_running_fw_base(void) { return fw_flash_base(); }
uint32_t fota_running_fw_size(void) { return fw_image_size(); }

__attribute__((unused))
static void print_sha_full(const uint8_t* h) {
    for (int i = 0; i < 32; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)h[i]); }
    FOTA_DEBUG_PRINTLN("");
}

//en: "fota id" — print the FW identity and compute the full SHA256 of the running FW.
//en: The running sha256 is computed over [base, +image_size) AS-IS (including the filled
//en: trailer) → it MATCHES old_sha256 in .fotapkg.json (the PC computes that over the same
//en: app image). Trailer.sha256 is a different hash (self-hash with sha[]=0) — reference only.
//sk: "fota id" — vypíš FW identitu a dopočítaj plný SHA256 bežiaceho FW.
//sk: running sha256 sa počíta nad [base, +image_size) AS-IS (vrátane vyplneného
//sk: traileru) → ZHODUJE sa s old_sha256 v .fotapkg.json (to PC počíta nad rovnakým
//sk: app image). Trailer.sha256 je iný hash (self-hash so sha[]=0) — len referencia.
void fota_print_fw_id(char* reply) {
    uint32_t base      = fw_flash_base();
    uint32_t link_size = fw_image_size();
    uint32_t timg      = fw_id_trailer.image_size;
    uint32_t build     = fw_id_trailer.build_number;

    FOTA_DEBUG_PRINTLN("[FOTA] === FW identity ===");
    FOTA_DEBUG_PRINTLN("[FOTA] build #            = %lu", (unsigned long)build);
    FOTA_DEBUG_PRINTLN("[FOTA] app base           = 0x%lX", (unsigned long)base);
    FOTA_DEBUG_PRINTLN("[FOTA] image_size trailer = %lu", (unsigned long)timg);
    FOTA_DEBUG_PRINTLN("[FOTA] image_size linker  = %lu", (unsigned long)link_size);
    if (timg != link_size)
        FOTA_DEBUG_PRINTLN("[FOTA] !! POZOR: trailer != linker veľkosť — zlá board konfig?");
    FOTA_DEBUG_PRINT("[FOTA] trailer sha256     = "); print_sha_full(fw_id_trailer.sha256);

    uint8_t h[32]; memset(h, 0, sizeof(h));
    if (timg && timg <= (APP_FLASH_END - base)) {
        FotaSha256 sha;
        sha.update((const void*)base, timg);
        sha.finalize(h, sizeof(h));
        FOTA_DEBUG_PRINT("[FOTA] running sha256     = "); print_sha_full(h);
        FOTA_DEBUG_PRINTLN("[FOTA] ^ porovnaj s old_sha256 v .fotapkg.json");
    } else {
        FOTA_DEBUG_PRINTLN("[FOTA] running sha256: image_size neplatná");
    }
    if (reply) {
        sprintf(reply, "id b#%lu sz=%lu sha=%02X%02X%02X%02X",
                (unsigned long)build, (unsigned long)timg, h[0], h[1], h[2], h[3]);
    }
}

//en: Cross-check: does the declared old_fw_size match the actually running FW?
//en: Cheap gate before the expensive SHA256 — if the size differs, the base FW is different.
//sk: Cross-check: zodpovedá deklarovaná old_fw_size reálne bežiacemu FW?
//sk: Lacná brána pred drahým SHA256 — ak veľkosť nesedí, base FW je iný.
static bool fota_fw_size_matches(uint32_t fw_size) {
    uint32_t self = fw_image_size();
    if (fw_size == self) return true;
    FOTA_DEBUG_PRINTLN("[FOTA] base FW: old_fw_size %lu != bežiace %lu",
                       (unsigned long)fw_size, (unsigned long)self);
    return false;
}

// =====================================================================
//en: Base FW cache — fast validation without redoing the SHA256 computation
// =====================================================================
static bool fota_base_fw_validated(uint32_t fw_size, const uint8_t* prefix) {
    //en: Cross-check against the running FW (from linker symbols) — reject right away
    //en: without SHA256 if the patch targets a different base.
    //sk: Cross-check oproti bežiacemu FW (z linker symbolov) — odmietni hneď
    //sk: bez SHA256, ak patch cieli na iný base.
    if (!fota_fw_size_matches(fw_size)) return false;
    //en: If the cache is filled and the size matches → compare the prefix
    if (fota.base_fw_size == fw_size) {
        return memcmp(fota.base_fw_sha256, prefix, 4) == 0;
    }
    //en: Size changed or cache empty → recompute
    if (fw_size > (APP_FLASH_END - fw_flash_base())) {
        FOTA_DEBUG_PRINTLN("[FOTA] base FW: fw_size %lu > app okno", (unsigned long)fw_size);
        return false;
    }
    FotaSha256 sha;
    sha.update((const void*)fw_flash_base(), fw_size);
    uint8_t h[32];
    sha.finalize(h, sizeof(h));
    fota.base_fw_size = fw_size;
    memcpy(fota.base_fw_sha256, h, 32);
    FOTA_DEBUG_PRINT("[FOTA] base FW cached: size=%luB sha256=", (unsigned long)fw_size);
    for (int i = 0; i < 4; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)h[i]); }
    FOTA_DEBUG_PRINTLN("...");
    return memcmp(fota.base_fw_sha256, prefix, 4) == 0;
}

//en: Base FW verification via the complete SHA256 (for a HEADER with full old_sha256)
static bool fota_base_fw_check_full(uint32_t fw_size, const uint8_t* sha256_full) {
    if (!fota_fw_size_matches(fw_size)) return false;
    if (fw_size > (APP_FLASH_END - fw_flash_base())) return false;
    FotaSha256 sha;
    sha.update((const void*)fw_flash_base(), fw_size);
    uint8_t h[32];
    sha.finalize(h, sizeof(h));
    fota.base_fw_size = fw_size;
    memcpy(fota.base_fw_sha256, h, 32);
    return memcmp(h, sha256_full, 32) == 0;
}

//en: Base FW gating for META/SIG — they carry the full old_sha256[32] but NOT old_fw_size.
//en: Verify it against the running FW ALWAYS (even when HEADER/SIG arrives BEFORE any chunk):
//en:  - if the cache is already filled (base_fw_size>0, set by a chunk/earlier META) → compare
//en:    cheaply without a new SHA (base_fw_sha256 == SHA of the running FW; base_fw_size is
//en:    always == fw_image_size, because fota_fw_size_matches gates it before the cache write);
//en:  - otherwise compute the SHA over the whole running image (fw_image_size). For a legitimate
//en:    patch old_fw_size == fw_image_size holds (invariant from FwId.h), so it matches.
//en: Without this, HEADER-first / SIG-first used to start a session for a FOREIGN patch.
//sk: Base FW gating pre META/SIG — tie nesú plný old_sha256[32] ale NIE old_fw_size.
//sk: Over ho voči bežiacemu FW VŽDY (aj keď HEADER/SIG príde PRED akýmkoľvek chunkom):
//sk:  - ak už máme cache (base_fw_size>0, naplnené chunkom/skorším META) → porovnaj lacno
//sk:    bez nového SHA (base_fw_sha256 == SHA bežiaceho FW; base_fw_size je vždy ==
//sk:    fw_image_size, lebo fota_fw_size_matches to gat­uje pred cache zápisom);
//sk:  - inak doráta SHA nad celým bežiacim image (fw_image_size). Pre legitímny patch
//sk:    platí old_fw_size == fw_image_size (invariant z FwId.h), takže to sedí.
//sk: Bez tohto sa pri HEADER-first / SIG-first zakladala session pre CUDZÍ patch.
static bool fota_meta_base_ok(const uint8_t* old_sha256_full) {
    if (fota.base_fw_size > 0) {
        return memcmp(fota.base_fw_sha256, old_sha256_full, 32) == 0;
    }
    return fota_base_fw_check_full(fw_image_size(), old_sha256_full);
}

// =====================================================================
//en: Internal helpers
// =====================================================================
static void fota_clear() {
    memset(&fota, 0, sizeof(fota));
    fota.status = FOTA_ST_IDLE;
    s_bitmap_dirty = 0;
}

static void fota_set_error(uint8_t code) {
    fota.status   = FOTA_ST_ERROR;
    fota.err_code = code;
    FOTA_DEBUG_PRINTLN("[FOTA] CHYBA=0x%X", (unsigned)code);
}

//en: Kernighan bit count
static uint16_t bitmap_popcount() {
    uint16_t n = 0, bytes = (fota.total_chunks + 7u) / 8u;
    for (uint16_t i = 0; i < bytes; i++) {
        uint8_t b = fota.bitmap[i];
        while (b) { n++; b &= b - 1u; }
    }
    return n;
}

// =====================================================================
//en: CustomLFS — meta.bin
// =====================================================================
static bool save_meta() {
    FotaMetaPersist mp;
    mp.magic        = FOTA_META_MAGIC;
    mp.status       = fota.status;
    mp.err_code     = fota.err_code;
    mp.total_chunks = fota.total_chunks;
    mp.patch_size   = fota.patch_size;
    memcpy(mp.patch_sha256, fota.patch_sha256, 32);
    memcpy(mp.new_sha256,   fota.new_sha256,   32);
    mp.old_fw_size = fota.old_fw_size;
    memcpy(mp.old_sha256,   fota.old_sha256,   32);
    mp.fota_prot_inf = fota.fota_prot_inf;
    mp.meta_recv    = fota.meta_recv;
    mp.sig_recv     = fota.sig_recv;
    mp.hdr_key_id   = fota.hdr_key_id;
    memcpy(mp.hdr_sig, fota.hdr_sig, 64);
    mp.crc16 = fota_crc16((const uint8_t*)&mp, (uint16_t)(sizeof(mp) - 2u));

    FotaFS.remove(FOTA_FS_META);
    FotaFile f(FotaFS);
    if (!f.open(FOTA_FS_META, FILE_O_WRITE)) {
        FOTA_DEBUG_PRINTLN("[FOTA] meta: zápis zlyhal"); return false;
    }
    bool ok = (f.write((const uint8_t*)&mp, sizeof(mp)) == (int)sizeof(mp));
    f.close();
    return ok;
}

static bool load_meta(FotaMetaPersist* out) {
    FotaFile f(FotaFS);
    if (!f.open(FOTA_FS_META, FILE_O_READ)) return false;
    bool ok = (f.read((uint8_t*)out, sizeof(*out)) == (int)sizeof(*out));
    f.close();
    if (!ok) return false;
    if (out->magic != FOTA_META_MAGIC) return false;
    uint16_t crc = fota_crc16((const uint8_t*)out, (uint16_t)(sizeof(*out) - 2u));
    return crc == out->crc16;
}

// =====================================================================
//en: CustomLFS — bitmap.bin
// =====================================================================
static void save_bitmap() {
    if (fota.total_chunks == 0) return;
    uint16_t nbytes = (fota.total_chunks + 7u) / 8u;
    FotaFS.remove(FOTA_FS_BITMAP);
    FotaFile f(FotaFS);
    if (!f.open(FOTA_FS_BITMAP, FILE_O_WRITE)) return;
    f.write(fota.bitmap, nbytes);
    f.close();
    s_bitmap_dirty = 0;
}

static bool load_bitmap() {
    if (fota.total_chunks == 0) return false;
    uint16_t nbytes = (fota.total_chunks + 7u) / 8u;
    FotaFile f(FotaFS);
    if (!f.open(FOTA_FS_BITMAP, FILE_O_READ)) return false;
    bool ok = (f.read(fota.bitmap, nbytes) == (int)nbytes);
    f.close();
    return ok;
}

// =====================================================================
//en: CustomLFS — recv.log (append log)
//en: Each record: [idx 2B LE][data_len 2B LE][data N]
//sk: Každý záznam: [idx 2B LE][data_len 2B LE][data N]
// =====================================================================
static bool log_append(uint16_t idx, const uint8_t* data, uint16_t data_len) {
    FotaFile f(FotaFS);
    if (!f.open(FOTA_FS_LOG, FILE_O_WRITE)) {
        FOTA_DEBUG_PRINTLN("[FOTA] log: zápis zlyhal"); return false;
    }
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(idx);       hdr[1] = (uint8_t)(idx >> 8);
    hdr[2] = (uint8_t)(data_len);  hdr[3] = (uint8_t)(data_len >> 8);
    f.write(hdr, 4);
    f.write(data, data_len);
    f.close();
    return true;
}

// =====================================================================
//en: Assembly from recv.log — shared helpers
// =====================================================================
//en: Exact length of chunk i (without AES padding): full chunks = FOTA_CHUNK_DATA_MAX,
//en: the last one = the remainder of patch_size.
//sk: Presná dĺžka chunku i (bez AES paddingu): plné chunky = FOTA_CHUNK_DATA_MAX,
//sk: posledný = zvyšok z patch_size.
static uint16_t chunk_exp_len(uint16_t i) {
    if (i < fota.total_chunks - 1u) return FOTA_CHUNK_DATA_MAX;
    uint32_t rem = fota.patch_size - (uint32_t)(fota.total_chunks - 1u) * FOTA_CHUNK_DATA_MAX;
    return (rem > FOTA_CHUNK_DATA_MAX) ? FOTA_CHUNK_DATA_MAX : (uint16_t)rem;
}

//en: Pass 1: fill s_log_data_offset[] from recv.log (the last occurrence of an idx wins).
//sk: Prechod 1: naplň s_log_data_offset[] z recv.log (posledný výskyt idx vyhrá).
static void build_log_offsets(FotaFile& log_r) {
    memset(s_log_data_offset, 0xFF, sizeof(s_log_data_offset));
    uint32_t log_pos = 0;
    uint8_t  hdr[4];
    log_r.seek(0);
    while (log_r.read(hdr, 4) == 4) {
        uint16_t idx      = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
        uint16_t data_len = (uint16_t)(hdr[2] | ((uint16_t)hdr[3] << 8));
        if (idx < fota.total_chunks) s_log_data_offset[idx] = log_pos + 4;
        log_pos += 4u + data_len;
        log_r.seek(log_pos);
    }
}

//en: Assembles the patch from recv.log directly into RAM (buf, capacity cap).
//en: Returns the assembled size (== fota.patch_size) or 0 on error/missing chunk.
//sk: Zostaví patch z recv.log priamo do RAM (buf, kapacita cap).
//sk: Vráti zostavenú veľkosť (== fota.patch_size) alebo 0 pri chybe/chýbajúcom chunku.
static uint32_t assemble_log_to_buf(uint8_t* buf, uint32_t cap) {
    if (fota.total_chunks == 0 || fota.patch_size == 0 || fota.patch_size > cap) return 0;
    FotaFile log_r(FotaFS);
    if (!log_r.open(FOTA_FS_LOG, FILE_O_READ)) return 0;
    build_log_offsets(log_r);
    uint32_t out_pos = 0;
    for (uint16_t i = 0; i < fota.total_chunks; i++) {
        if (s_log_data_offset[i] == 0xFFFFFFFFu) { log_r.close(); return 0; }
        uint16_t exp_len = chunk_exp_len(i);
        if (out_pos + exp_len > cap) { log_r.close(); return 0; }
        log_r.seek(s_log_data_offset[i]);
        if (log_r.read(buf + out_pos, exp_len) != (int)exp_len) { log_r.close(); return 0; }
        out_pos += exp_len;
    }
    log_r.close();
    return out_pos;
}

#ifndef USE_PATCHBIN_FILE
//en: RAM mode: patch SHA256 streamed from recv.log (no patch.bin, no large buffer).
//sk: RAM mód: SHA256 patchu streamovo z recv.log (bez patch.bin, bez veľkého buffra).
static bool verify_log_sha() {
    FotaFile log_r(FotaFS);
    if (!log_r.open(FOTA_FS_LOG, FILE_O_READ)) {
        FOTA_DEBUG_PRINTLN("[FOTA] log: čítanie zlyhal"); return false;
    }
    build_log_offsets(log_r);
    FotaSha256 sha;
    uint8_t buf[FOTA_CHUNK_DATA_MAX];
    for (uint16_t i = 0; i < fota.total_chunks; i++) {
        if (s_log_data_offset[i] == 0xFFFFFFFFu) {
            FOTA_DEBUG_PRINTLN("[FOTA] chýba chunk %u", (unsigned)i);
            log_r.close(); return false;
        }
        uint16_t exp_len = chunk_exp_len(i);
        log_r.seek(s_log_data_offset[i]);
        if (log_r.read(buf, exp_len) != (int)exp_len) { log_r.close(); return false; }
        sha.update(buf, exp_len);
    }
    log_r.close();
    uint8_t hash[32];
    sha.finalize(hash, sizeof(hash));
    if (memcmp(hash, fota.patch_sha256, 32) != 0) {
        FOTA_DEBUG_PRINT("[FOTA] SHA256 NESÚHLASÍ  got=");
        for (int i = 0; i < 8; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)hash[i]); }
        FOTA_DEBUG_PRINTLN("...");
        return false;
    }
    return true;
}
#endif

// =====================================================================
//en: Assembly + SHA256 verification (after COMPLETE)
//en:   default:           verify the SHA streamed from recv.log, do NOT write patch.bin (saves FS)
//en:   USE_PATCHBIN_FILE: assemble recv.log → patch.bin (FS) + verify the SHA, delete recv.log
//sk: Assembly + SHA256 verifikácia (po COMPLETE)
//sk:   default:           over SHA streamovo z recv.log, NEpíš patch.bin (úspora FS)
//sk:   USE_PATCHBIN_FILE: zostav recv.log → patch.bin (FS) + over SHA, zmaž recv.log
// =====================================================================
static bool assemble_and_verify() {
#ifndef USE_PATCHBIN_FILE
    FOTA_DEBUG_PRINTLN("[FOTA] Overujem patch SHA256 (RAM, bez patch.bin)...");
    if (!verify_log_sha()) return false;
    FOTA_DEBUG_PRINTLN("[FOTA] patch SHA256 OK (recv.log ostáva ako zdroj)");
    return true;
#else
    FOTA_DEBUG_PRINTLN("[FOTA] Zostavujem patch.bin...");
    FotaFile log_r(FotaFS);
    if (!log_r.open(FOTA_FS_LOG, FILE_O_READ)) {
        FOTA_DEBUG_PRINTLN("[FOTA] log: čítanie zlyhal"); return false;
    }
    build_log_offsets(log_r);

    FotaFS.remove(FOTA_FS_PATCH);
    FotaFile out_f(FotaFS);
    if (!out_f.open(FOTA_FS_PATCH, FILE_O_WRITE)) { log_r.close(); return false; }

    FotaSha256 sha;
    bool   ok = true;
    uint8_t buf[FOTA_CHUNK_DATA_MAX];
    for (uint16_t i = 0; i < fota.total_chunks && ok; i++) {
        if (s_log_data_offset[i] == 0xFFFFFFFFu) {
            FOTA_DEBUG_PRINTLN("[FOTA] chýba chunk %u", (unsigned)i); ok = false; break;
        }
        uint16_t exp_len = chunk_exp_len(i);
        log_r.seek(s_log_data_offset[i]);
        int n = log_r.read(buf, exp_len);
        if (n != (int)exp_len) { ok = false; break; }
        sha.update(buf, (size_t)n);
        out_f.write(buf, (size_t)n);
    }
    log_r.close();
    out_f.close();
    if (!ok) { FOTA_DEBUG_PRINTLN("[FOTA] zostava zlyhala"); return false; }

    uint8_t hash[32];
    sha.finalize(hash, sizeof(hash));
    if (memcmp(hash, fota.patch_sha256, 32) != 0) {
        FOTA_DEBUG_PRINT("[FOTA] SHA256 NESÚHLASÍ  got=");
        for (int i = 0; i < 8; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)hash[i]); }
        FOTA_DEBUG_PRINTLN("...");
        return false;
    }
    FOTA_DEBUG_PRINTLN("[FOTA] patch.bin SHA256 OK");
    FotaFS.remove(FOTA_FS_LOG);   //en: recv.log cleanup — patch.bin is the source from now on
    FOTA_DEBUG_PRINTLN("[FOTA] recv.log zmazaný (patch.bin je zdroj)");
    return true;
#endif
}

// =====================================================================
//en: fota_acquire_patch_ram — patch into a freshly malloc'd RAM buffer.
//en:   default:           assembles from recv.log (no patch.bin, no 2× FS)
//en:   USE_PATCHBIN_FILE: reads /ota/patch.bin
//en: Caller frees via free(). *out_size = size. NULL on error/malloc failure.
//sk: fota_acquire_patch_ram — patch do čerstvo malloc-nutého RAM buffra.
//sk:   default:           zostaví z recv.log (žiadny patch.bin, žiadny 2× FS)
//sk:   USE_PATCHBIN_FILE: prečíta /ota/patch.bin
//sk: Caller uvoľní cez free(). *out_size = veľkosť. NULL pri chybe/malloc zlyhaní.
// =====================================================================
uint8_t* fota_acquire_patch_ram(uint32_t* out_size) {
#ifdef USE_PATCHBIN_FILE
    FotaFile f(FotaFS);
    if (!f.open(FOTA_FS_PATCH, FILE_O_READ)) { FOTA_DEBUG_PRINTLN("[FOTA] patch.bin chýba"); return nullptr; }
    uint32_t sz = (uint32_t)f.size();
    if (sz == 0 || sz > FOTA_FS_FLASH_SIZE) { f.close(); return nullptr; }
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) { f.close(); FOTA_DEBUG_PRINTLN("[FOTA] malloc %lu B zlyhal", (unsigned long)sz); return nullptr; }
    bool ok = ((uint32_t)f.read(buf, sz) == sz);
    f.close();
    if (!ok) { free(buf); FOTA_DEBUG_PRINTLN("[FOTA] čítanie patch.bin zlyhalo"); return nullptr; }
    *out_size = sz;
    return buf;
#else
    uint32_t sz = fota.patch_size;
    if (sz == 0 || sz > FOTA_FS_FLASH_SIZE) { FOTA_DEBUG_PRINTLN("[FOTA] neplatná patch_size"); return nullptr; }
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) {
        FOTA_DEBUG_PRINTLN("[FOTA] malloc %lu B zlyhal (RAM assembly) — pre veľké patche skús -D USE_PATCHBIN_FILE", (unsigned long)sz);
        return nullptr;
    }
    if (assemble_log_to_buf(buf, sz) != sz) {
        free(buf); FOTA_DEBUG_PRINTLN("[FOTA] RAM assembly z recv.log zlyhala"); return nullptr;
    }
    *out_size = sz;
    return buf;
#endif
}

// =====================================================================
//en: Resume after reboot
// =====================================================================
static void try_resume() {
    FotaMetaPersist mp;
    if (!load_meta(&mp)) return;

    if (!(mp.status & (FOTA_ST_RECEIVING | FOTA_ST_COMPLETE | FOTA_ST_VERIFIED | FOTA_ST_DONE))) return;
    if (mp.total_chunks == 0 || mp.total_chunks > FOTA_MAX_CHUNKS) return;

    fota.total_chunks = mp.total_chunks;
    fota.patch_size   = mp.patch_size;
    memcpy(fota.patch_sha256, mp.patch_sha256, 32);
    memcpy(fota.new_sha256,   mp.new_sha256,   32);
    fota.old_fw_size = mp.old_fw_size;
    memcpy(fota.old_sha256,   mp.old_sha256,   32);
    fota.fota_prot_inf = mp.fota_prot_inf;
    fota.meta_recv    = mp.meta_recv;
    fota.sig_recv     = mp.sig_recv;
    fota.hdr_key_id   = mp.hdr_key_id;
    memcpy(fota.hdr_sig, mp.hdr_sig, 64);
    fota.status   = mp.status;
    fota.err_code = mp.err_code;

    load_bitmap();
    fota.recv_count = bitmap_popcount();

    FOTA_DEBUG_PRINTLN("[FOTA] RESUME %u/%u chunks  st=0x%02X", (unsigned)fota.recv_count, (unsigned)fota.total_chunks, (unsigned)fota.status);
}

// =====================================================================
//en: Public API
// =====================================================================
void fota_init() {
    fota_clear();
    if (!FotaFS.begin()) {
        //en: After the flasher, 0xD4000 is overwritten with raw patch data — reformat
        //sk: Po flasheri je 0xD4000 prepísaný raw patch dátami — reformátuj
        FOTA_DEBUG_PRINTLN("[FOTA] FS poškodený (post-flash?), reformátujem...");
        FotaFS.format();
        if (!FotaFS.begin()) {
            FOTA_DEBUG_PRINTLN("[FOTA] FS: format+begin zlyhalo — FS nedostupný");
        }
    }
    FotaFS.mkdir(FOTA_FS_DIR);
    try_resume();
    FOTA_DEBUG_PRINTLN("[FOTA] init  (CustomLFS 92kB @ 0xD4000)");
}

const FotaState* fota_get_state() { return &fota; }

void fota_print_status() {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "[FOTA] %u/%u  st=0x%02X  size=%lu",
                     (unsigned)fota.recv_count, (unsigned)fota.total_chunks,
                     (unsigned)fota.status, (unsigned long)fota.patch_size);
    if (fota.err_code && n > 0 && n < (int)sizeof(buf) - 16) {
        snprintf(buf + n, sizeof(buf) - n, "  err=0x%02X", (unsigned)fota.err_code);
    }
    FOTA_DEBUG_PRINTLN("%s", buf);
}

void fota_send_nack() {
    if (fota.total_chunks == 0) return;
    uint16_t missing[FOTA_NACK_MAX_IDX];
    uint8_t  cnt = 0;
    for (uint16_t i = 0; i < fota.total_chunks && cnt < FOTA_NACK_MAX_IDX; i++)
        if (!FOTA_BIT_GET(fota.bitmap, i))
            missing[cnt++] = i;

    FOTA_DEBUG_PRINT("[FOTA] NACK missing=%u", (unsigned)cnt);
    if (cnt) {
        FOTA_DEBUG_PRINT("  [%u", (unsigned)missing[0]);
        if (cnt > 1) { FOTA_DEBUG_PRINT("..%u", (unsigned)missing[cnt-1]); }
        FOTA_DEBUG_PRINT("]");
    }
    FOTA_DEBUG_PRINTLN("");
}

//en: Total chunk count for DIAGNOSTICS: the promoted (signature-verified) total when
//en: available, otherwise the UNVERIFIED estimate derived from the received META
//en: (ceil(patch_size/FOTA_CHUNK_DATA_MAX)), 0 = no info. The estimate must NEVER be
//en: used for completion/flash gating (that stays behind try_verify_header) — the worst
//en: a forged META can do here is inflate a diagnostic listing.
//sk: Celkový počet chunkov pre DIAGNOSTIKU: promotnutý (podpisom overený) total ak je,
//sk: inak NEOVERENÝ odhad z prijatej META (ceil(patch_size/FOTA_CHUNK_DATA_MAX)),
//sk: 0 = žiadna info. Odhad sa NIKDY nesmie použiť na completion/flash gating (to
//sk: ostáva za try_verify_header) — podvrhnutá META tu nanajvýš nafúkne diagnostický výpis.
uint16_t fota_total_est(void) {
    if (fota.total_chunks > 0) return fota.total_chunks;
    if (fota.meta_recv && fota.patch_size > 0) {
        uint32_t tc = (fota.patch_size + FOTA_CHUNK_DATA_MAX - 1u) / FOTA_CHUNK_DATA_MAX;
        if (tc >= 1 && tc <= FOTA_MAX_CHUNKS) return (uint16_t)tc;
    }
    return 0;
}

//en: Range for counting missing chunks [*lo .. *hi].
//en:  - total known/estimated (fota_total_est>0): [0 .. est-1] — with a received META the
//en:    range covers TRAILING chunks too (miss report is complete already before the SIG).
//en:  - no META: window [0 .. highest received] from the bitmap (trailing chunks above the
//en:    highest received cannot be claimed without any META info).
//en: Returns false = "zero info yet" (no chunk and no META).
//sk: Rozsah na počítanie chýbajúcich chunkov [*lo .. *hi].
//sk:  - total známy/odhadnutý (fota_total_est>0): [0 .. est-1] — s prijatou META pokrýva
//sk:    rozsah aj CHVOSTOVÉ chunky (miss report je kompletný už pred SIG-om).
//sk:  - bez META: okno [0 .. najvyšší prijatý] z bitmapy (chvost nad najvyšším prijatým
//sk:    sa bez META nárokovať nedá).
//sk: Vracia false = "zero info yet" (žiaden chunk a žiadna META).
static bool fota_missing_range(uint16_t* lo, uint16_t* hi) {
    uint16_t est = fota_total_est();
    if (est > 0) { *lo = 0; *hi = (uint16_t)(est - 1u); return true; }
    //en: No META — count holes from chunk 0 up to the HIGHEST received (chunks below
    //en: the lowest received really exist and are missing, hence we count from 0).
    //sk: Bez META — počítaj diery od chunku 0 po NAJVYŠŠÍ prijatý (chunky pod
    //sk: najnižším prijatým reálne existujú a chýbajú, preto počítame od 0).
    int fhi = -1;
    for (uint16_t i = 0; i < FOTA_MAX_CHUNKS; i++)
        if (FOTA_BIT_GET(fota.bitmap, i)) fhi = (int)i;
    if (fhi < 0) return false;   //en: no chunk
    *lo = 0; *hi = (uint16_t)fhi; return true;
}

//en: Computes the missing chunks within the range from fota_missing_range().
//en: Return value: total number of missing ones; -1 = "zero info yet".
//en: out[] (if != NULL) is filled with the first max_out indices, *out_n = how many are there.
//sk: Vypočíta chýbajúce chunky v rozsahu z fota_missing_range().
//sk: Návratová hodnota: celkový počet chýbajúcich; -1 = "zero info yet".
//sk: out[] (ak != NULL) sa naplní prvými max_out indexmi, *out_n = koľko ich tam je.
int fota_calc_missing(uint16_t* out, int max_out, int* out_n) {
    if (out_n) *out_n = 0;
    uint16_t lo, hi;
    if (!fota_missing_range(&lo, &hi)) return -1;
    int n = 0, total = 0;
    for (uint16_t i = lo; ; i++) {
        if (!FOTA_BIT_GET(fota.bitmap, i)) {
            if (out && n < max_out) out[n++] = i;
            total++;
        }
        if (i == hi) break;   //en: safe even for uint16_t (hi can be 0/65535)
    }
    if (out_n) *out_n = n;
    return total;
}

//en: Prints the missing CHUNKS to Serial (no prefix/newline; H/S and the line are the caller's job).
//en: A contiguous run of missing ones is merged into a "from-to" range (e.g. "4-11"), a single one as "5".
//en: 'limit' = cap in TOKENS (a single number = 1 token, a "from-to" range = 2); <=0 = no cap.
//en: A run is NOT truncated — it is printed whole; once tokens are exhausted the rest is summarized
//en: as "+N" (count of the remaining missing CHUNKS). Prints nothing if there is no range.
//sk: Vypíše chýbajúce CHUNKY na Serial (bez prefixu/newline; H/S a riadok rieši volajúci).
//sk: Súvislý beh chýbajúcich sa zlúči do rozsahu "od-do" (napr. "4-11"), jednotlivý ako "5".
//sk: 'limit' = strop v TOKENOCH (jednotlivé číslo = 1 token, rozsah "od-do" = 2); <=0 = bez stropu.
//sk: Beh sa NEoreže — vypíše sa celý; po vyčerpaní tokenov sa zvyšok zhrnie do "+N" (počet
//sk: zvyšných chýbajúcich CHUNKOV). Nič netlačí ak niet rozsahu.
void fota_print_missing(int limit, const char* lead) {
    uint16_t lo, hi;
    const char* sep = lead;   //en: separator before the NEXT token ("," after the first one)
    if (!fota_missing_range(&lo, &hi)) {
        //en: No chunk received and no META → the whole file is the unknown tail.
        //sk: Žiaden prijatý chunk a žiadna META → celý súbor je neznámy chvost.
        if (fota_total_est() == 0) FOTA_DEBUG_PRINT("%s0-??", sep);
        return;
    }
    int total = 0, shown = 0, tokens = 0;
    bool in_run = false; uint16_t rs = 0, re = 0;
    for (uint16_t i = lo; ; i++) {
        bool missing = !FOTA_BIT_GET(fota.bitmap, i);
        if (missing) {
            total++;
            if (!in_run) { rs = re = i; in_run = true; } else re = i;
        }
        if (in_run && (!missing || i == hi)) {     //en: end of run: print it whole (if budget allows)
            if (limit <= 0 || tokens < limit) {
                //en: pair "a,b" (reads better than "a-b"); longer runs as ranges
                //sk: dvojica "a,b" (čitateľnejšie než "a-b"); dlhšie behy ako rozsahy
                if (re == rs)          { FOTA_DEBUG_PRINT("%s%u", sep, (unsigned)rs); tokens += 1; }
                else if (re == rs + 1) { FOTA_DEBUG_PRINT("%s%u,%u", sep, (unsigned)rs, (unsigned)re); tokens += 2; }
                else                   { FOTA_DEBUG_PRINT("%s%u-%u", sep, (unsigned)rs, (unsigned)re); tokens += 2; }
                sep = ",";
                shown += (int)(re - rs + 1);
            }
            in_run = false;
        }
        if (i == hi) break;
    }
    if (limit > 0 && total > shown) { FOTA_DEBUG_PRINT("%s+%d", sep, total - shown); sep = ","; }
    //en: No META → chunks above the highest received are invisible; mark the tail "N-??"
    //en: (N = highest received + 1). N carries the exact tail start for the app — a bare
    //en: "??" would make it guess from the highest MISSING, re-sending whole received
    //en: islands above it. N may not exist (total unknown) — the app drops the marker
    //en: when N is beyond the package total.
    //sk: Bez META sú chunky nad najvyšším prijatým neviditeľné; chvost označ "N-??"
    //sk: (N = najvyšší prijatý + 1). N nesie appke presný začiatok chvosta — holé "??"
    //sk: by hádala od najvyššieho CHÝBAJÚCEHO a preposlala celé prijaté ostrovy nad ním.
    //sk: N nemusí existovať (total nepoznáme) — appka marker zahodí, ak je N za totalom balíka.
    if (fota_total_est() == 0) FOTA_DEBUG_PRINT("%s%u-??", sep, (unsigned)(hi + 1u));
}

//en: Formats the missing CHUNKS into 'out' as ranges with a leading space (" 5", " 4-11").
//en: 'limit' = cap in TOKENS (number = 1, range = 2); <=0 = no cap. A run is NOT truncated.
//en: Once tokens are exhausted OR out fills up, the rest is summarized as " +N" (chunk count).
//en: Returns the number of chars. No large stack buffer — writes directly into 'out' (LoRa reply ~160 B).
//sk: Naformátuje chýbajúce CHUNKY do 'out' ako rozsahy s vedúcou medzerou (" 5", " 4-11").
//sk: 'limit' = strop v TOKENOCH (číslo = 1, rozsah = 2); <=0 = bez stropu. Beh sa NEoreže.
//sk: Po vyčerpaní tokenov ALEBO pri zaplnení out sa zvyšok zhrnie do " +N" (počet chunkov).
//sk: Vracia počet znakov. Bez veľkého stack-bufferu — píše priamo do 'out' (LoRa reply ~160 B).
int fota_format_missing(char* out, int out_sz, int limit, const char* lead) {
    if (out_sz <= 0) return 0;
    out[0] = 0;
    uint16_t lo, hi;
    char* p = out;
    const char* sep = lead;   //en: separator before the NEXT token ("," after the first one)
    if (!fota_missing_range(&lo, &hi)) {
        //en: No chunk received and no META → the whole file is the unknown tail.
        //sk: Žiaden prijatý chunk a žiadna META → celý súbor je neznámy chvost.
        if (fota_total_est() == 0) p += snprintf(p, out_sz, "%s0-??", sep);
        return (int)(p - out);
    }
    char* cap = out + out_sz - 22;                 //en: reserve for ",+NNNNN" + ",NNNNN-??"
    int total = 0, shown = 0, tokens = 0;
    bool full = false;                             //en: buffer full (rest goes into "+N")
    bool in_run = false; uint16_t rs = 0, re = 0;
    for (uint16_t i = lo; ; i++) {
        bool missing = !FOTA_BIT_GET(fota.bitmap, i);
        if (missing) {
            total++;
            if (!in_run) { rs = re = i; in_run = true; } else re = i;
        }
        if (in_run && (!missing || i == hi)) {
            if (!full && (limit <= 0 || tokens < limit)) {
                //en: pair "a,b" (reads better than "a-b"); longer runs as ranges
                //sk: dvojica "a,b" (čitateľnejšie než "a-b"); dlhšie behy ako rozsahy
                int w;
                if (re == rs)          w = snprintf(p, cap - p, "%s%u", sep, (unsigned)rs);
                else if (re == rs + 1) w = snprintf(p, cap - p, "%s%u,%u", sep, (unsigned)rs, (unsigned)re);
                else                   w = snprintf(p, cap - p, "%s%u-%u", sep, (unsigned)rs, (unsigned)re);
                if (w < 0 || p + w >= cap) full = true;     //en: does not fit → rest goes into "+N"
                else { p += w; tokens += (re == rs) ? 1 : 2; sep = ","; shown += (int)(re - rs + 1); }
            }
            in_run = false;
        }
        if (i == hi) break;
    }
    if (total > shown) { p += snprintf(p, out + out_sz - p, "%s+%d", sep, total - shown); sep = ","; }
    //en: No META → chunks above the highest received are invisible; mark the tail "N-??"
    //en: (N = highest received + 1). N carries the exact tail start for the app — a bare
    //en: "??" would make it guess from the highest MISSING, re-sending whole received
    //en: islands above it (e.g. recv 1,3,7-15 → missing "0,2,4-6" → guess would re-send
    //en: 7-15 too). N may not exist (total unknown) — the app drops the marker when N is
    //en: beyond the package total.
    //sk: Bez META sú chunky nad najvyšším prijatým neviditeľné; chvost označ "N-??"
    //sk: (N = najvyšší prijatý + 1). N nesie appke presný začiatok chvosta — holé "??"
    //sk: by hádala od najvyššieho CHÝBAJÚCEHO a preposlala celé prijaté ostrovy nad ním
    //sk: (napr. recv 1,3,7-15 → missing "0,2,4-6" → hádanie by preposlalo aj 7-15).
    //sk: N nemusí existovať (total nepoznáme) — appka marker zahodí, ak je N za totalom balíka.
    if (fota_total_est() == 0) p += snprintf(p, out + out_sz - p, "%s%u-??", sep, (unsigned)(hi + 1u));
    return (int)(p - out);
}

//en: ---- Deferred flash request (the "accepted" ACK must go out BEFORE the reboot) ----
//sk: ---- Odložená žiadosť o flash (ACK „accepted" musí odísť PRED rebootom) ----
static bool s_apply_pending = false;
void fota_request_apply()      { s_apply_pending = true; }
bool fota_apply_pending()      { return s_apply_pending; }
void fota_clear_apply_pending(){ s_apply_pending = false; }

//en: Build a STATUS packet (6B). Always available if there is a session.
int fota_build_status(uint8_t* out) {
    if (fota.total_chunks == 0) return 0;
    FotaStatusPkt* p = (FotaStatusPkt*)out;
    p->type         = FOTA_PKT_STATUS;
    p->recv_count   = fota.recv_count;
    p->total_chunks = fota.total_chunks;
    p->status       = fota.status;
    return (int)sizeof(FotaStatusPkt);
}

//en: Build a NACK packet (2 + count*2). Returns 0 if nothing is missing.
int fota_build_nack(uint8_t* out) {
    if (fota.total_chunks == 0) return 0;
    FotaNackPkt* p = (FotaNackPkt*)out;
    p->type  = FOTA_PKT_NACK;
    p->count = 0;
    for (uint16_t i = 0; i < fota.total_chunks && p->count < FOTA_NACK_MAX_IDX; i++)
        if (!FOTA_BIT_GET(fota.bitmap, i))
            p->idx[p->count++] = i;
    if (p->count == 0) return 0;
    return 2 + (int)p->count * 2;
}

// =====================================================================
//en: FOTA_HEADER processing
// =====================================================================
//en: Reconstructs the 102 B META from the stored fields (MUST be byte-identical to
//en: FotaHeaderPkt and to what the sender signed — otherwise Ed25519 verify fails).
//sk: Zrekonštruuje 102 B META z uložených polí (MUSÍ byť bajt-identické s FotaHeaderPkt
//sk: a s tým, čo podpísal sender — inak Ed25519 verify zlyhá).
static void rebuild_meta(uint8_t out[102]) {
    out[0] = FOTA_PKT_HEADER;
    out[1] = fota.fota_prot_inf;
    memcpy(out + 2,  &fota.patch_size, 4);
    memcpy(out + 6,  fota.patch_sha256, 32);
    memcpy(out + 38, fota.new_sha256, 32);
    memcpy(out + 70, fota.old_sha256, 32);
}

//en: Once we have both META and SIG → verify the signature and "promote" the header (set total_chunks).
//en: Security invariant: total_chunks (and thus completion/flash) is set ONLY after a
//en: successful signature verification over the reconstructed 102 B META.
//sk: Keď máme META aj SIG → over podpis a "promuj" hlavičku (nastav total_chunks).
//sk: Bezpečnostný invariant: total_chunks (a teda completion/flash) sa nastaví LEN po
//sk: úspešnom overení podpisu nad rekonštruovanou 102 B META.
static void try_verify_header() {
    if (!(fota.meta_recv && fota.sig_recv)) return;
    if (fota.total_chunks > 0) return;            //en: already promoted

    uint8_t meta[102];
    rebuild_meta(meta);
    bool ok;
#ifdef FOTA_ALLOW_UNSIGNED
    bool is_unsigned = (fota.hdr_sig[0] == 0 && fota.hdr_sig[1] == 0 &&
                        fota.hdr_sig[2] == 0 && fota.hdr_sig[3] == 0);
    if (is_unsigned) { FOTA_DEBUG_PRINTLN("[FOTA] HEADER: UNSIGNED (FOTA_ALLOW_UNSIGNED)"); ok = true; }
    else
#endif
    ok = verify_header_signature(fota.hdr_sig, meta, 102u, fota.hdr_key_id);

    if (!ok) {
        FOTA_DEBUG_PRINTLN("[FOTA] HEADER: INVALID signature — rejecting");
        fota_set_error(FOTA_ERR_SIGNATURE);
        return;
    }

    uint32_t tc = (fota.patch_size + FOTA_CHUNK_DATA_MAX - 1u) / FOTA_CHUNK_DATA_MAX;
    if (tc == 0 || tc > FOTA_MAX_CHUNKS) {
        FOTA_DEBUG_PRINTLN("[FOTA] HEADER: zlé total_chunks=%lu", (unsigned long)tc); return;
    }
    fota.total_chunks = (uint16_t)tc;
    fota.recv_count   = bitmap_popcount();
    fota.status       = FOTA_ST_RECEIVING;
    save_bitmap();
    save_meta();
    FOTA_DEBUG_PRINTLN("[FOTA] HEADER OK (META+SIG overené) chunks=%lu  mám %u chunkov", (unsigned long)tc, (unsigned)fota.recv_count);

    //en: Chunks may have arrived before the header → check COMPLETE right away
    //sk: Chunky mohli doraziť pred hlavičkou → over COMPLETE hneď
    if (fota.recv_count >= fota.total_chunks) {
        fota.status |= FOTA_ST_COMPLETE;
        save_meta();
        FOTA_DEBUG_PRINTLN("[FOTA] COMPLETE — assembly + SHA256...");
        if (assemble_and_verify()) {
            fota.status |= FOTA_ST_VERIFIED; save_meta();
            FOTA_DEBUG_PRINTLN("[FOTA] VERIFIED — 'fota verify'=dry-run | 'fota flash'=flash+reboot");
        } else {
            fota_set_error(FOTA_ERR_SHA256); save_meta();
        }
    }
}

//en: FOTA_PKT_HEADER = META (patch metadata, signed). Idempotent (receiving it again
//en: just rewrites the same fields). Verify+promotion is done by try_verify_header.
//sk: FOTA_PKT_HEADER = META (metadáta patchu, podpisované). Idempotentné (opätovné
//sk: prijatie len prepíše rovnaké polia). Verify+promócia spraví try_verify_header.
static void handle_meta(const uint8_t* plain, int plen) {
    if (plen < (int)sizeof(FotaHeaderPkt)) { FOTA_DEBUG_PRINTLN("[FOTA] META: krátky"); return; }
    const FotaHeaderPkt* pkt = (const FotaHeaderPkt*)plain;

    //en: Base FW gating — META.old_sha256 MUST match the running FW, otherwise the patch
    //en: does not belong to this device. Applies EVEN when the HEADER arrives before the
    //en: first chunk (then base_fw_size==0 and the SHA is computed over fw_image_size).
    //en: Drop (not ERROR) — a foreign packet must neither kill nor start OUR session.
    //en: Print it (like for a chunk).
    //sk: Base FW gating — META.old_sha256 MUSÍ sedieť s bežiacim FW, inak patch nepatrí
    //sk: tomuto zariadeniu. Platí AJ keď HEADER príde pred prvým chunkom (vtedy
    //sk: base_fw_size==0 a SHA sa doráta nad fw_image_size). Drop (nie ERROR) — cudzí
    //sk: paket nesmie zhodiť ani založiť NAŠU session. Vypíš (ako pri chunku).
    if (!fota_meta_base_ok(pkt->old_sha256)) {
        FOTA_DEBUG_PRINTLN("[FOTA] META: base FW nezhoda — patch nie je pre toto zariadenie, drop");
        return;
    }

    //en: Re-send of an identical META? Evaluate BEFORE a possible fota_clear (it zeroes
    //en: patch_sha256). If it is a DUP we skip save_meta() — a flash write blocks the
    //en: RX path (nRF52 NVMC halt) and causes loss of the following SIG/APPLY packet.
    //sk: Re-send identickej META? Spočítaj PRED prípadným fota_clear (ten zeruje
    //sk: patch_sha256). Ak je to DUP, preskočíme save_meta() — flash-zápis blokuje
    //sk: RX cestu (nRF52 NVMC halt) a spôsobí stratu nasledujúceho SIG/APPLY paketu.
    bool dup_meta = fota.meta_recv && (fota.status & FOTA_ST_RECEIVING)
                 && fota.patch_size == pkt->patch_size
                 && memcmp(fota.patch_sha256, pkt->patch_sha256, 32) == 0;

    bool partial = (fota.status & FOTA_ST_RECEIVING) && fota.total_chunks == 0;
    bool other_patch = (fota.status & FOTA_ST_RECEIVING) && fota.total_chunks > 0 &&
                       memcmp(fota.patch_sha256, pkt->patch_sha256, 32) != 0;
    if (!(fota.status & FOTA_ST_RECEIVING) || other_patch) {
        //en: New session (or a different patch is running) — clean the FS
        FotaFS.remove(FOTA_FS_LOG);
        FotaFS.remove(FOTA_FS_PATCH);
        FotaFS.remove(FOTA_FS_BITMAP);
        fota_clear();
        fota.status = FOTA_ST_RECEIVING;
        fota.total_chunks = 0;
    }
    (void)partial;   //en: partial chunks are kept (merge), nothing is deleted

    fota.fota_prot_inf = pkt->fota_prot_inf;
    fota.patch_size   = pkt->patch_size;
    memcpy(fota.patch_sha256, pkt->patch_sha256, 32);
    memcpy(fota.new_sha256,   pkt->new_sha256,   32);
    memcpy(fota.old_sha256,   pkt->old_sha256,   32);
    fota.meta_recv = 1;
    if (!dup_meta) save_meta();   //en: DUP re-send → no flash write (do not stall RX)
    FOTA_DEBUG_PRINT("[FOTA] META prijaté patch_size=%lu B  patch_sha256=", (unsigned long)pkt->patch_size);
    for (int i = 0; i < 6; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)pkt->patch_sha256[i]); }
    FOTA_DEBUG_PRINTLN("...  %s", dup_meta ? "meta_recv=1 DUP → skip save" : "NEW/CHANGED → save");
    try_verify_header();
}

//en: FOTA_PKT_HDR_SIG = SIG (Ed25519 signature of META). Gating via old_sha256.
static void handle_sig(const uint8_t* plain, int plen) {
    if (plen < (int)sizeof(FotaHdrSigPkt)) { FOTA_DEBUG_PRINTLN("[FOTA] SIG: krátky"); return; }
    const FotaHdrSigPkt* pkt = (const FotaHdrSigPkt*)plain;

    if (fota.meta_recv) {
        if (memcmp(fota.old_sha256, pkt->old_sha256, 32) != 0) {
            FOTA_DEBUG_PRINTLN("[FOTA] SIG: old_sha256 nezhoda s META — drop"); return;
        }
    } else if (!fota_meta_base_ok(pkt->old_sha256)) {
        //en: SIG arrived before META — verify the base FW ALWAYS (even with base_fw_size==0),
        //en: otherwise SIG-first would start a session for a foreign patch.
        //en: (SIG.old_sha256 = "the gating belongs to me".)
        //sk: SIG prišiel pred META — over base FW VŽDY (aj pri base_fw_size==0), inak by
        //sk: SIG-first založil session pre cudzí patch. (SIG.old_sha256 = "gating patrí mne".)
        FOTA_DEBUG_PRINTLN("[FOTA] SIG: base FW nezhoda — patch nie je pre toto zariadenie, drop"); return;
    }
    if (!(fota.status & FOTA_ST_RECEIVING)) { fota.status = FOTA_ST_RECEIVING; fota.total_chunks = 0; }

    //en: Re-send of an identical SIG? DUP → skip save_meta() (same reason as META).
    //sk: Re-send identického SIG? DUP → preskoč save_meta() (rovnaký dôvod ako META).
    bool dup_sig = fota.sig_recv && fota.hdr_key_id == pkt->key_id
                && memcmp(fota.hdr_sig, pkt->signature, 64) == 0;
    fota.hdr_key_id = pkt->key_id;
    memcpy(fota.hdr_sig, pkt->signature, 64);
    fota.sig_recv = 1;
    if (!dup_sig) save_meta();   //en: DUP re-send → no flash write (do not stall RX)
    FOTA_DEBUG_PRINTLN("[FOTA] SIG prijaté key_id=0x%X  %s", (unsigned)pkt->key_id,
        dup_sig ? "sig_recv=1 DUP → skip save" : "NEW/CHANGED → save");
    try_verify_header();
}

// =====================================================================
//en: FOTA_CHUNK processing
// =====================================================================
static void handle_chunk(const uint8_t* plain, int plen) {
    if (plen < 13) return;  //en: min: type(1)+idx(2)+crc(2)+old_fw_size(4)+prefix(4) = 13B

    const FotaChunkPkt* pkt = (const FotaChunkPkt*)plain;
    uint16_t idx = pkt->chunk_idx;
    uint16_t rx_crc = pkt->crc16;
    const uint8_t* data = pkt->data;
    uint16_t data_len = (uint16_t)(plen - (int)(sizeof(FotaChunkPkt) - FOTA_CHUNK_DATA_MAX));

    //en: Base FW validation — verify that the chunk is for the FW currently on the device
    if (!fota_base_fw_validated(pkt->old_fw_size, pkt->old_sha256_prefix)) {
        FOTA_DEBUG_PRINTLN("[FOTA] CHUNK: base FW nezhoda — drop");
        return;
    }

    //en: Session init or merge:
    //en:  - no session → create a partial one (total_chunks=0, base from the chunk);
    //en:    early chunks are buffered right away, the HEADER "promotes" them later.
    //en:  - existing session with a DIFFERENT base FW → ignore (do not mix patches).
    //sk: Session init alebo merge:
    //sk:  - žiadna session → vytvor partial (total_chunks=0, base z chunku);
    //sk:    skoré chunky sa rovno bufferujú, HEADER ich neskôr "promuje".
    //sk:  - existujúca session s INÝM base FW → ignoruj (nemiešaj patche).
    if (!(fota.status & FOTA_ST_RECEIVING)) {
        FotaFS.remove(FOTA_FS_LOG);
        FotaFS.remove(FOTA_FS_PATCH);
        FotaFS.remove(FOTA_FS_BITMAP);
        fota_clear();
        fota.old_fw_size = pkt->old_fw_size;
        memcpy(fota.old_sha256, pkt->old_sha256_prefix, 4);  //en: the rest is filled in by the HEADER
        fota.status = FOTA_ST_RECEIVING;
        fota.total_chunks = 0;  //en: waiting for the HEADER (or promotion)
        save_meta();
        FOTA_DEBUG_PRINTLN("[FOTA] CHUNK: partial session z chunku (čaká HEADER)");
    } else if (memcmp(fota.old_sha256, pkt->old_sha256_prefix, 4) != 0) {
        return;  //en: the chunk belongs to a different base FW than the running session
    }
    //en: The HEADER does not carry old_fw_size — the session takes it from the chunk (the
    //en: base was already verified above via fota_base_fw_validated). Without this it would
    //en: stay 0 on the HEADER path.
    //sk: HEADER nenesie old_fw_size — session ho preberá z chunku (base už overený
    //sk: vyššie cez fota_base_fw_validated). Bez tohto by ostal 0 po HEADER ceste.
    fota.old_fw_size = pkt->old_fw_size;

    //en: idx bound: until total_chunks is known (before the HEADER), buffer up to MAX.
    //sk: Hranica idx: kým nepoznáme total_chunks (pred HEADER), bufferuj až po MAX.
    uint16_t max_idx = (fota.total_chunks > 0) ? fota.total_chunks : (uint16_t)FOTA_MAX_CHUNKS;
    if (idx >= max_idx) {
        if (fota.total_chunks > 0) fota_set_error(FOTA_ERR_OVERFLOW);
        return;
    }

    //en: Exact chunk length — AES-ECB pads the plaintext to a 16B block; the padding must
    //en: be stripped, otherwise the CRC (the sender computes it over the exact length) fails.
    //en: The last chunk's length comes from patch_size, known only after the HEADER — hence
    //en: the last chunk is NOT stored BEFORE the HEADER (its CRC fails on the padding) and
    //en: arrives again in the next cycle.
    //sk: Presná dĺžka chunku — AES-ECB dopĺňa plaintext na 16B blok; padding treba
    //sk: strhnúť, inak CRC (sender ráta cez presnú dĺžku) nesedí. Posledný chunk má
    //sk: dĺžku z patch_size, tú poznáme až po HEADER — preto sa posledný chunk PRED
    //sk: HEADER neuloží (CRC zlyhá na paddingu) a príde znova v ďalšom cykle.
    uint16_t exp_len = FOTA_CHUNK_DATA_MAX;
    if (fota.total_chunks > 0 && idx == (uint16_t)(fota.total_chunks - 1u)) {
        uint32_t rem = fota.patch_size - (uint32_t)(fota.total_chunks - 1u) * FOTA_CHUNK_DATA_MAX;
        exp_len = (rem > FOTA_CHUNK_DATA_MAX) ? FOTA_CHUNK_DATA_MAX : (uint16_t)rem;
    }
    if (data_len > exp_len) data_len = exp_len;

    uint16_t calc_crc = fota_crc16(data, data_len);
    if (calc_crc != rx_crc) {
        FOTA_DEBUG_PRINTLN("[FOTA] CRC ERR idx=%u  exp=0x%04X  got=0x%04X", (unsigned)idx, (unsigned)rx_crc, (unsigned)calc_crc);
        return;
    }

    if (FOTA_BIT_GET(fota.bitmap, idx)) return;  //en: duplicate with OK CRC

    if (!log_append(idx, data, data_len)) {
        fota_set_error(FOTA_ERR_STORAGE); return;
    }

    FOTA_BIT_SET(fota.bitmap, idx);
    fota.recv_count++;

    s_bitmap_dirty++;
    bool complete = (fota.total_chunks > 0) && (fota.recv_count >= fota.total_chunks);
    if (s_bitmap_dirty >= FOTA_BITMAP_SAVE_EVERY || complete)
        save_bitmap();

    if (fota.recv_count % 20 == 0 || complete) {
        FOTA_DEBUG_PRINTLN("[FOTA] %u/%u", (unsigned)fota.recv_count, (unsigned)fota.total_chunks);
    }

    if (complete) {
        fota.status |= FOTA_ST_COMPLETE;
        save_meta();
        FOTA_DEBUG_PRINTLN("[FOTA] COMPLETE — assembly + SHA256...");

        if (assemble_and_verify()) {
            fota.status |= FOTA_ST_VERIFIED;
            save_meta();
            FOTA_DEBUG_PRINTLN("[FOTA] VERIFIED — 'fota verify'=dry-run | 'fota flash'=flash+reboot");
        } else {
            fota_set_error(FOTA_ERR_SHA256);
            save_meta();
        }
    }
}

// =====================================================================
//en: FOTA_APPLY processing
// =====================================================================
static void handle_apply(const uint8_t* plain, int plen) {
    if (plen < (int)sizeof(FotaApplyPkt)) return;
    const FotaApplyPkt* pkt = (const FotaApplyPkt*)plain;
    if (memcmp(pkt->sha256, fota.patch_sha256, 32) != 0) {
        FOTA_DEBUG_PRINTLN("[FOTA] APPLY: SHA256 nesúhlasí"); return;
    }
    fota_apply();
}

// =====================================================================
//en: FOTA packet dump
// =====================================================================
void fota_print_pkt(const uint8_t* plain, int plen, float rssi, float snr) {
    if (plen < 1) return;
    uint8_t type = plain[0];

    switch (type) {
        case FOTA_PKT_HEADER: {   //en: META
            if (plen < (int)sizeof(FotaHeaderPkt)) { FOTA_DEBUG_PRINTLN("[FOTA] META (krátky)"); return; }
            const FotaHeaderPkt* p = (const FotaHeaderPkt*)plain;
            uint32_t ps; memcpy(&ps, &p->patch_size, 4);
            uint32_t tc = (ps + FOTA_CHUNK_DATA_MAX - 1u) / FOTA_CHUNK_DATA_MAX;
            FOTA_DEBUG_PRINT("[FOTA] META  v%u  size=%lu B  chunks~%lu  patch=", (unsigned)p->fota_prot_inf, (unsigned long)ps, (unsigned long)tc);
            for (int i = 0; i < 4; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)p->patch_sha256[i]); }
            FOTA_DEBUG_PRINT("...  new=");
            for (int i = 0; i < 4; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)p->new_sha256[i]); }
            FOTA_DEBUG_PRINTLN("...");
            return;
        }
        case FOTA_PKT_HDR_SIG: {  //en: SIG
            if (plen < (int)sizeof(FotaHdrSigPkt)) { FOTA_DEBUG_PRINTLN("[FOTA] SIG (krátky)"); return; }
            const FotaHdrSigPkt* p = (const FotaHdrSigPkt*)plain;
            FOTA_DEBUG_PRINT("[FOTA] SIG  v%u  key_id=0x%X  sig=", (unsigned)p->fota_prot_inf, (unsigned)p->key_id);
            for (int i = 0; i < 4; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)p->signature[i]); }
            FOTA_DEBUG_PRINT("...  old=");
            for (int i = 0; i < 4; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)p->old_sha256[i]); }
            FOTA_DEBUG_PRINTLN("...");
            return;
        }
        case FOTA_PKT_CHUNK: {
            if (plen < (int)(sizeof(FotaChunkPkt) - FOTA_CHUNK_DATA_MAX)) { FOTA_DEBUG_PRINTLN("[FOTA] CHUNK (krátky)"); return; }
            const FotaChunkPkt* p = (const FotaChunkPkt*)plain;
            uint16_t dlen = (uint16_t)(plen - (int)(sizeof(FotaChunkPkt) - FOTA_CHUNK_DATA_MAX));
            uint16_t calc  = fota_crc16(p->data, dlen);
            bool crc_ok    = (calc == p->crc16);

            FOTA_DEBUG_PRINT("[FOTA] CHUNK  idx=%u", (unsigned)p->chunk_idx);
            if (fota.total_chunks > 0) { FOTA_DEBUG_PRINT("/%u", (unsigned)fota.total_chunks); }
            FOTA_DEBUG_PRINT("  len=%u B  crc=0x%04X %s", (unsigned)dlen, (unsigned)p->crc16, crc_ok ? "OK" : "BAD");
            if (p->chunk_idx < FOTA_MAX_CHUNKS && FOTA_BIT_GET(fota.bitmap, p->chunk_idx)) FOTA_DEBUG_PRINT(" DUP");
            FOTA_DEBUG_PRINT("  base=");
            for (int i = 0; i < 4; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)p->old_sha256_prefix[i]); }
            FOTA_DEBUG_PRINTLN("");
            return;
        }
        case FOTA_PKT_APPLY: {
            if (plen < (int)sizeof(FotaApplyPkt)) { FOTA_DEBUG_PRINTLN("[FOTA] APPLY (krátky)"); return; }
            const FotaApplyPkt* p = (const FotaApplyPkt*)plain;
            bool sha_ok = (memcmp(p->sha256, fota.patch_sha256, 32) == 0);
            FOTA_DEBUG_PRINT("[FOTA] APPLY  sha256=");
            for (int i = 0; i < 8; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)p->sha256[i]); }
            FOTA_DEBUG_PRINTLN("%s", sha_ok ? "...  OK" : "...  NESEDÍ");
            return;
        }
        default:
            FOTA_DEBUG_PRINTLN("[FOTA] ? type=0x%X", (unsigned)type);
            return;
    }
}

// =====================================================================
//en: Dispatch
// =====================================================================
bool fota_process(const uint8_t* plain, int plen) {
    if (plen < 1) return false;
    switch (plain[0]) {
        case FOTA_PKT_HEADER:  handle_meta(plain, plen);  return true;
        case FOTA_PKT_HDR_SIG: handle_sig(plain, plen);   return true;
        case FOTA_PKT_CHUNK:   handle_chunk(plain, plen);  return true;
        case FOTA_PKT_APPLY:   handle_apply(plain, plen);  return true;
        default:             return false;
    }
}

// =====================================================================
//en: fota_apply — starts the real flash (fota_flash_via_flasher, does NOT return on success)
//sk: fota_apply — spustí skutočný flash (fota_flash_via_flasher, NEVRÁTI SA pri úspechu)
// =====================================================================
bool fota_apply() {
    if (!(fota.status & FOTA_ST_VERIFIED)) {
        FOTA_DEBUG_PRINTLN("[FOTA] APPLY: nie je verifikované — spusti príjem chunkov"); return false;
    }
    uint8_t prev_status = fota.status;   //en: to restore if the flash fails (base-check etc.)
    fota.status = FOTA_ST_APPLYING;
    save_meta();

    extern bool fota_flash_via_flasher();
    if (fota_flash_via_flasher()) return true;  //en: does NOT return on success

    //en: The flash failed before the jump (e.g. base FW != old). Restore VERIFIED.
    //sk: Flash zlyhal pred skokom (napr. base FW != old). Obnov VERIFIED.
    fota.status = prev_status;
    save_meta();
    return false;
}

// =====================================================================
//en: fota_clear_session — deletes the FOTA files from the FS, resets the RAM state
// =====================================================================
void fota_clear_session() {
    FOTA_DEBUG_PRINTLN("[FOTA] Mazem FOTA session...");
    int removed = 0;
    const char* files[] = { FOTA_FS_META, FOTA_FS_BITMAP, FOTA_FS_LOG, FOTA_FS_PATCH };
    for (int i = 0; i < 4; i++) {
        if (FotaFS.remove(files[i])) {
            FOTA_DEBUG_PRINTLN("[FOTA] rm %s", files[i]);
            removed++;
        }
    }
    fota_clear();
    FOTA_DEBUG_PRINTLN("[FOTA] Hotovo — vymazaných %d súborov", removed);
}

#endif  // WITH_LORA_FOTA
