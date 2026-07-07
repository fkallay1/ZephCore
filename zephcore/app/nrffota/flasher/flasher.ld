/* flasher.ld — linker script pre standalone flasher binary (MeshCore)
 *
 * Cieľová adresa: 0xEB000 (mimo FOTA FS, app flash aj InternalFS)
 * Kód a rodata: FLASH 4kB
 * BSS (NOLOAD):  RAM  — iba pre linker, nepotrebuje inicializáciu,
 *                       objcopy vytiahne len .text (flasher_code.h)
 */

/* STREAMING flasher: patch sa NEdekomprimuje celý do RAM. Spodných 128kB RAM
 * (0x20000000-0x20020000) je vstupný KOMPRIMOVANÝ patch (~60kB, PATCH_RAM_ADDR),
 * z ktorého puff_stream streamuje. Flasher .bss (puff_stream_t + FlashCtx +
 * temp_cache 48kB = 32kB FOTA_MAX_EXTRA_SAFE + 16kB read cache) + stack idú
 * NAD patch (0x20020000-0x20040000 = 128kB) — využitých ~60kB, rezerva bohatá.
 * Po sd_softdevice_disable je celá RAM voľná. */
/* ORIGIN kodu je parametrizovany cez ld --defsym FLASHER_ORIGIN=<addr>
 * (build_flasher.py --origin). Default 0xEB000 = MeshCore flash mapa;
 * ZephCore RAM flasher pouziva 0x20020000 (kod v RAM, .bss hned nad nim). */
__flasher_origin = DEFINED(FLASHER_ORIGIN) ? FLASHER_ORIGIN : 0xEB000;
/* RAM origin .bss: MeshCore default 0x20020000 (hned nad patch oblastou).
 * ZephCore RAM flasher ma KOD na 0x20020000 -> .bss sa posuva na 0x20021000
 * (build_flasher.py --platform zephcore prida --defsym FLASHER_RAM_ORIGIN). */
__flasher_ram = DEFINED(FLASHER_RAM_ORIGIN) ? FLASHER_RAM_ORIGIN : 0x20020000;
/* Velkost kodovej oblasti: flash flasher je viazany 4kB strankou (0xEB000);
 * RAM flasher (ZephCore) limit nema -> 8kB (--defsym FLASHER_CODE_LEN). */
__flasher_code_len = DEFINED(FLASHER_CODE_LEN) ? FLASHER_CODE_LEN : 4096;

MEMORY {
    CODE (rx)   : ORIGIN = __flasher_origin, LENGTH = __flasher_code_len
    RAM   (rwx) : ORIGIN = __flasher_ram, LENGTH = 0x1F000   /* .bss + stack, nad patchom */
}

SECTIONS {
    .text : {
        KEEP(*(.text.flasher_entry))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > CODE

    /* BSS — linker potrebuje vedieť kam, ale nie je súčasťou .bin */
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } > RAM

    /DISCARD/ : { *(.data*) *(.comment) *(.ARM*) }
}
