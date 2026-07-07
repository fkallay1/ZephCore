#!/usr/bin/env python3
"""
gen_fotapkg_hook.py — PlatformIO POST-build hook.

Po tom, čo gen_fw_trailer.py vyplní FwIdTrailer vo firmware.hex, tento hook:
  - archivuje app image aktuálneho buildu (builds/fw_<N>.bin),
  - vygeneruje fw.fotapkg.json (upgrade) + fw_reverse.fotapkg.json (rollback)
    medzi POSLEDNÝM a PREDPOSLEDNÝM buildom (cez gen_fotapkg.py --from-hex).

NEFATÁLNE: akékoľvek zlyhanie (chýbajúci predošlý build, hdiffi, privkey…) len
vypíše varovanie a NIKDY nezhodí build.

Zapojené vo FOTA env PO gen_fw_trailer (poradie v extra_scripts určuje poradie
post-akcií na firmware.hex):
    post:test_nrf-fota/gen_fw_trailer.py
    post:test_nrf-fota/gen_fotapkg_hook.py

Vypnutie: zakomentuj riadok v variants/promicro/platformio.ini, alebo nastav
env premennú FOTAPKG_SKIP=1 (legacy OTAPKG_SKIP=1 tiež funguje).
"""
import locale
import os
import subprocess
import sys
from pathlib import Path

Import("env")  # type: ignore  # PlatformIO SCons kontext


def _safe_print(s):  # noqa: ANN001
    # PlatformIO náš stdout re-echo-uje cez click.secho do SVOJHO stdoutu, ktorý
    # je na Windows cp1250. Keď riadok obsahuje '→' (U+2192) a iné znaky mimo
    # cp1250, padne až ten vonkajší pio proces (náš print prejde). Preto riadok
    # sanitizujeme do locale kódovania UŽ TU: '→' → '->', ostatné nereprezentova-
    # teľné → '?'. Slovenské znaky sú v cp1250 OK, takže ostanú.
    s = s.replace("→", "->")
    sink = locale.getpreferredencoding(False) or "utf-8"
    print(s.encode(sink, "replace").decode(sink))


def _post(source, target, env):  # noqa: ANN001
    if os.environ.get("FOTAPKG_SKIP") or os.environ.get("OTAPKG_SKIP"):
        print("[fotapkg] hook: FOTAPKG_SKIP nastavené — preskakujem")
        return
    hexf = env.subst("$BUILD_DIR/${PROGNAME}.hex")
    script = Path(env.subst("$PROJECT_DIR")) / "test_nrf-fota" / "gen_fotapkg.py"
    # názov zariadenia z env: "ProMicro_repeater_fota" → "promicro"
    device = env.subst("$PIOENV").split("_")[0].lower() or "device"
    try:
        # Vynúť UTF-8 na oboch stranách: dieťa tlačí slovenské znaky v UTF-8,
        # bez tohto by parent na Windows dekódoval cez locale (cp1250) a spadol
        # na 0x88 (UTF-8 continuation byte) v reader-threade. errors="replace"
        # je poistka, aby hook nikdy nepadol na dekódovaní.
        child_env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUTF8="1")
        r = subprocess.run([sys.executable, str(script), "--from-hex", hexf, "--device", device],
                           capture_output=True, encoding="utf-8", errors="replace", env=child_env)
        for ln in ((r.stdout or "") + (r.stderr or "")).splitlines():
            if ln.startswith(("[fotapkg]", "[export]")):
                _safe_print(ln)
        if r.returncode != 0:
            _safe_print("[fotapkg] hook: json nevygenerovaný (možno len 1 build v archíve) — OK")
    except Exception as e:  # noqa: BLE001
        _safe_print(f"[fotapkg] hook chyba (nefatálne): {e}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.hex", _post)  # type: ignore
