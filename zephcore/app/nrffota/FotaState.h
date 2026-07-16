#pragma once
// =====================================================================
//en: FotaState.h — FOTA session RAM state + persistent header (MeshCore port)
//en:
//en: Port of FK_lora-sniffer/src/ota.h.
// =====================================================================
#include "FotaProtocol.h"

// =====================================================================
//en: Limits
// =====================================================================
#define FOTA_MAX_CHUNKS     1024                        //en: absolute max
#define FOTA_BITMAP_BYTES   ((FOTA_MAX_CHUNKS + 7) / 8)  //en: 128 B

// =====================================================================
//en: Status bits (FotaState.status + FotaMetaPersist.status)
// =====================================================================
#define FOTA_ST_IDLE       0x00   //en: no session
#define FOTA_ST_RECEIVING  0x01   //en: receiving chunks
#define FOTA_ST_COMPLETE   0x02   //en: all chunks received
#define FOTA_ST_VERIFIED   0x04   //en: SHA256 verified
#define FOTA_ST_APPLYING   0x08   //en: applying the patch
#define FOTA_ST_DONE       0x10   //en: done, waiting for reboot
#define FOTA_ST_ERROR      0x80   //en: error — detail in err_code

//en: Error codes
#define FOTA_ERR_NONE      0x00
#define FOTA_ERR_SHA256    0x01   //en: hash mismatch
#define FOTA_ERR_CRC16     0x02   //en: chunk CRC error
#define FOTA_ERR_STORAGE   0x03   //en: LittleFS I/O
#define FOTA_ERR_PATCH     0x04   //en: hdiffpatch failed
#define FOTA_ERR_OVERFLOW    0x05   //en: chunk_idx out of range
#define FOTA_ERR_SIGNATURE   0x06   //en: Ed25519 signature of the HEADER packet is not valid
#define FOTA_ERR_BASEFW      0x07   //en: base FW mismatch — chunk/HEADER for a different FW

// =====================================================================
//en: Persistent header  /ota/meta.bin  (114 B)
//en:
//en: Written only on state transitions (HEADER / COMPLETE / VERIFIED / DONE).
//en: CRC16 covers everything except itself — if the CRC doesn't match, the write
//en: was interrupted and the session is discarded.
//sk: Perzistentná hlavička  /ota/meta.bin  (114 B)
//sk:
//sk: Zapisuje sa len pri stavových prechodoch (HEADER / COMPLETE / VERIFIED / DONE).
//sk: CRC16 pokrýva všetko okrem seba — ak CRC nesedí, zápis bol prerušený
//sk: a session sa zahodí.
// =====================================================================
#define FOTA_META_MAGIC  0x4F544102u   //en: "OTA\x02" — v2: + hdr_signer_prefix (old meta.bin is discarded after FW update)
                                       //sk: "OTA\x02" — v2: + hdr_signer_prefix (starý meta.bin sa po update FW zahodí)

typedef struct __attribute__((packed)) {
    uint32_t magic;             //en: FOTA_META_MAGIC
    uint8_t  status;            //en: FOTA_ST_* — state at the time of writing
    uint8_t  err_code;          //en: FOTA_ERR_*
    uint16_t total_chunks;
    uint32_t patch_size;        //en: [B]
    uint8_t  patch_sha256[32];  //en: SHA256 of patch.bin (to verify reception)
    uint8_t  new_sha256[32];    //en: SHA256 of the new firmware (to verify application)
    uint32_t old_fw_size;       //en: size of the old fw (patch base) — survives reboot/resume
    uint8_t  old_sha256[32];    //en: SHA256 of the old fw — base verification before rewrite
    //en: Unified format v0 — split HEADER (META+SIG), persists across reboot:
    //sk: Zjednotený formát v0 — rozdelený HEADER (META+SIG), perzistuje cez reboot:
    uint8_t  fota_prot_inf;      //en: protocol version from META
    uint8_t  meta_recv;         //en: META received
    uint8_t  sig_recv;          //en: SIG received
    uint8_t  hdr_key_id;        //en: key_id from SIG
    uint8_t  hdr_sig[64];       //en: Ed25519 signature from SIG (verify after META+SIG received)
    uint8_t  hdr_signer_prefix[4]; //en: v0-prefix: first 4 B of signer pubkey (key_id==0)
                                   //sk: v0-prefix: prvé 4 B pubkey podpisovateľa (key_id==0)
    uint16_t crc16;             //en: CRC16 of everything above
} FotaMetaPersist;

// =====================================================================
//en: RAM state  (depends on FotaMetaPersist, bitmap extra)
//en: Total size approx. 176 B
// =====================================================================
typedef struct {
    uint16_t total_chunks;              //en: 0 = no session
    uint32_t patch_size;
    uint8_t  patch_sha256[32];          //en: SHA256 of patch.bin
    uint8_t  new_sha256[32];            //en: SHA256 of the new firmware
    uint32_t old_fw_size;               //en: size of the old fw (patch base), 0 = unknown
    uint8_t  old_sha256[32];            //en: SHA256 of the old fw — base verification before rewrite

    //en: Base FW cache — for fast validation without restarting the SHA256 computation
    //sk: Base FW cache — pre rýchlu validáciu bez reštartu SHA256 výpočtu
    uint32_t base_fw_size;              //en: 0 = not validated
    uint8_t  base_fw_sha256[32];        //en: SHA256 of the FW currently on the device

    uint16_t recv_count;               //en: recomputed from bitmap on resume
    uint8_t  status;                   //en: FOTA_ST_*
    uint8_t  err_code;                 //en: FOTA_ERR_*

    //en: Unified format v0 — split HEADER (META+SIG):
    //sk: Zjednotený formát v0 — rozdelený HEADER (META+SIG):
    uint8_t  fota_prot_inf;             //en: version from META
    uint8_t  meta_recv;                //en: META received
    uint8_t  sig_recv;                 //en: SIG received
    uint8_t  hdr_key_id;               //en: key_id from SIG
    uint8_t  hdr_sig[64];              //en: Ed25519 signature from SIG (verify after META+SIG received)
    uint8_t  hdr_signer_prefix[4];     //en: v0-prefix: first 4 B of signer pubkey (key_id==0)
                                       //sk: v0-prefix: prvé 4 B pubkey podpisovateľa (key_id==0)

    //en: bitmap: bit N = 1 → chunk N received and written (128 B covers 1024 chunks)
    //sk: bitová mapa: bit N = 1 → chunk N prijatý a zapísaný (128 B pokryje 1024 chunkov)
    uint8_t  bitmap[FOTA_BITMAP_BYTES];
} FotaState;

// =====================================================================
//en: LittleFS paths  (shared by the receiver and the patcher)
// =====================================================================
#if defined(FOTA_ZEPHCORE_BUILD)
//en: ZephCore: shared /lfs partition, FOTA subdirectory
//sk: ZephCore: zdielana /lfs particia, FOTA podadresar
#define FOTA_FS_DIR     "/lfs/fota"
#define FOTA_FS_META    "/lfs/fota/meta.bin"
#define FOTA_FS_BITMAP  "/lfs/fota/bitmap.bin"
#define FOTA_FS_LOG     "/lfs/fota/recv.log"
#define FOTA_FS_PATCH   "/lfs/fota/patch.bin"
#else
//en: MeshCore: dedicated CustomLFS @ 0xD4000
//sk: MeshCore: dedikovany CustomLFS @ 0xD4000
#define FOTA_FS_DIR     "/ota"
#define FOTA_FS_META    "/ota/meta.bin"
#define FOTA_FS_BITMAP  "/ota/bitmap.bin"
#define FOTA_FS_LOG     "/ota/recv.log"
#define FOTA_FS_PATCH   "/ota/patch.bin"
#endif

// =====================================================================
//en: Authorization table for Ed25519 verification (defined in FotaReceiver_signkey.cpp).
//en: Keys are addressed by pubkey prefix (v0-prefix format); legacy key_id N maps
//en: to s_authors[N-1] (test key = index 0 = legacy key_id 1).
//sk: Autorizačná tabuľka pre Ed25519 verify (definovaná vo FotaReceiver_signkey.cpp).
//sk: Kľúče sa adresujú prefixom pubkey (v0-prefix formát); legacy key_id N mapuje
//sk: na s_authors[N-1] (test kľúč = index 0 = legacy key_id 1).
// =====================================================================
#define FOTA_MAX_AUTHORS  8

typedef struct {
    uint8_t  pub_key[32];  //en: Ed25519 public key
} FotaAuthorEntry;

//en: External declaration of the authors table
extern const FotaAuthorEntry s_authors[];
extern const int s_author_count;

// =====================================================================
//en: Bit macros (no runtime overhead)
// =====================================================================
#define FOTA_BIT_SET(bm, n)  ((bm)[(n) >> 3] |=  (uint8_t)(1u << ((n) & 7u)))
#define FOTA_BIT_CLR(bm, n)  ((bm)[(n) >> 3] &= (uint8_t)(~(1u << ((n) & 7u))))
#define FOTA_BIT_GET(bm, n)  (((bm)[(n) >> 3] >> ((n) & 7u)) & 1u)
