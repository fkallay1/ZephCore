# ZephCore LoRa-FOTA — používateľský prehľad a workflow

Port LoRa-FOTA (delta patch) z MeshCore `features/nrf-fota` do ZephCore
repeatera. Stav: **naportované a buildy prechádzajú; HW e2e test ešte nebol
vykonaný** (viď `fcl_e2e_runbook_zephcore_fota.md`).

## Rýchly štart

```bash
# 1) FOTA build (ProMicro SX1262)
west build -b promicro_sx1262 zephcore --pristine -- \
  -DEXTRA_CONF_FILE="boards/common/repeater.conf;boards/common/fota.conf"

# 2) flash cez UF2 (build/zephyr/zephyr.uf2 — trailer uz vyplneny)

# 3) druhy build (build# sa bumpne sam) -> fotapkg
python test_nrf-fota/gen_fotapkg.py --old <stary.bin> --new <novy.bin>

# 4) poslanie cez bridge
python test_nrf-fota/fota_sender.py --mode meshcore ...
# na repeateri: fota status -> verify -> flash
```

## Kľúčové fakty

| Vec | Hodnota |
|---|---|
| Kconfig gate | `CONFIG_ZEPHCORE_LORA_FOTA` (default n; `boards/common/fota.conf`) |
| Kanál | `CONFIG_ZEPHCORE_FOTA_CHANNEL_NAME`, default `#fkotanrf` |
| FOTA dáta | `/lfs/fota/*` (zdieľaná /lfs partícia, bez zmeny flash mapy) |
| Flasher | RAM @ 0x2003E000 — vrch RAM rezervovaný cez fota.overlay (sram0 248 kB), MPU sa pred skokom vypína; flash cesta za `CONFIG_ZEPHCORE_FOTA_FLASHER_IN_FLASH` (pre budúce power-loss recovery) |
| Build číslo | `test_nrf-fota/build_number.txt` (zdieľané počítadlo, bump každý build) |
| Trailer | `fota_fwid` CMake target — hex+bin+UF2 po každom builde |
| CLI | `fota status|verify|flash|clear|miss|missall|getpath|setpath|nack|decompress|dbg|id|agc` (alias `ota`) |
| Debug výpisy | `CONFIG_ZEPHCORE_FOTA_DEBUG=y` (default v fota.conf) → `[FOTA] …` cez printk |

## Sync workflow s MeshCore (dôležité!)

Vývoj FOTA pokračuje v **MeshCore** (`D:\FkDev\FkProj\VSC\MeshCore`, vetva
`features/nrf-fota` / `features/nrf-fota-dualguard`). Zdieľané súbory
(`zephcore/app/nrffota/*` okrem glue, `test_nrf-fota/*`) musia ostať
byte-identické:

```bash
python test_nrf-fota/fota_mczc_scr_sync.py  # kontrola (exit 1 = divergencia)
python test_nrf-fota/fota_mczc_scr_sync.py --copy   # prenos MeshCore -> ZephCore
```

- Platformové rozdiely = duálne guardy `FOTA_MESHCORE_BUILD`/`FOTA_ZEPHCORE_BUILD`
  priamo v zdieľaných súboroch. Nový platformový kód pridávaj do OBOCH vetiev.
- Per-projekt (nesyncuje sa): `FotaRepeaterMesh.{h,cpp}` (ZephCore glue),
  `FotaMyMesh.{h,cpp}` (MeshCore glue), `flasher_code.h` (generovaný — v ZephCore
  `--origin 0x2003E000 --platform zephcore`).
- Zásahy do ZephCore kódu (Dispatcher/RepeaterMesh/…) vždy za `#ifdef WITH_LORA_FOTA`.

## Odporúčania z MeshCore testovania (platia aj tu)

- `agc.reset.interval` держať 0 a CAD vypnuté počas FOTA session
  (MeshCore tech doc §8.4/§8.7).
- `fota verify` (dry-run) pred prvým `fota flash`; sekvenciu verify→flash
  v jednom boote otestovať na HW skôr, než sa zapne v automatike.

## Dokumentácia

- `zephcore/app/nrffota/README.md` — technický popis ZephCore portu
- `fkclaude/specs/2026-07-07-fota-port-design.md` — dizajn portu
- `fkclaude/plans/2026-07-07-fota-port.md` — implementačný plán
- `fkclaude/fcl_e2e_runbook_zephcore_fota.md` — HW e2e postup
- MeshCore: `fkclaude/fcl_readme_nrf-fota.md`, `fcl_readme_tech_nrf-fota.md`
  (kompletná FOTA dokumentácia — protokol, CLI, riešené problémy)
