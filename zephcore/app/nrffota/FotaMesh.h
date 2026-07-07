#pragma once
// =====================================================================
//en: FotaMesh.h — glue between MeshCore (GRP_DATA channel, CLI) and the FOTA module.
//en:
//en: FOTA packets arrive as MeshCore GRP_DATA on a dedicated channel.
//en: MyMesh overrides searchChannelsByHash()/onGroupDataRecv() and hands the
//en: decrypted payload over here. Controlled via "fota ..." commands (serial and
//en: LoRa CLI; legacy alias "ota ..." — see FOTA-CLI-ALIAS in MyMesh.cpp).
//sk: FotaMesh.h — glue medzi MeshCore (GRP_DATA kanál, CLI) a FOTA modulom.
//sk:
//sk: FOTA pakety prichádzajú ako MeshCore GRP_DATA na dedikovanom kanáli.
//sk: MyMesh override-ne searchChannelsByHash()/onGroupDataRecv() a odovzdá
//sk: dešifrovaný payload sem. Ovládanie cez "fota ..." príkazy (serial aj LoRa CLI;
//sk: legacy alias "ota ..." — viď FOTA-CLI-ALIAS v MyMesh.cpp).
// =====================================================================
#if defined(FOTA_MESHCORE_BUILD)
  #include <Mesh.h>        //en: mesh::GroupChannel, PUB_KEY_SIZE, PATH_HASH_SIZE
#elif defined(FOTA_ZEPHCORE_BUILD)
  #include <mesh/Mesh.h>   //en: mesh::GroupChannel, PUB_KEY_SIZE, PATH_HASH_SIZE
#else
  #error "FotaMesh.h: define FOTA_MESHCORE_BUILD or FOTA_ZEPHCORE_BUILD"
#endif

//en: Default FOTA channel — MeshCore #-convention (secret = SHA256(name)[0:16]).
//en: Override via build_flags:  -D FOTA_CHANNEL_NAME='"#mojkanal"'
//en: Matches meshcore_py set_channel(idx, name) and fota_sender.py fota_channel_secret().
//sk: Default FOTA kanál — MeshCore #-konvencia (secret = SHA256(name)[0:16]).
//sk: Override cez build_flags:  -D FOTA_CHANNEL_NAME='"#mojkanal"'
//sk: Zhodné s meshcore_py set_channel(idx, name) aj fota_sender.py fota_channel_secret().
#ifndef FOTA_CHANNEL_NAME
  #define FOTA_CHANNEL_NAME "#fkotanrf"
#endif

//en: "fota miss" cap on the missing-chunk listing, in TOKENS (single number = 1,
//en: range = 2; H/S do not count as tokens). "fota missall" ignores the cap (limit 0).
//en: Override via build_flags:  -D FOTA_MISS_OUTTOKENS=30
//sk: "fota miss" strop výpisu chýbajúcich v TOKENOCH (jednotlivé číslo = 1, rozsah = 2;
//sk: H/S sa do tokenov nerátajú). "fota missall" ignoruje strop (limit 0). Override cez
//sk: build_flags:  -D FOTA_MISS_OUTTOKENS=30
#ifndef FOTA_MISS_OUTTOKENS
  #define FOTA_MISS_OUTTOKENS 20
#endif

//en: Build the FOTA GroupChannel from the name (secret = SHA256(name)[0:16] zero-padded
//en: to 32B, hash = SHA256(secret)[0]) — matches companion set_channel.
//sk: Postav FOTA GroupChannel z mena (secret = SHA256(name)[0:16] doplnené nulami na
//sk: 32B, hash = SHA256(secret)[0]) — zhodné s companion set_channel.
void fota_build_channel(mesh::GroupChannel& ch);

//en: Handle a "fota ..." CLI command (status|verify|flash|clear|decompress|nack|miss|missall|dbg|id).
//en: args = text after "fota"/"ota". reply = output buffer (serial/LoRa reply).
void fota_handle_command(const char* args, char* reply);
