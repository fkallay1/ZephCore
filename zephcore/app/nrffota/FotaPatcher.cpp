// =====================================================================
//en: FotaPatcher.cpp — FOTA patch application (MeshCore port from FK_lora-sniffer)
// =====================================================================
#ifdef WITH_LORA_FOTA
#include "FotaPatcher.h"
#include "FotaFs.h"
#include "FotaState.h"
#include "FotaBuffer.h"   //en: shared scratch (static .bss, not stack)
#include "FotaDebug.h"
#if defined(FOTA_MESHCORE_BUILD)
#include <Arduino.h>
#elif defined(FOTA_ZEPHCORE_BUILD)
#include <zephyr/kernel.h>
#include <cmsis_core.h>      //en: __disable_irq / NVIC_SystemReset / __DSB/__ISB
#include <stdio.h>           //en: sprintf (CLI replies)
#include <stdlib.h>          //en: malloc/free (patch RAM)
#include <string.h>
#endif
#include "FotaCrypto.h"      //en: SHA256 platform shim (rweather / PSA)
#if defined(FOTA_MESHCORE_BUILD)
#include <nrf.h>             //en: NRF_NVMC, NVMC_CONFIG_WEN_*
#elif defined(FOTA_FLASHER_IN_FLASH)
#include <nrfx.h>            //en: NRF_NVMC cez hal_nordic MDK (Zephyr)
#endif

//en: HPatchLite — vendored in nrffota/hpatchlite/ (include path from build_flags)
#if __has_include("hpatch_lite.h")
  #include "hpatch_lite.h"
  #define FOTA_HAS_HPATCH 1
#else
  #define FOTA_HAS_HPATCH 0
#endif

//en: Streaming DEFLATE decompressor for the ZLIB dry-run
#include "puff_stream.h"

//en: ONE board-agnostic flasher blob — the app base is passed at RUNTIME (4th arg of
//en: flasher_entry, see s_app_base in flasher.c), not compile-time. Generate with:
//en:   python nrffota/tools/build_flasher.py
//sk: JEDEN board-agnostický flasher blob — app base dostáva RUNTIME (4. arg flasher_entry,
//sk: viď s_app_base vo flasher.c), nie compile-time. Generuj:
//sk:   python nrffota/tools/build_flasher.py
#if __has_include("flasher_code.h")
  #include "flasher_code.h"
  #define FOTA_HAS_FLASHER 1
#else
  #define FOTA_HAS_FLASHER 0
#endif

extern const FotaState* fota_get_state();
//en: Patch into RAM: by default assembled from recv.log, -D USE_PATCHBIN_FILE reads patch.bin
//sk: Patch do RAM: default zostaví z recv.log, -D USE_PATCHBIN_FILE číta patch.bin
extern uint8_t* fota_acquire_patch_ram(uint32_t* out_size);
//en: App base from a linker symbol (v6=0x26000, v7=0x27000) — more robust than a macro.
//sk: App base z linker symbolu (v6=0x26000, v7=0x27000) — viac robustné než makro.
extern uint32_t fota_running_fw_base(void);

__attribute__((unused))
static void print_sha16(const uint8_t* h) {
    for (int i = 0; i < 16; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)h[i]); }
}

//en: Verifies that the currently running FW (app flash @ APP_FLASH_START) matches the
//en: 'old' from which fota_sender.py generated the patch. If the base doesn't match →
//en: do NOT rewrite.
//sk: Overí, že aktuálne bežiaci FW (app flash @ APP_FLASH_START) zodpovedá 'old'
//sk: z ktorého fota_sender.py vygeneroval patch. Ak base nesedí → NEPREPISOVAŤ.
static bool fota_verify_old_fw() {
    const FotaState* st = fota_get_state();
    bool all_zero = true;
    for (int i = 0; i < 32 && all_zero; i++) if (st->old_sha256[i]) all_zero = false;
    if (all_zero || st->old_fw_size == 0) {
        FOTA_DEBUG_PRINTLN("[OLD] old_sha256 neznámy — kontrola base preskočená");
        return true;
    }
    if (st->old_fw_size > (APP_FLASH_END - fota_running_fw_base())) {
        FOTA_DEBUG_PRINTLN("[OLD] CHYBA: old_fw_size %lu > app okno", (unsigned long)st->old_fw_size);
        return false;
    }
    FotaSha256 sha; sha.reset();
    sha.update((const void*)fota_running_fw_base(), st->old_fw_size);
    uint8_t h[32]; sha.finalize(h, sizeof(h));
    FOTA_DEBUG_PRINT("[OLD] base app flash SHA256="); print_sha16(h); FOTA_DEBUG_PRINTLN("...");
    FOTA_DEBUG_PRINT("[OLD] očakávaný old_sha256 ="); print_sha16(st->old_sha256); FOTA_DEBUG_PRINTLN("...");
    if (memcmp(h, st->old_sha256, 32) != 0) {
        FOTA_DEBUG_PRINTLN("[OLD] BASE NESEDÍ — bežiaci FW != old z patchu! NEPREPISUJEM.");
        FOTA_DEBUG_PRINTLN("[OLD]   Vygeneruj patch voči aktuálnemu firmvéru.");
        return false;
    }
    FOTA_DEBUG_PRINTLN("[OLD] base FW sedí s patchom");
    return true;
}

#if FOTA_HAS_FLASHER
//en: Flash-resident flasher path (MeshCore default; ZephCore only with
//en: FOTA_FLASHER_IN_FLASH + a dedicated partition). The ZephCore default
//en: copies the blob to RAM (FLASHER_RAM_ADDR) instead — no NVMC write.
//sk: Cesta flash-rezidentneho flashera (MeshCore default; ZephCore len s
//sk: FOTA_FLASHER_IN_FLASH + dedikovanou particiou). ZephCore default
//sk: kopiruje blob do RAM (FLASHER_RAM_ADDR) — bez NVMC zapisu.
#if defined(FOTA_MESHCORE_BUILD) || defined(FOTA_FLASHER_IN_FLASH)
// ── NVMC (direct access after sd_softdevice_disable) ───────────────────
static void nvmc_erase_page(uint32_t addr) {
    while (!NRF_NVMC->READY);
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een;
    __DMB(); __ISB();
    NRF_NVMC->ERASEPAGE = addr;
    while (!NRF_NVMC->READY);
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
    __DMB();
}

static void nvmc_write_words(uint32_t dst_addr, const uint32_t* src, uint32_t word_count) {
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
    __DMB(); __ISB();
    volatile uint32_t* dst = (volatile uint32_t*)dst_addr;
    for (uint32_t i = 0; i < word_count; i++) {
        dst[i] = src[i];
        while (!NRF_NVMC->READY);
    }
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
    __DMB();
}

static bool ensure_flasher_written() {
    if (memcmp((const void*)FLASHER_CODE_ADDR, flasher_code, FLASHER_CODE_SIZE) == 0) {
        FOTA_DEBUG_PRINTLN("[FLASH] Flasher je aktuálny");
        return true;
    }
    FOTA_DEBUG_PRINTLN("[FLASH] Zapisujem flasher do 0xEB000...");
    nvmc_erase_page(FLASHER_CODE_ADDR);
    nvmc_write_words(FLASHER_CODE_ADDR, (const uint32_t*)flasher_code, (FLASHER_CODE_SIZE + 3) / 4);
    if (memcmp((const void*)FLASHER_CODE_ADDR, flasher_code, FLASHER_CODE_SIZE) != 0) {
        FOTA_DEBUG_PRINTLN("[FLASH] CHYBA: overenie flasher zlyhalo!");
        return false;
    }
    FOTA_DEBUG_PRINTLN("[FLASH] Flasher zapísaný OK");
    return true;
}
#endif  // FOTA_MESHCORE_BUILD || FOTA_FLASHER_IN_FLASH
#endif

// ================================================================
//en: HPatchLite callbacks
//en: Patch format: HPatchLite inplaceB (hdiffi -inplaceB old.bin new.bin patch.bin)
// ================================================================
#if FOTA_HAS_HPATCH

//en: Sequential read from File (kept for possible file-based paths; the patch is now
//en: loaded into RAM via fota_acquire_patch_ram, so it is unused)
//sk: Sekvenčné čítanie z File (ponechané pre prípadné súborové cesty; teraz sa patch
//sk: načítava do RAM cez fota_acquire_patch_ram, takže je nepoužité → unused)
__attribute__((unused))
static hpi_BOOL patch_file_read(hpi_TInputStreamHandle h,
                                hpi_byte* out, hpi_size_t* size) {
    if (*size == 0) return hpi_TRUE;
    FotaFile* f = (FotaFile*)h;
    int n = f->read(out, (uint32_t)*size);
    if (n <= 0) { *size = 0; return hpi_FALSE; }
    *size = (hpi_size_t)n;
    return hpi_TRUE;
}

//en: SHA256-only listener for test mode (hpatchi_listener_t must be the first member)
//sk: SHA256-only listener pre test mód (hpatchi_listener_t musí byť prvý člen)
typedef struct {
    hpatchi_listener_t base;   //en: MUST be first
    FotaSha256 sha;
    uint32_t written;
} ShaListener;

static hpi_BOOL sha_read_old(hpatchi_listener_t* l,
                             hpi_pos_t pos, hpi_byte* out, hpi_size_t size) {
    (void)l;
    uint32_t base = fota_running_fw_base();
    if ((uint32_t)pos + (uint32_t)size > (APP_FLASH_END - base)) return hpi_FALSE;
    memcpy(out, (const void*)(base + (uint32_t)pos), size);
    return hpi_TRUE;
}
static hpi_BOOL sha_write_new(hpatchi_listener_t* l,
                              const hpi_byte* data, hpi_size_t size) {
    ShaListener* s = (ShaListener*)l;
    s->sha.update(data, size);
    s->written += size;
    return hpi_TRUE;
}

//en: Magic of the compressed patch format: 'Z','L','I','B' (LE uint32)
#define ZPATCH_MAGIC  0x42494C5Au

//en: HPatchLite read_diff callback — streaming DEFLATE from a RAM buffer
static hpi_BOOL patch_zlib_read(hpi_TInputStreamHandle h,
                                hpi_byte* out, hpi_size_t* size) {
    if (*size == 0) return hpi_TRUE;
    puff_stream_t* ps = (puff_stream_t*)h;
    uint32_t n = puff_stream_read(ps, (uint8_t*)out, (uint32_t)*size);
    *size = (hpi_size_t)n;
    return (n > 0 || ps->state == PS_DONE) ? hpi_TRUE : hpi_FALSE;
}

//en: Sequential read from a RAM buffer (for an uncompressed patch assembled into RAM)
typedef struct { const uint8_t* p; uint32_t len; uint32_t pos; } MemStream;
static hpi_BOOL patch_mem_read(hpi_TInputStreamHandle h,
                               hpi_byte* out, hpi_size_t* size) {
    if (*size == 0) return hpi_TRUE;
    MemStream* m = (MemStream*)h;
    uint32_t avail = m->len - m->pos;
    uint32_t n = ((uint32_t)*size < avail) ? (uint32_t)*size : avail;
    if (n == 0) { *size = 0; return hpi_FALSE; }
    memcpy(out, m->p + m->pos, n);
    m->pos += n;
    *size = (hpi_size_t)n;
    return hpi_TRUE;
}

//en: Short machine-readable reason into 'err' (ASCII — also goes over the LoRa CLI). err may be NULL.
//sk: Krátky strojový dôvod do 'err' (ASCII — ide aj cez LoRa CLI). err môže byť NULL.
static void set_err(char* err, size_t n, const char* msg) {
    if (err && n) { strncpy(err, msg, n - 1); err[n - 1] = 0; }
}

// ── TEST MODE: SHA256-only, writes nothing to flash ───────────────────
//en: err/err_sz (optional, may be NULL): short FAIL reason or a note on OK.
bool fota_patch_to_file(char* err, size_t err_sz) {
    const FotaState* st = fota_get_state();
    FOTA_DEBUG_PRINTLN("[PATCH] Test: SHA256 verify (bez flash)...");
    fota_verify_old_fw();   //en: informative only in the dry-run (does not block the test)

    uint32_t patch_size = 0;
    uint8_t* patch_buf = fota_acquire_patch_ram(&patch_size);   //en: RAM: from recv.log | patch.bin
    if (!patch_buf) { FOTA_DEBUG_PRINTLN("[PATCH] patch nedostupný"); set_err(err, err_sz, "ziadne patch data"); return false; }

    //en: Detect the compressed format (magic 'ZLIB' in the first 4 bytes)
    uint32_t magic = 0;
    if (patch_size >= 4) memcpy(&magic, patch_buf, 4);
    if (magic == ZPATCH_MAGIC) {
        // ── ZLIB streaming dry-run (compressed patch in RAM) ───────────────
        uint32_t uncomp_sz = 0, new_fw_sz = 0;
        memcpy(&uncomp_sz, patch_buf + 4, 4);
        memcpy(&new_fw_sz, patch_buf + 8, 4);
        uint32_t comp_sz = patch_size - 12;

        FOTA_DEBUG_PRINTLN("[PATCH] ZLIB: compressed=%lu B  raw=%lu B  new_fw=%lu B", (unsigned long)comp_sz, (unsigned long)uncomp_sz, (unsigned long)new_fw_sz);

        puff_stream_t* ps = (puff_stream_t*)malloc(sizeof(puff_stream_t));
        if (!ps) {
            free(patch_buf);
            FOTA_DEBUG_PRINTLN("[PATCH] malloc puff_stream zlyhalo");
            set_err(err, err_sz, "OOM puff_stream");
            return false;
        }
        puff_stream_init(ps, patch_buf + 12, comp_sz);   //en: compressed body from RAM

        hpi_compressType compress_type = hpi_compressType_no;
        hpi_pos_t new_size = 0, uncomp_size_hpi = 0;
        hpi_size_t extra_safe = 0;
        if (!hpatchi_inplace_open(ps, patch_zlib_read,
                                  &compress_type, &new_size,
                                  &uncomp_size_hpi, &extra_safe)) {
            FOTA_DEBUG_PRINTLN("[PATCH] Neplatny HPatchLite header (ZLIB) ps_err=%d", ps->error);
            free(ps); free(patch_buf);
            set_err(err, err_sz, "zly ZLIB/hpatch header");
            return false;
        }
        FOTA_DEBUG_PRINTLN("[PATCH] hpatchi: new_size=%lu B  extra_safe=%lu B", (unsigned long)new_size, (unsigned long)extra_safe);

        //en: Pre-check vs the flasher limit (shared FOTA_MAX_EXTRA_SAFE from flash_layout.h).
        //en: The SHA dry-run itself does NOT need extra_safe (plain hpatch_lite_patch reads
        //en: the intact old FW from flash) — so verify would happily pass a patch that the
        //en: flasher later rejects with 0xE5 and a reset. Fail HERE with a clear reason
        //en: instead, so the CLI user learns about the problem BEFORE attempting the flash.
        //sk: Predkontrola voči limitu flashera (zdieľané FOTA_MAX_EXTRA_SAFE z flash_layout.h).
        //sk: Samotný SHA dry-run extra_safe NEPOTREBUJE (obyčajný hpatch_lite_patch číta
        //sk: nedotknutý starý FW z flashe) — verify by teda ochotne pustil patch, ktorý
        //sk: flasher neskôr odmietne s 0xE5 a resetom. Radšej zlyhaj TU s jasným dôvodom,
        //sk: nech sa to používateľ CLI dozvie PRED pokusom o flash.
        if (extra_safe > FOTA_MAX_EXTRA_SAFE) {
            FOTA_DEBUG_PRINTLN("[PATCH] extra_safe %lu > max %lu — flasher by patch ODMIETOL (0xE5)!", (unsigned long)extra_safe, (unsigned long)FOTA_MAX_EXTRA_SAFE);
            free(ps); free(patch_buf);
            char b[48]; snprintf(b, sizeof(b), "extraSafe %lu > max %lu, flash zlyha", (unsigned long)extra_safe, (unsigned long)FOTA_MAX_EXTRA_SAFE);
            set_err(err, err_sz, b);
            return false;
        }

        ShaListener sl;
        sl.written = 0;
        sl.base.diff_data = ps;
        sl.base.read_diff = patch_zlib_read;
        sl.base.read_old  = sha_read_old;
        sl.base.write_new = sha_write_new;

        //en: scratch from FotaBuffer (static .bss, NOT the stack — the LoRa RX callstack
        //en: is tight; 2 kB on the stack here overflowed the loop-task stack → dead radio).
        //sk: scratch z FotaBuffer (static .bss, NIE stack — LoRa RX callstack je
        //sk: tesný; 2 kB na stacku tu pretekalo loop-task stack → mŕtve rádio).
        uint8_t* cache = fota_get_buffer(FOTA_BUF_CAP);
        if (!cache) { free(ps); free(patch_buf); FOTA_DEBUG_PRINTLN("[PATCH] scratch buffer nedostupný"); set_err(err, err_sz, "scratch busy"); return false; }
        bool ok = (bool)hpatch_lite_patch(&sl.base, new_size, cache, FOTA_BUF_CAP);
        fota_put_buffer(cache);
        int ps_err = ps->error;
        free(ps);
        free(patch_buf);

        if (!ok) {
            if (ps_err) {
                FOTA_DEBUG_PRINTLN("[PATCH] Dekompresia zlyhal: err=%d", ps_err);
                char b[40]; snprintf(b, sizeof(b), "dekompresia err=%d", ps_err);
                set_err(err, err_sz, b);
            } else {
                FOTA_DEBUG_PRINTLN("[PATCH] HPatchLite ZLYHALO (ZLIB)");
                set_err(err, err_sz, "hpatch zlyhal (ZLIB)");
            }
            return false;
        }

        uint8_t result_sha[32];
        sl.sha.finalize(result_sha, sizeof(result_sha));
        FOTA_DEBUG_PRINT("[PATCH] SHA256="); print_sha16(result_sha); FOTA_DEBUG_PRINTLN("...");

        bool all_zero = true;
        for (int i = 0; i < 32 && all_zero; i++) if (st->new_sha256[i]) all_zero = false;
        if (all_zero) {
            FOTA_DEBUG_PRINTLN("[PATCH] Ocakavany SHA256 nezname — overuj manualne");
            set_err(err, err_sz, "ocak. SHA neznama");
            return true;
        }
        if (memcmp(result_sha, st->new_sha256, 32) != 0) {
            FOTA_DEBUG_PRINT("[PATCH] SHA256 NESEDI  exp="); print_sha16(st->new_sha256); FOTA_DEBUG_PRINTLN("...");
            set_err(err, err_sz, "SHA256 nesedi");
            return false;
        }
        FOTA_DEBUG_PRINTLN("[PATCH] ZLIB patch overeny!");
        return true;
    }

    //en: Original format: uncompressed HPatchLite (from RAM via MemStream)
    MemStream ms = { patch_buf, patch_size, 0 };
    hpi_compressType compress_type = hpi_compressType_no;
    hpi_pos_t new_size = 0, uncomp_size = 0;
    hpi_size_t extra_safe = 0;
    if (!hpatchi_inplace_open(&ms, patch_mem_read,
                              &compress_type, &new_size,
                              &uncomp_size, &extra_safe)) {
        FOTA_DEBUG_PRINTLN("[PATCH] Neplatny format patchu (hpatchi_inplace_open)");
        free(patch_buf);
        set_err(err, err_sz, "zly format patchu");
        return false;
    }
    if (compress_type != hpi_compressType_no) {
        FOTA_DEBUG_PRINTLN("[PATCH] Komprimovany patch nie je podporovany");
        free(patch_buf);
        set_err(err, err_sz, "komprimovany nepodporovany");
        return false;
    }

    FOTA_DEBUG_PRINTLN("[PATCH] new=%lu B  patch=%lu B  extraSafe=%lu B", (unsigned long)new_size, (unsigned long)patch_size, (unsigned long)extra_safe);

    //en: Same pre-check as in the ZLIB branch — see the comment there (verify itself
    //en: doesn't need extra_safe, but the flasher would reject the patch with 0xE5).
    //sk: Rovnaká predkontrola ako v ZLIB vetve — viď komentár tam (verify samotné
    //sk: extra_safe nepotrebuje, ale flasher by patch odmietol s 0xE5).
    if (extra_safe > FOTA_MAX_EXTRA_SAFE) {
        FOTA_DEBUG_PRINTLN("[PATCH] extra_safe %lu > max %lu — flasher by patch ODMIETOL (0xE5)!", (unsigned long)extra_safe, (unsigned long)FOTA_MAX_EXTRA_SAFE);
        free(patch_buf);
        char b[48]; snprintf(b, sizeof(b), "extraSafe %lu > max %lu, flash zlyha", (unsigned long)extra_safe, (unsigned long)FOTA_MAX_EXTRA_SAFE);
        set_err(err, err_sz, b);
        return false;
    }

    ShaListener sl;
    sl.written = 0;
    sl.base.diff_data = &ms;
    sl.base.read_diff = patch_mem_read;
    sl.base.read_old  = sha_read_old;
    sl.base.write_new = sha_write_new;

    //en: scratch from FotaBuffer (static .bss, NOT the stack — see comment above)
    uint8_t* cache = fota_get_buffer(FOTA_BUF_CAP);
    if (!cache) { free(patch_buf); FOTA_DEBUG_PRINTLN("[PATCH] scratch buffer nedostupný"); set_err(err, err_sz, "scratch busy"); return false; }
    bool ok = (bool)hpatch_lite_patch(&sl.base, new_size, cache, FOTA_BUF_CAP);
    fota_put_buffer(cache);

    if (!ok) { FOTA_DEBUG_PRINTLN("[PATCH] HPatchLite ZLYHALO"); free(patch_buf); set_err(err, err_sz, "hpatch zlyhal"); return false; }

    uint8_t result_sha[32];
    sl.sha.finalize(result_sha, sizeof(result_sha));
    FOTA_DEBUG_PRINT("[PATCH] SHA256="); print_sha16(result_sha); FOTA_DEBUG_PRINTLN("...");
    free(patch_buf);

    bool all_zero = true;
    for (int i = 0; i < 32 && all_zero; i++)
        if (st->new_sha256[i]) all_zero = false;

    if (all_zero) {
        FOTA_DEBUG_PRINTLN("[PATCH] Očakávaný SHA256 neznámy — overuj manuálne");
        set_err(err, err_sz, "ocak. SHA neznama");
        return true;
    }
    if (memcmp(result_sha, st->new_sha256, 32) != 0) {
        FOTA_DEBUG_PRINT("[PATCH] SHA256 NESEDÍ  exp=");
        print_sha16(st->new_sha256); FOTA_DEBUG_PRINTLN("...");
        set_err(err, err_sz, "SHA256 nesedi");
        return false;
    }
    FOTA_DEBUG_PRINTLN("[PATCH] OK — patch overený!");
    return true;
}

// ── PRODUCTION MODE: patch→RAM → jump flasher@0xEB000 ────────────────
bool fota_flash_via_flasher() {
#if !FOTA_HAS_FLASHER
    FOTA_DEBUG_PRINTLN("[FLASHER] flasher_code.h chýba.");
    FOTA_DEBUG_PRINTLN("[FLASHER] Spusti: python nrffota/tools/build_flasher.py");
    return false;
#else
    // ── 0: base FW verification ──
    if (!fota_verify_old_fw()) {
        FOTA_DEBUG_PRINTLN("[FLASHER] PRERUŠENÉ — base FW nesedí, neriskujem prepis.");
        return false;
    }

    // ── 1: load the patch into RAM (recv.log assembly | patch.bin), new_fw_size from header ──
    //en: fota_acquire_patch_ram: by default assembles from recv.log straight into RAM
    //en: (no patch.bin), -D USE_PATCHBIN_FILE reads /ota/patch.bin. The FS is used HERE,
    //en: before FotaFS.end() below.
    //sk: fota_acquire_patch_ram: default zostaví z recv.log priamo do RAM (žiadny patch.bin),
    //sk: -D USE_PATCHBIN_FILE číta /ota/patch.bin. FS sa použije TU, pred FotaFS.end() nižšie.
    uint32_t patch_size = 0;
    uint8_t* patch_buf = fota_acquire_patch_ram(&patch_size);
    if (!patch_buf) {
        FOTA_DEBUG_PRINTLN("[FLASHER] patch nedostupný (RAM/súbor)");
        return false;
    }
    if (patch_size == 0 || patch_size > FOTA_FS_FLASH_SIZE) {
        FOTA_DEBUG_PRINTLN("[FLASHER] Neplatná veľkosť patchu: %lu", (unsigned long)patch_size);
        free(patch_buf);
        return false;
    }

    uint32_t new_fw_size = 0;
    {
        uint32_t magic = 0;
        if (patch_size >= 12) memcpy(&magic, patch_buf, 4);
        if (magic == ZPATCH_MAGIC) {
            uint32_t uncomp_sz = 0;
            memcpy(&uncomp_sz, patch_buf + 4, 4);
            memcpy(&new_fw_size, patch_buf + 8, 4);
            FOTA_DEBUG_PRINTLN("[FLASHER] Komprimovany format: staged=%lu B  raw=%lu B  new_fw=%lu B", (unsigned long)patch_size, (unsigned long)uncomp_sz, (unsigned long)new_fw_size);

            //en: Peek the hpatchi header (through puff) for extra_safe and abort HERE if it
            //en: exceeds FOTA_MAX_EXTRA_SAFE — the flasher would reject it anyway (0xE5),
            //en: but only AFTER FS unmount + sd_disable + reset, which costs a reboot and
            //en: leaves just a trace code. Failing here keeps the app running with a clear
            //en: log. Any OTHER open failure is left to the flasher (it has its own checks).
            //sk: Nakukni do hpatchi hlavičky (cez puff) po extra_safe a skonči TU, ak
            //sk: presahuje FOTA_MAX_EXTRA_SAFE — flasher by ho aj tak odmietol (0xE5),
            //sk: ale až PO FS unmount + sd_disable + resete, čo stojí reboot a ostane len
            //sk: trace kód. Zlyhanie tu nechá appku bežať s jasným logom. AKÉKOĽVEK iné
            //sk: zlyhanie open nechávame na flasher (má vlastné kontroly).
            puff_stream_t* ps = (puff_stream_t*)malloc(sizeof(puff_stream_t));
            if (ps) {
                puff_stream_init(ps, patch_buf + 12, patch_size - 12);
                hpi_compressType ct = hpi_compressType_no;
                hpi_pos_t ns = 0, us = 0;
                hpi_size_t es = 0;
                bool hdr_ok = (bool)hpatchi_inplace_open(ps, patch_zlib_read, &ct, &ns, &us, &es);
                free(ps);
                if (hdr_ok && es > FOTA_MAX_EXTRA_SAFE) {
                    free(patch_buf);
                    FOTA_DEBUG_PRINTLN("[FLASHER] PRERUŠENÉ — extra_safe %lu > max %lu (flasher by odmietol, 0xE5)", (unsigned long)es, (unsigned long)FOTA_MAX_EXTRA_SAFE);
                    return false;
                }
            }
        } else {
            MemStream ms = { patch_buf, patch_size, 0 };
            hpi_compressType compress_type = hpi_compressType_no;
            hpi_pos_t new_fw_size64 = 0, uncomp_size = 0;
            hpi_size_t extra_safe = 0;
            if (!hpatchi_inplace_open(&ms, patch_mem_read,
                                      &compress_type, &new_fw_size64,
                                      &uncomp_size, &extra_safe)) {
                free(patch_buf);
                FOTA_DEBUG_PRINTLN("[FLASHER] Neplatny format patchu");
                return false;
            }
            if (compress_type != hpi_compressType_no) {
                free(patch_buf);
                FOTA_DEBUG_PRINTLN("[FLASHER] Komprimovany HPatchLite nie je podporovany");
                return false;
            }
            new_fw_size = (uint32_t)new_fw_size64;
            FOTA_DEBUG_PRINTLN("[FLASHER] Nekomprimovany format: new_fw=%lu B  patch=%lu B", (unsigned long)new_fw_size, (unsigned long)patch_size);
            //en: Same pre-check as in the ZLIB branch — abort before FS unmount/sd_disable.
            //sk: Rovnaká predkontrola ako v ZLIB vetve — skonči pred FS unmount/sd_disable.
            if (extra_safe > FOTA_MAX_EXTRA_SAFE) {
                free(patch_buf);
                FOTA_DEBUG_PRINTLN("[FLASHER] PRERUŠENÉ — extra_safe %lu > max %lu (flasher by odmietol, 0xE5)", (unsigned long)extra_safe, (unsigned long)FOTA_MAX_EXTRA_SAFE);
                return false;
            }
        }
    }
    FOTA_DEBUG_PRINTLN("[FLASHER] Patch v RAM (%lu B)", (unsigned long)patch_size);

    const uint32_t PATCH_RAM_ADDR = 0x20000000u;   //en: matches flasher.c
    if (patch_size > 0x20000u) {   //en: 128kB — limit of the patch RAM region (flasher.ld)
        free(patch_buf);
        FOTA_DEBUG_PRINTLN("[FLASHER] Patch > 128kB — nezmestí sa do RAM oblasti");
        return false;
    }

#if defined(FOTA_ZEPHCORE_BUILD) && !defined(FOTA_FLASHER_IN_FLASH)
    //en: RAM-flasher sanity: the patch buffer must lie entirely BELOW the flasher
    //en: RAM region (code @ FLASHER_RAM_ADDR, .bss+stack above it). The flasher
    //en: itself moves the patch to PATCH_RAM_ADDR (FLASHER_COPY_PATCH) — the app
    //en: cannot memmove over the running kernel/stack.
    //sk: RAM-flasher poistka: patch buffer musi lezat cely POD flasher RAM regionom
    //sk: (kod @ FLASHER_RAM_ADDR, .bss+stack nad nim). Patch si na PATCH_RAM_ADDR
    //sk: presunie sam flasher (FLASHER_COPY_PATCH) — app nemoze memmove-ovat cez
    //sk: beziaci kernel/stack.
    if ((uint32_t)(uintptr_t)patch_buf + patch_size > FLASHER_RAM_ADDR) {
        FOTA_DEBUG_PRINTLN("[FLASHER] PRERUŠENÉ — patch buffer zasahuje do flasher RAM regionu (0x%X+%lu)",
                           (unsigned)(uintptr_t)patch_buf, (unsigned long)patch_size);
        free(patch_buf);
        return false;
    }
#endif

    //en: ── 3: close CustomLFS (unmount only — FS data STAYS in flash) ──
    //sk: ── 3: zavrieť CustomLFS (len odmount — FS dáta vo flash ZOSTANÚ) ──
    FotaFS.end();

    //en: ── 4: BYE + disable SoftDevice (USB CDC disappears) ──
    //en: app_base from a linker symbol (v6=0x26000, v7=0x27000) — passed to the flasher
    //en: as the 4th arg, so there is ONE board-agnostic blob (not compile-time per-board).
    //sk: ── 4: BYE + disable SoftDevice (USB CDC zmizne) ──
    //sk: app_base z linker symbolu (v6=0x26000, v7=0x27000) — odovzdáme flasheru ako
    //sk: 4. arg, takže je JEDEN board-agnostický blob (nie compile-time per-board).
    uint32_t app_base = fota_running_fw_base();
    typedef void(*flasher_fn_t)(uint32_t, uint32_t, uint32_t, uint32_t);
#if defined(FOTA_MESHCORE_BUILD) || defined(FOTA_FLASHER_IN_FLASH)
    const uint32_t flasher_jump = FLASHER_CODE_ADDR;
#else
    const uint32_t flasher_jump = FLASHER_RAM_ADDR;
#endif
    FOTA_DEBUG_PRINTLN("[FLASHER] → 0x%X [BYE] (streaming)", (unsigned)flasher_jump);
#if defined(FOTA_MESHCORE_BUILD)
    Serial.flush();

    extern uint32_t sd_softdevice_disable(void);
    sd_softdevice_disable();
#elif defined(FOTA_ZEPHCORE_BUILD)
    //en: no SoftDevice on Zephyr; give the USB CDC console a moment to drain
    //sk: na Zephyre nie je SoftDevice; nechaj USB CDC konzolu dobehnut
    k_msleep(50);
#endif

    //en: After sd_disable, DISABLE IRQs before the NVMC write + jump to the flasher.
    //en: Without this, a radio DIO1 / SysTick ISR can arrive during the NVMC window →
    //en: jump via VTOR into an app handler (SD already disabled, FS unmounted) →
    //en: fault/hang (the flasher doesn't start, or starts with a corrupted blob; empty
    //en: trace). We leave sd_disable with IRQs enabled (the SVC completes); we protect
    //en: the critical NVMC window. The flasher does its own cpsid i.
    //sk: Po sd_disable ZAKÁŽ IRQ pred NVMC zápisom + skokom na flasher. Bez tohto
    //sk: môže počas NVMC okna prísť rádio DIO1 / SysTick ISR → skok cez VTOR do app
    //sk: handlera (SD už disabled, FS odmountovaný) → fault/hang (flasher nenabehne
    //sk: alebo s pokazeným blobom; prázdny trace). sd_disable necháme s IRQ povolenými
    //sk: (SVC sa dokončí); chránime kritické NVMC okno. Flasher si robí vlastný cpsid i.
    __disable_irq();

#if defined(FOTA_MESHCORE_BUILD) || defined(FOTA_FLASHER_IN_FLASH)
    //en: ── 5: flasher code into flash (nvmc, only after sd_disable) ──
    //sk: ── 5: flasher kód do flash (nvmc, až po sd_disable) ──
    if (!ensure_flasher_written()) {
        NVIC_SystemReset();
    }

    //en: ── 6: compressed patch into RAM @ PATCH_RAM_ADDR. Past this point NO
    //en:       Serial (the USB buffer may have been overwritten). FS is NOT touched. ──
    //sk: ── 6: komprimovaný patch do RAM @ PATCH_RAM_ADDR. Po tomto bode ŽIADNE
    //sk:       Serial (USB buffer mohol byť prepísaný). FS sa NEdotýka. ──
    memmove((void*)PATCH_RAM_ADDR, patch_buf, patch_size);

    //en: ── 7: jump to the flasher — DOES NOT RETURN. ──
    //sk: ── 7: skok na flasher — NEVRÁTI SA. ──
    ((flasher_fn_t)(flasher_jump | 1u))(PATCH_RAM_ADDR, patch_size, new_fw_size, app_base);
#else
    //en: ── 5: RAM flasher — copy the blob to FLASHER_RAM_ADDR and jump. The patch
    //en:       stays at patch_buf (heap, below the flasher region — guarded above);
    //en:       the flasher moves it to PATCH_RAM_ADDR itself (FLASHER_COPY_PATCH),
    //en:       running from its own SP at the top of RAM. IRQs are already off.
    //sk: ── 5: RAM flasher — skopiruj blob na FLASHER_RAM_ADDR a skoc. Patch ostava
    //sk:       na patch_buf (heap, pod flasher regionom — poistka vyssie); na
    //sk:       PATCH_RAM_ADDR si ho presunie sam flasher (FLASHER_COPY_PATCH),
    //sk:       beziac s vlastnym SP na vrchu RAM. IRQ su uz vypnute.
    memcpy((void*)FLASHER_RAM_ADDR, flasher_code, FLASHER_CODE_SIZE);
    __DSB(); __ISB();

    //en: ── 6: jump to the flasher — DOES NOT RETURN. ──
    //sk: ── 6: skok na flasher — NEVRÁTI SA. ──
    ((flasher_fn_t)(flasher_jump | 1u))((uint32_t)(uintptr_t)patch_buf, patch_size, new_fw_size, app_base);
#endif
    while (1);
    return false;  //en: unreachable
#endif  // FOTA_HAS_FLASHER
}

#else  // FOTA_HAS_HPATCH == 0

bool fota_patch_to_file(char* err, size_t err_sz) {
    FOTA_DEBUG_PRINTLN("[PATCH] HPatchLite nie je nainštalovaná (nrffota/hpatchlite/).");
    if (err && err_sz) { strncpy(err, "hpatchlite chyba", err_sz - 1); err[err_sz - 1] = 0; }
    return false;
}
bool fota_flash_via_flasher() {
    FOTA_DEBUG_PRINTLN("[FLASHER] HPatchLite nie je nainštalovaná.");
    return false;
}

#endif  // FOTA_HAS_HPATCH

// ── Flasher debug marker — NRF_POWER->GPREGRET2 ──────────────────────────
#define NRF_POWER_GPREGRET2 (*(volatile uint32_t*)0x40000514u)
#define NRF_POWER_RESETREAS (*(volatile uint32_t*)0x40000400u)
static uint8_t  s_flasher_step  = 0;
static uint32_t s_gpret2_raw    = 0;
static uint32_t s_resetreas_raw = 0;

void fota_check_flasher_debug() {
    s_gpret2_raw = NRF_POWER_GPREGRET2 & 0xFFu;
    if (s_gpret2_raw != 0u) {
        NRF_POWER_GPREGRET2 = 0u;          //en: clear it (SD not running yet → direct write OK)
        s_flasher_step = (uint8_t)s_gpret2_raw;
    }
    s_resetreas_raw = NRF_POWER_RESETREAS;
    NRF_POWER_RESETREAS = s_resetreas_raw; //en: write-1-to-clear
}

__attribute__((unused))
static void print_step(uint8_t step) {
    FOTA_DEBUG_PRINT("[FLASHER-DBG] Step=0x%02X  ", (unsigned)step);
    switch (step) {
        case 0xFF: FOTA_DEBUG_PRINTLN("STARTED — crashed before ZLIB check"); break;
        case 0x01: FOTA_DEBUG_PRINTLN("ZLIB detected OK"); break;
        case 0x02: FOTA_DEBUG_PRINTLN("puff() OK"); break;
        case 0x03: FOTA_DEBUG_PRINTLN("hpatchi_inplace_open OK"); break;
        case 0x04: FOTA_DEBUG_PRINTLN("hpatchi_inplaceB OK"); break;
        case 0x05: FOTA_DEBUG_PRINTLN("flush last page OK"); break;
        case 0x06: FOTA_DEBUG_PRINTLN("RESET issued — patch complete!"); break;
        case 0xE0: FOTA_DEBUG_PRINTLN("ERR: uncomp_size 0 or > max"); break;
        case 0xE1: FOTA_DEBUG_PRINTLN("ERR: puff() failed"); break;
        case 0xE2: FOTA_DEBUG_PRINTLN("ERR: puff destlen mismatch"); break;
        case 0xE3: FOTA_DEBUG_PRINTLN("ERR: hpatchi_inplace_open failed"); break;
        case 0xE4: FOTA_DEBUG_PRINTLN("ERR: compress_type != no"); break;
        case 0xE5: FOTA_DEBUG_PRINTLN("ERR: extra_safe > MAX"); break;
        case 0xE6: FOTA_DEBUG_PRINTLN("ERR: hpatchi_inplaceB failed"); break;
        case 0xFE: FOTA_DEBUG_PRINTLN("FAIL — flasher zlyhal a resetoval sa"); break;
        default:
            if (step >= 0x20u && step <= 0x3Fu) {
                uint32_t kb = (uint32_t)(step - 0x20u) * 16u;
                FOTA_DEBUG_PRINTLN("puff progress: dekomprimovaných ~%lu kB", (unsigned long)kb);
            } else {
                FOTA_DEBUG_PRINTLN("(unknown)");
            }
            break;
    }
}

//en: ── Flash trace log — the flasher appends events to FLASH_TRACE_ADDR ──
//sk: ── Flash trace log — flasher appenduje eventy do FLASH_TRACE_ADDR ──
#define FLASH_TRACE_MAX  512u
static void fota_print_flasher_trace() {
#if !defined(FLASH_TRACE_ADDR)
    //en: no trace region in this build (ZephCore default map has none)
    //sk: v tomto builde nie je trace region (ZephCore default mapa ho nema)
    FOTA_DEBUG_PRINTLN("[FLASHER-TRACE] (nedostupny — bez trace regionu)");
#else
    const volatile uint32_t* t = (const volatile uint32_t*)FLASH_TRACE_ADDR;
    if (t[0] == 0xFFFFFFFFu) {
        FOTA_DEBUG_PRINTLN("[FLASHER-TRACE] (prázdny — flasher nezapísal trace)");
        return;
    }
    FOTA_DEBUG_PRINTLN("[FLASHER-TRACE] sekvencia eventov flashera:");
    for (uint32_t i = 0; i < FLASH_TRACE_MAX; i++) {
        uint32_t code = t[i];
        if (code == 0xFFFFFFFFu) break;
        if (code == 0xD0u) { FOTA_DEBUG_PRINTLN("  [chk] FNV-1a výstupu (nový FW) = 0x%X", (unsigned)t[i + 1]); i++; continue; }
        if (code == 0xD1u) { FOTA_DEBUG_PRINTLN("  [vfy] FNV-1a zapísanej flash    = 0x%X", (unsigned)t[i + 1]); i++; continue; }
        if (code == 0xD2u) { FOTA_DEBUG_PRINTLN("  [vfy] VERIFY OK — flash == hpatchi výstup"); continue; }
        if (code == 0xEAu) { FOTA_DEBUG_PRINTLN("  [vfy] VERIFY FAIL — skok do DFU!"); continue; }
        FOTA_DEBUG_PRINT("  [%2lu] ", (unsigned long)i);
        print_step((uint8_t)(code & 0xFFu));
    }
#endif  // FLASH_TRACE_ADDR
}

void fota_print_flasher_debug() {
    FOTA_DEBUG_PRINT("[FLASHER-DBG] GPREGRET2=0x%X  RESETREAS=0x%X (", (unsigned)s_gpret2_raw, (unsigned)s_resetreas_raw);
    bool any = false;
    if (s_resetreas_raw & 0x01u) { FOTA_DEBUG_PRINT("PIN "); any = true; }
    if (s_resetreas_raw & 0x02u) { FOTA_DEBUG_PRINT("WDT! "); any = true; }
    if (s_resetreas_raw & 0x04u) { FOTA_DEBUG_PRINT("SREQ "); any = true; }
    if (s_resetreas_raw & 0x08u) { FOTA_DEBUG_PRINT("LOCKUP! "); any = true; }
    if (!any && s_resetreas_raw == 0u) FOTA_DEBUG_PRINT("power-on/none");
    FOTA_DEBUG_PRINTLN(")");
    if (s_flasher_step != 0u) {
        FOTA_DEBUG_PRINT("[FLASHER-DBG] posledný krok: ");
        print_step(s_flasher_step);
        s_flasher_step = 0;
    }
    fota_print_flasher_trace();
}

//en: ── DEBUG: decompress patch.bin via puff_stream, print the FNV of the whole raw output ──
//sk: ── DEBUG: dekomprimuj patch.bin cez puff_stream, vypíš FNV celého raw ──
void fota_debug_decompress() {
    FOTA_DEBUG_PRINT("[DBG] app flash @0x%X [0:16]= ", (unsigned)fota_running_fw_base());
    const uint8_t* app = (const uint8_t*)fota_running_fw_base();
    for (int i = 0; i < 16; i++) { FOTA_DEBUG_PRINT("%02X ", (unsigned)app[i]); }
    FOTA_DEBUG_PRINTLN("");

    FotaFile f(FotaFS);
    if (!f.open(FOTA_FS_PATCH, FILE_O_READ)) { FOTA_DEBUG_PRINTLN("[DBG] patch.bin chýba"); return; }
    uint32_t sz = (uint32_t)f.size();
    uint32_t magic = 0, uncomp = 0, newfw = 0;
    f.read((uint8_t*)&magic, 4); f.read((uint8_t*)&uncomp, 4); f.read((uint8_t*)&newfw, 4);
    if (magic != 0x42494C5Au) { FOTA_DEBUG_PRINTLN("[DBG] nie ZLIB formát"); f.close(); return; }
    uint32_t comp_sz = sz - 12;
    uint8_t* comp = (uint8_t*)malloc(comp_sz);
    if (!comp) { FOTA_DEBUG_PRINTLN("[DBG] malloc comp fail"); f.close(); return; }
    uint32_t rd = (uint32_t)f.read(comp, comp_sz); f.close();
    if (rd != comp_sz) { FOTA_DEBUG_PRINTLN("[DBG] read fail"); free(comp); return; }

    puff_stream_t* ps = (puff_stream_t*)malloc(sizeof(puff_stream_t));
    if (!ps) { FOTA_DEBUG_PRINTLN("[DBG] malloc ps fail"); free(comp); return; }
    puff_stream_init(ps, comp, comp_sz);

    uint32_t fnv = 2166136261u, total = 0;
    uint8_t buf[256];
    for (;;) {
        uint32_t n = puff_stream_read(ps, buf, sizeof(buf));
        if (n == 0) break;
        for (uint32_t i = 0; i < n; i++) { fnv ^= (uint32_t)buf[i]; fnv *= 16777619u; }
        total += n;
    }
    int err = ps->error;
    free(ps); free(comp);

    FOTA_DEBUG_PRINTLN("[DBG] puff_stream raw=%lu B (exp %lu)  FNV=0x%X  err=%d  %s",
        (unsigned long)total, (unsigned long)uncomp, (unsigned)fnv, err,
        (total == uncomp && err == 0) ? "[dekompr OK]" : "[DEKOMPR CHYBA!]");
}

#endif  // WITH_LORA_FOTA
