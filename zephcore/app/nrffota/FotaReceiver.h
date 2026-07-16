#pragma once
// =====================================================================
//en: FotaReceiver.h — FOTA receiver: packet processing, CustomLFS, reboot-resilient
//
//en: Port of FK_lora-sniffer/src/fota_receiver.h. Processes the already DECRYPTED
//en: FOTA payload (MeshCore GRP_DATA → mesh::Utils::MACThenDecrypt → onGroupDataRecv).
//en: No dynamic memory (static state + static buffers).
//
//sk: FotaReceiver.h — FOTA prijímač: spracovanie paketov, CustomLFS, reboot-resilient
//
//sk: Port z FK_lora-sniffer/src/fota_receiver.h. Spracováva už DEŠIFROVANÝ FOTA
//sk: payload (MeshCore GRP_DATA → mesh::Utils::MACThenDecrypt → onGroupDataRecv).
//sk: Žiadna dynamická pamäť (statický stav + statické buffre).
// =====================================================================
#include "FotaState.h"

//en: Save the bitmap every N new chunks (trade-off: flash wear vs. loss on reboot)
//sk: Zapisuj bitmap každých N nových chunkov (kompromis wear vs. strata pri reboote)
#define FOTA_BITMAP_SAVE_EVERY  8

// =====================================================================
//en: Public API
// =====================================================================

//en: Initialization — mount CustomLFS @ 0xD4000, attempt resume after reboot.
//sk: Inicializácia — mount CustomLFS @ 0xD4000, pokus o resume po reboote.
void fota_init();

//en: Process one FOTA packet (plaintext, starts with a FOTA_PKT_* type byte).
//en: Returns true if the packet was recognized as FOTA.
//sk: Spracuj jeden FOTA paket (plaintext, začína typovým bajtom FOTA_PKT_*).
//sk: Vracia true ak bol paket rozpoznaný ako FOTA.
bool fota_process(const uint8_t* plain, int plen);

//en: v0-prefix ACL hook — the app layer (FotaMyMesh.cpp) returns pubkeys of ACL
//en: admins whose first 4 B match `prefix`. Default (non-MeshCore builds): 0.
//sk: v0-prefix ACL hook — aplikačná vrstva (FotaMyMesh.cpp) vráti pubkey ACL
//sk: adminov, ktorých prvé 4 B sedia s `prefix`. Default (ne-MeshCore buildy): 0.
#define FOTA_ACL_MAX_CANDIDATES 4
int fota_acl_admin_pubkeys(const uint8_t prefix[4], const uint8_t* out_keys[], int max);

//en: Manual trigger of the patch application (if FOTA_ST_VERIFIED).
bool fota_apply();

//en: Diagnostics via Serial.
void fota_print_status();
void fota_send_nack();

//en: Missing chunks. fota_calc_missing returns the total count (-1 = zero info yet) and
//en: fills out[] with the first max_out indices (*out_n; both may be NULL = count only).
//en: fota_print_missing prints to Serial, fota_format_missing writes into a buffer (LoRa reply)
//en: — both as "from-to" ranges; 'limit' = cap in TOKENS (number=1, range=2), <=0 = all.
//en: Range: total known OR estimated from META → [0..est-1]; otherwise the window of received ones.
//sk: Chýbajúce chunky. fota_calc_missing vráti celkový počet (-1 = zero info yet) a
//sk: naplní out[] prvými max_out indexmi (*out_n; oba môžu byť NULL = len počet).
//sk: fota_print_missing vypíše na Serial, fota_format_missing zapíše do bufferu (LoRa reply)
//sk: — obe ako rozsahy "od-do"; 'limit' = strop v TOKENOCH (číslo=1, rozsah=2), <=0 = všetky.
//sk: Rozsah: total známy ALEBO odhadnutý z META → [0..est-1]; inak okno prijatých.
//en: Output: comma-separated tokens ("0-4,6,8,9"; a pair as "a,b"); 'lead' is emitted
//en: before the FIRST token (caller-supplied "," or " " glue). With no META the unknown
//en: tail is marked "N-??" (N = highest received + 1 — the exact tail start for the app;
//en: N itself may not exist, the app drops the marker when N is beyond the package total).
//sk: Výstup: tokeny oddelené čiarkou ("0-4,6,8,9"; dvojica ako "a,b"); 'lead' sa emitne
//sk: pred PRVÝM tokenom (lepidlo od volajúceho: "," alebo " "). Bez META sa neznámy chvost
//sk: označí "N-??" (N = najvyšší prijatý + 1 — presný začiatok chvosta pre appku; N nemusí
//sk: existovať, appka marker zahodí, ak je N za totalom balíka).
int  fota_calc_missing(uint16_t* out, int max_out, int* out_n);
void fota_print_missing(int limit, const char* lead);
int  fota_format_missing(char* out, int out_sz, int limit, const char* lead);
//en: Diagnostic total: promoted total_chunks, else UNVERIFIED ceil(patch_size/144) from a
//en: received META, else 0. Never gates completion/flash.
//sk: Diagnostický total: promotnutý total_chunks, inak NEOVERENÝ ceil(patch_size/144)
//sk: z prijatej META, inak 0. Nikdy negatuje completion/flash.
uint16_t fota_total_est(void);

//en: Deferred flash: 'fota flash' first sends an "accepted" ACK; the flash (fota_apply)
//en: starts from loop() only AFTER the ACK actually went out (otherwise reboot happens
//en: before the ACK is transmitted).
//sk: Odložený flash: 'fota flash' najprv pošle ACK „accepted", flash (fota_apply) sa
//sk: spustí z loop() AŽ keď ACK reálne odíde (inak reboot skôr než sa ACK odvysiela).
void fota_request_apply();
bool fota_apply_pending();
void fota_clear_apply_pending();

//en: Dump of a decoded FOTA packet (diagnostics before processing).
//en: plain = pointer to the FOTA packet (starts with a FOTA_PKT_* type byte).
void fota_print_pkt(const uint8_t* plain, int plen, float rssi, float snr);

//en: Session state (read-only).
const FotaState* fota_get_state();

//en: Real app base and size of the running FW from linker symbols (v6=0x26000,
//en: v7=0x27000) — source of truth for device-side SHA/verify instead of the macro.
//sk: Reálny app base a veľkosť bežiaceho FW z linker symbolov (v6=0x26000,
//sk: v7=0x27000) — zdroj pravdy pre device-side SHA/verify namiesto makra.
uint32_t fota_running_fw_base(void);
uint32_t fota_running_fw_size(void);

//en: Print the FW identity (build#, image_size, trailer sha256) + the computed full
//en: SHA256 of the running FW — to compare with old_sha256 in .fotapkg.json. reply =
//en: short reply for LoRa; details go to Serial.
//sk: Výpis FW identity (build#, image_size, trailer sha256) + dopočítaný plný
//sk: SHA256 bežiaceho FW — na porovnanie s old_sha256 v .fotapkg.json. reply =
//sk: krátka odpoveď pre LoRa; detaily idú na Serial.
void fota_print_fw_id(char* reply);

//en: Deletes all FOTA files from CustomLFS + resets the RAM state.
void fota_clear_session();

//en: Loads the patch into a freshly malloc'd RAM buffer (caller frees with free()).
//en:   default:           assembles from /ota/recv.log directly into RAM (no patch.bin)
//en:   -D USE_PATCHBIN_FILE: reads /ota/patch.bin
//en: *out_size = patch size. Returns NULL on error (including malloc failure).
//sk: Načíta patch do čerstvo malloc-nutého RAM buffra (caller uvoľní free()).
//sk:   default:           zostaví z /ota/recv.log priamo do RAM (bez patch.bin)
//sk:   -D USE_PATCHBIN_FILE: prečíta /ota/patch.bin
//sk: *out_size = veľkosť patchu. Vracia NULL pri chybe (vrátane malloc zlyhania).
uint8_t* fota_acquire_patch_ram(uint32_t* out_size);

//en: Build a STATUS/NACK packet into out[] (for sending back over the LoRa FOTA channel).
//en: Returns the payload length, or 0 if there is nothing to send.
int fota_build_status(uint8_t* out);
int fota_build_nack(uint8_t* out);
