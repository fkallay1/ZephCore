#pragma once
// =====================================================================
//en: FotaPatcher.h — FOTA patch application (MeshCore port from FK_lora-sniffer)
//en:
//en: TEST (receive verification):  fota_patch_to_file()
//en:   • old=XIP flash, patch=CustomLFS/patch.bin
//en:   • HPatchLite streaming → SHA256 only (writes nothing)
//en:
//en: PRODUCTION — streaming in-place flasher:  fota_flash_via_flasher()
//en:   • new_fw_size from the patch header
//en:   • malloc(patch_size) ~50kB (one-shot, right before the jump)
//en:   • FotaFS.end() + sd_softdevice_disable()
//en:   • flasher code into 0xEB000 (outside app flash + InternalFS)
//en:   • jump to flasher@0xEB001: streaming HDiffPatch in-place
//en:     old=XIP, patch=RAM, new=APP_FLASH_START (2-page window)
//en:   • flasher: SystemReset — DOES NOT RETURN
//en:
//en: Dependencies: HPatchLite (nrffota/hpatchlite/), puff_stream, flasher_code.h
//en:   generate flasher_code.h with: python nrffota/tools/build_flasher.py
//sk: FotaPatcher.h — Aplikácia FOTA patchu (MeshCore port z FK_lora-sniffer)
//sk:
//sk: ┌─────────────────────────────────────────────────────────────────┐
//sk: │ TEST (overenie príjmu):  fota_patch_to_file()                    │
//sk: │   • old=XIP flash, patch=CustomLFS/patch.bin                    │
//sk: │   • HPatchLite streaming → SHA256 only (nič nezapisuje)         │
//sk: │                                                                 │
//sk: │ PRODUKCIA — streaming in-place flasher:  fota_flash_via_flasher()│
//sk: │   • new_fw_size z patch hlavičky                                │
//sk: │   • malloc(patch_size) ~50kB (jednorazovo, tesne pred skokom)   │
//sk: │   • FotaFS.end() + sd_softdevice_disable()                       │
//sk: │   • flasher kód do 0xEB000 (mimo app flash + InternalFS)        │
//sk: │   • skok na flasher@0xEB001: streaming HDiffPatch in-place      │
//sk: │     old=XIP, patch=RAM, new=APP_FLASH_START (2-page okno)       │
//sk: │   • flasher: SystemReset — NEVRÁTI SA                           │
//sk: └─────────────────────────────────────────────────────────────────┘
//sk:
//sk: Závislosť: HPatchLite (nrffota/hpatchlite/), puff_stream, flasher_code.h
//sk:   flasher_code.h generuj: python nrffota/tools/build_flasher.py
// =====================================================================

#include "FotaState.h"
#include <stdint.h>
#include <stddef.h>   //en: size_t

#include "flash_layout.h"   //en: APP_FLASH_START/MAX, FLASH_PAGE_SIZE

//en: Verifies reception: applies the patch → SHA256 (no writes to flash).
//en: Returns true if the patch is OK and the SHA256 matches.
//en: err/err_sz (optional): short ASCII FAIL reason (or a note on OK) for the CLI.
bool fota_patch_to_file(char* err = nullptr, size_t err_sz = 0);

//en: FS-region flasher: patch→RAM→jump to flasher@0xEB000.
//en: THIS FUNCTION DOES NOT RETURN on success. Returns false only on error (before the jump).
//sk: FS-region flasher: patch→RAM→jump na flasher@0xEB000.
//sk: TÁTO FUNKCIA SA NEVRÁTI ak uspeje. Vracia false len pri chybe (pred skokom).
bool fota_flash_via_flasher();

//en: DEBUG: decompress patch.bin via puff_stream, print the FNV of the whole raw output.
void fota_debug_decompress();

//en: Read the flasher debug marker from GPREGRET2/RESETREAS. Call once in setup()
//en: BEFORE SoftDevice enable. The value is printed via fota_print_flasher_debug().
//sk: Prečítaj debug marker flashera z GPREGRET2/RESETREAS. Volaj raz v setup()
//sk: PRED SoftDevice enable. Hodnota sa vytlačí cez fota_print_flasher_debug().
void fota_check_flasher_debug();

//en: Print the stored flasher debug marker if present (call at banner/connect).
void fota_print_flasher_debug();
