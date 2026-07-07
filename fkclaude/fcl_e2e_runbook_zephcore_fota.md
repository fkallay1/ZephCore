# E2E FOTA test — runbook (ZephCore nRF52840 repeater, LoRa delta-patch)

Postup pre prvý HW test ZephCore FOTA portu. Vychádza z MeshCore runbooku
(`../MeshCore/fkclaude/fcl_e2e_runbook_nrf-fota.md`) — topológia a bridge sú
rovnaké, cieľ (DUT) beží ZephCore.

> **STAV: HW e2e ešte NEBEŽAL.** Port je overený len buildmi (promicro stock aj
> FOTA, pio 3 envy, pytest, fotapkg 329 B medzi #277→#278). Prvé HW kolo urob
> manuálne po krokoch nižšie — automatický runner
> (`fota_test_lora_repeater.py`) je stavaný na PIO build/DFU a na ZephCore
> zatiaľ NEbol adaptovaný (build fáza by volala pio; použi manuálny postup).

## Topológia (rovnaká ako MeshCore)
```
PC ──USB(COM3)── XIAO bridge ──LoRa(CZ)── ProMicro repeater (ZephCore) ──USB(COM5)── PC
```
- Bridge: XIAO nRF52840 (`2886:8044`), FK_lora `gateway_fw`, CZ preset.
- DUT: ProMicro nRF52840 (`239A:00B3`), ZephCore FOTA build, CZ preset
  (`869.525 MHz, SF7, BW62.5` — nastav cez CLI `set freq/sf/bw` alebo prefs).
- Porty identifikuj podľa VID:PID (`Get-CimInstance Win32_SerialPort`).

## Predpoklady
1. Build prostredie: `.venv` (python 3.12 + west), SDK `D:\FkDev\zephyr_sdk`
   (`$env:ZEPHYR_SDK_INSTALL_DIR`), PATH s `.venv\Scripts`.
2. `test_nrf-fota/test_key.der` ⚠️ GITIGNORED — skopíruj z MeshCore
   (pubkey `c22f8ae0…` = `FotaReceiver_signkey.cpp`, key_id=1). Bez neho
   HEADER nepodpíše → repeater odmietne (0x6).
3. `pip install -r test_nrf-fota/requirements.txt` (pyserial, pycryptodome)
   do `.venv`.
4. Na repeateri drž `agc.reset.interval=0` a CAD vypnuté (MeshCore §8.4/§8.7).

## Postup (manuálny, prvé kolo)

```bash
# 0) OLD build + flash (UF2 dvojklik-reset -> PROMICRO disk, skopiruj zephyr.uf2)
west build -b promicro_sx1262 zephcore --pristine -- \
  -DEXTRA_CONF_FILE="boards/common/repeater.conf;boards/common/fota.conf"
cp build/zephyr/zephyr.bin old.bin        # OLD image (build #N)

# 1) NEW build (build# sa bumpne automaticky)
west build -b promicro_sx1262 zephcore -- \
  -DEXTRA_CONF_FILE="boards/common/repeater.conf;boards/common/fota.conf"
cp build/zephyr/zephyr.bin new.bin        # NEW image (build #N+1)

# 2) over konzolu DUT (COM5): boot banner "[FOTA] build #N", heartbeat AALIVE
#    a 'fota id' (image_size + sha musia sediet s old.bin)

# 3) posli patch cez bridge (podpisany)
python test_nrf-fota/fota_sender.py --mode meshcore --bridge-port COM3 \
  --old old.bin --new new.bin --privkey test_nrf-fota/test_key.der

# 4) na DUT (COM5):
#    fota status      -> X/X st=0x07 (VERIFIED); ak chybaju chunky: fota miss + re-send
#    fota verify      -> dry-run "FOTA dry-run OK" (prvykrat NErob verify+flash v jednom boote)
#    (reboot)
#    fota flash       -> "FOTA flash accepted" -> [FOTA] ACK odoslany — spustam flash -> reset

# 5) over: boot banner "[FOTA] build #N+1" (NEW) — PASS
```

## Na čo si dať pozor pri PRVOM behu (nové veci ZephCore portu)
- **RAM flasher**: pred skokom sa vypíše `[FLASHER] → 0x20020000 [BYE]`.
  Ak sa objaví `PRERUŠENÉ — patch buffer zasahuje do flasher RAM regionu`,
  heap alokoval patch nad 0x20020000 → treba zmenšiť/presunúť malloc arénu
  (fota.conf) — nahlás, rieši sa konfiguráciou.
- **`fota verify` (dry-run)** používa rovnaký RAM guard; over ho pred flashom.
- Po neúspešnom flashi je obnova rovnaká ako MeshCore: dvojklik RESET →
  UF2 disk `PROMICRO` → skopíruj funkčný `zephyr.uf2`.
- `fota dbg` GPREGRET2 funguje (číta sa priamo register); trace je bez
  partície nedostupný („nedostupny — bez trace regionu" je očakávaný výpis).
- Konzola: `CONFIG_ZEPHCORE_FOTA_DEBUG=y` dáva `[FOTA] …` výpisy cez printk
  (USB CDC). Repeater CLI beží na tej istej konzole.

## Po prvom úspešnom kole
- Otestuj `fota verify` → `fota flash` v JEDNOM boote (v MeshCore historicky
  problém, po RAM-assembly fixe OK — na ZephCore over).
- Zváž adaptáciu `fota_test_lora_repeater.py` (baseline/run) na west build
  + UF2/DFU flash cestu, aby bežal samobežne aj pre ZephCore.
