// =====================================================================
//en: FotaReceiver_signkey.cpp — authorization keys for Ed25519 verification of the FOTA HEADER
//
#ifdef WITH_LORA_FOTA
#include "FotaState.h"

//en: Authorization keys — extern → visible to the linker from other .cpp modules
//sk: Autorizačné kľúče — extern → viditeľné pre linker z iných .cpp módulov
extern const FotaAuthorEntry s_authors[] = {
    //en: key_id=1 — test keypair (privkey in test_nrf-fota/test_key.der)
    //sk: key_id=1 — test keypair (privkey v test_nrf-fota/test_key.der)
    { 1, { 0xC2, 0x2F, 0x8A, 0xE0, 0x03, 0x51, 0xE7, 0x4A,
           0x81, 0x33, 0x8A, 0x93, 0xB8, 0x6C, 0x87, 0x45,
           0x8F, 0x05, 0xC4, 0xDD, 0x6A, 0xE9, 0xCE, 0xA5,
           0x49, 0xB0, 0x58, 0xE2, 0x3A, 0x0B, 0x5B, 0x51 } },
};

const int s_author_count = sizeof(s_authors) / sizeof(s_authors[0]);

#endif  // WITH_LORA_FOTA
