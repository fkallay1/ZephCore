# Port LoRa-FOTA (delta patch) z MeshCore do ZephCore — dizajn

Dátum: 2026-07-07 · Stav: schválený používateľom (session „01 Port FOTA z meshcore")

## Cieľ

Naportovať funkčný LoRa-FOTA systém (delta patch cez GRP_DATA kanál, standalone
in-place flasher, HPatchLite + streaming DEFLATE) z Arduino MeshCore vetvy
`features/nrf-fota` (`D:\FkDev\FkProj\VSC\MeshCore`) do ZephCore repeatera tak, aby:

1. FOTA fungovalo na ZephCore nRF52840 repeateri (prvý cieľ: **ProMicro SX1262**,
   sdv6, app base 0x26000).
2. Zdieľané zdrojáky ostali **byte-identické** medzi oboma repozitármi — ďalší vývoj
   pokračuje v MeshCore a prenáša sa sem priebežne (sync skript).
3. Stock ZephCore buildy boli nedotknuté (Kconfig gate, default off) a flash mapa
   sa **nemenila**.

Mimo rozsah: companion `CMD_SEND_RETURN_PATH` (0x70) — odložené; power-loss
recovery flashera — neimplementuje sa, ale kódové cesty preň ostávajú pripravené.

## Prenositeľnosť — hybrid s duálnymi guardami (variant C)

Platformové miesta v zdieľaných súboroch sa rozvetvia:

```c
#if defined(FOTA_MESHCORE_BUILD)
  // pôvodná Arduino/PIO cesta (CustomLFS, sd_softdevice_disable, Serial, /ota/*)
#elif defined(FOTA_ZEPHCORE_BUILD)
  // Zephyr cesta (fs_open/fs_read, bez SD, printk/FOTA_DEBUG, /lfs/fota/*)
#else
  #error "define FOTA_MESHCORE_BUILD or FOTA_ZEPHCORE_BUILD"
#endif
```

Skupiny súborov (kanonický zoznam bude v `tools/fota_sync.py`):

| Skupina | Súbory | Zdieľanie |
|---|---|---|
| 1 — platform-free | `hpatchlite/*`, `puff_stream.{c,h}`, `flasher/flasher.{c,ld}`, `FotaProtocol.h`, `FotaState.h`, `FotaBuffer.{h,cpp}`, `tools/build_flasher.py` | byte-identické, bez úprav |
| 2 — duálne guardy | `FotaReceiver.{h,cpp}`, `FotaReceiver_signkey.cpp`, `FotaPatcher.{h,cpp}`, `FotaFs.h`, `FotaDebug.h`, `FwId.{h,cpp}`, `flash_layout.h`, `flasher_code.h` (generovaný) | byte-identické po refaktore |
| 3 — per-projekt glue | MeshCore: `FotaMesh.{h,cpp}`, `FotaMyMesh.{h,cpp}`, MyMesh hooky, PIO envy · ZephCore: `FotaRepeaterMesh.{h,cpp}`, RepeaterMesh hooky, Kconfig/CMake | nezdieľa sa |

`tools/fota_sync.py` (v ZephCore): porovná zdieľané súbory so susedným
`../MeshCore/examples/simple_repeater/nrffota/`, vypíše divergencie, voliteľne
skopíruje MeshCore→ZephCore (`--copy`). Beží aj opačným smerom len ako report.

## Zmeny v MeshCore (vetva `features/nrf-fota-dualguard`)

Čisto preprocesorový refaktor, bez zmeny logiky ani správania:

- Obaliť platformové volania skupiny 2 do `FOTA_MESHCORE_BUILD` vetiev
  (+ doplniť `FOTA_ZEPHCORE_BUILD` vetvy tak, aby súbory boli identické s kópiami
  v ZephCore).
- `-D FOTA_MESHCORE_BUILD=1` do envov `ProMicro_repeater_fota`,
  `SenseCap_Solar_repeater_fota`, `Xiao_nrf52_repeater_fota`.
- Verifikácia: `pio run -e ProMicro_repeater_fota` prejde; žiadna zmena flash mapy
  ani logiky (review diffu = len ifdef obálky).

Merge do `features/nrf-fota` robí používateľ po vlastnom overení.

## ZephCore integrácia

### Umiestnenie

- `zephcore/app/nrffota/` — zrkadlo MeshCore `examples/simple_repeater/nrffota/`
  (skupiny 1+2 + ZephCore glue `FotaRepeaterMesh.{h,cpp}`).
- `test_nrf-fota/` v koreni ZephCore — kópia PC toolingu (parita ciest pre sync).
- `tools/fota_sync.py` — sync/diff skript.

### Kconfig / CMake

- `CONFIG_ZEPHCORE_LORA_FOTA` — bool, `depends on ZEPHCORE_ROLE_REPEATER && SOC_NRF52840`,
  default n. Zapne kompiláciu `app/nrffota/*` a definuje `WITH_LORA_FOTA=1` +
  `FOTA_ZEPHCORE_BUILD=1`.
- Sub-voľby (`if ZEPHCORE_LORA_FOTA`):
  - `ZEPHCORE_FOTA_DEBUG` (default y v dev, definuje `FOTA_DEBUG=1`)
  - `ZEPHCORE_FOTA_CHANNEL_NAME` (string, default `"#fkotanrf"`)
  - `ZEPHCORE_FOTA_FLASHER_IN_FLASH` (default n — RAM beh; y = NVMC zápis blobu
    do flash partície, vyžaduje DTS overlay s partíciou; pripravené na budúce
    power-loss recovery)
  - `ZEPHCORE_FOTA_FLASHER_TRACE` (default n — debug zápis trace stránky flashera,
    adresa parametrizovaná)
- Stock buildy: s vypnutým Kconfig sa nrffota zdroje vôbec nekompilujú.

### Core hook

- `logTxRaw` do `include/mesh/Dispatcher.h` + `src/Dispatcher.cpp` — prázdny
  virtuál volaný v `checkSend()` hneď po `startSendRaw` (zrkadlo Arduino zmeny;
  `logRxRaw` už v ZephCore existuje).

### RepeaterMesh glue (`FotaRepeaterMesh`, vzor `FotaMyMesh`)

- Overridy `searchChannelsByHash()` / `onGroupDataRecv()` (repeater dnes GRP_DATA
  nedekóduje — net-new, vzor `BaseChatMesh.cpp:408/419`). Kanál odvodený
  #-konvenciou z `ZEPHCORE_FOTA_CHANNEL_NAME` (SHA256(meno)[0:16], hash byte).
- CLI vetva `fota …` v `RepeaterMesh::handleCommand` pred `_cli` fallbackom
  (status/verify/flash/clear/miss/missall/getpath/setpath/nack/decompress/dbg/id/agc;
  `agc` sa adaptuje na ZephCore radio API alebo vynechá — rozhodne plán).
- Deferred model ostáva: `onGroupDataRecv` len kopíruje do pending buffra a signálne
  zobudí event slučku; `fota_process` (FS I/O) beží v `main_repeater.cpp` event
  slučke po mesh loope (ekvivalent Arduino `fotaLoop`).
- Return-path ACL (`setpath`/`getpath`/`missall <cesta>`) sa adaptuje na ZephCore
  repeater ACL.

### FS a krypto

- Session dáta v zdieľanom `/lfs` (partícia 0xD4000, 128 kB) pod `/lfs/fota/*`
  (Arduino `/ota/*` — cesty za guardom vo `FotaFs.h`). Bez novej partície.
- Krypto: `mesh::Utils::sha256` / `MACThenDecrypt` — API v ZephCore identické
  (PSA backend), bez zmien vo FOTA kóde.

### Flasher

- **Default: beh z RAM.** `build_flasher.py --origin <addr>` (jeden `flasher.ld`,
  ORIGIN parameter). `FotaPatcher` blob skopíruje na fixnú RAM adresu (top of RAM,
  konkrétna adresa sa určí v pláne — mimo patch buffra) a skočí. `__disable_irq()`
  poistka pred kopírovaním + skokom ostáva (§8.6 pôvodných docs);
  `sd_softdevice_disable()` len v `FOTA_MESHCORE_BUILD` vetve (Zephyr nemá
  SoftDevice runtime).
- Flash cesta (`ensure_flasher_written` + skok na flash adresu) ostáva v kóde za
  `FOTA_FLASHER_IN_FLASH`; trace zápis za `FOTA_FLASHER_TRACE`. Obe default off.
- Flash mapa ZephCore sa nemení; poznámka: samotný flasher vo flashi na recovery
  nestačí — bude treba aj mechanizmus vstupu po výpadku (rieši sa až s recovery).
- Bezpečnostné poistky ostávajú: base-FW SHA256 predkontrola, FNV-1a verify po
  zápise + skok do DFU pri nezhode.

### Build + tooling

- `test_nrf-fota/` sa kopíruje bez zmien (`fota_sender.py`, `fota_sender_mcpy.py`,
  `fota_test_lora_repeater.py`, `gen_fotapkg.py`, `hdiffi.exe`, testy…).
- Nové glue (per-projekt): build number (`gen_build_info.py`) a FW trailer
  (`gen_fw_trailer.py`) napojené cez CMake post-build namiesto PIO `extra_scripts`;
  trailer (FwId) cez Zephyr linker sekciu. `gen_fotapkg.py` berie
  `build/zephyr/zephyr.bin`.
- Delta patch je zmysluplný len **ZephCore↔ZephCore** (old/new musia byť Zephyr
  buildy; Arduino→Zephyr delta by bol prakticky celý image).

## Testovanie

1. Build check: stock repeater (Kconfig off) binárne nezmenený tok; FOTA build
   ProMicro prejde (`west build -b promicro_sx1262 … repeater.conf + fota.conf`).
2. MeshCore: `pio run -e ProMicro_repeater_fota` po refaktore prejde.
3. `fota_sync.py` hlási 0 divergencií po porte.
4. HW e2e (ProMicro + existujúci bridge): `fota_test_lora_repeater.py baseline/run`,
   `fota verify` pred prvým `fota flash`; sekvenciu verify→flash v jednom boote
   netestovať ako prvú (viď §8.2 pôvodných docs).
5. Interakcie: overiť, či ZephCore rádio vrstva nerobí periodické resety/CAD
   ekvivalentné Arduino `agc.reset`/`cad` (§8.4/§8.7) — ak áno, počas FOTA session
   držať vypnuté.

## Riziká

- **Flasher z RAM** — jediná odchýlka od odladeného správania (XIP→RAM beh);
  bare-metal Thumb2 s vypnutými IRQ, riziko nízke, overí sa e2e.
- **Trailer/linker** — FwId v Zephyr builde vyžaduje novú linker sekciu + post-build;
  presný mechanizmus určí plán (existujúci vzor: `adapters/ota/sections-rom.ld`).
- **Deferred timing** — ZephCore event slučka je iná ako Arduino `loop()`; treba
  ustrážiť, aby FS I/O nebežalo v RX ceste (rovnaký princíp, iný mechanizmus).
