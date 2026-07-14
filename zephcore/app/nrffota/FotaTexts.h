#pragma once
// =====================================================================
//en: FotaTexts.h — central catalog of FOTA user-facing texts (CLI replies).
//
//en: WHAT BELONGS HERE: texts that end up in the CLI 'reply' buffer — they are
//en: functional output for the client (Serial console AND LoRa admin CLI,
//en: reply cap ~160 B; dry-run reason buffer is 48 B — keep reasons short).
//en: WHAT DOES NOT: FOTA_DEBUG_PRINT/PRINTLN diagnostics — those stay inline
//en: (in English) at the call sites; see the CATALOG at the bottom of this file.
//
//en: LANGUAGE: default is English; -D FOTA_LANG_SK=1 selects Slovak (set in the
//en: FOTA envs in variants/*/platformio.ini). MACHINE-PARSED formats (status,
//en: miss=, id, path listings) are language-NEUTRAL and defined only once —
//en: the Flutter app and test_nrf-fota scripts parse them, do NOT localize.
//
//en: USAGE RULES:
//en:  - static message  -> pass the macro as a const char* (no copying)
//en:  - formatted text  -> the macro is the format string; snprintf into reply
//
//sk: FotaTexts.h — centrálny katalóg FOTA textov pre používateľa (CLI odpovede).
//
//sk: ČO SEM PATRÍ: texty, ktoré končia v CLI 'reply' buffri — sú to funkčné
//sk: výstupy pre klienta (Serial konzola AJ LoRa admin CLI, strop odpovede
//sk: ~160 B; buffer dôvodu dry-runu má 48 B — dôvody drž krátke).
//sk: ČO NIE: FOTA_DEBUG_PRINT/PRINTLN diagnostika — tá ostáva inline
//sk: (po anglicky) na mieste volania; viď KATALÓG na konci tohto súboru.
//
//sk: JAZYK: default angličtina; -D FOTA_LANG_SK=1 vyberie slovenčinu (nastavené
//sk: vo FOTA env vo variants/*/platformio.ini). STROJOVO PARSOVANÉ formáty
//sk: (status, miss=, id, výpisy ciest) sú jazykovo NEUTRÁLNE a definované len
//sk: raz — parsuje ich Flutter appka a test_nrf-fota skripty, NElokalizovať.
//
//sk: PRAVIDLÁ POUŽITIA:
//sk:  - statická hláška   -> odovzdaj makro ako const char* (bez kopírovania)
//sk:  - formátovaný text  -> makro je formátovací reťazec; snprintf do reply
// =====================================================================

// ─────────────────────────────────────────────────────────────────────
//en: LANGUAGE-NEUTRAL, machine-parsed / language-free (single definition).
//sk: JAZYKOVO NEUTRÁLNE, strojovo parsované / bez jazyka (jediná definícia).
// ─────────────────────────────────────────────────────────────────────
//en: 'fota status' reply — FotaMesh.cpp: fota_handle_command(). PARSED by
//en: test_nrf-fota/fota_test_lora_repeater.py + fota_sender.py (regex "FOTA d/d st=0x..").
//sk: Odpoveď 'fota status' — FotaMesh.cpp: fota_handle_command(). PARSUJÚ ju
//sk: test_nrf-fota/fota_test_lora_repeater.py + fota_sender.py (regex "FOTA d/d st=0x..").
#define FOTA_TXT_STATUS_FMT           "FOTA %u/%u st=0x%02X size=%lu err=0x%02X"

//en: 'fota verify' (dry-run) result — FotaMesh.cpp: fota_handle_command(); %s = "OK"/"FAIL"(+reason).
//sk: Výsledok 'fota verify' (dry-run) — FotaMesh.cpp: fota_handle_command(); %s = "OK"/"FAIL"(+dôvod).
#define FOTA_TXT_DRYRUN_RESULT_FMT    "FOTA dry-run %s: %s"
#define FOTA_TXT_DRYRUN_RESULT_SHORT_FMT "FOTA dry-run %s"

//en: 'fota flash' accepted ACK (flash starts from loop()) — FotaMesh.cpp: fota_handle_command().
//sk: ACK prijatia 'fota flash' (flash spúšťa loop()) — FotaMesh.cpp: fota_handle_command().
#define FOTA_TXT_FLASH_ACCEPTED       "FOTA flash accepted"

//en: 'fota clear' done — FotaMesh.cpp: fota_handle_command().
//sk: 'fota clear' hotovo — FotaMesh.cpp: fota_handle_command().
#define FOTA_TXT_CLEARED              "FOTA cleared"

//en: Commands whose full output goes to Serial only — FotaMesh.cpp: fota_handle_command().
//sk: Príkazy s plným výstupom len na Serial — FotaMesh.cpp: fota_handle_command().
#define FOTA_TXT_DECOMP_TO_SERIAL     "FOTA decompress -> serial"
#define FOTA_TXT_NACK_TO_SERIAL       "FOTA nack -> serial"
#define FOTA_TXT_DBG_TO_SERIAL        "FOTA dbg -> serial"

//en: 'fota miss' with no session info at all — FotaMesh.cpp: fota_handle_command().
//en: Part of the miss-report format the Flutter app parses — do not change.
//sk: 'fota miss' bez akejkoľvek session informácie — FotaMesh.cpp: fota_handle_command().
//sk: Súčasť miss-report formátu, ktorý parsuje Flutter appka — nemeniť.
#define FOTA_TXT_MISS_ZERO_INFO       "FOTA miss: Zero info yet"

//en: 'fota id' reply (build#, image size, sha prefix) — FotaReceiver.cpp: fota_print_fw_id().
//sk: Odpoveď 'fota id' (build#, veľkosť image, sha prefix) — FotaReceiver.cpp: fota_print_fw_id().
#define FOTA_TXT_ID_FMT               "id b#%lu sz=%lu sha=%02X%02X%02X%02X"

//en: 'fota agc' radio gain diagnostics reply — MeshCore: FotaMyMesh.cpp runFotaCli();
//en: ZephCore variant (no raw SX1262 register access) — FotaRepeaterMesh.cpp runFotaCli().
//sk: Odpoveď 'fota agc' — diagnostika gain rádia — MeshCore: FotaMyMesh.cpp runFotaCli();
//sk: ZephCore variant (bez prístupu k SX1262 registrom) — FotaRepeaterMesh.cpp runFotaCli().
#define FOTA_TXT_AGC_FMT              "AGC gain=0x%02X(%s) boost=%s rssi=%ddBm nf=%d agc_reset=%lus"
#define FOTA_TXT_AGC_ZEPHYR_FMT       "AGC (zephyr) nf=%d rxpkts=%lu rxerr=%lu agc_reset=%lus"

//en: 'fota getpath' (LoRa, this client) — FotaMyMesh.cpp: fotaHandleLoRaCli().
//sk: 'fota getpath' (LoRa, tento klient) — FotaMyMesh.cpp: fotaHandleLoRaCli().
#define FOTA_TXT_PATH_UNKNOWN         "FOTA path: unknown (reply=flood)"
#define FOTA_TXT_PATH_HEADER_FMT      "FOTA path (%uB,%u):"

//en: 'fota getpath <pfx>' (Serial debug CLI) — FotaMyMesh.cpp: fotaHandleSerialPathCli().
//sk: 'fota getpath <pfx>' (Serial debug CLI) — FotaMyMesh.cpp: fotaHandleSerialPathCli().
#define FOTA_TXT_GETPATH_USAGE        "FOTA getpath <pubkey-prefix-hex>"
#define FOTA_TXT_PATH_OF_CLIENT_FMT   "FOTA path[%.*s] %s"
#define FOTA_TXT_GETPATH_ERR_FMT      "FOTA getpath ERR: %s"

//en: 'fota setpath' results — FotaMyMesh.cpp: fotaHandleLoRaCli() (plain) and
//en: fotaHandleSerialPathCli() ([%.*s] prefix variant). %s = FOTA_TXT_HOP_SG/PL.
//sk: Výsledky 'fota setpath' — FotaMyMesh.cpp: fotaHandleLoRaCli() (holý) a
//sk: fotaHandleSerialPathCli() (variant s [%.*s] prefixom). %s = FOTA_TXT_HOP_SG/PL.
#define FOTA_TXT_SETPATH_OK_FMT       "FOTA setpath OK: %u %s (%uB)"
#define FOTA_TXT_SETPATH_PFX_OK_FMT   "FOTA setpath[%.*s] OK: %u %s (%uB)"
#define FOTA_TXT_SETPATH_ERR_FMT      "FOTA setpath ERR: %s"

//en: out_path formatter fallback — FotaMyMesh.cpp: fota_client_path_str().
//sk: Fallback formátovania out_path — FotaMyMesh.cpp: fota_client_path_str().
#define FOTA_TXT_PATHSTR_UNKNOWN      "unknown"

//en: Hop-list parse error (language-free) — FotaMyMesh.cpp: fota_parse_path_arg().
//sk: Chyba parsovania hop-listu (bez jazyka) — FotaMyMesh.cpp: fota_parse_path_arg().
#define FOTA_TXT_ERR_HOP_TOO_BIG      "hop > 3B"

//en: Dry-run reasons (language-free) — FotaPatcher.cpp: fota_patch_to_file().
//sk: Dôvody dry-runu (bez jazyka) — FotaPatcher.cpp: fota_patch_to_file().
#define FOTA_TXT_VFY_OOM_PUFF         "OOM puff_stream"
#define FOTA_TXT_VFY_SCRATCH_BUSY     "scratch busy"

// ─────────────────────────────────────────────────────────────────────
//en: LOCALIZED texts (EN default / SK with -D FOTA_LANG_SK=1)
//sk: LOKALIZOVANÉ texty (default EN / SK cez -D FOTA_LANG_SK=1)
// ─────────────────────────────────────────────────────────────────────

//en: LoRa CLI defer branch — FotaMyMesh.cpp: fotaHandleLoRaCli().
//en: FOTA_TXT_PROCESSING is sent only with -D FOTA_INFO_MSG (default OFF).
//sk: Defer vetva LoRa CLI — FotaMyMesh.cpp: fotaHandleLoRaCli().
//sk: FOTA_TXT_PROCESSING sa posiela len s -D FOTA_INFO_MSG (default VYP).
#ifdef FOTA_LANG_SK
  #define FOTA_TXT_BUSY               "FOTA: zaneprázdnené, skús neskôr"
  #define FOTA_TXT_PROCESSING         "FOTA: spracúvam, výsledok o chvíľu..."
  #define FOTA_TXT_DEFER_FAILED       "FOTA: defer zlyhal (buffer)"
#else
  #define FOTA_TXT_BUSY               "FOTA: busy, try again later"
  #define FOTA_TXT_PROCESSING         "FOTA: processing, result in a moment..."
  #define FOTA_TXT_DEFER_FAILED       "FOTA: defer failed (buffer)"
#endif

//en: setpath/missall usage + errors — FotaMyMesh.cpp: fotaHandleLoRaCli()
//en: (FOTA_TXT_SETPATH_USAGE, FOTA_TXT_MISSALL_PATH_ERR_FMT) and
//en: fotaHandleSerialPathCli() (FOTA_TXT_SETPATH_PFX_USAGE).
//sk: setpath/missall usage + chyby — FotaMyMesh.cpp: fotaHandleLoRaCli()
//sk: (FOTA_TXT_SETPATH_USAGE, FOTA_TXT_MISSALL_PATH_ERR_FMT) a
//sk: fotaHandleSerialPathCli() (FOTA_TXT_SETPATH_PFX_USAGE).
#ifdef FOTA_LANG_SK
  #define FOTA_TXT_SETPATH_USAGE      "FOTA setpath: zadaj hopy nn,nn / nnnn,... / nnnnnn,... (1-3B)"
  #define FOTA_TXT_SETPATH_PFX_USAGE  "FOTA setpath <pubkey-prefix-hex> <cesta nn,nn|nnnn,...|nnnnnn,...>"
  #define FOTA_TXT_MISSALL_PATH_ERR_FMT "FOTA missall ERR cesta: %s"
#else
  #define FOTA_TXT_SETPATH_USAGE      "FOTA setpath: enter hops nn,nn / nnnn,... / nnnnnn,... (1-3B)"
  #define FOTA_TXT_SETPATH_PFX_USAGE  "FOTA setpath <pubkey-prefix-hex> <path nn,nn|nnnn,...|nnnnnn,...>"
  #define FOTA_TXT_MISSALL_PATH_ERR_FMT "FOTA missall ERR path: %s"
#endif

//en: Hop word for setpath OK replies (singular/plural) — FotaMyMesh.cpp:
//en: fotaHandleLoRaCli() + fotaHandleSerialPathCli().
//sk: Slovo "hop" pre setpath OK odpovede (jednotné/množné) — FotaMyMesh.cpp:
//sk: fotaHandleLoRaCli() + fotaHandleSerialPathCli().
#ifdef FOTA_LANG_SK
  #define FOTA_TXT_HOP_SG             "hop"
  #define FOTA_TXT_HOP_PL             "hopy"
#else
  #define FOTA_TXT_HOP_SG             "hop"
  #define FOTA_TXT_HOP_PL             "hops"
#endif

//en: 'fota getacl' summary (full list goes to Serial) — FotaMyMesh.cpp: fotaHandleSerialPathCli().
//sk: Súhrn 'fota getacl' (plný zoznam ide na Serial) — FotaMyMesh.cpp: fotaHandleSerialPathCli().
#ifdef FOTA_LANG_SK
  #define FOTA_TXT_GETACL_FMT         "FOTA getacl: %d klientov -> serial"
#else
  #define FOTA_TXT_GETACL_FMT         "FOTA getacl: %d clients -> serial"
#endif

//en: Hop-list parse errors — FotaMyMesh.cpp: fota_parse_path_arg().
//en: (returned via const char** err; embedded into setpath/missall ERR replies)
//sk: Chyby parsovania hop-listu — FotaMyMesh.cpp: fota_parse_path_arg().
//sk: (vracané cez const char** err; vkladané do setpath/missall ERR odpovedí)
#ifdef FOTA_LANG_SK
  #define FOTA_TXT_ERR_PATH_EMPTY     "prazdna cesta"
  #define FOTA_TXT_ERR_PATH_NOT_HEX   "cesta nie je hex"
  #define FOTA_TXT_ERR_HOP_WIDTH      "hop musi mat 2/4/6 hex znakov"
  #define FOTA_TXT_ERR_HOPS_MIXED     "hopy maju roznu dlzku"
  #define FOTA_TXT_ERR_PATH_TOO_LONG  "cesta pridlha"
#else
  #define FOTA_TXT_ERR_PATH_EMPTY     "empty path"
  #define FOTA_TXT_ERR_PATH_NOT_HEX   "path is not hex"
  #define FOTA_TXT_ERR_HOP_WIDTH      "hop must be 2/4/6 hex chars"
  #define FOTA_TXT_ERR_HOPS_MIXED     "hops differ in length"
  #define FOTA_TXT_ERR_PATH_TOO_LONG  "path too long"
#endif

//en: pub_key prefix lookup errors — FotaMyMesh.cpp: fota_client_by_prefix().
//en: (returned via const char** err; embedded into getpath/setpath ERR replies)
//sk: Chyby hľadania podľa pub_key prefixu — FotaMyMesh.cpp: fota_client_by_prefix().
//sk: (vracané cez const char** err; vkladané do getpath/setpath ERR odpovedí)
#ifdef FOTA_LANG_SK
  #define FOTA_TXT_ERR_PREFIX_LEN     "prefix = 2-12 hex znakov"
  #define FOTA_TXT_ERR_PREFIX_NOT_HEX "prefix nie je hex"
  #define FOTA_TXT_ERR_CLIENT_NOT_FOUND "klient nenajdeny v ACL"
#else
  #define FOTA_TXT_ERR_PREFIX_LEN     "prefix = 2-12 hex chars"
  #define FOTA_TXT_ERR_PREFIX_NOT_HEX "prefix is not hex"
  #define FOTA_TXT_ERR_CLIENT_NOT_FOUND "client not found in ACL"
#endif

//en: 'fota flash' refused (not VERIFIED yet) — FotaMesh.cpp: fota_handle_command().
//sk: 'fota flash' odmietnutý (ešte nie je VERIFIED) — FotaMesh.cpp: fota_handle_command().
#ifdef FOTA_LANG_SK
  #define FOTA_TXT_FLASH_NOT_VERIFIED "FOTA flash: nie je VERIFIED (najprv prijmi chunky + verify)"
#else
  #define FOTA_TXT_FLASH_NOT_VERIFIED "FOTA flash: not VERIFIED (receive chunks + verify first)"
#endif

//en: Usage / unknown subcommand — FotaMesh.cpp: fota_handle_command() (else branch).
//sk: Usage / neznámy podpríkaz — FotaMesh.cpp: fota_handle_command() (else vetva).
#ifdef FOTA_LANG_SK
  #define FOTA_TXT_USAGE "FOTA: status|verify|flash|clear|decompress|nack|miss|missall [cesta]|getpath|setpath|getacl|dbg|id"
#else
  #define FOTA_TXT_USAGE "FOTA: status|verify|flash|clear|decompress|nack|miss|missall [path]|getpath|setpath|getacl|dbg|id"
#endif

//en: Dry-run FAIL/OK reasons (48 B buffer!) — FotaPatcher.cpp: fota_patch_to_file()
//en: (via set_err); shown as "FOTA dry-run FAIL: <reason>" by FotaMesh.cpp.
//sk: Dôvody dry-run FAIL/OK (48 B buffer!) — FotaPatcher.cpp: fota_patch_to_file()
//sk: (cez set_err); zobrazené ako "FOTA dry-run FAIL: <dôvod>" vo FotaMesh.cpp.
#ifdef FOTA_LANG_SK
  #define FOTA_TXT_VFY_NO_PATCH_DATA  "ziadne patch data"
  #define FOTA_TXT_VFY_BAD_ZLIB_HDR   "zly ZLIB/hpatch header"
  #define FOTA_TXT_VFY_EXTRASAFE_FMT  "extraSafe %lu > max %lu, flash zlyha"
  #define FOTA_TXT_VFY_DECOMP_ERR_FMT "dekompresia err=%d"
  #define FOTA_TXT_VFY_HPATCH_FAILED_ZLIB "hpatch zlyhal (ZLIB)"
  #define FOTA_TXT_VFY_BAD_PATCH_FORMAT "zly format patchu"
  #define FOTA_TXT_VFY_COMPRESSED_UNSUPPORTED "komprimovany nepodporovany"
  #define FOTA_TXT_VFY_EXP_SHA_UNKNOWN "ocak. SHA neznama"
  #define FOTA_TXT_VFY_SHA_MISMATCH   "SHA256 nesedi"
  #define FOTA_TXT_VFY_HPATCH_FAILED  "hpatch zlyhal"
  #define FOTA_TXT_VFY_HPATCHLITE_MISSING "hpatchlite chyba"
#else
  #define FOTA_TXT_VFY_NO_PATCH_DATA  "no patch data"
  #define FOTA_TXT_VFY_BAD_ZLIB_HDR   "bad ZLIB/hpatch header"
  #define FOTA_TXT_VFY_EXTRASAFE_FMT  "extraSafe %lu > max %lu, flash fails"
  #define FOTA_TXT_VFY_DECOMP_ERR_FMT "decompress err=%d"
  #define FOTA_TXT_VFY_HPATCH_FAILED_ZLIB "hpatch failed (ZLIB)"
  #define FOTA_TXT_VFY_BAD_PATCH_FORMAT "bad patch format"
  #define FOTA_TXT_VFY_COMPRESSED_UNSUPPORTED "compressed unsupported"
  #define FOTA_TXT_VFY_EXP_SHA_UNKNOWN "expected SHA unknown"
  #define FOTA_TXT_VFY_SHA_MISMATCH   "SHA256 mismatch"
  #define FOTA_TXT_VFY_HPATCH_FAILED  "hpatch failed"
  #define FOTA_TXT_VFY_HPATCHLITE_MISSING "hpatchlite missing"
#endif

// =====================================================================
//en: DEBUG MESSAGE CATALOG (FOTA_DEBUG_PRINT/PRINTLN) — reference only.
//en: The strings live INLINE at the call sites, in ENGLISH (not localized;
//en: compiled out entirely without -D FOTA_DEBUG=1). This catalog maps each
//en: message with prose to its Slovak meaning and its location, grouped by
//en: file -> function. Pure numeric/hex-dump formats are omitted.
//en: NOTE: "FNV-1a of output" is parsed by test_nrf-fota/fota_test_lora_repeater.py.
//sk: KATALÓG DEBUG SPRÁV (FOTA_DEBUG_PRINT/PRINTLN) — len referencia.
//sk: Reťazce žijú INLINE na mieste volania, po ANGLICKY (nelokalizujú sa;
//sk: bez -D FOTA_DEBUG=1 sa vôbec nekompilujú). Tento katalóg mapuje každú
//sk: správu s textom na jej slovenský význam a miesto, zoskupené podľa
//sk: súbor -> funkcia. Čisto číselné/hex-dump formáty sú vynechané.
//sk: POZN.: "FNV-1a of output" parsuje test_nrf-fota/fota_test_lora_repeater.py.
// =====================================================================
//
// ── FotaMyMesh.cpp ───────────────────────────────────────────────────
//en: (ZephCore glue FotaRepeaterMesh.cpp mirrors these messages 1:1 — apply changes to both.)
//sk: (ZephCore glue FotaRepeaterMesh.cpp tieto správy zrkadlí 1:1 — zmeny rob v oboch.)
//en: onGroupDataRecv():      "WARN pending buffer busy, packet dropped"
//sk:                          WARN pending buffer obsadený, paket zahodený
//en: fotaHandleLoRaCli():    "setpath: %u hops (hs=%u) stored into ACL"
//sk:                          setpath: %u hopov (hs=%u) uložených do ACL
//en: fotaHandleLoRaCli():    "missall: path of %u hops (hs=%u) stored into ACL"
//sk:                          missall: cesta %u hopov (hs=%u) uložená do ACL
//en: fotaHandleSerialPathCli(): "ACL: %d clients"
//sk:                          ACL: %d klientov
//en: fotaHandleSerialPathCli(): "setpath[%.*s]: %u hops (hs=%u) stored into ACL"
//sk:                          setpath[%.*s]: %u hopov (hs=%u) uložených do ACL
//en: runFotaCli():           "AGC rxgain_reg=... agc_reset=%lus(0=off)"
//sk:                          AGC rxgain_reg=... agc_reset=%lus(0=vypnuté)
//en: fotaLoop():             "ACK sent — starting flash"
//sk:                          ACK odoslaný — spúšťam flash
//en: fotaLoop():             "flash failed before the jump (see above)"
//sk:                          flash zlyhal pred skokom (pozri vyššie)
//
// ── FotaMesh.cpp ─────────────────────────────────────────────────────
//en: fota_build_channel():   "channel %s hash=0x%02X"
//sk:                          kanál %s hash=0x%02X
//en: fota_handle_command():  "miss Zero info yet"
//sk:                          miss zatiaľ žiadna informácia
//
// ── FotaReceiver.cpp ─────────────────────────────────────────────────
//en: verify_header_signature(): "UNKNOWN key_id=0x%X"
//sk:                          NEZNÁMY key_id=0x%X
//en: fota_print_fw_id():     "!! WARNING: trailer != linker size — wrong board config?"
//sk:                          !! POZOR: trailer != linker veľkosť — zlá board konfig?
//en: fota_print_fw_id():     "^ compare with old_sha256 in .fotapkg.json"
//sk:                          ^ porovnaj s old_sha256 v .fotapkg.json
//en: fota_print_fw_id():     "running sha256: image_size invalid"
//sk:                          running sha256: image_size neplatná
//en: fota_fw_size_matches(): "base FW: old_fw_size %lu != running %lu"
//sk:                          base FW: old_fw_size %lu != bežiace %lu
//en: fota_base_fw_validated(): "base FW: fw_size %lu > app window"
//sk:                          base FW: fw_size %lu > app okno
//en: fota_set_error():       "ERROR=0x%X"
//sk:                          CHYBA=0x%X
//en: save_meta():            "meta: write failed"
//sk:                          meta: zápis zlyhal
//en: log_append():           "log: write failed"
//sk:                          log: zápis zlyhal
//en: verify_log_sha() + assemble_and_verify(): "log: read failed"
//sk:                          log: čítanie zlyhalo
//en: verify_log_sha() + assemble_and_verify(): "missing chunk %u"
//sk:                          chýba chunk %u
//en: verify_log_sha() + assemble_and_verify(): "SHA256 MISMATCH  got="
//sk:                          SHA256 NESÚHLASÍ  got=
//en: assemble_and_verify():  "Verifying patch SHA256 (RAM, no patch.bin)..."
//sk:                          Overujem patch SHA256 (RAM, bez patch.bin)...
//en: assemble_and_verify():  "patch SHA256 OK (recv.log remains the source)"
//sk:                          patch SHA256 OK (recv.log ostáva ako zdroj)
//en: assemble_and_verify():  "Assembling patch.bin..." / "assembly failed"
//sk:                          Zostavujem patch.bin... / zostava zlyhala
//en: assemble_and_verify():  "recv.log deleted (patch.bin is the source)"
//sk:                          recv.log zmazaný (patch.bin je zdroj)
//en: fota_acquire_patch_ram(): "patch.bin missing" / "malloc %lu B failed" /
//en:                         "reading patch.bin failed" / "invalid patch_size"
//sk:                          patch.bin chýba / malloc %lu B zlyhal /
//sk:                          čítanie patch.bin zlyhalo / neplatná patch_size
//en: fota_acquire_patch_ram(): "malloc %lu B failed (RAM assembly) — for large patches try -D USE_PATCHBIN_FILE"
//sk:                          malloc %lu B zlyhal (RAM assembly) — pre veľké patche skús -D USE_PATCHBIN_FILE
//en: fota_acquire_patch_ram(): "RAM assembly from recv.log failed"
//sk:                          RAM assembly z recv.log zlyhala
//en: fota_init():            "FS corrupted (post-flash?), reformatting..."
//sk:                          FS poškodený (post-flash?), reformátujem...
//en: fota_init():            "FS: format+begin failed — FS unavailable"
//sk:                          FS: format+begin zlyhalo — FS nedostupný
//en: try_verify_header():    "HEADER: UNSIGNED (FOTA_ALLOW_UNSIGNED)" / "HEADER: INVALID signature — rejecting"
//sk:                          HEADER: NEPODPÍSANÝ (FOTA_ALLOW_UNSIGNED) / HEADER: NEPLATNÝ podpis — odmietam
//en: try_verify_header():    "HEADER: bad total_chunks=%lu"
//sk:                          HEADER: zlé total_chunks=%lu
//en: try_verify_header():    "HEADER OK (META+SIG verified) chunks=%lu  have %u chunks"
//sk:                          HEADER OK (META+SIG overené) chunks=%lu  mám %u chunkov
//en: try_verify_header() + handle_chunk(): "COMPLETE — assembly + SHA256..."
//sk:                          COMPLETE — zostavenie + SHA256...
//en: try_verify_header() + handle_chunk(): "VERIFIED — 'fota verify'=dry-run | 'fota flash'=flash+reboot"
//sk:                          VERIFIED — 'fota verify'=dry-run | 'fota flash'=flash+reboot
//en: handle_meta():          "META: short" (also "(short)" in fota_print_pkt dumps)
//sk:                          META: krátky ( "(krátky)" aj vo fota_print_pkt výpisoch)
//en: handle_meta():          "META: base FW mismatch — patch is not for this device, drop"
//sk:                          META: base FW nezhoda — patch nie je pre toto zariadenie, drop
//en: handle_meta():          "META received patch_size=%lu B  patch_sha256=..." + "DUP → skip save" / "NEW/CHANGED → save"
//sk:                          META prijaté patch_size=%lu B  patch_sha256=... + DUP → preskoč zápis / NOVÉ/ZMENENÉ → zapíš
//en: handle_sig():           "SIG: short" / "SIG: old_sha256 mismatch vs META — drop"
//sk:                          SIG: krátky / SIG: old_sha256 nezhoda s META — drop
//en: handle_sig():           "SIG: base FW mismatch — patch is not for this device, drop"
//sk:                          SIG: base FW nezhoda — patch nie je pre toto zariadenie, drop
//en: handle_sig():           "SIG received key_id=0x%X  ..."
//sk:                          SIG prijaté key_id=0x%X  ...
//en: handle_chunk():         "CHUNK: base FW mismatch — drop"
//sk:                          CHUNK: base FW nezhoda — drop
//en: handle_chunk():         "CHUNK: partial session from a chunk (waiting for HEADER)"
//sk:                          CHUNK: partial session z chunku (čaká HEADER)
//en: handle_apply():         "APPLY: SHA256 mismatch" (also "MISMATCH" in fota_print_pkt)
//sk:                          APPLY: SHA256 nesúhlasí ( "NESEDÍ" aj vo fota_print_pkt)
//en: fota_apply():           "APPLY: not verified — receive chunks first"
//sk:                          APPLY: nie je verifikované — najprv prijmi chunky
//en: fota_clear_session():   "Clearing FOTA session..." / "Done — %d files removed"
//sk:                          Mažem FOTA session... / Hotovo — vymazaných %d súborov
//
// ── FotaPatcher.cpp ──────────────────────────────────────────────────
//en: fota_verify_old_fw():   "old_sha256 unknown — base check skipped"
//sk:                          old_sha256 neznámy — kontrola base preskočená
//en: fota_verify_old_fw():   "ERROR: old_fw_size %lu > app window"
//sk:                          CHYBA: old_fw_size %lu > app okno
//en: fota_verify_old_fw():   "expected old_sha256 ="
//sk:                          očakávaný old_sha256 =
//en: fota_verify_old_fw():   "BASE MISMATCH — running FW != patch old! NOT overwriting." + "Generate the patch against the current firmware."
//sk:                          BASE NESEDÍ — bežiaci FW != old z patchu! NEPREPISUJEM. + Vygeneruj patch voči aktuálnemu firmvéru.
//en: fota_verify_old_fw():   "base FW matches the patch"
//sk:                          base FW sedí s patchom
//en: ensure_flasher_written(): "Flasher is up to date" / "Writing flasher to 0xEB000..." /
//en:                         "ERROR: flasher verification failed!" / "Flasher written OK"
//sk:                          Flasher je aktuálny / Zapisujem flasher do 0xEB000... /
//sk:                          CHYBA: overenie flashera zlyhalo! / Flasher zapísaný OK
//en: fota_patch_to_file():   "Test: SHA256 verify (no flash)..."
//sk:                          Test: SHA256 verify (bez flashu)...
//en: fota_patch_to_file():   "patch unavailable" / "malloc puff_stream failed" / "scratch buffer unavailable"
//sk:                          patch nedostupný / malloc puff_stream zlyhalo / scratch buffer nedostupný
//en: fota_patch_to_file():   "Invalid HPatchLite header (ZLIB) ps_err=%d"
//sk:                          Neplatný HPatchLite header (ZLIB) ps_err=%d
//en: fota_patch_to_file():   "extra_safe %lu > max %lu — the flasher would REJECT the patch (0xE5)!"
//sk:                          extra_safe %lu > max %lu — flasher by patch ODMIETOL (0xE5)!
//en: fota_patch_to_file():   "Decompression failed: err=%d" / "HPatchLite FAILED (ZLIB)"
//sk:                          Dekompresia zlyhala: err=%d / HPatchLite ZLYHALO (ZLIB)
//en: fota_patch_to_file():   "Expected SHA256 unknown — verify manually"
//sk:                          Očakávaný SHA256 neznámy — overuj manuálne
//en: fota_patch_to_file():   "SHA256 MISMATCH  exp="
//sk:                          SHA256 NESEDÍ  exp=
//en: fota_patch_to_file():   "ZLIB patch verified!" / "OK — patch verified!"
//sk:                          ZLIB patch overený! / OK — patch overený!
//en: fota_patch_to_file():   "Invalid patch format (hpatchi_inplace_open)" / "Compressed patch not supported"
//sk:                          Neplatný formát patchu (hpatchi_inplace_open) / Komprimovaný patch nie je podporovaný
//en: fota_flash_via_flasher(): "flasher_code.h missing." + "Run: python nrffota/tools/build_flasher.py"
//sk:                          flasher_code.h chýba. + Spusti: python nrffota/tools/build_flasher.py
//en: fota_flash_via_flasher(): "ABORTED — base FW mismatch, not risking an overwrite."
//sk:                          PRERUŠENÉ — base FW nesedí, neriskujem prepis.
//en: fota_flash_via_flasher(): "patch unavailable (RAM/file)" / "Invalid patch size: %lu"
//sk:                          patch nedostupný (RAM/súbor) / Neplatná veľkosť patchu: %lu
//en: fota_flash_via_flasher(): "Compressed format: staged=..." / "Uncompressed format: new_fw=..."
//sk:                          Komprimovaný formát: staged=... / Nekomprimovaný formát: new_fw=...
//en: fota_flash_via_flasher(): "ABORTED — extra_safe %lu > max %lu (flasher would reject, 0xE5)"
//sk:                          PRERUŠENÉ — extra_safe %lu > max %lu (flasher by odmietol, 0xE5)
//en: fota_flash_via_flasher(): "Patch in RAM (%lu B)" / "Patch > 128kB — does not fit the RAM region"
//sk:                          Patch v RAM (%lu B) / Patch > 128kB — nezmestí sa do RAM oblasti
//en: fota_flash_via_flasher(): "ABORTED — patch buffer overlaps the flasher RAM region (0x%X+%lu)"
//sk:                          PRERUŠENÉ — patch buffer zasahuje do flasher RAM regiónu (0x%X+%lu)
//en: fota_patch_to_file()/fota_flash_via_flasher() [no HPatchLite]: "HPatchLite not installed (nrffota/hpatchlite/)."
//sk:                          HPatchLite nie je nainštalovaná (nrffota/hpatchlite/).
//en: print_step():           0xFE: "FAIL — flasher failed and reset"
//sk:                          0xFE: FAIL — flasher zlyhal a resetoval sa
//en: print_step():           "puff progress: decompressed ~%lu kB"
//sk:                          puff progress: dekomprimovaných ~%lu kB
//en: fota_print_flasher_trace(): "(unavailable — no trace region)" / "(empty — flasher wrote no trace)"
//sk:                          (nedostupný — bez trace regiónu) / (prázdny — flasher nezapísal trace)
//en: fota_print_flasher_trace(): "flasher event sequence:"
//sk:                          sekvencia eventov flashera:
//en: fota_print_flasher_trace(): "[chk] FNV-1a of output (new FW) = 0x%X"   <- PARSED by fota_test_lora_repeater.py
//sk:                          [chk] FNV-1a výstupu (nový FW) = 0x%X
//en: fota_print_flasher_trace(): "[vfy] FNV-1a of written flash  = 0x%X"
//sk:                          [vfy] FNV-1a zapísanej flash = 0x%X
//en: fota_print_flasher_trace(): "[vfy] VERIFY OK — flash == hpatchi output" / "[vfy] VERIFY FAIL — jumping to DFU!"
//sk:                          [vfy] VERIFY OK — flash == hpatchi výstup / [vfy] VERIFY FAIL — skok do DFU!
//en: fota_print_flasher_debug(): "last step: "
//sk:                          posledný krok:
//en: fota_debug_decompress(): "patch.bin missing" / "not ZLIB format" / "[decomp OK]" / "[DECOMP ERROR!]"
//sk:                          patch.bin chýba / nie je ZLIB formát / [dekompr OK] / [DEKOMPR CHYBA!]
//
// ── FotaBuffer.cpp ───────────────────────────────────────────────────
//en: fota_get_buffer():      "buffer: already borrowed (reentrancy?)"
//sk:                          buffer: už požičaný (reentrancia?)
//en: fota_put_buffer():      "buffer: put of a foreign pointer — ignoring"
//sk:                          buffer: put cudzieho smerníka — ignorujem
