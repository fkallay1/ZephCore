# nrffota — LoRa-FOTA (delta-patch) pre ZephCore repeater (nRF52840)

Port FOTA systému z MeshCore (`examples/simple_repeater/nrffota/`, vetva
`features/nrf-fota`). Aktualizácia firmvéru repeatera **cez LoRa** prenosom
malého delta-patchu (rozdiel starý↔nový FW), nie celého firmvéru.

> Toto je iné ako ZephCore BLE DFU (`CONFIG_ZEPHCORE_BLE_DFU`) aj WiFi OTA
> (`CONFIG_ZEPHCORE_WIFI_OTA`, ESP32). Tu ide o patch cez LoRa GRP_DATA.

## Zdieľanie s MeshCore

Zdrojáky v tomto adresári sú **byte-identické** s MeshCore
(`../MeshCore/examples/simple_repeater/nrffota/`) — vývoj pokračuje v MeshCore
a prenáša sa sem cez:

```bash
python zephcore/tools/fota_sync.py          # report divergencií
python zephcore/tools/fota_sync.py --copy   # MeshCore -> ZephCore
```

Platformové rozdiely riešia duálne guardy `FOTA_MESHCORE_BUILD` /
`FOTA_ZEPHCORE_BUILD` (shim hlavičky `FotaFs.h`, `FotaDebug.h`, `FotaCrypto.h`,
`flash_layout.h`). Per-projekt glue: `FotaRepeaterMesh.{h,cpp}` (tu) ↔
`FotaMyMesh.{h,cpp}` (MeshCore). Výnimka zo sync: `flasher_code.h` — generovaný
per-repo (iný ORIGIN).

## Ako to funguje (ZephCore špecifiká)

```
PC (test_nrf-fota/fota_sender.py)        ZephCore repeater (nRF52840)
  hdiffi -inplaceB old new patch          RepeaterMesh::onGroupDataRecv()  ← GRP_DATA
  zlib(-9,wbits=-9) → staged              → deferred fota_process() v loop()
  GRP_DATA (AES-128-ECB + HMAC) ──LoRa──▶ chunky → /lfs/fota/recv.log
                                          COMPLETE → assemble + SHA256 → VERIFIED
                                          'fota flash' → RAM flasher @ 0x20020000:
                                            HPatchLite inplaceB + puff_stream
                                            NVMC in-place zápis + FNV verify → reset
```

- **FS:** zdieľaná `/lfs` partícia (0xD4000, 128 kB), FOTA súbory pod
  `/lfs/fota/`. Žiadna zmena flash mapy — stock aj FOTA buildy sú zameniteľné.
- **Flasher beží z RAM** (`FLASHER_RAM_ADDR` 0x20020000, blob ~4,1 kB): app ho
  skopíruje pred skokom, patch ostáva na heape (guard: celý pod 0x20020000)
  a na `PATCH_RAM_ADDR` (0x20000000) si ho presunie flasher sám
  (`FLASHER_COPY_PATCH`) — beží s vlastným SP na vrchu RAM.
  Cesta zápisu flashera do flashu ostáva v kóde za
  `CONFIG_ZEPHCORE_FOTA_FLASHER_IN_FLASH` (budúce power-loss recovery; vyžaduje
  dedikovanú DTS partíciu). Trace stránka za `..._FLASHER_TRACE`.
- **Krypto:** `mesh::Utils::sha256/MACThenDecrypt` (PSA), Ed25519 podpis
  HEADERu cez Monocypher (`FotaCrypto.h`).
- **Deferred model:** `onGroupDataRecv` len uloží payload; FS I/O beží
  z `RepeaterMesh::loop()` (po `mesh::Mesh::loop()` = po re-arme rádia).
  Deferred flash čaká, kým ACK odíde z outbound queue.
- Bez SoftDevice — `sd_softdevice_disable()` odpadá, IRQ poistka
  (`__disable_irq()` pred blob copy + skokom) ostáva.

## Build

```bash
# blob flashera (raz / po zmene flasher.c) — POZOR: iny ORIGIN nez MeshCore
python zephcore/app/nrffota/tools/build_flasher.py --origin 0x20020000 --platform zephcore

# FOTA repeater (ProMicro; funguje kazdy nRF52840 board)
west build -b promicro_sx1262 zephcore --pristine -- \
  -DEXTRA_CONF_FILE="boards/common/repeater.conf;boards/common/fota.conf"
```

Post-build automaticky: `fota_build_info` (bump build# — zdieľané počítadlo
`test_nrf-fota/build_number.txt`), `fota_fwid` (FwId trailer do hex+bin +
UF2 regen). Kanál: `CONFIG_ZEPHCORE_FOTA_CHANNEL_NAME` (default `"#fkotanrf"`).

## Ovládanie

CLI príkazy (USB serial aj LoRa admin CLI) sú zhodné s MeshCore verziou:
`fota status|verify|flash|clear|miss|missall [cesta]|getpath|setpath|nack|
decompress|dbg|id|agc` (legacy alias `ota …`). Detailný popis:
MeshCore `fkclaude/fcl_readme_nrf-fota.md` a `nrffota/README.md`.
Rozdiel: `fota agc` na Zephyre nemá prístup k SX1262 registrom — hlási
noise floor + RX počítadlá.

## Posielanie patchu z PC

`test_nrf-fota/` je kópia MeshCore toolingu (sync-ovaná):

```bash
# fotapkg medzi dvoma ZephCore buildmi (old/new .bin z build/zephyr/)
python test_nrf-fota/gen_fotapkg.py --old old.bin --new new.bin --device promicro-zeph

# poslanie cez bridge / companion
python test_nrf-fota/fota_sender.py --mode meshcore ...
python test_nrf-fota/fota_sender_mcpy.py ...
```

Delta patch je zmysluplný len **ZephCore↔ZephCore** (Arduino→Zephyr by bol
prakticky celý image).

## Bezpečnostné poistky

Zhodné s MeshCore: base-FW SHA256 predkontrola (nesedí → nepíše),
extra_safe limit (`FOTA_MAX_EXTRA_SAFE`), FNV-1a verify po zápise + skok do
DFU pri nezhode, `fota verify` dry-run pred `fota flash`.
