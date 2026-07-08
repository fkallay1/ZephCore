# ZephCore LoRa-FOTA — používateľský prehľad a workflow

Port LoRa-FOTA (delta patch) z MeshCore `features/nrf-fota` do ZephCore
repeatera. Stav: **HW e2e PREŠIEL (2026-07-08)** — dva čisté flash cykly na
ProMicro (#289→#290 po DFU baseline, #290→#291 čisto cez FOTA). Detail:
`fcl_e2e_runbook_zephcore_fota.md`.

## Build prostredie (bootstrap na tomto stroji)

ZephCore je Zephyr/west projekt — na stroji nebol inicializovaný workspace,
tak som ho nabootstrapoval (jednorazovo, mimo repa, gitignorované):

- **Python 3.12** (Zephyr 4.4 vyžaduje ≥3.12; systémový 3.11 nestačil) —
  standalone v `D:\FkDev\Tools\python312`.
- **`.venv`** v koreni ZephCore: `west`, `cmake`, `ninja`,
  `-r zephyr/scripts/requirements-base.txt`, `pyserial`, `pycryptodome`,
  `adafruit-nrfutil`, `meshcore` (`pip install -e ../meshcore_py`), `pytest`.
- **`west init -l zephcore && west update`** → stiahne `zephyr/`, `modules/`,
  `bootloader/`, `tools/` (všetko gitignorované, reprodukovateľné).
- **Zephyr SDK** `D:\FkDev\zephyr_sdk` (bez ARM toolchainu — doinštalovaný
  `setup.cmd /t arm-zephyr-eabi /c`, vyžaduje cmake + 7z v PATH).

Buildy preto bežia s:
```
PATH má D:\FkDev\FkProj\VSC\ZephCore\.venv\Scripts
ZEPHYR_SDK_INSTALL_DIR=D:\FkDev\zephyr_sdk
```

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
- `fota verify` (dry-run) pred prvým `fota flash` — overené aj same-boot
  verify→flash na ZephCore (PASS).

## Poznatky z HW e2e (2026-07-08)

- **COM porty:** ZephCore sa hlási ako Zephyr USB zariadenie (VID 2FE3) →
  Windows mu dá iný COM port než Arduino build (VID 239A). Remapol som ho
  späť na COM5 cez registry `PortName` (bootloader je stále 239A/COM5).
  Companion XIAO = COM3, DUT ProMicro = COM5.
- **Flash cesty:** UF2 drag&drop (dvojklik RESET → disk PROMICRO), alebo
  `adafruit-nrfutil dfu serial --package build-fota/zephyr/zephyr.zip -p COM5
  --singlebank` (zip vyrobí `adafruit-nrfutil dfu genpkg --dev-type 0x0052
  --sd-req 0x00B6 --application zephyr.hex`).
- **Companion sender:** `fota_sender_mcpy.py --port COM3 --freq 869.618
  --bw 62.5 --sf 8 --cr 8 --privkey test_key.der --scope zerohop` (rádio
  parametre MUSIA sedieť s DUT: `get radio`). Companion občas po sende nič
  neodvysiela (session ostane 0/0 alebo 1/0) → opakuj send, príp. reboot
  companiona; DUT rádio je OK (advert obojsmerne overený).
- **RAM flasher fixy** (už vo firmvéri, detail MeshCore tech doc §12.1):
  flasher okno na vrchu RAM 0x2003E000 (rezervované `fota.overlay`, sram0
  248 kB), `MPU->CTRL=0` pred skokom, `fw_image_size` z FwId traileru.
- **`test_key.der`** je gitignored — skopíruj z MeshCore `test_nrf-fota/`
  (pubkey `c22f8ae0…`, key_id 1), inak HEADER nepodpíše → repeater odmietne.

## VS Code — spúšťanie skriptov

Globálny (user-level) task **„Run súbor"** (`D:\FkDev\vscode\data\user-data\
User\tasks.json` + `run_active_file.ps1`) spustí aktívny súbor podľa prípony:
`.py` cez `.venv`/penv python, `.ps1`/`.bat`/`.exe` priamo (pýta si argumenty),
text (`.md`/`.c`/…) otvorí v multi-tab Notepade. Platí vo všetkých repoch.
ZephCore `.vscode/tasks.json` má navyše FOTA tlačidlá (sync check/copy, west
build) — `.vscode/` je gitignored.

## Dokumentácia

- `zephcore/app/nrffota/README.md` — technický popis ZephCore portu
- `fkclaude/specs/2026-07-07-fota-port-design.md` — dizajn portu
- `fkclaude/plans/2026-07-07-fota-port.md` — implementačný plán
- `fkclaude/fcl_e2e_runbook_zephcore_fota.md` — HW e2e postup
- MeshCore: `fkclaude/fcl_readme_nrf-fota.md`, `fcl_readme_tech_nrf-fota.md`
  (kompletná FOTA dokumentácia — protokol, CLI, riešené problémy)
