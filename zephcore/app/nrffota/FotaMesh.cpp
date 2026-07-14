// =====================================================================
//en: FotaMesh.cpp — glue between MeshCore and the FOTA module.
// =====================================================================
#ifdef WITH_LORA_FOTA
#include "FotaMesh.h"
#include "FotaReceiver.h"
#include "FotaPatcher.h"
#include "FotaDebug.h"
#include "FotaTexts.h"   //en: FOTA_TXT_* — central catalog of CLI reply texts (EN/SK)
                         //sk: FOTA_TXT_* — centrálny katalóg textov CLI odpovedí (EN/SK)
#if defined(FOTA_MESHCORE_BUILD)
  #include <Utils.h>
  #include <Arduino.h>
#elif defined(FOTA_ZEPHCORE_BUILD)
  #include <mesh/Utils.h>
  #include <stdio.h>       //en: sprintf (CLI replies)
#endif
#include <string.h>

void fota_build_channel(mesh::GroupChannel& ch) {
    //en: MeshCore #-convention: secret = SHA256(FOTA_CHANNEL_NAME)[0:16] (name INCLUDING '#',
    //en: matches meshcore_py set_channel device.py:216). secret contains 0x00 →
    //en: do NOT hash it as a string. AES key = secret[:16], HMAC key = secret[:32].
    //sk: MeshCore #-konvencia: secret = SHA256(FOTA_CHANNEL_NAME)[0:16] (meno VRÁTANE '#',
    //sk: zhodné s meshcore_py set_channel device.py:216). secret obsahuje 0x00 →
    //sk: NEhashovať ako string. AES kľúč = secret[:16], HMAC kľúč = secret[:32].
    const char* name = FOTA_CHANNEL_NAME;
    uint8_t full[32];
    mesh::Utils::sha256(full, sizeof(full), (const uint8_t*)name, (int)strlen(name));
    memset(ch.secret, 0, PUB_KEY_SIZE);
    memcpy(ch.secret, full, 16);

    //en: channel hash = SHA256(secret)[0..PATH_HASH_SIZE] (matches companion addChannel)
    uint8_t h[32];
    mesh::Utils::sha256(h, sizeof(h), ch.secret, 16);
    memcpy(ch.hash, h, PATH_HASH_SIZE);

    FOTA_DEBUG_PRINTLN("[FOTA] channel %s hash=0x%02X", name, (unsigned)ch.hash[0]);   //en: expected 0xA4 for #fkotanrf
}

void fota_handle_command(const char* args, char* reply) {
    while (*args == ' ') args++;

    if (*args == 0 || strcmp(args, "status") == 0) {
        const FotaState* st = fota_get_state();
        sprintf(reply, FOTA_TXT_STATUS_FMT,
                (unsigned)st->recv_count, (unsigned)st->total_chunks,
                (unsigned)st->status, (unsigned long)st->patch_size,
                (unsigned)st->err_code);
        fota_print_status();
    } else if (strcmp(args, "verify") == 0 || strcmp(args, "dryrun") == 0) {
        char reason[48]; reason[0] = 0;
        bool ok = fota_patch_to_file(reason, sizeof(reason));
        const char* tag = ok ? "OK" : "FAIL";
        if (reason[0]) sprintf(reply, FOTA_TXT_DRYRUN_RESULT_FMT, tag, reason);
        else           sprintf(reply, FOTA_TXT_DRYRUN_RESULT_SHORT_FMT, tag);
    } else if (strcmp(args, "flash") == 0 || strcmp(args, "apply") == 0) {
        //en: Do NOT flash here: on success fota_apply() DOES NOT RETURN (jump to flasher
        //en: + reboot), so the ACK would never be transmitted. Send "accepted" first;
        //en: loop() starts the flash ONLY after the ACK actually leaves the outbound
        //en: queue (fota_apply_pending()).
        //sk: NEFLASHUJ tu: fota_apply() sa pri úspechu NEVRÁTI (skok na flasher + reboot),
        //sk: takže by sa ACK nikdy neodvysielal. Najprv pošli „accepted", flash spustí
        //sk: loop() AŽ keď ACK reálne odíde z outbound queue (fota_apply_pending()).
        if (fota_get_state()->status & FOTA_ST_VERIFIED) {
            fota_request_apply();
            strcpy(reply, FOTA_TXT_FLASH_ACCEPTED);
        } else {
            strcpy(reply, FOTA_TXT_FLASH_NOT_VERIFIED);
        }
    } else if (strcmp(args, "clear") == 0) {
        fota_clear_session();
        strcpy(reply, FOTA_TXT_CLEARED);
    } else if (strcmp(args, "decompress") == 0 || strcmp(args, "decomp") == 0) {
        fota_debug_decompress();
        strcpy(reply, FOTA_TXT_DECOMP_TO_SERIAL);
    } else if (strcmp(args, "nack") == 0) {
        fota_send_nack();
        strcpy(reply, FOTA_TXT_NACK_TO_SERIAL);
    } else if (strcmp(args, "miss") == 0 || strcmp(args, "missall") == 0) {
        //en: Missing items: H (META) and S (SIG) first if missing, then chunks (from 0)
        //en: as ranges — a contiguous run as "from-to" (e.g. "4-11"), a single one as "5".
        //en: miss = cap of FOTA_MISS_OUTTOKENS tokens (number = 1, range = 2; H/S are NOT
        //en: counted and always printed); missall = all (capped only by the LoRa packet
        //en: length). Remaining chunks as "+N". Both show the TOTAL count.
        //en: "Zero info yet" only if nothing at all has arrived.
        //sk: Chýbajúce: na začiatku H (META) a S (SIG) ak chýbajú, potom chunky (od 0)
        //sk: ako rozsahy — súvislý beh "od-do" (napr. "4-11"), jednotlivý ako "5".
        //sk: miss = strop FOTA_MISS_OUTTOKENS tokenov (číslo = 1, rozsah = 2; H/S sa NErátajú a vypíšu sa vždy);
        //sk: missall = všetky (capnuté len na dĺžku LoRa paketu). Zvyšné chunky ako "+N".
        //sk: Oba ukážu CELKOVÝ počet. "Zero info yet" len ak neprišlo vôbec nič.
        bool show_all = (args[4] == 'a');               //en: "missall" has 'a' at args[4]
        const FotaState* st = fota_get_state();
        bool miss_h = !st->meta_recv;
        bool miss_s = !st->sig_recv;
        int chunk_missing = fota_calc_missing(NULL, 0, NULL);   //en: total count only

        bool any_info = st->meta_recv || st->sig_recv || st->recv_count > 0 || st->total_chunks > 0;
        if (!any_info) {
            FOTA_DEBUG_PRINTLN("[FOTA] miss Zero info yet");
            strcpy(reply, FOTA_TXT_MISS_ZERO_INFO);
        } else {
            int chunk_total = (chunk_missing < 0) ? 0 : chunk_missing;
            int hs = (miss_h ? 1 : 0) + (miss_s ? 1 : 0);
            int total = hs + chunk_total;
            int tok_lim = show_all ? 0 : FOTA_MISS_OUTTOKENS;   //en: 0 = all; otherwise token cap (H/S excluded)
            //en: Total marker: verified total "/T"; META-only estimate "/~T" (unverified,
            //en: covers trailing chunks too); no META → "(noH)"/"(noHS)" says WHICH header
            //en: part is missing (glued to the miss= token so the app parser skips it).
            //sk: Marker totalu: overený total "/T"; odhad len z META "/~T" (neoverený,
            //sk: pokrýva aj chvostové chunky); bez META → "(noH)"/"(noHS)" hovorí, KTORÁ
            //sk: časť hlavičky chýba (prilepené k miss= tokenu, aby to parser appky preskočil).
            uint16_t est = fota_total_est();

            //en: Serial: full detail (H/S always + chunks, comma-separated)
            FOTA_DEBUG_PRINT("[FOTA] miss %d", total);
            if (st->total_chunks > 0) { FOTA_DEBUG_PRINT("/%u", (unsigned)st->total_chunks); }
            else if (est > 0)         { FOTA_DEBUG_PRINT("/~%u (no S)", (unsigned)est); }
            else                        FOTA_DEBUG_PRINT(" (no H%s)", miss_s ? " S" : "");
            FOTA_DEBUG_PRINT(": ");
            if (miss_h) FOTA_DEBUG_PRINT("H");
            if (miss_s) FOTA_DEBUG_PRINT(miss_h ? ",S" : "S");
            fota_print_missing(tok_lim, (miss_h || miss_s) ? "," : "");
            FOTA_DEBUG_PRINTLN("");

            //en: Reply (LoRa and Serial CLI): count + H/S + range list, capped to packet length
            char* p = reply;
            p += sprintf(p, "FOTA miss=%d", total);
            if (st->total_chunks > 0) p += sprintf(p, "/%u", (unsigned)st->total_chunks);
            else if (est > 0)         p += sprintf(p, "/~%u(noS)", (unsigned)est);
            else                       p += sprintf(p, "(no%s%s)", miss_h ? "H" : "", miss_s ? "S" : "");
            if (total > 0) *p++ = ':';
            if (miss_h) p += sprintf(p, " H");
            if (miss_s) p += sprintf(p, miss_h ? ",S" : " S");
            int avail = 158 - (int)(p - reply);          //en: cap for LoRa (~160 B)
            if (avail > 8) p += fota_format_missing(p, avail, tok_lim,
                                                    (miss_h || miss_s) ? "," : " ");
            *p = 0;
        }
    } else if (strcmp(args, "dbg") == 0) {
        fota_print_flasher_debug();
        strcpy(reply, FOTA_TXT_DBG_TO_SERIAL);
    } else if (strcmp(args, "id") == 0 || strcmp(args, "fwid") == 0) {
        fota_print_fw_id(reply);   //en: build#, image_size, full running sha256 -> serial
    } else {
        //en: getpath/setpath/missall <cesta> are LoRa-only (need the ACL client; handled
        //en: inline in fotaHandleLoRaCli) — listed here so the usage reply advertises them.
        //sk: getpath/setpath/missall <cesta> sú len LoRa (potrebujú ACL klienta; riešené
        //sk: inline vo fotaHandleLoRaCli) — tu ich uvádzame, aby ich usage odpoveď ponúkla.
        strcpy(reply, FOTA_TXT_USAGE);
    }
}

#endif  // WITH_LORA_FOTA
