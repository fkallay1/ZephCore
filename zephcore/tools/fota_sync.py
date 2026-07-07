#!/usr/bin/env python3
"""fota_sync.py — drží zdieľané FOTA súbory byte-identické MeshCore ↔ ZephCore.

Zdroj pravdy: MeshCore (vývoj pokračuje tam, vetva features/nrf-fota*).
Default: report divergencií (exit 1 pri rozdieloch).

  python zephcore/tools/fota_sync.py                 # diff report
  python zephcore/tools/fota_sync.py --copy          # skopíruje MeshCore -> ZephCore
  python zephcore/tools/fota_sync.py --meshcore D:/cesta/k/MeshCore

Pozn.: flasher_code.h NIE JE v zozname — generuje sa per-repo (iný ORIGIN:
MeshCore 0xEB000 flash, ZephCore 0x20020000 RAM). Generuj:
  python zephcore/app/nrffota/tools/build_flasher.py --origin 0x20020000 --platform zephcore
"""
import argparse
import filecmp
import shutil
import sys
from pathlib import Path

ZEPH_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_MESHCORE = ZEPH_ROOT.parent / "MeshCore"

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
    # PC tooling (byte-identicky)
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--meshcore", default=str(DEFAULT_MESHCORE),
                    help=f"cesta k MeshCore repu (default {DEFAULT_MESHCORE})")
    ap.add_argument("--copy", action="store_true", help="skopiruj MeshCore -> ZephCore")
    args = ap.parse_args()

    mc = Path(args.meshcore)
    if not mc.is_dir():
        print(f"[fota_sync] MeshCore nenajdeny: {mc}")
        return 2

    diverged, missing = [], []
    for mrel, zrel in SHARED:
        src, dst = mc / mrel, ZEPH_ROOT / zrel
        if not src.is_file():
            missing.append(f"MC:{mrel}")
            continue
        if not dst.is_file() or not filecmp.cmp(src, dst, shallow=False):
            diverged.append((src, dst))

    for tag in missing:
        print(f"[fota_sync] CHYBA zdroj: {tag}")
    for src, dst in diverged:
        if args.copy:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            print(f"[fota_sync] copy  {dst.relative_to(ZEPH_ROOT)}")
        else:
            print(f"[fota_sync] DIFF  {dst.relative_to(ZEPH_ROOT)}")

    if not diverged and not missing:
        print(f"[fota_sync] OK — {len(SHARED)} suborov identickych")
        return 0
    return 0 if (args.copy and not missing) else 1


if __name__ == "__main__":
    sys.exit(main())
