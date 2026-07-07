#!/usr/bin/env python3
"""
gen_fw_trailer.py — vyplní FwIdTrailer vo firmware image (hex/bin) + voliteľne UF2.

Nájde magic "FKFWID01" v app image, dopočíta:
  - image_size = veľkosť app image (B)
  - sha256     = SHA256 nad celým app image so sha256[] poľom VYNULOVANÝM
a vpíše ich späť.

DVA režimy (zdieľaný skript MeshCore ↔ ZephCore):

1) PlatformIO POST-build (MeshCore): zavesené ako post-action na firmware.hex
   (ElfToHex). Beží PRED PackageDfu (genpkg .zip, ktorý počíta CRC16
   init-packetu z binu) aj pred uf2conv → .hex / .zip / .uf2 nesú správne
   vyplnený trailer a navzájom sedia.
     extra_scripts =
       pre:test_nrf-fota/gen_build_info.py
       post:test_nrf-fota/gen_fw_trailer.py

2) Samostatné CLI (ZephCore, CMake post-build):
     python gen_fw_trailer.py --hex build/zephyr/zephyr.hex \
                              --bin build/zephyr/zephyr.bin \
                              --uf2 build/zephyr/zephyr.uf2
   Patchne .hex aj .bin (rovnaká transformácia → zhodné SHA; pri nezhode
   skončí chybou) a z patchnutého .bin pregeneruje UF2 (base z hexu,
   family nRF52840 0xADA52840). Zephyr build totiž UF2 vyrába PRED naším
   post-buildom, takže ho treba prepísať.

Layout FwIdTrailer (packed, viď nrffota/FwId.h):
  char magic[8]; uint32_t image_size; uint32_t build_number; uint8_t sha256[32];  // 48 B
"""
import hashlib
import struct
import sys
from pathlib import Path

MAGIC      = b"FKFWID01"
OFF_SIZE   = 8      # uint32 LE
OFF_BUILD  = 12     # uint32 LE (len pre výpis)
OFF_SHA    = 16     # 32 B
SHA_LEN    = 32

_PIO_ENV = None
try:
    Import("env")  # type: ignore  # PlatformIO SCons kontext
    _PIO_ENV = env  # type: ignore  # noqa: F821
except Exception:
    _PIO_ENV = None


# ---- Intel HEX → flat image ----
def read_ihex(path: Path):
    ext = 0
    mem = {}
    mn = None
    mx = None
    for line in path.read_text().splitlines():
        if not line.startswith(":"):
            continue
        b = bytes.fromhex(line[1:])
        ln, rectype = b[0], b[3]
        off = (b[1] << 8) | b[2]
        data = b[4:4 + ln]
        if rectype == 0x00:            # data
            a = ext + off
            for i, by in enumerate(data):
                mem[a + i] = by
            lo, hi = a, a + ln - 1
            mn = lo if mn is None else min(mn, lo)
            mx = hi if mx is None else max(mx, hi)
        elif rectype == 0x04:          # extended linear address
            ext = ((data[0] << 8) | data[1]) << 16
        elif rectype == 0x02:          # extended segment address
            ext = ((data[0] << 8) | data[1]) << 4
        elif rectype == 0x01:          # EOF
            break
    if mn is None:
        raise SystemExit("[fwid] HEX neobsahuje dáta")
    # gap fill 0xFF — erased flash aj zephyr.bin (objcopy --gap-fill) maju v
    # dierach 0xFF; s 0x00 by SHA z hexu nesedela s binom ani s realnym flashom
    flat = bytearray(b"\xff" * (mx - mn + 1))
    for a, by in mem.items():
        flat[a - mn] = by
    return mn, flat


# ---- flat image → Intel HEX (re-emit, base zachovaný cez type-04) ----
def _rec(rectype, off, data):
    body = bytes([len(data), (off >> 8) & 0xFF, off & 0xFF, rectype]) + data
    chk = (-sum(body)) & 0xFF
    return ":" + (body + bytes([chk])).hex().upper()


def write_ihex(path: Path, base: int, flat: bytearray):
    lines = []
    cur_ext = None
    i = 0
    n = len(flat)
    while i < n:
        addr = base + i
        ext = (addr >> 16) & 0xFFFF
        if ext != cur_ext:
            lines.append(_rec(0x04, 0, bytes([(ext >> 8) & 0xFF, ext & 0xFF])))
            cur_ext = ext
        # max 16 B/riadok a nikdy neprekroč 64k hranicu
        cnt = min(16, n - i, 0x10000 - (addr & 0xFFFF))
        lines.append(_rec(0x00, addr & 0xFFFF, bytes(flat[i:i + cnt])))
        i += cnt
    lines.append(":00000001FF")
    path.write_text("\n".join(lines) + "\n")


# ---- jadro: patch trailer vo flat image ----
def patch_flat(flat: bytearray, base: int, label: str):
    idx = flat.find(MAGIC)
    if idx < 0:
        print(f"[fwid] {label}: magic FKFWID01 nenájdený — trailer nezapojený? preskakujem")
        return None
    if flat.find(MAGIC, idx + 1) != -1:
        raise SystemExit(f"[fwid] {label}: magic nájdený VIACKRÁT — kolízia, prerušujem")

    # 1) image_size = dĺžka app image
    struct.pack_into("<I", flat, idx + OFF_SIZE, len(flat))

    # 2) sha256 nad celým flat so sha-poľom = 0 (size aj build už vyplnené)
    tmp = bytearray(flat)
    for k in range(SHA_LEN):
        tmp[idx + OFF_SHA + k] = 0
    digest = hashlib.sha256(tmp).digest()
    flat[idx + OFF_SHA: idx + OFF_SHA + SHA_LEN] = digest

    build = struct.unpack_from("<I", flat, idx + OFF_BUILD)[0]
    print(f"[fwid] {label}: trailer @ 0x{base + idx:X} (base 0x{base:X}, off 0x{idx:X})")
    print(f"[fwid]   image_size = {len(flat)} B (0x{len(flat):X})")
    print(f"[fwid]   build #     = {build}")
    print(f"[fwid]   sha256      = {digest.hex()}")
    return digest


def patch_hex(hex_path):
    p = Path(hex_path)
    if not p.exists():
        print(f"[fwid] {p} neexistuje — preskakujem")
        return None, None
    base, flat = read_ihex(p)
    digest = patch_flat(flat, base, p.name)
    if digest is None:
        return None, None
    write_ihex(p, base, flat)
    print(f"[fwid] {p.name} aktualizovaný")
    return base, digest


def patch_bin(bin_path, base: int):
    p = Path(bin_path)
    if not p.exists():
        print(f"[fwid] {p} neexistuje — preskakujem")
        return None
    flat = bytearray(p.read_bytes())
    digest = patch_flat(flat, base, p.name)
    if digest is None:
        return None
    p.write_bytes(bytes(flat))
    print(f"[fwid] {p.name} aktualizovaný")
    return digest


# ---- UF2 z patchnutého .bin (family nRF52840, Adafruit bootloader) ----
UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGICF = 0x0AB16F30
UF2_FAMILY_NRF52840 = 0xADA52840
UF2_FLAG_FAMILY = 0x00002000


def write_uf2(bin_path, uf2_path, base: int):
    data = Path(bin_path).read_bytes()
    nblk = (len(data) + 255) // 256
    out = bytearray()
    for i in range(nblk):
        chunk = data[i * 256:(i + 1) * 256]
        blk = struct.pack("<IIIIIIII", UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY,
                          base + i * 256, 256, i, nblk, UF2_FAMILY_NRF52840)
        blk += chunk + b"\x00" * (476 - len(chunk))
        blk += struct.pack("<I", UF2_MAGICF)
        out += blk
    Path(uf2_path).write_bytes(bytes(out))
    print(f"[fwid] {Path(uf2_path).name} pregenerovaný ({nblk} blokov, base 0x{base:X})")


def _cli():
    import argparse
    ap = argparse.ArgumentParser(description="FwId trailer patcher (hex/bin/uf2)")
    ap.add_argument("--hex", help="Intel HEX na patchnutie (base sa z neho prečíta)")
    ap.add_argument("--bin", help="raw .bin na patchnutie (base z --hex alebo --base)")
    ap.add_argument("--uf2", help="UF2 na PREgenerovanie z patchnutého --bin")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=None,
                    help="app base (nutný pre --bin/--uf2 bez --hex)")
    a = ap.parse_args()
    if not a.hex and not a.bin:
        ap.error("zadaj aspoň --hex alebo --bin")

    base = a.base
    hex_digest = bin_digest = None
    if a.hex:
        hex_base, hex_digest = patch_hex(a.hex)
        if base is None:
            base = hex_base
    if a.bin:
        if base is None:
            ap.error("--bin bez --hex potrebuje --base")
        bin_digest = patch_bin(a.bin, base)
    if hex_digest and bin_digest and hex_digest != bin_digest:
        raise SystemExit("[fwid] CHYBA: SHA z .hex a .bin sa NEZHODUJÚ — image nie je identický!")
    if a.uf2:
        if not a.bin:
            ap.error("--uf2 potrebuje --bin (UF2 sa generuje z patchnutého binu)")
        write_uf2(a.bin, a.uf2, base)


if _PIO_ENV is not None:
    # PlatformIO post-build režim — zaveš na firmware.hex (po ElfToHex,
    # PRED PackageDfu/uf2conv).
    def _post(source, target, env):  # noqa: ANN001
        patch_hex(env.subst("$BUILD_DIR/${PROGNAME}.hex"))

    _PIO_ENV.AddPostAction("$BUILD_DIR/${PROGNAME}.hex", _post)
elif __name__ == "__main__":
    _cli()
