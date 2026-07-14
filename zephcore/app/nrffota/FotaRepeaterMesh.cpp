// =====================================================================
//en: FotaRepeaterMesh.cpp — FOTA part of the RepeaterMesh class (ZephCore
//en: port of nrffota/FotaMyMesh.cpp; method bodies + local helpers).
//
//en: Keeps the whole FOTA integration OUT of app/RepeaterMesh.cpp so the
//en: repeater's diff against stock ZephCore stays minimal — RepeaterMesh.cpp
//en: keeps only thin 1–3 line hooks, RepeaterMesh.h one guarded #include.
//en: Everything here is gated behind WITH_LORA_FOTA (CONFIG_ZEPHCORE_LORA_FOTA).
//
//en: Diagnostic output goes through FOTA_DEBUG_PRINT/PRINTLN (FotaDebug.h,
//en: printk), enabled by CONFIG_ZEPHCORE_FOTA_DEBUG; CLI replies are
//en: functional output and stay as sprintf.
//
//en: Differences vs the Arduino original (FotaMyMesh.cpp):
//en:  - radio stats via mesh::Radio virtuals (_radio->getLastRSSI(), ...)
//en:    instead of the raw RadioLib object; 'fota agc' has no SX1262
//en:    register access here (reports noise floor + counters only).
//en:  - no FreeRTOS stack diagnostics (FK_DEBUG_STACKTRACE) — Zephyr threads.
//en:  - heartbeat gated by FOTA_DEBUG (Arduino used FK_DEBUG).
//
//sk: FotaRepeaterMesh.cpp — FOTA časť triedy RepeaterMesh (ZephCore port
//sk: nrffota/FotaMyMesh.cpp; telá metód + lokálne helpery).
//
//sk: Drží celú FOTA integráciu MIMO app/RepeaterMesh.cpp, aby bol diff
//sk: repeatera oproti stock ZephCore minimálny — v RepeaterMesh.cpp ostávajú
//sk: len tenké 1–3 riadkové hooky, v RepeaterMesh.h jeden guardovaný
//sk: #include. Všetko tu je gated WITH_LORA_FOTA (CONFIG_ZEPHCORE_LORA_FOTA).
//
//sk: Rozdiely oproti Arduino originálu (FotaMyMesh.cpp):
//sk:  - štatistiky rádia cez mesh::Radio virtuály (_radio->getLastRSSI(), ...)
//sk:    namiesto surového RadioLib objektu; 'fota agc' tu nemá prístup k
//sk:    SX1262 registrom (hlási len noise floor + počítadlá).
//sk:  - bez FreeRTOS stack diagnostiky (FK_DEBUG_STACKTRACE) — Zephyr vlákna.
//sk:  - heartbeat gated FOTA_DEBUG (Arduino používal FK_DEBUG).
// =====================================================================
#ifdef WITH_LORA_FOTA

#include "../RepeaterMesh.h"
#include "FotaMesh.h"
#include "FotaReceiver.h"
#include "FotaPatcher.h"
#include "FotaBuffer.h"   //en: shared scratch — parking spot for the deferred CLI snapshot
#include "FotaDebug.h"
#include "FotaTexts.h"    //en: FOTA_TXT_* — central catalog of CLI reply texts (EN/SK)
                          //sk: FOTA_TXT_* — centrálny katalóg textov CLI odpovedí (EN/SK)
#include <helpers/TxtDataHelpers.h>   //en: TXT_TYPE_PLAIN / TXT_TYPE_CLI_DATA
#include <stdio.h>
#include <string.h>

#if __has_include("build_info.h")
  #include "build_info.h"   //en: TEMPORARY: test_nrf-fota/gen_build_info.py (CMake pre-build)
#endif
#ifndef FW_BUILD_NUMBER
  #define FW_BUILD_NUMBER 0
#endif

//en: CLI reply delay — matches CLI_REPLY_DELAY_MILLIS in app/RepeaterMesh.cpp
//en: (the constant is private there; keep the values in sync).
//sk: Oneskorenie CLI odpovede — musí sedieť s CLI_REPLY_DELAY_MILLIS v
//sk: app/RepeaterMesh.cpp (konštanta je tam súkromná; drž hodnoty v synku).
#define FOTA_CLI_REPLY_DELAY_MILLIS  600

//en: Recognize a FOTA CLI command and return a pointer to the arguments (the part
//en: AFTER 'fota'/'ota', including the leading space), or NULL if it is not a FOTA
//en: command. Single source of truth for the detection — used by both the LoRa
//en: (defer) and Serial (inline) paths.
//en: FOTA-CLI-ALIAS: once clients switch to 'fota', delete the 'ota' branch.
//sk: Rozpoznaj FOTA CLI príkaz a vráť smerník na argumenty (časť ZA 'fota'/'ota',
//sk: vrátane vedúcej medzery), alebo NULL ak to nie je FOTA príkaz. Jediný zdroj
//sk: pravdy pre detekciu — používa LoRa (defer) aj Serial (inline) cesta.
//sk: FOTA-CLI-ALIAS: keď klienti prejdú na 'fota', zmaž 'ota' vetvu.
static const char* fota_args_of(const char* cmd) {
  if (memcmp(cmd, "fota", 4) == 0 && (cmd[4] == 0 || cmd[4] == ' ')) return cmd + 4;
  if (memcmp(cmd, "ota",  3) == 0 && (cmd[3] == 0 || cmd[3] == ' ')) return cmd + 3;  //en: FOTA-CLI-ALIAS
  return nullptr;
}

//en: PAYLOAD_TYPE name for a readable RAW log.
__attribute__((unused))
static const char* payload_type_name(uint8_t t) {
  switch (t) {
    case PAYLOAD_TYPE_REQ:        return "REQ";
    case PAYLOAD_TYPE_RESPONSE:   return "RESPONSE";
    case PAYLOAD_TYPE_TXT_MSG:    return "TXT_MSG";
    case PAYLOAD_TYPE_ACK:        return "ACK";
    case PAYLOAD_TYPE_ADVERT:     return "ADVERT";
    case PAYLOAD_TYPE_GRP_TXT:    return "GRP_TXT";
    case PAYLOAD_TYPE_GRP_DATA:   return "GRP_DATA";
    case PAYLOAD_TYPE_ANON_REQ:   return "ANON_REQ";
    case PAYLOAD_TYPE_PATH:       return "PATH";
    case PAYLOAD_TYPE_TRACE:      return "TRACE";
    case PAYLOAD_TYPE_MULTIPART:  return "MULTIPART";
    case PAYLOAD_TYPE_CONTROL:    return "CONTROL";
    case PAYLOAD_TYPE_RAW_CUSTOM: return "RAW_CUSTOM";
    default:                      return "?";
  }
}

//en: Snapshot of the client context for a deferred CLI reply. Parked in the shared
//en: FotaBuffer (~182 B within the 512 B window), so RepeaterMesh gains no permanent member.
//sk: Snapshot kontextu klienta pre odloženú CLI odpoveď. Parkuje sa v zdieľanom
//sk: FotaBuffer (~182 B v 512 B okne), takže RepeaterMesh nerastie o permanentný člen.
struct FotaCliDefer {
  uint8_t  dest_pub[PUB_KEY_SIZE];     //en: 32 — whom to reply to (Identity)
  uint8_t  secret[PUB_KEY_SIZE];       //en: 32 — shared secret for encrypting the reply
  uint8_t  out_path[MAX_PATH_SIZE];    //en: 64 — known out-path (if any)
  uint32_t sender_timestamp;           //en: ts of the received command — the reply must use a DIFFERENT one (CLI dedup)
  uint8_t  out_path_len;               //en: OUT_PATH_UNKNOWN → flood reply
  uint8_t  path_hash_size;             //en: for flood reply
  char     fargs[48];                  //en: arguments (" verify", " agc", …)
  char     tag[4];                     //en: "NN|" companion-CLI tag to reflect in the reply ("" = none)
};

// =====================================================================
//en: RAW log — every raw (CRC-OK) frame, on RX BEFORE any decoding/decryption and
//en: on TX right after it is handed to the radio. Answers "do packets reach the
//en: repeater / does it actually send?" — if rawrx grows but GRP_DATA/onGroupDataRecv
//en: does not, the fault is in decoding, not RF; rawtx shows what we put on air.
//en: Shared body for both directions; called from logRxRaw / logTxRaw overrides.
//sk: RAW log — každý surový (CRC-OK) rámec, na RX EŠTE PRED dekódovaním/dešifrovaním a
//sk: na TX hneď po odovzdaní rádiu. Odpoveď na "prichádzajú pakety na repeater / naozaj
//sk: odosiela?" — ak rawrx rastie ale GRP_DATA/onGroupDataRecv nie, chyba je v dekódovaní,
//sk: nie v RF; rawtx ukazuje, čo sme dali do éteru.
//sk: Zdieľané telo pre oba smery; volané z logRxRaw / logTxRaw overridov.
// =====================================================================
//en: fota_channel_hash0 = our FOTA channel_hash[0], or -1 when FOTA not ready.
//en: have_sig=true → print rssi/snr (RX only); TX has no receive metrics.
//sk: fota_channel_hash0 = channel_hash[0] nášho FOTA kanála, alebo -1 keď FOTA nie je ready.
//sk: have_sig=true → vypíš rssi/snr (len RX); TX nemá prijímacie metriky.
static void fota_log_raw_line(const char* dir, unsigned long seq, bool have_sig,
                              float rssi, float snr, int fota_channel_hash0,
                              const uint8_t raw[], int len) {
  if (len > 0) {
    //en: PAYLOAD_TYPE = (hdr >> 2) & 0x0F (NOT the low 4 bits — those are route+part of type);
    //en: route = hdr & 0x03.  (4=ADVERT, 7=ANON_REQ, 2=TXT, 1=RESPONSE, 5=GRP_TXT, 6=GRP_DATA)
    //sk: PAYLOAD_TYPE = (hdr >> 2) & 0x0F (NIE dolné 4 bity — tie sú route+časť typu);
    //sk: route = hdr & 0x03.  (4=ADVERT, 7=ANON_REQ, 2=TXT, 1=RESPONSE, 5=GRP_TXT, 6=GRP_DATA)
    uint8_t pt    = (raw[0] >> 2) & 0x0F;
    uint8_t route = raw[0] & 0x03;
    //en: Replicates the parser offsets so we can print the PATH and recognize
    //en: FOTA without decrypting:  [hdr][?transport 4B][path_len][path…][payload].
    //sk: Replikácia parser offsetov, aby sme vedeli vypísať CESTU a rozoznať
    //sk: FOTA bez dešifrovania:  [hdr][?transport 4B][path_len][path…][payload].
    int i = 1;
    if (route == ROUTE_TYPE_TRANSPORT_FLOOD || route == ROUTE_TYPE_TRANSPORT_DIRECT) i += 4;
    int path_count = -1, path_off = i + 1, path_byte_len = 0, payload_off = -1;
    if (i < len) {
      uint8_t plen  = raw[i];
      uint8_t hsize = (plen >> 6) + 1;       //en: path hash size (1 or 2 B)
      path_count    = plen & 63;             //en: hop count (0 = zero-hop / fresh flood)
      path_byte_len = path_count * hsize;
      payload_off   = path_off + path_byte_len;
    }

    //en: FK - only packets with path < 4 - not to mess debug output with many data
    if (path_count > 4) return;

    if (have_sig)
      FOTA_DEBUG_PRINT("[FOTA] %s RAW #%lu len=%d rssi=%d snr=%d.%d",
                       dir, seq, len, (int)rssi, (int)snr,
                       (int)((snr - (int)snr) * 10 + 0.5f));
    else
      FOTA_DEBUG_PRINT("[FOTA] %s RAW #%lu len=%d", dir, seq, len);

    //en: Our FOTA? GRP_DATA on our FOTA channel — channel_hash is the 1st payload byte
    //en: (Mesh.cpp: channel_hash = payload[0]); readable already here, before decryption.
    //sk: Naša FOTA? GRP_DATA na našom FOTA kanáli — channel_hash je 1. bajt payloadu
    //sk: (Mesh.cpp: channel_hash = payload[0]); čitateľné už tu, pred dešifrovaním.
    bool is_fota = (pt == PAYLOAD_TYPE_GRP_DATA && fota_channel_hash0 >= 0
                    && payload_off >= 0 && payload_off < len
                    && raw[payload_off] == (uint8_t)fota_channel_hash0);
    FOTA_DEBUG_PRINT(" type=%u(%s%s)",
                     (unsigned)pt, payload_type_name(pt), is_fota ? "/FOTA" : "");
    //en: 1B hashes readable without decryption, by payload type (Mesh.cpp layouts):
    //en:  PATH/REQ/RESPONSE/TXT_MSG: [dest_hash][src_hash][MAC+cipher] → dsth+srch
    //en:  ANON_REQ: [dest_hash][sender pub_key 32B][…] → dsth + srch=pub_key[0]
    //en:  ADVERT:   [pub_key 32B][…]                   → srch=pub_key[0]
    //en:  GRP_TXT/GRP_DATA: [channel_hash][MAC+cipher] → chah
    //en:  ACK/others: nothing.
    //sk: 1B hashe čitateľné bez dešifrovania, podľa typu payloadu (layouty z Mesh.cpp):
    //sk:  PATH/REQ/RESPONSE/TXT_MSG: [dest_hash][src_hash][MAC+šifra] → dsth+srch
    //sk:  ANON_REQ: [dest_hash][sender pub_key 32B][…] → dsth + srch=pub_key[0]
    //sk:  ADVERT:   [pub_key 32B][…]                   → srch=pub_key[0]
    //sk:  GRP_TXT/GRP_DATA: [channel_hash][MAC+šifra] → chah
    //sk:  ACK/ostatné: nič.
    if (payload_off >= 0) {
      if ((pt == PAYLOAD_TYPE_PATH || pt == PAYLOAD_TYPE_REQ ||
           pt == PAYLOAD_TYPE_RESPONSE || pt == PAYLOAD_TYPE_TXT_MSG ||
           pt == PAYLOAD_TYPE_ANON_REQ) && payload_off + 1 < len) {
        FOTA_DEBUG_PRINT(" srch=%02X dsth=%02X",
                         (unsigned)raw[payload_off + 1], (unsigned)raw[payload_off]);
      } else if (pt == PAYLOAD_TYPE_ADVERT && payload_off < len) {
        FOTA_DEBUG_PRINT(" srch=%02X", (unsigned)raw[payload_off]);
      } else if ((pt == PAYLOAD_TYPE_GRP_TXT || pt == PAYLOAD_TYPE_GRP_DATA)
                 && payload_off < len) {
        FOTA_DEBUG_PRINT(" chah=%02X", (unsigned)raw[payload_off]);
      }
    }
    FOTA_DEBUG_PRINT(" route=%u hdr=0x%X", (unsigned)route, (unsigned)raw[0]);
    //en: Path: hop count + hash bytes.  path[0] = zero-hop or a fresh flood from the
    //en: source; every forwarding repeater appends its own hash → path[N] gets longer.
    //sk: Cesta: počet hopov + hash bajty.  path[0] = zero-hop alebo čerstvý flood od
    //sk: zdroja; každý preposielajúci repeater pripojí svoj hash → path[N] dlhšia.
    if (path_count >= 0) {
      FOTA_DEBUG_PRINT(" path[%d]", path_count);
      if (path_byte_len > 0 && (path_off + path_byte_len) <= len) {
        FOTA_DEBUG_PRINT("=");
        for (int k = 0; k < path_byte_len; k++) FOTA_DEBUG_PRINT("%02X", (unsigned)raw[path_off + k]);
      }
    }
  }
  FOTA_DEBUG_PRINT(" first=");
  int n8 = len < 8 ? len : 8;
  for (int k = 0; k < n8; k++) FOTA_DEBUG_PRINT("%02X", (unsigned)raw[k]);
  FOTA_DEBUG_PRINTLN("");
}

//en: RX RAW — heard frames (before decode). Called from RepeaterMesh::logRxRaw.
//sk: RX RAW — počuté rámce (pred dekódovaním). Volané z RepeaterMesh::logRxRaw.
void RepeaterMesh::fotaLogRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
  _fota_raw_rx++;
  _fota_raw_last_len  = (uint32_t)len;
  _fota_raw_last_rssi = rssi;
  _fota_raw_last_snr  = snr;
  fota_log_raw_line("RX", (unsigned long)_fota_raw_rx, true, rssi, snr,
                    _fota_ready ? (int)_fota_channel.hash[0] : -1, raw, len);
}

//en: TX RAW — frames the repeater sent (own adverts/ACKs + forwarded floods/direct).
//en: logTxRaw is the Dispatcher core hook (mirror of the Arduino FOTA branch).
//sk: TX RAW — rámce, ktoré repeater odoslal (vlastné adverty/ACK + preposlané flood/direct).
//sk: logTxRaw je core hook Dispatchera (zrkadlo Arduino FOTA vetvy).
void RepeaterMesh::logTxRaw(const uint8_t raw[], int len) {
  fotaLogTxRaw(raw, len);
}

void RepeaterMesh::fotaLogTxRaw(const uint8_t raw[], int len) {
  _fota_raw_tx++;
  fota_log_raw_line("TX", (unsigned long)_fota_raw_tx, false, 0.0f, 0.0f,
                    _fota_ready ? (int)_fota_channel.hash[0] : -1, raw, len);
}

// =====================================================================
//en: GRP_DATA channel — the repeater "subscribes" a single FOTA channel; when the
//en: channel_hash matches, the core decrypts the GRP_DATA with fota_channel.secret
//en: and calls onGroupDataRecv(). (Stock ZephCore repeater has no channels at all —
//en: these overrides are the FOTA-only additions, pattern from BaseChatMesh.)
//sk: GRP_DATA kanál — repeater "subscribne" jediný FOTA kanál; keď sa
//sk: channel_hash zhoduje, jadro dešifruje GRP_DATA cez fota_channel.secret
//sk: a zavolá onGroupDataRecv(). (Stock ZephCore repeater kanály nemá —
//sk: tieto overridy sú FOTA-only doplnky, vzor BaseChatMesh.)
// =====================================================================
int RepeaterMesh::searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) {
  if (_fota_ready && max_matches > 0 && hash[0] == _fota_channel.hash[0]) {
    channels[0] = _fota_channel;
    return 1;
  }
  return 0;
}

//en: Decrypted GRP_DATA payload: [ts 4B LE][fota_type 1B][...]. The FOTA payload
//en: starts after the 4B timestamp (matches fota_sender.py meshcore_grp_data_packet).
//sk: Dešifrovaný GRP_DATA payload: [ts 4B LE][fota_type 1B][...]. FOTA payload
//sk: začína za 4B timestampom (zhodné s fota_sender.py meshcore_grp_data_packet).
void RepeaterMesh::onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel,
                                   uint8_t* data, size_t len) {
  if (type != PAYLOAD_TYPE_GRP_DATA) return;
  if (channel.hash[0] != _fota_channel.hash[0]) return;   //en: not our FOTA channel
  //en: Unified FOTA format: standard GRP_DATA plaintext = [data_type 2B][len 1B][ts 4B][fota_payload].
  //en: Peel off [data_type][len]; if data_type != FOTA_MAGIC, it is not FOTA. After peeling, the
  //en: buffer has the shape [ts 4B][fota_payload] — the rest of the pipeline (loop +4) stays unchanged.
  //sk: Zjednotený FOTA formát: štandardný GRP_DATA plaintext = [data_type 2B][len 1B][ts 4B][fota_payload].
  //sk: Odlúpni [data_type][len]; ak data_type != FOTA_MAGIC, nie je to FOTA. Po odlúpnutí má
  //sk: buffer tvar [ts 4B][fota_payload] — zvyšok pipeline (loop +4) ostáva nezmenený.
  if (len < 3 + 5) return;                               //en: [dt2][len1] + [ts4][type1]
  uint16_t dtype = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
  if (dtype != FOTA_MAGIC) return;                        //en: not our FOTA data_type
  //en: data[2] = true length of [ts4][fota_payload]. MACThenDecrypt returns the AES-padded
  //en: (16B) length, so do NOT compare against len; use data[2] to strip the padding.
  //sk: data[2] = pravá dĺžka [ts4][fota_payload]. MACThenDecrypt vracia AES-padovanú
  //sk: (16B) dĺžku, preto NEporovnávaj s len; použi data[2] na strhnutie paddingu.
  uint8_t inner = data[2];
  if (inner < 5 || (size_t)(3 + inner) > len) return;    //en: sanity vs padded buffer
  data += 3; len = inner;                                //en: → exact [ts4][fota_payload]
  //en: Defer the payload — the slow FS I/O is done in loop() AFTER the dispatcher
  //en: re-arms the radio into RX. Writing to FS right here delayed the re-arm and the
  //en: radio stopped receiving after the first packet. If a previous one is still pending,
  //en: drop this one (loop processes it before the next LoRa packet arrives at SF7).
  //sk: Odlož payload — pomalé FS I/O sa spraví v loop() PO tom, čo dispatcher
  //sk: re-armne rádio do RX. FS zápis priamo tu oneskoroval re-arm a rádio po prvom
  //sk: pakete prestávalo prijímať. Ak ešte čaká predošlý, tento zahodíme (loop ho
  //sk: stihne spracovať skôr ako príde ďalší LoRa paket pri SF7).
  if (_fota_pending_len == 0) {
    int n = (int)len;
    if (n > (int)sizeof(_fota_pending)) n = (int)sizeof(_fota_pending);
    memcpy(_fota_pending, data, n);
    _fota_pending_len  = n;
    _fota_pending_rssi = _radio->getLastRSSI();
    _fota_pending_snr  = packet->getSNR();
  } else {
    FOTA_DEBUG_PRINTLN("[FOTA] WARN pending buffer busy, packet dropped");
  }
}

// =====================================================================
//en: Initialization (hooks from RepeaterMesh::begin)
// =====================================================================
//en: At the START of RepeaterMesh::begin(): read the flasher debug marker as early
//en: after boot as possible (GPREGRET2/RESETREAS) + reset the FOTA state.
//sk: Na ZAČIATKU RepeaterMesh::begin(): prečítaj flasher debug marker čo najskôr
//sk: po boote (GPREGRET2/RESETREAS) + vynuluj FOTA stav.
void RepeaterMesh::fotaEarlyInit() {
  fota_check_flasher_debug();
  _fota_ready = false;
  _fota_pending_len = 0;
  _fota_cli_pending = false;
  _fota_cli_buf = nullptr;
  _fota_apply_deadline = 0;
  _fota_raw_rx = 0;
  _fota_raw_tx = 0;
  _fota_raw_last_len = 0;
  _fota_raw_last_rssi = _fota_raw_last_snr = 0;
}

//en: AT THE END of RepeaterMesh::begin(): FOTA FS dir + channel + boot banner.
//sk: NA KONCI RepeaterMesh::begin(): FOTA FS adresár + kanál + boot banner.
void RepeaterMesh::fotaBegin() {
  fota_init();                       //en: /lfs/fota dir + resume session
  fota_build_channel(_fota_channel); //en: FOTA GRP_DATA channel (#-convention from FOTA_CHANNEL_NAME)
  _fota_ready = true;
  fota_print_flasher_debug();        //en: in case we just returned from the flasher
  FOTA_DEBUG_PRINTLN("[FOTA] build #%lu  freq=%d.%03d sf=%u",
                     (unsigned long)FW_BUILD_NUMBER, (int)_prefs.freq,
                     (int)((_prefs.freq - (int)_prefs.freq) * 1000 + 0.5f),
                     (unsigned)_prefs.sf);
}

// =====================================================================
//en: CLI — both the Serial (inline) and LoRa (deferred) paths
// =====================================================================
//en: Serial/inline path (hook from RepeaterMesh::handleCommand). Returns false if it
//en: is not a FOTA command ('fota …' / legacy 'ota …'). Runs on the main event loop
//en: thread (shallow, off-ISR). The LoRa path never gets here — onPeerDataRecv
//en: defers it via fotaHandleLoRaCli.
//sk: Serial/inline cesta (hook z RepeaterMesh::handleCommand). Vracia false ak to
//sk: nie je FOTA príkaz ('fota …' / legacy 'ota …'). Beží na hlavnom event-loop
//sk: vlákne (plytko, mimo ISR). LoRa cesta sem nepríde — onPeerDataRecv ju odloží
//sk: cez fotaHandleLoRaCli.
bool RepeaterMesh::fotaHandleCliCommand(const char* command, char* reply) {
  const char* fargs = fota_args_of(command);
  if (!fargs) return false;
#if FOTA_DEBUG
  //en: Serial-only debug path commands (getacl / getpath|setpath <pub_key prefix>).
  //sk: Serial-only debug path príkazy (getacl / getpath|setpath <pub_key prefix>).
  if (fotaHandleSerialPathCli(fargs, reply)) return true;
#endif
  runFotaCli(fargs, reply);
  return true;
}

//en: Parse a comma hop list ("a1,3f" / "11aa,22bb" / "112233,..."): token width
//en: (2/4/6 hex chars) selects the hash size (1/2/3 B per hop). Hops are in the
//en: order the REPEATER transmits them (repeater -> client). On success fills
//en: out[] and the ENCODED path_len ((hash_size-1)<<6 | hop_count); on failure
//en: points *err at a static reason from FotaTexts.h (no copying).
//sk: Parsuje čiarkový zoznam hopov ("a1,3f" / "11aa,22bb" / "112233,..."): šírka
//sk: tokenu (2/4/6 hex znakov) určuje hash size (1/2/3 B na hop). Hopy sú v poradí,
//sk: v akom ich REPEATER vysiela (repeater -> klient). Pri úspechu naplní out[]
//sk: a ENCODED path_len ((hash_size-1)<<6 | hop_count); pri chybe nasmeruje *err
//sk: na statický dôvod z FotaTexts.h (bez kopírovania).
static bool fota_parse_path_arg(const char* s, uint8_t out[MAX_PATH_SIZE],
                                uint8_t* encoded_len, const char** err) {
  while (*s == ' ') s++;
  if (*s == 0) { *err = FOTA_TXT_ERR_PATH_EMPTY; return false; }
  int tok_w = -1, count = 0, nbytes = 0;
  const char* p = s;
  while (*p) {
    uint8_t tokbytes[3];
    int w = 0;
    while (*p && *p != ',' && *p != ' ') {
      char c = *p;
      int v = (c >= '0' && c <= '9') ? c - '0'
            : (c >= 'a' && c <= 'f') ? c - 'a' + 10
            : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (v < 0) { *err = FOTA_TXT_ERR_PATH_NOT_HEX; return false; }
      if (w >= 6) { *err = FOTA_TXT_ERR_HOP_TOO_BIG; return false; }
      if ((w & 1) == 0) tokbytes[w / 2] = (uint8_t)(v << 4);
      else              tokbytes[w / 2] |= (uint8_t)v;
      w++; p++;
    }
    if (w != 2 && w != 4 && w != 6) { *err = FOTA_TXT_ERR_HOP_WIDTH; return false; }
    if (tok_w < 0) tok_w = w;
    else if (w != tok_w) { *err = FOTA_TXT_ERR_HOPS_MIXED; return false; }
    if (count >= 63 || nbytes + w / 2 > MAX_PATH_SIZE) { *err = FOTA_TXT_ERR_PATH_TOO_LONG; return false; }
    memcpy(&out[nbytes], tokbytes, w / 2);
    nbytes += w / 2;
    count++;
    while (*p == ' ') p++;
    if (*p == ',') { p++; while (*p == ' ') p++; }
  }
  *encoded_len = (uint8_t)(((tok_w / 2 - 1) << 6) | count);
  return true;
}

//en: LoRa path (hook from RepeaterMesh::onPeerDataRecv). Returns false if it is not a
//en: FOTA command. Does NOT process inline — we are deep in the RX processing path;
//en: heavy work (verify = hpatch+SHA over the whole FW) is deferred to fotaLoop().
//en: Snapshot the client → shared FotaBuffer; loop() processes it and replies.
//en: EXCEPTION: getpath/setpath/missall-path-prefix are handled inline right here — they are
//en: cheap (no LFS I/O) and need the live ACL client entry, which the deferred path lacks.
//sk: LoRa cesta (hook z RepeaterMesh::onPeerDataRecv). Vracia false ak to nie je FOTA
//sk: príkaz. NEspracúva inline — sme hlboko v RX ceste; ťažká práca (verify =
//sk: hpatch+SHA nad celým FW) sa odkladá do fotaLoop(). Snapshot klienta →
//sk: zdieľaný FotaBuffer, spracuje a odpovie loop().
//sk: VÝNIMKA: getpath/setpath/missall-s-cestou sa riešia inline priamo tu — sú lacné
//sk: (žiadne LFS I/O) a potrebujú živý ACL záznam klienta, ktorý deferred cesta nemá.
bool RepeaterMesh::fotaHandleLoRaCli(ClientInfo* client, const uint8_t* secret,
                                     const char* command, char* reply,
                                     uint8_t path_hash_size, uint32_t sender_timestamp) {
  //en: Echo the command received over the LoRa admin CLI (diagnostics — the operator at
  //en: the board sees what was issued remotely; retry duplicates are filtered by the caller).
  //en: Paired tags: [LoRa->CLI] = arrived from LoRa, [CLI->LoRa] = reply being sent.
  //sk: Echo prijatého príkazu z LoRa admin CLI (diagnostika — operátor pri doske
  //sk: vidí, čo bolo zadané vzdialene; retry duplikáty filtruje volajúci).
  //sk: Párové značky: [LoRa->CLI] = prišlo z LoRa, [CLI->LoRa] = posielaná odpoveď.
  FOTA_DEBUG_PRINTLN("[LoRa->CLI] %s", command);
  //en: Optional "NN|" companion-CLI tag (the app's RepeaterCommandService frames every
  //en: command as "NN|cmd" and matches the response by the reflected tag) — mirror of
  //en: RepeaterMesh::handleCommand. Without this, tagged 'fota …' commands fell through
  //en: to the CLI fallback: missall-with-path was never parsed AND heavy FOTA
  //en: commands ran inline instead of being deferred.
  //sk: Voliteľný "NN|" companion-CLI tag (appkin RepeaterCommandService balí každý
  //sk: príkaz ako "NN|cmd" a odpoveď páruje podľa zrkadleného tagu) — zrkadlo
  //sk: RepeaterMesh::handleCommand. Bez tohto tagované 'fota …' príkazy prepadli do
  //sk: CLI fallbacku: missall-s-cestou sa neparsoval A ťažké FOTA príkazy bežali
  //sk: inline namiesto deferu.
  while (*command == ' ') command++;
  const char* tag = NULL;
  if (strlen(command) > 4 && command[2] == '|') { tag = command; command += 3; }
  const char* fargs = fota_args_of(command);
  if (!fargs) return false;
  char* reply_all = reply;             //en: full buffer incl. tag (defer branch blanks it)
  if (tag) { memcpy(reply, tag, 3); reply += 3; }   //en: reflect tag into inline replies
  *reply = 0;

  //en: Inline return-path commands (reply is sent by the caller via client->out_path,
  //en: so a fresh setpath already answers DIRECT down the new route).
  //sk: Inline príkazy spätnej cesty (odpoveď posiela volajúci cez client->out_path,
  //sk: takže čerstvý setpath už odpovedá DIRECT po novej ceste).
  {
    const char* a = fargs;
    while (*a == ' ') a++;

    if (strcmp(a, "getpath") == 0) {
      //en: Report the ACL out_path for THIS client (what the repeater replies along).
      //sk: Vypíš ACL out_path pre TOHTO klienta (kadiaľ mu repeater odpovedá).
      if (client->out_path_len == OUT_PATH_UNKNOWN) {
        strcpy(reply, FOTA_TXT_PATH_UNKNOWN);
      } else {
        uint8_t hs  = (uint8_t)((client->out_path_len >> 6) + 1);
        uint8_t cnt = (uint8_t)(client->out_path_len & 63);
        char* p = reply + sprintf(reply, FOTA_TXT_PATH_HEADER_FMT, (unsigned)hs, (unsigned)cnt);
        for (uint8_t i = 0; i < cnt; i++) {
          if (p - reply > 150) { *p++ = '+'; break; }   //en: LoRa reply cap  //sk: strop LoRa odpovede
          *p++ = (i == 0) ? ' ' : ',';
          for (uint8_t b = 0; b < hs; b++) p += sprintf(p, "%02x", client->out_path[i * hs + b]);
        }
        *p = 0;
      }
      return true;
    }

    if (strncmp(a, "setpath", 7) == 0 && (a[7] == ' ' || a[7] == 0)) {
      uint8_t path[MAX_PATH_SIZE]; uint8_t enc; const char* err = "";
      const char* arg = a + 7;
      while (*arg == ' ') arg++;
      if (*arg == 0) {
        strcpy(reply, FOTA_TXT_SETPATH_USAGE);
      } else if (!fota_parse_path_arg(arg, path, &enc, &err)) {
        sprintf(reply, FOTA_TXT_SETPATH_ERR_FMT, err);
      } else {
        uint8_t hs = (uint8_t)((enc >> 6) + 1), cnt = (uint8_t)(enc & 63);
        memcpy(client->out_path, path, (size_t)cnt * hs);
        client->out_path_len = enc;
        FOTA_DEBUG_PRINTLN("[FOTA] setpath: %u hops (hs=%u) stored into ACL", (unsigned)cnt, (unsigned)hs);
        sprintf(reply, FOTA_TXT_SETPATH_OK_FMT, (unsigned)cnt, cnt == 1 ? FOTA_TXT_HOP_SG : FOTA_TXT_HOP_PL, (unsigned)hs);
      }
      return true;
    }

    if (strncmp(a, "missall ", 8) == 0) {
      //en: missall with a return path: store the path into the ACL first, then defer a
      //en: plain "missall" — the defer snapshot below copies the FRESH out_path, so the
      //en: (potentially long) missing list already goes back DIRECT.
      //sk: missall so spätnou cestou: cestu najprv ulož do ACL a defer-ni holé
      //sk: "missall" — defer snapshot nižšie skopíruje ČERSTVÝ out_path, takže
      //sk: (potenciálne dlhý) zoznam chýbajúcich ide späť už DIRECT.
      uint8_t path[MAX_PATH_SIZE]; uint8_t enc; const char* err = "";
      if (!fota_parse_path_arg(a + 8, path, &enc, &err)) {
        sprintf(reply, FOTA_TXT_MISSALL_PATH_ERR_FMT, err);
        return true;
      }
      uint8_t hs = (uint8_t)((enc >> 6) + 1), cnt = (uint8_t)(enc & 63);
      memcpy(client->out_path, path, (size_t)cnt * hs);
      client->out_path_len = enc;
      FOTA_DEBUG_PRINTLN("[FOTA] missall: path of %u hops (hs=%u) stored into ACL", (unsigned)cnt, (unsigned)hs);
      fargs = " missall";
    }
  }

  if (_fota_cli_pending) {
    strcpy(reply, FOTA_TXT_BUSY);
  } else if (deferFotaCli(client, secret, fargs, path_hash_size, sender_timestamp, tag)) {
#ifdef FOTA_INFO_MSG
    //en: Optional intermediate "processing" packet. DEFAULT OFF: through a repeater two
    //en: packets (this one + the result from loop()) go out back-to-back and the second
    //en: — the important one — can get lost. Without the flag we send a single packet: the final result.
    //sk: Voliteľný medzi-paket "spracúvam". DEFAULT VYP: cez repeater idú dva
    //sk: pakety (tento + výsledok z loop()) tesne za sebou a druhý — podstatný
    //sk: — sa môže stratiť. Bez flagu pošleme len jeden paket: finálny výsledok.
    strcpy(reply, FOTA_TXT_PROCESSING);
#else
    reply_all[0] = 0;   //en: no intermediate packet (not even a bare tag); the reply is sent only once, from loop()
#endif
  } else {
    strcpy(reply, FOTA_TXT_DEFER_FAILED);
  }
  return true;
}

#if FOTA_DEBUG
// =====================================================================
//en: Serial-only debug CLI for return paths (gated by FOTA_DEBUG). The serial
//en: console has no ACL client context, so these take an explicit pub_key hex
//en: prefix: "fota getpath <pfx>", "fota setpath <pfx> <cesta>", "fota getacl".
//en: The LoRa admin CLI has the client-context variants inline in fotaHandleLoRaCli.
//sk: Serial-only debug CLI pre spätné cesty (gated FOTA_DEBUG). Serial konzola
//sk: nemá ACL kontext klienta, preto tieto berú explicitný hex prefix pub_key:
//sk: "fota getpath <pfx>", "fota setpath <pfx> <cesta>", "fota getacl".
//sk: LoRa admin CLI má klientské varianty inline vo fotaHandleLoRaCli.
// =====================================================================

//en: Format a client's out_path as "(1B,2): a1,3f" (or "unknown") into buf.
//sk: Naformátuj out_path klienta ako "(1B,2): a1,3f" (alebo "unknown") do buf.
static const char* fota_client_path_str(const ClientInfo* c, char* buf, int cap) {
  if (c->out_path_len == OUT_PATH_UNKNOWN) {
    strncpy(buf, FOTA_TXT_PATHSTR_UNKNOWN, cap); buf[cap - 1] = 0; return buf;
  }
  uint8_t hs  = (uint8_t)((c->out_path_len >> 6) + 1);
  uint8_t cnt = (uint8_t)(c->out_path_len & 63);
  char* p = buf + sprintf(buf, "(%uB,%u):", (unsigned)hs, (unsigned)cnt);
  for (uint8_t i = 0; i < cnt; i++) {
    if ((int)(p - buf) + hs * 2 + 3 >= cap) { *p++ = '+'; break; }
    *p++ = (i == 0) ? ' ' : ',';
    for (uint8_t b = 0; b < hs; b++) p += sprintf(p, "%02x", c->out_path[i * hs + b]);
  }
  *p = 0;
  return buf;
}

//en: Find an ACL client by a pub_key hex prefix (2-12 hex chars, even count).
//en: On failure points *err at a static reason from FotaTexts.h (no copying).
//sk: Nájdi ACL klienta podľa hex prefixu pub_key (2-12 hex znakov, párny počet).
//sk: Pri chybe nasmeruje *err na statický dôvod z FotaTexts.h (bez kopírovania).
static ClientInfo* fota_client_by_prefix(ClientACL& acl, const char* pfx, int pfx_len, const char** err) {
  uint8_t key[6];
  if (pfx_len < 2 || pfx_len > 12 || (pfx_len & 1)) { *err = FOTA_TXT_ERR_PREFIX_LEN; return NULL; }
  for (int i = 0; i < pfx_len; i++) {
    char c = pfx[i];
    int v = (c >= '0' && c <= '9') ? c - '0'
          : (c >= 'a' && c <= 'f') ? c - 'a' + 10
          : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    if (v < 0) { *err = FOTA_TXT_ERR_PREFIX_NOT_HEX; return NULL; }
    if (i & 1) key[i / 2] |= (uint8_t)v;
    else       key[i / 2]  = (uint8_t)(v << 4);
  }
  ClientInfo* c = acl.getClient(key, pfx_len / 2);
  if (!c) *err = FOTA_TXT_ERR_CLIENT_NOT_FOUND;
  return c;
}

bool RepeaterMesh::fotaHandleSerialPathCli(const char* fargs, char* reply) {
  const char* a = fargs;
  while (*a == ' ') a++;

  if (strcmp(a, "getacl") == 0) {
    int n = acl.getNumClients();
    FOTA_DEBUG_PRINTLN("[FOTA] ACL: %d clients", n);
    for (int i = 0; i < n; i++) {
      ClientInfo* c = acl.getClientByIdx(i);
      char pb[96];
      const uint8_t* k = c->id.pub_key;
      FOTA_DEBUG_PRINTLN("[FOTA] acl[%d] %02x%02x%02x%02x%02x%02x perm=0x%02X%s path %s",
                         i, k[0], k[1], k[2], k[3], k[4], k[5], (unsigned)c->permissions,
                         c->isAdmin() ? " admin" : "", fota_client_path_str(c, pb, sizeof(pb)));
    }
    sprintf(reply, FOTA_TXT_GETACL_FMT, n);
    return true;
  }

  if (strncmp(a, "getpath", 7) == 0 && (a[7] == ' ' || a[7] == 0)) {
    const char* arg = a + 7;
    while (*arg == ' ') arg++;
    if (*arg == 0) { strcpy(reply, FOTA_TXT_GETPATH_USAGE); return true; }
    int len = 0;
    while (arg[len] && arg[len] != ' ') len++;
    const char* err = "";
    ClientInfo* c = fota_client_by_prefix(acl, arg, len, &err);
    if (!c) { sprintf(reply, FOTA_TXT_GETPATH_ERR_FMT, err); return true; }
    char pb[96];
    sprintf(reply, FOTA_TXT_PATH_OF_CLIENT_FMT, len, arg, fota_client_path_str(c, pb, sizeof(pb)));
    return true;
  }

  if (strncmp(a, "setpath", 7) == 0 && (a[7] == ' ' || a[7] == 0)) {
    const char* arg = a + 7;
    while (*arg == ' ') arg++;
    int plen = 0;
    while (arg[plen] && arg[plen] != ' ') plen++;
    const char* rest = arg + plen;
    while (*rest == ' ') rest++;
    if (*arg == 0 || *rest == 0) {
      strcpy(reply, FOTA_TXT_SETPATH_PFX_USAGE);
      return true;
    }
    const char* err = "";
    ClientInfo* c = fota_client_by_prefix(acl, arg, plen, &err);
    if (!c) { sprintf(reply, FOTA_TXT_SETPATH_ERR_FMT, err); return true; }
    uint8_t path[MAX_PATH_SIZE]; uint8_t enc;
    if (!fota_parse_path_arg(rest, path, &enc, &err)) {
      sprintf(reply, FOTA_TXT_SETPATH_ERR_FMT, err);
      return true;
    }
    uint8_t hs = (uint8_t)((enc >> 6) + 1), cnt = (uint8_t)(enc & 63);
    memcpy(c->out_path, path, (size_t)cnt * hs);
    c->out_path_len = enc;
    FOTA_DEBUG_PRINTLN("[FOTA] setpath[%.*s]: %u hops (hs=%u) stored into ACL",
                       plen, arg, (unsigned)cnt, (unsigned)hs);
    sprintf(reply, FOTA_TXT_SETPATH_PFX_OK_FMT,
            plen, arg, (unsigned)cnt, cnt == 1 ? FOTA_TXT_HOP_SG : FOTA_TXT_HOP_PL, (unsigned)hs);
    return true;
  }

  return false;
}
#endif  // FOTA_DEBUG

//en: Body of the FOTA CLI (agc diagnostics + fota_handle_command). Called from
//en: handleCommand (Serial, inline) and from loop() (deferred LoRa path).
//sk: Telo FOTA CLI (agc diagnostika + fota_handle_command). Volané z handleCommand
//sk: (Serial, inline) aj z loop() (odložená LoRa cesta).
void RepeaterMesh::runFotaCli(const char* fargs, char* reply) {
  if (strcmp(fargs, " agc") == 0) {
    //en: Radio diagnostics (READ-ONLY). The ZephCore radio adapter has no raw
    //en: SX1262 register access here — report noise floor + RX counters instead
    //en: (the Arduino build reads the RxGain register 0x08AC).
    //sk: Diagnostika rádia (READ-ONLY). ZephCore radio adapter tu nemá surový
    //sk: prístup k SX1262 registrom — hlásime noise floor + RX počítadlá
    //sk: (Arduino build číta RxGain register 0x08AC).
    FOTA_DEBUG_PRINTLN("[FOTA] AGC nf=%d rxpkts=%lu rxerr=%lu agc_reset=%lus(0=off)",
                       (int)_radio->getNoiseFloor(),
                       (unsigned long)_radio->getPacketsRecv(),
                       (unsigned long)_radio->getPacketsRecvErrors(),
                       (unsigned long)(((uint32_t)_prefs.agc_reset_interval) * 4));
    sprintf(reply, FOTA_TXT_AGC_ZEPHYR_FMT,
            (int)_radio->getNoiseFloor(),
            (unsigned long)_radio->getPacketsRecv(),
            (unsigned long)_radio->getPacketsRecvErrors(),
            (unsigned long)(((uint32_t)_prefs.agc_reset_interval) * 4));
  } else {
    //en: WARNING: do NOT combine AGC auto-reset (set agc.reset.interval > 0) with FOTA flashing!
    //en: If AGC resets run during a FOTA session, the next 'fota flash' fails (see the
    //en: MeshCore tech doc §8.4). With agc_reset=0 both reception and flash work reliably.
    //sk: POZOR: AGC auto-reset (set agc.reset.interval > 0) NEKOMBINOVAŤ s FOTA flashom!
    //sk: Ak agc resety bežia počas FOTA session, nasledujúci 'fota flash' zlyhá (viď
    //sk: MeshCore tech doc §8.4). Pri agc_reset=0 funguje príjem aj flash spoľahlivo.
    fota_handle_command(fargs, reply);   //en: LoRa-FOTA: status|verify|flash|clear|id|...
  }
}

//en: Park the client snapshot in the shared FotaBuffer and mark a pending command.
//en: false = buffer unavailable (already borrowed). The buffer holds the snapshot until
//en: loop() reads and releases it (after which fota_patch_to_file may borrow it for the hpatch cache).
//sk: Zaparkuj snapshot klienta do zdieľaného FotaBuffer a označ čakajúci príkaz.
//sk: false = buffer nedostupný (už požičaný). Buffer drží snapshot až kým ho loop()
//sk: neprečíta a neuvoľní (potom ho fota_patch_to_file môže požičať na hpatch cache).
bool RepeaterMesh::deferFotaCli(const ClientInfo* client, const uint8_t* secret,
                                const char* fargs, uint8_t path_hash_size,
                                uint32_t sender_timestamp, const char* tag) {
  uint8_t* buf = fota_get_buffer(sizeof(FotaCliDefer));
  if (!buf) return false;
  FotaCliDefer* s = (FotaCliDefer*)buf;
  memcpy(s->dest_pub, client->id.pub_key, PUB_KEY_SIZE);
  memcpy(s->secret, secret, PUB_KEY_SIZE);
  s->sender_timestamp = sender_timestamp;
  if (tag) { memcpy(s->tag, tag, 3); s->tag[3] = 0; }
  else     { s->tag[0] = 0; }
  s->out_path_len   = client->out_path_len;
  s->path_hash_size = path_hash_size;
  //en: out_path_len is the ENCODED path_len (hop count in low 6 bits, hash_size-1 in top 2),
  //en: NOT a byte count — decode to the real byte length and guard THAT (see the Arduino
  //en: original for the full history of this bug).
  //sk: out_path_len je ENCODED path_len (počet hopov v spodných 6 bitoch, hash_size-1
  //sk: v horných 2), NIE počet bajtov — dekóduj na skutočnú bajtovú dĺžku a strážiž TÚ
  //sk: (plná história bugu v Arduino origináli).
  if (client->out_path_len != OUT_PATH_UNKNOWN) {
    uint8_t nbytes = (client->out_path_len & 63) * ((client->out_path_len >> 6) + 1);
    if (nbytes <= MAX_PATH_SIZE) memcpy(s->out_path, client->out_path, nbytes);
  }
  strncpy(s->fargs, fargs, sizeof(s->fargs) - 1);
  s->fargs[sizeof(s->fargs) - 1] = 0;
  _fota_cli_buf     = buf;
  _fota_cli_pending = true;
  return true;
}

//en: Send the CLI text reply to the client from the snapshot (from loop(), after the
//en: deferred command finishes). Mirrors the onPeerDataRecv TXT_MSG reply path.
//sk: Pošli CLI textovú odpoveď klientovi zo snapshotu (z loop(), po dobehnutí
//sk: odloženého príkazu). Zrkadlí reply cestu onPeerDataRecv TXT_MSG vetvy.
void RepeaterMesh::sendDeferredCliReply(const uint8_t* dest_pub, const uint8_t* secret,
                                        const uint8_t* out_path, uint8_t out_path_len,
                                        uint8_t path_hash_size, const char* text,
                                        uint32_t sender_timestamp) {
  int text_len = strlen(text);
  if (text_len <= 0) return;
  if (text_len > 160) text_len = 160;
  //en: Symmetric to the "[LoRa->CLI]" echo — the operator at the board also sees the
  //en: outgoing reply.
  //sk: Symetria k "[LoRa->CLI]" echu — operátor pri doske vidí aj odchádzajúcu odpoveď.
  FOTA_DEBUG_PRINTLN("[CLI->LoRa] %s", text);
  uint8_t temp[166];
  uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
  if (timestamp <= sender_timestamp) {
    //en: The reply must have a HIGHER timestamp than the command, otherwise the companion
    //en: (seen-table dedup by ts) filters it out as a duplicate and never shows it. Covers TWO cases:
    //en:  1) synced clock + fast reply at idle → reply_ts == sender_ts (collision),
    //en:  2) WRONG repeater clock (after a reboot) → reply_ts < sender_ts → the companion
    //en:     would sort the reply into the past / filter it out. In both we pull it above sender_ts.
    //sk: Odpoveď musí mať timestamp VYŠŠÍ než príkaz, inak ju companion (seen-table
    //sk: dedup podľa ts) odfiltruje ako duplikát a nezobrazí. Pokrýva DVA prípady:
    //sk:  1) zosynced čas + rýchla odpoveď v kľude → reply_ts == sender_ts (kolízia),
    //sk:  2) ZLÝ čas repeatera (po reboote) → reply_ts < sender_ts → companion by
    //sk:     odpoveď zoradil do minulosti / odfiltroval. V oboch ho ťaháme nad sender_ts.
    timestamp = sender_timestamp + 1;
  }
  memcpy(temp, &timestamp, 4);
  temp[4] = (TXT_TYPE_CLI_DATA << 2);
  memcpy(&temp[5], text, text_len);
  mesh::Identity id(dest_pub);
  auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, id, secret, temp, 5 + text_len);
  if (!pkt) return;
  if (out_path_len == OUT_PATH_UNKNOWN) {
    sendFloodReply(pkt, FOTA_CLI_REPLY_DELAY_MILLIS, path_hash_size);
  } else {
    sendDirect(pkt, (uint8_t*)out_path, out_path_len, FOTA_CLI_REPLY_DELAY_MILLIS);
  }
}

// =====================================================================
//en: fotaLoop — deferred work (hook from RepeaterMesh::loop, AFTER mesh::Mesh::loop()).
//sk: fotaLoop — deferred práca (hook z RepeaterMesh::loop, PO mesh::Mesh::loop()).
// =====================================================================
void RepeaterMesh::fotaLoop() {
  //en: Deferred FOTA processing — mesh::Mesh::loop() above has already re-armed the radio
  //en: into RX, so the slow FS I/O here no longer blocks reception of the next packet.
  //sk: Odložené FOTA spracovanie — mesh::Mesh::loop() vyššie už re-armol rádio do RX,
  //sk: takže pomalé FS I/O tu už nezablokuje príjem ďalšieho paketu.
  if (_fota_pending_len > 0) {
    int n = _fota_pending_len;
    fota_print_pkt(_fota_pending + 4, n - 4, _fota_pending_rssi, _fota_pending_snr);
    fota_process(_fota_pending + 4, n - 4);
    _fota_pending_len = 0;   //en: release the buffer only after processing
  }

  //en: Deferred FOTA CLI (from LoRa) — runs here off the RX path like the Serial path.
  //en: Copy the snapshot from the shared FotaBuffer onto the loop stack, RELEASE
  //en: the buffer (so fota_patch_to_file can borrow it for the hpatch cache), and only THEN
  //en: run the command. Send the result to the client. (flash → fota_apply does not return.)
  //sk: Odložené FOTA CLI (z LoRa) — tu beží mimo RX cesty ako Serial cesta.
  //sk: Snapshot skopíruj zo zdieľaného FotaBuffer na loop stack, buffer UVOĽNI
  //sk: (aby ho fota_patch_to_file mohol požičať na hpatch cache), a až POTOM
  //sk: spusti príkaz. Výsledok pošli klientovi. (flash → fota_apply nevráti sa.)
  if (_fota_cli_pending && _fota_cli_buf) {
    FotaCliDefer snap;
    memcpy(&snap, _fota_cli_buf, sizeof(snap));
    fota_put_buffer(_fota_cli_buf);
    _fota_cli_buf     = nullptr;
    _fota_cli_pending = false;

    char reply[166];
    //en: Reflect the "NN|" tag (if the command carried one) so the app's
    //en: RepeaterCommandService can match the deferred reply to its request.
    //sk: Zrkadli "NN|" tag (ak ho príkaz niesol), aby appkin RepeaterCommandService
    //sk: vedel odloženú odpoveď spárovať s requestom.
    char* rp = reply;
    if (snap.tag[0]) { memcpy(rp, snap.tag, 3); rp += 3; }
    rp[0] = 0;
    runFotaCli(snap.fargs, rp);   //en: heavy work (verify=hpatch+SHA)
    if (rp[0]) {
      sendDeferredCliReply(snap.dest_pub, snap.secret, snap.out_path,
                           snap.out_path_len, snap.path_hash_size, reply,
                           snap.sender_timestamp);
    }
  }

  //en: Deferred flash: 'fota flash' sets fota_apply_pending() and sends an "accepted" ACK.
  //en: The actual flash (fota_apply, does NOT return) starts ONLY once the ACK has actually
  //en: left the outbound queue — otherwise the reboot would come before the ACK is transmitted
  //en: and the sender would receive nothing. Safety net: a deadline so the flash does not
  //en: wait forever when other traffic sits in the queue.
  //sk: Odložený flash: 'fota flash' nastaví fota_apply_pending() a pošle ACK „accepted".
  //sk: Skutočný flash (fota_apply, NEVRÁTI sa) spustíme AŽ keď ACK reálne odíde z
  //sk: outbound queue — inak by reboot prišiel skôr než sa ACK odvysiela a odosielateľ
  //sk: by nič nedostal. Safety net: deadline, aby flash nečakal donekonečna pri inej
  //sk: premávke v queue.
  if (fota_apply_pending()) {
    if (_fota_apply_deadline == 0) _fota_apply_deadline = futureMillis(6000);
    if (_mgr->getOutboundTotal() == 0 || millisHasNowPassed(_fota_apply_deadline)) {
      fota_clear_apply_pending();
      _fota_apply_deadline = 0;
      FOTA_DEBUG_PRINTLN("[FOTA] ACK sent — starting flash");
      fota_apply();   //en: does NOT return on success (jump to flasher + reboot)
      FOTA_DEBUG_PRINTLN("[FOTA] flash failed before the jump (see above)");
    }
  }

#if FOTA_DEBUG
  //en: Heartbeat with build# (to detect the version during FOTA tests over the console).
  //en: Note: the ZephCore main loop is event-driven (k_event_wait + 5s housekeeping),
  //en: so the heartbeat period is approximate.
  //sk: Heartbeat s build# (na detekciu verzie počas FOTA testov cez konzolu).
  //sk: Pozn.: ZephCore hlavná slučka je event-driven (k_event_wait + 5s housekeeping),
  //sk: takže perióda heartbeatu je približná.
  static unsigned long s_next_build_print = 0;
  if (s_next_build_print == 0 || millisHasNowPassed(s_next_build_print)) {
    s_next_build_print = futureMillis(25000);
    FOTA_DEBUG_PRINTLN("[FOTA] AALIVE build #%lu  sf=%u rawrx=%lu rawtx=%lu rxpkts=%lu rxerr=%lu nf=%d",
                       (unsigned long)FW_BUILD_NUMBER, (unsigned)_prefs.sf,
                       (unsigned long)_fota_raw_rx, (unsigned long)_fota_raw_tx,
                       (unsigned long)_radio->getPacketsRecv(),
                       (unsigned long)_radio->getPacketsRecvErrors(),
                       (int)_radio->getNoiseFloor());
  }
#endif // FOTA_DEBUG
}

#endif  // WITH_LORA_FOTA
