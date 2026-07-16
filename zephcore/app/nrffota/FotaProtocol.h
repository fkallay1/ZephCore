#pragma once
// =====================================================================
//en: FotaProtocol.h — FOTA-over-LoRa on-air protocol (MeshCore port)
//en:
//en: Port of FK_lora-sniffer/src/fota_proto.h. Portable: no Arduino
//en: dependencies, usable on the PC side too (fota_sender.py / ctypes).
//en:
//en: In MeshCore, FOTA packets arrive wrapped in GRP_DATA (AES-128-ECB +
//en: HMAC-SHA256); the repeater decrypts them via mesh::Utils::MACThenDecrypt
//en: and hands them to FotaReceiver in onGroupDataRecv(). The plaintext is:
//en:   [ts 4B LE][fota_type 1B][...]   — the FOTA payload starts at fota_type.
//sk: FotaProtocol.h — on-air protokol FOTA-over-LoRa (MeshCore port)
//sk:
//sk: Port z FK_lora-sniffer/src/fota_proto.h. Portable: žiadne Arduino
//sk: závislosti, použiteľné aj na PC strane (fota_sender.py / ctypes).
//sk:
//sk: V MeshCore prichádzajú FOTA pakety zabalené v GRP_DATA (AES-128-ECB +
//sk: HMAC-SHA256), repeater ich dešifruje cez mesh::Utils::MACThenDecrypt
//sk: a v onGroupDataRecv() odovzdá do FotaReceiver. Plaintext má tvar:
//sk:   [ts 4B LE][fota_type 1B][...]   — FOTA payload začína na fota_type.
// =====================================================================
#include <stdint.h>

// =====================================================================
//en: FOTA packet types (first byte of the FOTA payload — after the 4B GRP_DATA timestamp)
// =====================================================================
#define FOTA_PKT_HEADER    0x10   //en: PC → device: META (patch metadata, signed)
#define FOTA_PKT_CHUNK    0x11   //en: PC → device: one chunk of patch data
#define FOTA_PKT_APPLY    0x12   //en: PC → device: apply the patch
#define FOTA_PKT_HDR_SIG   0x13   //en: PC → device: SIG (Ed25519 signature of META) — 2nd part of HEADER
#define FOTA_PKT_STATUS   0x20   //en: device → PC: reception status
#define FOTA_PKT_NACK     0x21   //en: device → PC: missing chunks

//en: Unified FOTA format v0 (bridge and companion) — see
//en: fkclaude/docs/superpowers/specs/2026-06-23-fota-companion-mcpy-design.md
//sk: Zjednotený FOTA formát v0 (bridge aj companion) — viď
//sk: fkclaude/docs/superpowers/specs/2026-06-23-fota-companion-mcpy-design.md
#define FOTA_MAGIC         0x07A0 //en: GRP_DATA data_type for FOTA (gating discriminator)
#define FOTA_PROT_INF_V0   0x00   //en: version of the FOTA protocol/structs

//en: key_id=0 in SIG = "v0-prefix" format: signer is identified by the first
//en: 4 B of their Ed25519 pubkey APPENDED after signature[] (SIG = 99+4 = 103 B).
//en: The receiver looks the prefix up in s_authors, then in the ACL admins.
//en: key_id>=1 = legacy 99 B format (s_authors[key_id-1]) for old FW.
//sk: key_id=0 v SIG = "v0-prefix" formát: podpisovateľa identifikujú prvé
//sk: 4 B jeho Ed25519 pubkey PRIPOJENÉ za signature[] (SIG = 99+4 = 103 B).
//sk: Receiver hľadá prefix v s_authors, potom v ACL adminoch.
//sk: key_id>=1 = legacy 99 B formát (s_authors[key_id-1]) pre staré FW.
#define FOTA_KEY_ID_PREFIX   0x00
#define FOTA_SIG_PREFIX_LEN  4

//en: Max data in one FOTA_CHUNK. Via standard GRP_DATA (sendGroupData),
//en: data_len ≤ MAX_GROUP_DATA_LENGTH(165); data = [ts 4B] + chunk(13 + DATA),
//en: so DATA ≤ 148. We pick 144 with margin (matches fota_sender.py FOTA_CHUNK_DATA).
//sk: Max dát v jednom FOTA_CHUNK. Cez štandardný GRP_DATA (sendGroupData) je
//sk: data_len ≤ MAX_GROUP_DATA_LENGTH(165); data = [ts 4B] + chunk(13 + DATA),
//sk: teda DATA ≤ 148. Volíme 144 s rezervou (zhodné s fota_sender.py FOTA_CHUNK_DATA).
#define FOTA_CHUNK_DATA_MAX  144

//en: Max chunks that fit into one FOTA_NACK
#define FOTA_NACK_MAX_IDX    60

// =====================================================================
//en: On-air structures (packed, no padding)
// =====================================================================

//en: FOTA_PKT_HEADER — META, 102 B = the WHOLE signed message (Ed25519). Fits into
//en: GRP_DATA (data_len = 4B ts + 102 = 106 ≤ 165). total_chunks is NOT sent
//en: (derived from patch_size/FOTA_CHUNK_DATA_MAX), old_sha256_prefix is NOT sent
//en: (= old_sha256[:4]) — both are functions of signed fields → integrity preserved.
//sk: FOTA_PKT_HEADER — META, 102 B = CELÁ podpisovaná správa (Ed25519). Zmestí sa do
//sk: GRP_DATA (data_len = 4B ts + 102 = 106 ≤ 165). total_chunks sa NEposiela
//sk: (odvodí sa z patch_size/FOTA_CHUNK_DATA_MAX), old_sha256_prefix sa NEposiela
//sk: (= old_sha256[:4]) — obe sú funkciou podpísaných polí → integrita zachovaná.
typedef struct __attribute__((packed)) {
    uint8_t  type;             //en: FOTA_PKT_HEADER
    uint8_t  fota_prot_inf;     //en: FOTA_PROT_INF_V0 — protocol/struct version
    uint32_t patch_size;       //en: LE [bytes]; total_chunks = ceil(patch_size/FOTA_CHUNK_DATA_MAX)
    uint8_t  patch_sha256[32]; //en: SHA256 of patch.bin (to verify reception)
    uint8_t  new_sha256[32];   //en: SHA256 of the new firmware after applying the patch
    uint8_t  old_sha256[32];   //en: SHA256 of the old fw — base gating + verification before rewrite
} FotaHeaderPkt;                //en: = 102 B

//en: FOTA_PKT_HDR_SIG — SIG, 99 B (2nd part of HEADER). data_len = 4 + 99 = 103 ≤ 165.
//en: The signature covers ONLY the 102 B META; key_id is outside the signature
//en: (a wrong key_id → verify fails).
//sk: FOTA_PKT_HDR_SIG — SIG, 99 B (2. časť HEADER). data_len = 4 + 99 = 103 ≤ 165.
//sk: Podpis kryje LEN 102 B META; key_id mimo podpisu (zlý key_id → verify zlyhá).
//en: With key_id==FOTA_KEY_ID_PREFIX a 4 B signer_prefix follows the struct
//en: (SIG = 103 B, data_len = 4 + 103 = 107 ≤ 165).
//sk: Pri key_id==FOTA_KEY_ID_PREFIX za štruktúrou nasleduje 4 B signer_prefix
//sk: (SIG = 103 B, data_len = 4 + 103 = 107 ≤ 165).
typedef struct __attribute__((packed)) {
    uint8_t  type;             //en: FOTA_PKT_HDR_SIG
    uint8_t  fota_prot_inf;     //en: FOTA_PROT_INF_V0 (same as META; META is authoritative)
    uint8_t  old_sha256[32];   //en: "belongs to me" gating (== META.old_sha256, pre-filter)
    uint8_t  key_id;           //en: which author signed it
    uint8_t  signature[64];    //en: Ed25519 signature over the 102 B META
} FotaHdrSigPkt;                //en: = 99 B

//en: FOTA_CHUNK — 13 + data_len B (max 13+144 = 157 B)
//en: +8B vs. the original: old_fw_size + old_sha256_prefix for session isolation
//en: GRP_DATA data = 4B ts + 157B = 161B ≤ 165B — fits
//sk: FOTA_CHUNK — 13 + data_len B (max 13+144 = 157 B)
//sk: +8B oproti pôvodnému: old_fw_size + old_sha256_prefix pre session izoláciu
//sk: GRP_DATA data = 4B ts + 157B = 161B ≤ 165B ✅
typedef struct __attribute__((packed)) {
    uint8_t  type;                 //en: FOTA_PKT_CHUNK
    uint16_t chunk_idx;            //en: LE, 0-based
    uint16_t crc16;                //en: CRC16/CCITT over data[] only
    uint32_t old_fw_size;          //en: LE — size of the base FW for validation
    uint8_t  old_sha256_prefix[4]; //en: first 4B of the base FW SHA256 — quick check
    uint8_t  data[FOTA_CHUNK_DATA_MAX];  //en: real length from pktlen-13
} FotaChunkPkt;

//en: FOTA_APPLY — 33 B
typedef struct __attribute__((packed)) {
    uint8_t  type;          //en: FOTA_PKT_APPLY
    uint8_t  sha256[32];    //en: confirmation — must match the received one
} FotaApplyPkt;

//en: FOTA_STATUS — 6 B
typedef struct __attribute__((packed)) {
    uint8_t  type;          //en: FOTA_PKT_STATUS
    uint16_t recv_count;    //en: number of received chunks
    uint16_t total_chunks;  //en: total count
    uint8_t  status;        //en: FOTA_ST_* flags (from FotaState.h)
} FotaStatusPkt;

//en: FOTA_NACK — 2 + count*2 B
typedef struct __attribute__((packed)) {
    uint8_t  type;          //en: FOTA_PKT_NACK
    uint8_t  count;         //en: number of entries in idx[]
    uint16_t idx[FOTA_NACK_MAX_IDX];  //en: missing chunk_idx (only the first count)
} FotaNackPkt;

// =====================================================================
//en: CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF) — matches fota_sender.py
// =====================================================================
static inline uint16_t fota_crc16(const uint8_t* data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000u) ? ((uint16_t)(crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}
