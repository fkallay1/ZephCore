//en: flasher/flasher.c — standalone nRF52840 HPatchLite in-place flasher
//en:
//en: We run from 0xF2000 (OUTSIDE the application flash 0x26000-0xD4000).
//en: We apply the HPatchLite inplaceB patch from 0xD4000 to 0x26000 in-place.
//en:
//en: INPUT (ARM AAPCS):
//en:   r0 = patch_addr  — 0xD4000 (staged patch — raw or ZLIB compressed)
//en:   r1 = patch_size  — size of the staged data in bytes
//en:   r2 = new_fw_size — (ignored, read from the patch header)
//en:
//en: Patch format — uncompressed:
//en:   HPatchLite inplaceB (hdiffi -inplaceB old.bin new.bin patch.bin)
//en:
//en: Patch format — compressed (preferred):
//en:   [magic 4B: 'Z','L','I','B']
//en:   [uncomp_size 4B LE]     → size of the decompressed HPatchLite patch
//en:   [new_fw_size 4B LE]     → size of the new firmware (for info)
//en:   [raw DEFLATE data...]   → Python: zlib.compress(patch, level=9, wbits=-9)
//en:
//en: Decompression:
//en:   puff() from 0xD4000+12 → RAM buffer @ DECOMP_BUF_ADDR (0x20000000)
//en:   Max uncomp_size: DECOMP_BUF_MAX (220kB)
//en:
//en: In-place write safety:
//en:   hpatchi_inplaceB delays the write by extraSafeSize bytes via a ring buffer.
//en:   While writing to address X, the old data at X is still in XIP flash.
//en:
//en: STANDALONE: no Arduino/BSP/FreeRTOS.
//en: COMPILE:    python tools/build_flasher.py  →  src/flasher_code.h
//sk: Bežíme z 0xF2000 (MIMO aplikačnej flash 0x26000-0xD4000).
//sk: Aplikujeme HPatchLite inplaceB patch z 0xD4000 do 0x26000 in-place.
//sk:
//sk: VSTUP (ARM AAPCS):
//sk:   r0 = patch_addr  — 0xD4000 (staged patch — raw alebo ZLIB komprimovaný)
//sk:   r1 = patch_size  — veľkosť staged dát v bajtoch
//sk:   r2 = new_fw_size — (ignoruje sa, čítame z patch hlavičky)
//sk:
//sk: Patch formát — nekomprimovaný:
//sk:   HPatchLite inplaceB (hdiffi -inplaceB old.bin new.bin patch.bin)
//sk:
//sk: Patch formát — komprimovaný (preferovaný):
//sk:   [magic 4B: 'Z','L','I','B']
//sk:   [uncomp_size 4B LE]     → veľkosť dekomprimovaného HPatchLite patchu
//sk:   [new_fw_size 4B LE]     → veľkosť nového firmvéru (pre info)
//sk:   [raw DEFLATE data...]   → Python: zlib.compress(patch, level=9, wbits=-9)
//sk:
//sk: Dekomprimácia:
//sk:   puff() z 0xD4000+12 → RAM buffer @ DECOMP_BUF_ADDR (0x20000000)
//sk:   Max uncomp_size: DECOMP_BUF_MAX (220kB)
//sk:
//sk: Bezpečnosť in-place zápisu:
//sk:   hpatchi_inplaceB oneskorí zápis o extraSafeSize bajtov cez ring buffer.
//sk:   Počas zápisu na adresu X sú staré dáta na X stále v XIP flash.
//sk:
//sk: STANDALONE: žiadne Arduino/BSP/FreeRTOS.
//sk: KOMPILUJ:   python tools/build_flasher.py  →  src/flasher_code.h

//en: Compiled ONLY into the standalone flasher via nrffota/tools/build_flasher.py
//en: (defines -DFOTA_FLASHER_BUILD). In the MeshCore FW build (where the recursive
//en: build_src_filter picks this file up) it stays EMPTY — the flasher runs outside
//en: the app flash.
//sk: Kompiluje sa LEN do standalone flashera cez nrffota/tools/build_flasher.py
//sk: (definuje -DFOTA_FLASHER_BUILD). V MeshCore FW builde (kde tento súbor zoberie
//sk: rekurzívny build_src_filter) ostáva PRÁZDNY — flasher beží mimo app flash.
#ifdef FOTA_FLASHER_BUILD

#include <stdint.h>
#include <string.h>
#include "hpatch_lite.h"
#include "puff_stream.h"   //en: STREAMING DEFLATE — the patch is NOT decompressed whole into RAM

/* ── Flash layout — per-board (single source, freestanding-safe) ────── */
//en: build_flasher.py copies shared/flash_layout.h into tmp and passes -DBOARD_*
#include "flash_layout.h"  /* APP_FLASH_START, APP_FLASH_MAX, FLASH_TRACE_ADDR */
#define PAGE_SIZE         FLASH_PAGE_SIZE

//en: Maximum extraSafeSize from the patch header — shared constant from flash_layout.h
//en: (32kB). extraSafeSize ≈ how far the image content shifted = how much the FW GREW
//en: between builds; with the old 4096 limit a growth > 4kB degenerated the patch to
//en: ~the whole image (see fkclaude/fcl_readme_fota_extrasafe.md). 32kB covers realistic
//en: inter-build growth; the buffer lives in flasher-only RAM (free during flashing).
//sk: Maximálny extraSafeSize z patch hlavičky — zdieľaná konštanta z flash_layout.h
//sk: (32kB). extraSafeSize ≈ o koľko sa obsah obrazu posunul = o koľko FW NARÁSTOL
//sk: medzi buildmi; so starým limitom 4096 rast > 4kB degeneroval patch na ~celý obraz
//sk: (viď fkclaude/fcl_readme_fota_extrasafe.md). 32kB pokryje realistické medzi-buildové
//sk: rasty; buffer žije vo flasher-only RAM (počas flashovania voľná).
#define MAX_EXTRA_SAFE    FOTA_MAX_EXTRA_SAFE

//en: Read cache for the hpatchi diff stream (besides extra_safe). Bigger = faster
//en: patch. temp_cache = MAX_EXTRA_SAFE + READ_CACHE, allocated in .bss.
//sk: Read cache pre hpatchi diff stream (okrem extra_safe). Väčší = rýchlejší
//sk: patch. temp_cache = MAX_EXTRA_SAFE + READ_CACHE, alokovaný v .bss.
#define READ_CACHE        16384u

//en: COMPRESSED patch (~60kB) in RAM — input for puff_stream. The regular FW copies
//en: it here before the jump (memmove after sd_disable). The flasher STREAMS from it.
//en: No big DECOMP_BUF — the patch is decompressed on-demand.
//sk: KOMPRIMOVANÝ patch (~60kB) v RAM — vstup pre puff_stream. Bežný FW ho sem
//sk: skopíruje pred skokom (memmove po sd_disable). Flasher z neho STREAMUJE.
//sk: Žiadny veľký DECOMP_BUF — patch sa dekomprimuje on-demand.
#define PATCH_RAM_ADDR   0x20000000u

//en: Magic for the compressed format: "ZLIB" as LE uint32
#define ZPATCH_MAGIC     0x42494C5Au      /* bytes: 5A 4C 49 42 = 'Z','L','I','B' */

//en: Magic for the compressed format: "ZLIB" as LE uint32
#define ZPATCH_MAGIC     0x42494C5Au      /* bytes: 5A 4C 49 42 = 'Z','L','I','B' */
#define ZPATCH_HDR_SIZE  12u              /* magic(4) + uncomp_size(4) + new_fw_size(4) */

/* ── Debug status marker — NRF_POWER->GPREGRET2 ───────────────────── */
//en: We use a hardware retention register (0x40000514), NOT RAM.
//en: Reason: RAM 0x2003FFF0 gets overwritten by the main firmware startup stack
//en: (SP=0x20040000, the prologue stores LR+regs at 0x2003FFxx) before
//en: fota_check_flasher_debug() can read it.
//en: GPREGRET2 survives SYSRESETREQ and the startup code never touches it.
//en: The SoftDevice is disabled while the flasher runs → a direct write is safe.
//en: The main firmware reads it via sd_power_gpregret_get(1, &val).
//en: Compile with -DFLASHER_DEBUG=0 to strip markers from production build.
//sk: Používame hardwarový retenčný register (0x40000514), NIE RAM.
//sk: Dôvod: RAM 0x2003FFF0 sa prepisuje main firmware startup stackom
//sk: (SP=0x20040000, prologue uloží LR+reg na 0x2003FFxx) skôr, ako
//sk: fota_check_flasher_debug() ho prečíta.
//sk: GPREGRET2 prežíva SYSRESETREQ a startup code ho nikdy netouchne.
//sk: SoftDevice je disabled keď flasher beží → direct zápis je bezpečný.
//sk: Main firmware číta cez sd_power_gpregret_get(1, &val).
#ifndef FLASHER_DEBUG
#   define FLASHER_DEBUG 1
#endif
//en: fmark() is now a no-op — replaced by the flash trace log (ftrace), which gives
//en: the complete event sequence, not just the last step. Kept to save space
//en: (the flasher code must be < 4kB) — all fmark() calls evaporate.
//sk: fmark() je teraz no-op — nahradené flash trace logom (ftrace), ktorý dáva
//sk: kompletnú sekvenciu eventov, nie len posledný krok. Ponechané kvôli úspore
//sk: miesta (flasher kód musí byť < 4kB) — všetky fmark() volania sa vyparia.
#define fmark(step) ((void)0)
/* Step codes: */
#define FM_STARTED          0xFFu  //en: flasher_main was called
#define FM_ZLIB_DETECTED    0x01u
#define FM_PUFF_OK          0x02u
#define FM_OPEN_OK          0x03u
#define FM_PATCH_OK         0x04u
#define FM_FLUSH_OK         0x05u
#define FM_RESET            0x06u
/* Error codes: */
#define FM_ERR_UNCOMP_SZ    0xE0u  /* uncomp_size == 0 or > max */
#define FM_ERR_PUFF         0xE1u  /* puff() != 0 */
#define FM_ERR_DESTLEN      0xE2u  /* destlen mismatch */
#define FM_ERR_OPEN         0xE3u  /* hpatchi_inplace_open failed */
#define FM_ERR_COMPRESS     0xE4u  /* compress_type != no */
#define FM_ERR_SAFE         0xE5u  /* extra_safe > MAX_EXTRA_SAFE */
#define FM_ERR_PATCH        0xE6u  /* hpatchi_inplaceB failed */

//en: ── WDT feeding — defensive, safe even if the WDT is not active ──
//en: Flash: 47 pages × 170ms ≈ 8s. The Adafruit BSP may have a WDT < 8s.
//en: nRF52840 WDT RR[0..7] @ 0x40010600; writing 0x6E524635 feeds the timer.
//sk: ── WDT feeding — defensive, bezpečné aj ak WDT nie je aktívny ──
//sk: Flash: 47 strán × 170ms ≈ 8s. Adafruit BSP môže mať WDT < 8s.
//sk: nRF52840 WDT RR[0..7] @ 0x40010600; zápis 0x6E524635 kŕmi časovač.
//en: non-static: also called by puff.c during the long decompression (extern decl in puff.c)
//sk: non-static: volá ju aj puff.c počas dlhej dekompresie (extern decl v puff.c)
void wdt_feed(void) {
    volatile uint32_t* rr = (volatile uint32_t*)0x40010600u;
    for (int i = 0; i < 8; i++) rr[i] = 0x6E524635UL;
}

/* ── NVMC (nRF52840 — direct access without BSP) ──────────────────── */
#define R_NVMC_READY      (*(volatile uint32_t*)0x4001E400u)
#define R_NVMC_CONFIG     (*(volatile uint32_t*)0x4001E504u)
#define R_NVMC_ERASEPAGE  (*(volatile uint32_t*)0x4001E508u)

static void nvmc_wait(void) {
    __asm volatile ("" ::: "memory");
    while (!(R_NVMC_READY & 1u));
    wdt_feed();
}
static void nvmc_erase_page(uint32_t addr) {
    nvmc_wait();
    R_NVMC_CONFIG = 2u;
    __asm volatile ("dsb" ::: "memory");
    R_NVMC_ERASEPAGE = addr;
    nvmc_wait();
    R_NVMC_CONFIG = 0u;
    __asm volatile ("dsb" ::: "memory");
}
static void nvmc_write_page(uint32_t addr, const uint8_t* src) {
    const uint32_t* s32 = (const uint32_t*)src;
    volatile uint32_t* d32 = (volatile uint32_t*)addr;
    R_NVMC_CONFIG = 1u;
    __asm volatile ("dsb" ::: "memory");
    for (uint32_t i = 0; i < PAGE_SIZE / 4u; i++) {
        d32[i] = s32[i];
        nvmc_wait();
    }
    R_NVMC_CONFIG = 0u;
    __asm volatile ("dsb" ::: "memory");
}

/* ── Flash trace log ──────────────────────────────────────────────────── */
//en: The flasher appends 32-bit event codes to the free page 0xF3000 (outside
//en: LittleFS/app/bootloader). The regular FW reads back the WHOLE event sequence.
//en: Under FLASHER_DEBUG=0 all of it is omitted (no space, no NVMC write).
//en:   ftrace(code) appends one word; ftrace_init() erases the page once.
//en: Trace codes: TR_* below. After reboot you see exactly how far the flasher got.
//sk: Flasher appenduje 32-bit event kódy do voľnej stránky 0xF3000 (mimo
//sk: LittleFS/app/bootloader). Bežný FW prečíta CELÚ sekvenciu eventov.
//sk: Pod FLASHER_DEBUG=0 sa celé vynechá (žiadne miesto, žiadny NVMC zápis).
//sk:   ftrace(code) appenduje jeden word; ftrace_init() raz vymaže stránku.
//sk: Trace kódy: TR_* nižšie. Po reboote vidno presne kam flasher došiel.
//en: FLASH_TRACE_ADDR comes from flash_layout.h (= FLASHER_META_ADDR, 0xF3000)
#define FLASH_TRACE_MAX  512u          //en: max 512 events (2kB of the 4kB page)
#if FLASHER_DEBUG
static uint32_t s_trace_idx = 0;
static void ftrace_init(void) {
    nvmc_erase_page(FLASH_TRACE_ADDR);
    s_trace_idx = 0;
}
static void ftrace(uint32_t code) {
    if (s_trace_idx >= FLASH_TRACE_MAX) return;
    volatile uint32_t* p = (volatile uint32_t*)(FLASH_TRACE_ADDR + s_trace_idx * 4u);
    R_NVMC_CONFIG = 1u;                 /* WEN */
    __asm volatile ("dsb" ::: "memory");
    *p = code;
    nvmc_wait();
    R_NVMC_CONFIG = 0u;                 /* REN */
    __asm volatile ("dsb" ::: "memory");
    s_trace_idx++;
}
#else
#  define ftrace_init() ((void)0)
#  define ftrace(code)  ((void)0)
#endif

//en: non-static wrapper for puff.c (extern) — progress during decompression
void ftrace_ext(unsigned long code) { ftrace((uint32_t)code); }

/* ── Patch stream — sequential read from a buffer (XIP or RAM) ─────── */
typedef struct {
    uint32_t pos;
    uint32_t addr;
    uint32_t size;
} PatchStream;

static hpi_BOOL patch_read(hpi_TInputStreamHandle h,
                            hpi_byte* out, hpi_size_t* size) {
    if (*size == 0) return hpi_TRUE;
    PatchStream* s = (PatchStream*)h;
    uint32_t avail = s->size - s->pos;
    uint32_t n = ((uint32_t)*size < avail) ? (uint32_t)*size : avail;
    if (n == 0) { *size = 0; return hpi_FALSE; }
    memcpy(out, (const uint8_t*)(s->addr + s->pos), n);
    s->pos += n;
    *size = (hpi_size_t)n;
    return hpi_TRUE;
}

//en: ── STREAMING read_diff: on-demand patch decompression via puff_stream ──
//en: For a ZLIB patch. handle = puff_stream_t*. HPatchLite reads the diff forward-only,
//en: puff_stream guarantees that. No big decompression buffer (unlike puff()).
//sk: ── STREAMING read_diff: dekompresia patchu on-demand cez puff_stream ──
//sk: Pre ZLIB patch. handle = puff_stream_t*. HPatchLite číta diff forward-only,
//sk: puff_stream to zaručuje. Žiadny veľký dekompresný buffer (na rozdiel od puff()).
static puff_stream_t s_ps;   //en: ~1.8kB in .bss

//en: App flash base (0x26000 v6 / 0x27000 v7) — passed at RUNTIME from the FW
//en: (fota_running_fw_base() = linker symbol), not a compile-time macro. Thanks to
//en: this there is ONE board-agnostic flasher. WITHOUT an initializer (.bss; .data
//en: would be discarded by ld).
//sk: App flash base (0x26000 v6 / 0x27000 v7) — odovzdaná RUNTIME z FW
//sk: (fota_running_fw_base() = linker symbol), nie compile-time makro. Vďaka tomu
//sk: je flasher JEDEN, board-agnostický. BEZ inicializátora (.bss; .data by ld discardol).
static uint32_t s_app_base;

static hpi_BOOL patch_zlib_read(hpi_TInputStreamHandle h,
                                 hpi_byte* out, hpi_size_t* size) {
    if (*size == 0) return hpi_TRUE;
    puff_stream_t* ps = (puff_stream_t*)h;
    uint32_t n = puff_stream_read(ps, (uint8_t*)out, (uint32_t)*size);
    *size = (hpi_size_t)n;
    return (n > 0 || ps->state == PS_DONE) ? hpi_TRUE : hpi_FALSE;
}

/* ── Flash write listener ───────────────────────────────────────────── */
//en: hpatchi_listener_t MUST be the first member — hpatchi_inplaceB casts the pointer.
//sk: hpatchi_listener_t MUSÍ byť prvý člen — hpatchi_inplaceB castuje pointer.
typedef struct {
    hpatchi_listener_t base;    //en: MUST be first
    uint8_t  page_buf[PAGE_SIZE];
    uint32_t page_used;
    uint32_t current_page;
} FlashCtx;

static hpi_BOOL flash_read_old(hpatchi_listener_t* l,
                                hpi_pos_t pos,
                                hpi_byte* out, hpi_size_t size) {
    (void)l;
    if ((uint32_t)pos + (uint32_t)size > (APP_FLASH_END - s_app_base)) return hpi_FALSE;
    memcpy(out, (const uint8_t*)(s_app_base + (uint32_t)pos), size);
    return hpi_TRUE;
}

//en: DRY-RUN: the flasher goes through the whole process (puff + hpatchi + simulated
//en: write) but does NOT WRITE to the app flash → the device won't brick during debug.
//en: Set FLASHER_DRYRUN=0 for a real write.
//sk: DRY-RUN: flasher prejde celý proces (puff + hpatchi + simulovaný zápis)
//sk: ale NEZAPÍŠE do app flash → zariadenie sa nezabrickuje pri debugu.
//sk: Nastav FLASHER_DRYRUN=0 pre ostrý zápis.
#ifndef FLASHER_DRYRUN
#   define FLASHER_DRYRUN 0   //en: 0 = real write (protected by verify+DFU); 1 = dry-run debug
#endif

//en: FNV-1a hash of the output (new FW) — verification of patch correctness.
//en: WITHOUT an initializer (otherwise .data → the ld script discards it). Init at runtime.
//sk: FNV-1a hash výstupu (nového FW) — verifikácia správnosti patchu.
//sk: BEZ inicializátora (inak .data → ld script ju discard-uje). Init za behu.
static uint32_t s_out_fnv;

static hpi_BOOL flash_write_new(hpatchi_listener_t* l,
                                 const hpi_byte* data, hpi_size_t size) {
    FlashCtx* c = (FlashCtx*)l;
    for (hpi_size_t i = 0; i < size; i++) {   //en: FNV-1a over the output bytes
        s_out_fnv ^= (uint32_t)data[i];
        s_out_fnv *= 16777619u;
    }
    while (size > 0) {
        uint32_t space = PAGE_SIZE - c->page_used;
        uint32_t n = ((uint32_t)size < space) ? (uint32_t)size : space;
        memcpy(c->page_buf + c->page_used, data, n);
        c->page_used += n;
        data += n;
        size -= n;
        if (c->page_used == PAGE_SIZE) {
#if !FLASHER_DRYRUN
            uint32_t addr = s_app_base + c->current_page * PAGE_SIZE;
            nvmc_erase_page(addr);
            nvmc_write_page(addr, c->page_buf);
#else
            wdt_feed();   //en: dry-run: don't write the app flash, just feed the WDT
#endif
            c->current_page++;
            c->page_used = 0;
        }
    }
    return hpi_TRUE;
}

//en: ── The actual implementation ──
//en: Called from flasher_entry after the SP is set up.
//en: Stack space: ~256kB minus overhead (NVIC/SP set up by flasher_entry).
//sk: ── Skutočná implementácia ──
//sk: Volaná z flasher_entry po nastavení SP.
//sk: Stack priestor: ~256kB minus overhead (NVIC/SP nastavuje flasher_entry).
void flasher_main(uint32_t patch_addr, uint32_t patch_size,
                   uint32_t new_fw_size, uint32_t app_base) {
    (void)new_fw_size;
    s_app_base = app_base;   //en: board-agnostic base from the FW (linker symbol), not a macro
    //en: Disable IRQs — the flasher runs OUTSIDE the OS/SoftDevice context. If an
    //en: interrupt arrived (SysTick/RADIO/USB), the CPU would jump via VTOR into an app
    //en: handler without valid SD/RTOS state → crash/reset. The flasher is purely sequential.
    //sk: Vypni IRQ — flasher beží MIMO OS/SoftDevice kontextu. Ak by prišlo
    //sk: prerušenie (SysTick/RADIO/USB), CPU by skočilo cez VTOR do app handlera
    //sk: bez platného SD/RTOS stavu → crash/reset. Flasher je čisto sekvenčný.
    __asm volatile ("cpsid i" ::: "memory");
    ftrace_init();        //en: erase the trace page 0xF3000

    //en: read_diff backend — depends on the patch format. For a raw patch a direct read
    //en: from RAM (PatchStream), for ZLIB streaming decompression (puff_stream).
    //sk: read_diff backend — podľa formátu patchu. Pre raw patch priame čítanie
    //sk: z RAM (PatchStream), pre ZLIB streaming dekompresia (puff_stream).
    PatchStream ps;                       //en: for a raw (uncompressed) patch
    hpi_TInputStreamHandle diff_handle;
    hpi_TInputStream_read  diff_rd;

    /* ── Detect the compressed format ── */
    if (patch_size >= ZPATCH_HDR_SIZE &&
        *(const uint32_t*)patch_addr == ZPATCH_MAGIC)
    {
        //en: ZLIB: STREAMING — puff_stream decompresses on-demand from the compressed
        //en: patch in RAM (PATCH_RAM_ADDR). No big decompression buffer.
        //en: uncomp/new_fw sizes in the header are informative (hpatchi reads from the patch).
        //sk: ZLIB: STREAMING — puff_stream dekomprimuje on-demand z komprimovaného
        //sk: patchu v RAM (PATCH_RAM_ADDR). Žiadny veľký dekompresný buffer.
        //sk: uncomp/new_fw veľkosti v hlavičke sú informatívne (hpatchi číta z patchu).
        fmark(FM_ZLIB_DETECTED); ftrace(FM_ZLIB_DETECTED);
        puff_stream_init(&s_ps, (const uint8_t*)(patch_addr + ZPATCH_HDR_SIZE),
                         patch_size - ZPATCH_HDR_SIZE);
        diff_handle = &s_ps;
        diff_rd     = patch_zlib_read;
    } else {
        //en: raw HPatchLite patch directly in RAM
        ps.pos = 0; ps.addr = patch_addr; ps.size = patch_size;
        diff_handle = &ps;
        diff_rd     = patch_read;
    }

    /* ── HPatchLite inplaceB (streaming read_diff) ── */
    {
        hpi_compressType compress_type = hpi_compressType_no;
        hpi_pos_t   new_size    = 0;
        hpi_pos_t   uncomp_size = 0;
        hpi_size_t  extra_safe  = 0;

        if (!hpatchi_inplace_open(diff_handle, diff_rd,
                                  &compress_type, &new_size,
                                  &uncomp_size, &extra_safe))
            { fmark(FM_ERR_OPEN); ftrace(FM_ERR_OPEN); goto FAIL; }

        //en: The HPatchLite patch itself must be uncompressed — we compress only the wrapper
        //sk: HPatchLite patch sám musí byť nekomprimovaný — komprimujeme len wrapper
        if (compress_type != hpi_compressType_no) { fmark(FM_ERR_COMPRESS); ftrace(FM_ERR_COMPRESS); goto FAIL; }
        if (extra_safe > MAX_EXTRA_SAFE)          { fmark(FM_ERR_SAFE);    ftrace(FM_ERR_SAFE);     goto FAIL; }
        fmark(FM_OPEN_OK);

        //en: FlashCtx + temp_cache in .bss (not on the stack) — page_buf 4kB + cache 48kB
        //en: (32kB extra_safe + 16kB read cache); fits the 128kB flasher RAM region easily
        //sk: FlashCtx + temp_cache v .bss (nie na stack) — page_buf 4kB + cache 48kB
        //sk: (32kB extra_safe + 16kB read cache); do 128kB flasher RAM regiónu sa zmestí ľahko
        static FlashCtx fc;
        static uint8_t  s_temp_cache[MAX_EXTRA_SAFE + READ_CACHE];
        memset(&fc, 0, sizeof(fc));
        fc.base.diff_data = diff_handle;
        fc.base.read_diff = diff_rd;
        fc.base.read_old  = flash_read_old;
        fc.base.write_new = flash_write_new;

        s_out_fnv = 2166136261u;   //en: init FNV-1a before the patch
        if (!hpatchi_inplaceB(&fc.base, new_size,
                              s_temp_cache, extra_safe, (hpi_size_t)sizeof(s_temp_cache)))
            { fmark(FM_ERR_PATCH); ftrace(FM_ERR_PATCH); goto FAIL; }
        fmark(FM_PATCH_OK); ftrace(FM_PATCH_OK);

        //en: Flush the last incomplete page
        if (fc.page_used > 0u) {
            memset(fc.page_buf + fc.page_used, 0xFF,
                   PAGE_SIZE - fc.page_used);
#if !FLASHER_DRYRUN
            uint32_t addr = s_app_base + fc.current_page * PAGE_SIZE;
            nvmc_erase_page(addr);
            nvmc_write_page(addr, fc.page_buf);
#endif
        }
        fmark(FM_FLUSH_OK);
        ftrace(0xD0u);            //en: marker: FNV-1a of the hpatchi output follows
        ftrace(s_out_fnv);        //en: 32-bit hash of the new FW (what hpatchi produced)

#if !FLASHER_DRYRUN && !FLASHER_DEBUG
        //en: ── Post-write verification: read BACK the written app flash, compute
        //en: FNV-1a and compare with the hpatchi output. If the NVMC write failed/was
        //en: partial (flash != hpatchi output) → jump into the DFU bootloader instead of
        //en: booting a corrupted FW (the device can be recovered over USB, not a brick).
        //sk: ── Verifikácia po zápise: prečítaj SPÄŤ zapísanú app flash, spočítaj
        //sk: FNV-1a a porovnaj s hpatchi výstupom. Ak NVMC zápis zlyhal/čiastočný
        //sk: (flash != hpatchi výstup) → skok do DFU bootloadera namiesto bootu
        //sk: pokazeného FW (zariadenie sa dá obnoviť cez USB, nie brick).
        {
            uint32_t vfnv = 2166136261u;
            const uint8_t* app = (const uint8_t*)s_app_base;
            for (uint32_t i = 0; i < (uint32_t)new_size; i++) {
                vfnv ^= (uint32_t)app[i];
                vfnv *= 16777619u;
                if ((i & 0x3FFFu) == 0u) wdt_feed();
            }
            ftrace(0xD1u); ftrace(vfnv);   //en: FNV of the flash readback
            if (vfnv != s_out_fnv) {
                ftrace(0xEAu);             /* VERIFY FAIL → DFU */
                /* Adafruit nRF52 bootloader: GPREGRET=0x57 (UF2/DFU magic) + reset */
                *(volatile uint32_t*)0x4000051Cu = 0x57u;
                __asm volatile ("dsb" ::: "memory");
                (*(volatile uint32_t*)0xE000ED0Cu) = 0x05FA0004u;
                __asm volatile ("dsb" ::: "memory");
                while (1);
            }
            ftrace(0xD2u);                 //en: VERIFY OK — flash == hpatchi output
        }
#endif
    }

    //en: SystemReset — we write to SCB->AIRCR
    fmark(FM_RESET); ftrace(FM_RESET);
    __asm volatile ("dsb" ::: "memory");
    (*(volatile uint32_t*)0xE000ED0Cu) = 0x05FA0004u;
    __asm volatile ("dsb" ::: "memory");
    while (1);

FAIL:
    //en: On failure it does NOT hang (with cpsid i it would hang forever without USB) —
    //en: it resets so the app boots and reads the trace log from 0xF3000. WARNING: if the
    //en: flasher already started writing the app flash, the app is corrupted → only the
    //en: bootloader comes up.
    //sk: Pri zlyhaní NEVISÍ (s cpsid i by visel navždy bez USB) — resetuje sa,
    //sk: aby app nabootovala a prečítala trace log z 0xF3000. POZOR: ak flasher
    //sk: už začal písať do app flash, app je corrupted → nabehne len bootloader.
    ftrace(0xFEu);   /* FAIL reached */
    __asm volatile ("dsb" ::: "memory");
    (*(volatile uint32_t*)0xE000ED0Cu) = 0x05FA0004u;
    __asm volatile ("dsb" ::: "memory");
    while (1);
}

//en: ── Entry point — sets its own SP before calling flasher_main ──
//en:
//en: Naked function: the compiler generates no prologue/epilogue.
//en: Parameters are in r0, r1, r2 per ARM AAPCS — preserved across bl flasher_main.
//en:
//en: Why an own SP: the FreeRTOS task stack is 2-4kB, the flasher needs ~60kB
//en: (FlashCtx 4kB + temp_cache 48kB + puff huffman tables ~4kB + overhead).
//en: After sd_softdevice_disable the entire 256kB of RAM is free.
//sk: ── Entry point — nastaví vlastný SP pred volaním flasher_main ──
//sk:
//sk: Naked funkcia: kompilátor negeneruje žiadny prológ/epilóg.
//sk: Parametre sú v r0, r1, r2 podľa ARM AAPCS — zachované pri bl flasher_main.
//sk:
//sk: Prečo vlastný SP: FreeRTOS task stack je 2-4kB, flasher potrebuje ~60kB
//sk: (FlashCtx 4kB + temp_cache 48kB + puff huffman tabuľky ~4kB + overhead).
//sk: Po sd_softdevice_disable je celých 256kB RAM voľných.
__attribute__((naked))
void flasher_entry(uint32_t patch_addr, uint32_t patch_size, uint32_t new_fw_size, uint32_t app_base)
{
    //en: r0..r3 = patch_addr, patch_size, new_fw_size, app_base (AAPCS) — preserved
    //en: for flasher_main. Set the SP via r12 (scratch reg), NOT r3 — otherwise the
    //en: 4th parameter (app_base) would be overwritten before calling flasher_main.
    //sk: r0..r3 = patch_addr, patch_size, new_fw_size, app_base (AAPCS) — zachované
    //sk: pre flasher_main. SP nastav cez r12 (scratch reg), NIE r3 — inak by sa
    //sk: 4. parameter (app_base) prepísal pred volaním flasher_main.
    __asm volatile (
        "ldr r12, =0x20040000\n\t"  /* top of nRF52840 RAM */
        "mov sp, r12\n\t"
        "bl  flasher_main\n\t"
        "1: b 1b\n\t"               /* never reached — flasher_main resets */
        ::: "r12"
    );
    (void)patch_addr; (void)patch_size; (void)new_fw_size; (void)app_base;  /* suppress warnings */
}

#endif  /* FOTA_FLASHER_BUILD */
