// =====================================================================
//en: FotaBuffer.cpp — implementation of the shared FOTA scratch.
//en: Today: one static buffer in .bss. Swap to malloc/free = only these 2 fns.
//sk: FotaBuffer.cpp — implementácia zdieľaného FOTA scratchu.
//sk: Dnes: jeden statický buffer v .bss. Swap na malloc/free = len tieto 2 fn.
// =====================================================================
#ifdef WITH_LORA_FOTA
#include "FotaBuffer.h"
#include "FotaDebug.h"
#include <Arduino.h>

//en: The only place that holds "where" the memory is. Aligned(4) for possible
//en: future word-oriented use.
//sk: Jediné miesto, ktoré drží "odkiaľ" je pamäť. Aligned(4) pre prípadné
//sk: budúce word-orientované použitie.
static uint8_t s_fota_buf[FOTA_BUF_CAP] __attribute__((aligned(4)));
static bool    s_fota_buf_in_use = false;

uint8_t* fota_get_buffer(uint32_t need) {
    if (need > FOTA_BUF_CAP) {
        FOTA_DEBUG_PRINTLN("[FOTA] buffer: need %lu B > cap %u B", (unsigned long)need, (unsigned)FOTA_BUF_CAP);
        return nullptr;
    }
    if (s_fota_buf_in_use) {
        FOTA_DEBUG_PRINTLN("[FOTA] buffer: už požičaný (reentrancia?)");
        return nullptr;
    }
    s_fota_buf_in_use = true;
    return s_fota_buf;
    //en: SWAP to heap: `return (uint8_t*)malloc(need);` (drop the in_use logic)
    //sk: SWAP na heap: `return (uint8_t*)malloc(need);` (zruš in_use logiku)
}

void fota_put_buffer(uint8_t* p) {
    if (p != s_fota_buf) {
        FOTA_DEBUG_PRINTLN("[FOTA] buffer: put cudzí smerník — ignorujem");
        return;
    }
    s_fota_buf_in_use = false;
    //en: SWAP to heap: `free(p);`
    //sk: SWAP na heap: `free(p);`
}

#endif // WITH_LORA_FOTA
