#pragma once
// =====================================================================
//en: FotaDebug.h — diagnostic output of the FOTA module (modelled after
//en: MESH_DEBUG_PRINT in src/MeshCore.h). Enabled by -D FOTA_DEBUG=1
//en: (FOTA envs in platformio.ini); without the flag the prints are not
//en: compiled at all (zero flash/CPU overhead).
//sk: FotaDebug.h — diagnostické výpisy FOTA modulu (vzor: MESH_DEBUG_PRINT
//sk: v src/MeshCore.h). Zapína -D FOTA_DEBUG=1 (FOTA env v platformio.ini);
//sk: bez flagu sa výpisy vôbec nekompilujú (žiadny flash/CPU overhead).
//
//en: Usage: printf style; the caller provides the full message text
//en: including its prefix (our messages use [FOTA], [DBG], [FLASHER-DBG]).
//en: NOTE: CLI replies (sprintf into the reply buffer) do NOT belong here —
//en: that is functional output for the client, not diagnostics.
//sk: Použitie: printf štýl, text správy vrátane "[FOTA] " prefixu si dáva
//sk: volajúci (naše správy majú rôzne prefixy: [FOTA], [DBG], [FLASHER-DBG]).
//sk: POZOR: CLI odpovede (sprintf do reply bufferu) sem NEpatria — to je
//sk: funkčný výstup pre klienta, nie diagnostika.
// =====================================================================
#if FOTA_DEBUG && defined(FOTA_ZEPHCORE_BUILD)
  #include <zephyr/sys/printk.h>
  //en: printk goes to the Zephyr console (USB CDC on nRF52 repeaters); CRLF
  //en: endings for terminal parity with the Arduino output.
  //sk: printk ide na Zephyr konzolu (USB CDC na nRF52 repeateroch); CRLF
  //sk: konce riadkov kvoli parite s Arduino vystupom v terminali.
  #define FOTA_DEBUG_PRINT(F, ...)   printk(F, ##__VA_ARGS__)
  #define FOTA_DEBUG_PRINTLN(F, ...) printk(F "
", ##__VA_ARGS__)
#elif FOTA_DEBUG && ARDUINO
  #include <Arduino.h>
  //en: "\r\n" (CRLF) like Serial.println — a bare "\n" produces "staircase"
  //en: output in terminals (new line without carriage return). Do NOT put
  //en: newlines into format strings manually; end lines via FOTA_DEBUG_PRINTLN.
  //sk: "\r\n" (CRLF) ako Serial.println — samotné "\n" robí v termináli "schodíky"
  //sk: (nový riadok bez návratu vozíka). Newline NEvkladaj do formátu ručne,
  //sk: ukonči riadok cez FOTA_DEBUG_PRINTLN.
  #define FOTA_DEBUG_PRINT(F, ...)   Serial.printf(F, ##__VA_ARGS__)
  #define FOTA_DEBUG_PRINTLN(F, ...) Serial.printf(F "\r\n", ##__VA_ARGS__)
#else
  #define FOTA_DEBUG_PRINT(...) {}
  #define FOTA_DEBUG_PRINTLN(...) {}
#endif
