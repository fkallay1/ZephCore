#pragma once
// =====================================================================
//en: flash_layout.h — FOTA flash map for the MeshCore nRF52840 repeater.
//sk: flash_layout.h — FOTA flash mapa pre MeshCore nRF52840 repeater.
//
//en: NUMERIC macros only → freestanding-safe (also compiled into the
//en: standalone flasher in tools/build_flasher.py, which runs without
//en: Arduino/newlib).
//sk: Iba NUMERICKÉ makrá → freestanding-safe (kompiluje sa aj do standalone
//sk: flashera v tools/build_flasher.py, ktorý beží bez Arduino/newlib).
//
//en: ASSUMPTION: the app is linked via boards/nrf52840_s140_v6_extrafs.ld
//en: (or _v7_extrafs.ld), where the app flash ENDS at 0xD4000 — same as
//en: companion_radio. That frees the 0xD4000-0xF4000 window (128kB) for FS.
//sk: PREDPOKLAD: app je linkovaná cez boards/nrf52840_s140_v6_extrafs.ld
//sk: (alebo _v7_extrafs.ld), kde app flash KONČÍ na 0xD4000 — rovnako ako
//sk: companion_radio. To uvoľní okno 0xD4000-0xF4000 (128kB) pre FS.
//
//en: Layout (same principle as FK_lora-sniffer, adapted to MeshCore):
//en:   APP_FLASH_START - 0xD4000 : application code (repeater FW)
//en:   0xD4000 - 0xEB000         : FOTA FS (CustomLFS, 92kB — recv.log/patch.bin/meta/bitmap)
//en:   0xEB000 - 0xEC000         : flasher code (4kB ARM Thumb2)
//en:   0xEC000 - 0xED000         : flasher metadata + trace log (4kB)
//en:   0xED000 - 0xF4000         : MeshCore InternalFS (28kB, UNTOUCHED — identity/prefs/ACL)
//en:   0xF4000+                  : bootloader
//sk: Layout (zhodný princíp s FK_lora-sniffer, prispôsobený MeshCore):
//sk:   APP_FLASH_START - 0xD4000 : aplikačný kód (repeater FW)
//sk:   0xD4000 - 0xEB000         : FOTA FS (CustomLFS, 92kB — recv.log/patch.bin/meta/bitmap)
//sk:   0xEB000 - 0xEC000         : flasher kód (4kB ARM Thumb2)
//sk:   0xEC000 - 0xED000         : flasher metadata + trace log (4kB)
//sk:   0xED000 - 0xF4000         : MeshCore InternalFS (28kB, NEDOTKNUTÝ — identity/prefs/ACL)
//sk:   0xF4000+                  : bootloader
//
//en: The flasher runs from 0xEB000 — OUTSIDE the application flash
//en: (0x26000-0xD4000) and OUTSIDE InternalFS (0xED000+), so it can safely
//en: rewrite the app flash without destroying itself or identity/prefs.
//sk: Flasher beží z 0xEB000 — MIMO aplikačnej flash (0x26000-0xD4000) aj
//sk: MIMO InternalFS (0xED000+), takže môže bezpečne prepisovať app flash
//sk: bez sebazničenia a bez poškodenia identity/prefs.
//
//en: The only difference between boards is APP_FLASH_START (SoftDevice size):
//en:   s140 v6  → 0x26000  (ProMicro, Heltec T096, ...)  — default
//en:   s140 v7  → 0x27000  (Seeed XIAO nRF52840, ...)
//sk: Jediný rozdiel medzi boardmi je APP_FLASH_START (veľkosť SoftDevice):
//sk:   s140 v6  → 0x26000  (ProMicro, Heltec T096, ...)  — default
//sk:   s140 v7  → 0x27000  (Seeed XIAO nRF52840, ...)
//
//en: NOTE: the APP_FLASH_START macro is LEGACY / compile-time fallback today.
//en: Neither the running FW (FotaReceiver/FotaPatcher) nor the flasher USE it
//en: — the app base is taken from the linker symbol __flash_arduino_start
//en: (ORIGIN(FLASH) of the active ld) via fota_running_fw_base() and passed
//en: to the flasher at runtime. Hence no board flag is needed.
//en: APP_FLASH_END (0xD4000) is board-independent (FS window).
//sk: POZOR: APP_FLASH_START makro je dnes LEGACY/compile-time fallback. Bežiaci FW
//sk: (FotaReceiver/FotaPatcher) ani flasher ho NEPOUŽÍVAJÚ — app base sa berie z
//sk: linker symbolu __flash_arduino_start (ORIGIN(FLASH) aktívneho ld) cez
//sk: fota_running_fw_base() a odovzdáva flasheru runtime. Preto NETREBA board flag.
//sk: APP_FLASH_END (0xD4000) je board-nezávislé (FS okno).
// =====================================================================

#if defined(BOARD_XIAO) || defined(FOTA_SOFTDEVICE_V7)
  //en: SoftDevice s140 v7.x
  #define APP_FLASH_START      0x27000u
#elif defined(FOTA_APP_FLASH_START)
  //en: explicit override from build_flags
  #define APP_FLASH_START      FOTA_APP_FLASH_START
#else
  //en: SoftDevice s140 v6.1.1 (default — ProMicro and most nRF52840 boards)
  #define APP_FLASH_START      0x26000u
#endif

#if defined(FOTA_ZEPHCORE_BUILD)
//en: ── ZephCore map ──────────────────────────────────────────────────────
//en: app 0x26000/0x27000-0xD0000 (code_partition) · NVS 0xD0000 (16kB) ·
//en: /lfs 0xD4000 (128kB, SHARED — FOTA files under /lfs/fota) · UF2 0xF4000.
//en: No free flash → the flasher runs from RAM by default: the blob is copied
//en: to FLASHER_RAM_ADDR right before the jump; its .bss+stack live above it
//en: (see flasher/flasher.ld). Flash-resident flasher (future power-loss
//en: recovery) needs FOTA_FLASHER_IN_FLASH + explicit addresses from a DTS
//en: partition. Trace page likewise (FOTA_FLASHER_TRACE).
//sk: ── ZephCore mapa ─────────────────────────────────────────────────────
//sk: app 0x26000/0x27000-0xD0000 (code_partition) · NVS 0xD0000 (16kB) ·
//sk: /lfs 0xD4000 (128kB, ZDIELANY — FOTA subory pod /lfs/fota) · UF2 0xF4000.
//sk: Vo flashi nie je volne miesto → flasher bezi default z RAM: blob sa
//sk: kopiruje na FLASHER_RAM_ADDR tesne pred skokom; jeho .bss+stack su nad
//sk: nim (vid flasher/flasher.ld). Flash-rezidentny flasher (buduce power-loss
//sk: recovery) vyzaduje FOTA_FLASHER_IN_FLASH + explicitne adresy z DTS
//sk: particie. Trace stranka rovnako (FOTA_FLASHER_TRACE).
#define APP_FLASH_END          0xD0000u
#define APP_FLASH_MAX          (APP_FLASH_END - APP_FLASH_START)
#define FLASH_PAGE_SIZE        4096u

//en: sanity bound for patch sizes (shared /lfs partition size)
//sk: horny limit velkosti patchu (velkost zdielanej /lfs particie)
#define FOTA_FS_FLASH_SIZE      0x20000u

//en: RAM flasher window = TOP 8kB of RAM (0x2003E000-0x20040000), RESERVED from
//en: Zephyr via boards/common/fota.overlay (sram0 shrunk to 248kB) — the kernel
//en: image spans past 0x20020000 (measured _image_ram_end 0x2002516c), so the
//en: blob copy destination must live outside app RAM. Flasher stack starts AT
//en: the code origin and grows DOWN into dead-app RAM; .bss stays at 0x20020000
//en: (dead app RAM at run time). Breadcrumb word: code origin + 0x1F00.
//sk: RAM flasher okno = VRCHNYCH 8kB RAM (0x2003E000-0x20040000), REZERVOVANE
//sk: pred Zephyrom cez boards/common/fota.overlay (sram0 zmenseny na 248kB) —
//sk: kernel image siaha za 0x20020000 (namerane _image_ram_end 0x2002516c),
//sk: takze ciel kopie blobu musi byt mimo app RAM. Stack flashera zacina NA
//sk: code origine a rastie DOLE do mrtvej app RAM; .bss ostava na 0x20020000
//sk: (pocas behu flashera uz mrtva app RAM). Breadcrumb word: code origin + 0x1F00.
#define FLASHER_RAM_ADDR       0x2003E000u
//en: breadcrumb word OUTSIDE the top-of-RAM bootloader startup stack (SP=0x20040000
//en: wipes the top ~kBs on every reset) — dead zone above flasher .bss, below its stack
//sk: breadcrumb word MIMO startovacieho stacku bootloadera na vrchu RAM (SP=0x20040000
//sk: prepise vrchne ~kB pri kazdom resete) — mrtva zona nad flasher .bss, pod jeho stackom
#define FLASHER_MARK_RAM_ADDR  0x20036000u

#if defined(FOTA_FLASHER_IN_FLASH) && !defined(FLASHER_CODE_ADDR)
  #error "FOTA_FLASHER_IN_FLASH: define FLASHER_CODE_ADDR (dedicated DTS partition)"
#endif
//en: flasher trace page = LAST page of the app window (0xCF000-0xD0000). The FW
//en: image ends far below (~0x5E200), the in-place patcher writes only
//en: [base, new_size) — the page is effectively free and survives power-cycle.
//en: The flasher writes it only when built with FLASHER_DEBUG=1
//en: (build_flasher.py --platform zephcore does this while FOTA is stabilised).
//sk: trace stranka flashera = POSLEDNA stranka app okna (0xCF000-0xD0000). FW
//sk: image konci hlboko pod nou (~0x5E200), in-place patcher pise len
//sk: [base, new_size) — stranka je fakticky volna a prezije power-cycle.
//sk: Flasher ju pise len ked je blob s FLASHER_DEBUG=1 (build_flasher.py
//sk: --platform zephcore pocas stabilizacie FOTA).
#ifndef FLASH_TRACE_ADDR
  #define FLASH_TRACE_ADDR     0xCF000u
#endif

#else /* FOTA_MESHCORE_BUILD alebo standalone flasher build (FOTA_FLASHER_BUILD) */
//en: Common to all boards (the FS window does not depend on SoftDevice size)
#define APP_FLASH_END          0xD4000u
#define APP_FLASH_MAX          (APP_FLASH_END - APP_FLASH_START)
#define FLASH_PAGE_SIZE        4096u

#define FOTA_FS_FLASH_ADDR      0xD4000u
#define FOTA_FS_FLASH_SIZE      (0xEB000u - 0xD4000u)   //en: 92kB (23 pages x 4096)
#define FOTA_FS_BLOCK_SIZE      128u                    //en: LittleFS block = 128B

#define FLASHER_CODE_ADDR      0xEB000u                //en: 4kB ARM Thumb2 flasher code
#define FLASHER_META_ADDR      0xEC000u                //en: 4kB metadata + trace log
#define FLASH_TRACE_ADDR       FLASHER_META_ADDR
#endif /* FOTA_ZEPHCORE_BUILD */

//en: Max extraSafeSize accepted from the hpatchi patch header — SINGLE SOURCE for
//en: the flasher (hard reject 0xE5 + temp_cache sizing) AND the app-side verify/apply
//en: pre-checks in FotaPatcher.cpp. extraSafeSize grows with how far the FW image
//en: content SHIFTS between old and new (≈ how much the FW grew); a patch needing
//en: more than this limit degenerated to ~full-image before, or got rejected.
//en: RAM cost: flasher .bss temp_cache = FOTA_MAX_EXTRA_SAFE + read cache — lives in
//en: the flasher-only RAM region (128kB @ 0x20020000, whole RAM is free during flash),
//en: so it costs the running app NOTHING. See fkclaude/fcl_readme_fota_extrasafe.md.
//sk: Max extraSafeSize akceptovaný z hpatchi hlavičky patchu — JEDINÝ ZDROJ pre
//sk: flasher (tvrdý reject 0xE5 + veľkosť temp_cache) AJ pre app-side verify/apply
//sk: predkontroly vo FotaPatcher.cpp. extraSafeSize rastie s tým, o koľko sa obsah
//sk: FW obrazu POSUNIE medzi starým a novým (≈ o koľko FW narástol); patch nad limit
//sk: predtým degeneroval na ~celý obraz, alebo bol odmietnutý.
//sk: RAM cena: flasher .bss temp_cache = FOTA_MAX_EXTRA_SAFE + read cache — žije vo
//sk: flasher-only RAM regióne (128kB @ 0x20020000, počas flashovania je voľná celá RAM),
//sk: takže bežiacu appku nestojí NIČ. Viď fkclaude/fcl_readme_fota_extrasafe.md.
#define FOTA_MAX_EXTRA_SAFE    32768u
