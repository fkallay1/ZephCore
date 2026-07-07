#pragma once
// =====================================================================
//en: FwId.h — FW identity trailer baked into the firmware at build time.
//en:
//en: The struct lives in .rodata (flash) — WITHOUT any ld script change. It is
//en: located by its 8-byte magic (FWID_MAGIC). The image_size and sha256 fields
//en: are filled in by the POST-build script test_nrf-fota/gen_fw_trailer.py
//en: directly in firmware.hex (before PackageDfu makes the .zip / uf2conv the
//en: .uf2 from it — so all artifacts carry them).
//en:
//en: SHA256 CONVENTION (the script and the device-side verification must agree):
//en:   sha256 = SHA256( the whole app image [APP_FLASH_START .. +image_size)
//en:                    with the sha256[] field zeroed )
//en: build_number and image_size ARE part of the hashed area (filled in before
//en: the hash is computed), so the device does NOT zero them before hashing —
//en: it zeroes only sha256[].
//en:
//en: image_size matches firmware.bin from the DFU zip (= old_fw_size from the
//en: sender) and fw_image_size() from linker symbols
//en: (__etext + SIZEOF(.data) - APP_FLASH_START).
//sk: FwId.h — FW identity trailer zapečený do firmvéru pri builde.
//sk:
//sk: Štruktúra leží v .rodata (flash) — BEZ zmeny ld scriptu. Nájde sa podľa
//sk: 8-bajtového magicu (FWID_MAGIC). Polia image_size a sha256 vyplní POST-build
//sk: skript test_nrf-fota/gen_fw_trailer.py priamo vo firmware.hex (pred tým, než
//sk: z neho PackageDfu vyrobí .zip / uf2conv .uf2 — takže ich nesú všetky artefakty).
//sk:
//sk: KONVENCIA SHA256 (musí sedieť skript aj device-side overenie):
//sk:   sha256 = SHA256( celý app image [APP_FLASH_START .. +image_size)
//sk:                    so sha256[] poľom vynulovaným )
//sk: build_number aj image_size SÚ súčasťou hashovanej oblasti (vyplnené pred
//sk: výpočtom), takže device ich pred hashom NEnuluje — nuluje len sha256[].
//sk:
//sk: image_size sa zhoduje s firmware.bin z DFU zipu (= old_fw_size od sendera)
//sk: a s fw_image_size() z linker symbolov (__etext + SIZEOF(.data) - APP_FLASH_START).
// =====================================================================
#include <stdint.h>

#define FWID_MAGIC      "FKFWID01"   //en: 8 bytes, no NUL terminator
#define FWID_MAGIC_LEN  8

typedef struct __attribute__((packed)) {
    char     magic[8];      //en: "FKFWID01"
    uint32_t image_size;    //en: LE — app image size (filled in post-build)
    uint32_t build_number;  //en: LE — FW_BUILD_NUMBER (compile-time)
    uint8_t  sha256[32];    //en: SHA256 of the image with sha256[]=0 (filled in post-build)
} FwIdTrailer;               //en: 48 B

//en: Instance (definition in FwId.cpp). const → .rodata → flash.
extern const FwIdTrailer fw_id_trailer;
