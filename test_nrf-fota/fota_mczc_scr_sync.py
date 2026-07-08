#!/usr/bin/env python3
"""fota_mczc_scr_sync.py — drží zdieľané FOTA zdrojáky byte-identické
MeshCore (MC) ↔ ZephCore (ZC).

Zdroj pravdy: MeshCore (vývoj pokračuje tam, vetva features/nrf-fota*).
Skript je obojstranný — beží z test_nrf-fota/ ktoréhokoľvek repa a druhé repo
si nájde ako susedný adresár (override: --meshcore / --zephcore). Kopíruje sa
VŽDY MeshCore -> ZephCore. Sám seba má v zozname, takže sa synchronizuje tiež.

  python test_nrf-fota/fota_mczc_scr_sync.py            # diff report (exit 1 pri rozdieloch)
  python test_nrf-fota/fota_mczc_scr_sync.py --copy     # skopíruje MeshCore -> ZephCore

Pozn.: flasher_code.h NIE JE v zozname — generuje sa per-repo (iný ORIGIN:
MeshCore 0xEB000 flash, ZephCore 0x2003E000 RAM). Generuj:
  python zephcore/app/nrffota/tools/build_flasher.py --origin 0x2003E000 --platform zephcore
"""
import argparse
import filecmp
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent     # <repo>/test_nrf-fota
REPO = HERE.parent

MC_FOTA = "examples/simple_repeater/nrffota"
ZC_FOTA = "zephcore/app/nrffota"

# (meshcore_rel, zephcore_rel)
SHARED = [
    # skupina 1 — platform-free
    (f"{MC_FOTA}/hpatchlite/hpatch_lite.c",             f"{ZC_FOTA}/hpatchlite/hpatch_lite.c"),
    (f"{MC_FOTA}/hpatchlite/hpatch_lite.h",             f"{ZC_FOTA}/hpatchlite/hpatch_lite.h"),
    (f"{MC_FOTA}/hpatchlite/hpatch_lite_input_cache.h", f"{ZC_FOTA}/hpatchlite/hpatch_lite_input_cache.h"),
    (f"{MC_FOTA}/hpatchlite/hpatch_lite_types.h",       f"{ZC_FOTA}/hpatchlite/hpatch_lite_types.h"),
    (f"{MC_FOTA}/puff_stream.c",    f"{ZC_FOTA}/puff_stream.c"),
    (f"{MC_FOTA}/puff_stream.h",    f"{ZC_FOTA}/puff_stream.h"),
    (f"{MC_FOTA}/flasher/flasher.c",  f"{ZC_FOTA}/flasher/flasher.c"),
    (f"{MC_FOTA}/flasher/flasher.ld", f"{ZC_FOTA}/flasher/flasher.ld"),
    (f"{MC_FOTA}/FotaProtocol.h",   f"{ZC_FOTA}/FotaProtocol.h"),
    (f"{MC_FOTA}/FotaState.h",      f"{ZC_FOTA}/FotaState.h"),
    (f"{MC_FOTA}/FotaBuffer.h",     f"{ZC_FOTA}/FotaBuffer.h"),
    (f"{MC_FOTA}/FotaBuffer.cpp",   f"{ZC_FOTA}/FotaBuffer.cpp"),
    (f"{MC_FOTA}/tools/build_flasher.py", f"{ZC_FOTA}/tools/build_flasher.py"),
    # skupina 2 — dualne guardy (FOTA_MESHCORE_BUILD / FOTA_ZEPHCORE_BUILD)
    (f"{MC_FOTA}/FotaMesh.h",       f"{ZC_FOTA}/FotaMesh.h"),
    (f"{MC_FOTA}/FotaMesh.cpp",     f"{ZC_FOTA}/FotaMesh.cpp"),
    (f"{MC_FOTA}/FotaCrypto.h",     f"{ZC_FOTA}/FotaCrypto.h"),
    (f"{MC_FOTA}/FotaFs.h",         f"{ZC_FOTA}/FotaFs.h"),
    (f"{MC_FOTA}/FotaDebug.h",      f"{ZC_FOTA}/FotaDebug.h"),
    (f"{MC_FOTA}/flash_layout.h",   f"{ZC_FOTA}/flash_layout.h"),
    (f"{MC_FOTA}/FwId.h",           f"{ZC_FOTA}/FwId.h"),
    (f"{MC_FOTA}/FwId.cpp",         f"{ZC_FOTA}/FwId.cpp"),
    (f"{MC_FOTA}/FotaReceiver.h",   f"{ZC_FOTA}/FotaReceiver.h"),
    (f"{MC_FOTA}/FotaReceiver.cpp", f"{ZC_FOTA}/FotaReceiver.cpp"),
    (f"{MC_FOTA}/FotaReceiver_signkey.cpp", f"{ZC_FOTA}/FotaReceiver_signkey.cpp"),
    (f"{MC_FOTA}/FotaPatcher.h",    f"{ZC_FOTA}/FotaPatcher.h"),
    (f"{MC_FOTA}/FotaPatcher.cpp",  f"{ZC_FOTA}/FotaPatcher.cpp"),
    # PC tooling (byte-identicky; vratane tohto skriptu)
    ("test_nrf-fota/fota_mczc_scr_sync.py",    "test_nrf-fota/fota_mczc_scr_sync.py"),
    ("test_nrf-fota/fota_sender.py",           "test_nrf-fota/fota_sender.py"),
    ("test_nrf-fota/fota_sender_mcpy.py",      "test_nrf-fota/fota_sender_mcpy.py"),
    ("test_nrf-fota/fota_test_lora_repeater.py","test_nrf-fota/fota_test_lora_repeater.py"),
    ("test_nrf-fota/fota_export_pkg.py",       "test_nrf-fota/fota_export_pkg.py"),
    ("test_nrf-fota/gen_fotapkg.py",           "test_nrf-fota/gen_fotapkg.py"),
    ("test_nrf-fota/gen_fw_trailer.py",        "test_nrf-fota/gen_fw_trailer.py"),
    ("test_nrf-fota/gen_build_info.py",        "test_nrf-fota/gen_build_info.py"),
    ("test_nrf-fota/push_fotapkg.py",          "test_nrf-fota/push_fotapkg.py"),
    ("test_nrf-fota/hdiffi.exe",               "test_nrf-fota/hdiffi.exe"),
    ("test_nrf-fota/requirements.txt",         "test_nrf-fota/requirements.txt"),
    ("test_nrf-fota/tools/emit_fota_golden.py","test_nrf-fota/tools/emit_fota_golden.py"),
    ("test_nrf-fota/tests/test_export_pkg.py", "test_nrf-fota/tests/test_export_pkg.py"),
    ("test_nrf-fota/tests/test_fota_format.py","test_nrf-fota/tests/test_fota_format.py"),
    ("test_nrf-fota/tests/test_grpdata_framing.py","test_nrf-fota/tests/test_grpdata_framing.py"),
    ("test_nrf-fota/tests/test_mcpy_frame.py", "test_nrf-fota/tests/test_mcpy_frame.py"),
]


def detect_roots(args):
    """Zisti, v ktorom repe skript beží, a nájdi druhé (susedné) repo."""
    if (REPO / "zephcore" / "app").is_dir():
        zc = REPO
        mc = Path(args.meshcore) if args.meshcore else REPO.parent / "MeshCore"
    elif (REPO / "examples" / "simple_repeater").is_dir():
        mc = REPO
        zc = Path(args.zephcore) if args.zephcore else REPO.parent / "ZephCore"
    else:
        sys.exit(f"[sync] neviem urcit repo pre {REPO} (ani ZephCore, ani MeshCore layout)")
    if args.meshcore:
        mc = Path(args.meshcore)
    if args.zephcore:
        zc = Path(args.zephcore)
    return mc, zc


def main() -> int:
    ap = argparse.ArgumentParser(description="MC<->ZC FOTA source sync (kopiruje sa vzdy MC -> ZC)")
    ap.add_argument("--meshcore", help="cesta k MeshCore repu (default: toto/susedne repo)")
    ap.add_argument("--zephcore", help="cesta k ZephCore repu (default: toto/susedne repo)")
    ap.add_argument("--copy", action="store_true", help="skopiruj MeshCore -> ZephCore")
    args = ap.parse_args()

    mc, zc = detect_roots(args)
    for name, p in (("MeshCore", mc), ("ZephCore", zc)):
        if not p.is_dir():
            print(f"[sync] {name} nenajdeny: {p}")
            return 2

    diverged, missing = [], []
    for mrel, zrel in SHARED:
        src, dst = mc / mrel, zc / zrel
        if not src.is_file():
            missing.append(f"MC:{mrel}")
            continue
        if not dst.is_file() or not filecmp.cmp(src, dst, shallow=False):
            diverged.append((src, dst))

    for tag in missing:
        print(f"[sync] CHYBA zdroj: {tag}")
    for src, dst in diverged:
        if args.copy:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            print(f"[sync] copy  {dst.relative_to(zc)}")
        else:
            print(f"[sync] DIFF  {dst.relative_to(zc)}")

    if not diverged and not missing:
        print(f"[sync] OK — {len(SHARED)} suborov identickych (MC={mc.name}, ZC={zc.name})")
        return 0
    return 0 if (args.copy and not missing) else 1


if __name__ == "__main__":
    sys.exit(main())
