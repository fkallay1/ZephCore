#!/usr/bin/env python3
"""
gen_fw_trailer.py — PlatformIO POST-build skript: vyplní FwIdTrailer vo firmware.hex.

Nájde magic "FKFWID01" v app image, dopočíta:
  - image_size = veľkosť app image (B)
  - sha256     = SHA256 nad celým app image so sha256[] poľom VYNULOVANÝM
a vpíše ich späť do firmware.hex.

DÔLEŽITÉ PORADIE: zavesené ako post-action na firmware.hex (ElfToHex). Beží PRED
PackageDfu (genpkg .zip, ktorý počíta CRC16 init-packetu z binu) aj pred uf2conv.
Vďaka tomu .hex / .zip / .uf2 nesú správne vyplnený trailer a navzájom sedia.

Layout FwIdTrailer (packed, viď nrffota/FwId.h):
  char magic[8]; uint32_t image_size; uint32_t build_number; uint8_t sha256[32];  // 48 B

Zapojené vo FOTA env (variants/promicro/platformio.ini):
    extra_scripts =
      pre:test_nrf-fota/gen_build_info.py
      post:test_nrf-fota/gen_fw_trailer.py
"""
import hashlib
import struct
from pathlib import Path

MAGIC      = b"FKFWID01"
OFF_SIZE   = 8      # uint32 LE
OFF_BUILD  = 12     # uint32 LE (len pre výpis)
OFF_SHA    = 16     # 32 B
SHA_LEN    = 32

Import("env")  # type: ignore  # PlatformIO SCons kontext


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
    flat = bytearray(mx - mn + 1)
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


def patch_hex(hex_path: str):
    p = Path(hex_path)
    if not p.exists():
        print(f"[fwid] {p} neexistuje — preskakujem")
        return

    base, flat = read_ihex(p)

    idx = flat.find(MAGIC)
    if idx < 0:
        print("[fwid] magic FKFWID01 nenájdený — trailer nezapojený? preskakujem")
        return
    if flat.find(MAGIC, idx + 1) != -1:
        raise SystemExit("[fwid] magic nájdený VIACKRÁT — kolízia, prerušujem")

    abs_addr = base + idx

    # 1) image_size = dĺžka app image
    struct.pack_into("<I", flat, idx + OFF_SIZE, len(flat))

    # 2) sha256 nad celým flat so sha-poľom = 0 (size aj build už vyplnené)
    tmp = bytearray(flat)
    for k in range(SHA_LEN):
        tmp[idx + OFF_SHA + k] = 0
    digest = hashlib.sha256(tmp).digest()
    flat[idx + OFF_SHA: idx + OFF_SHA + SHA_LEN] = digest

    write_ihex(p, base, flat)

    build = struct.unpack_from("<I", flat, idx + OFF_BUILD)[0]
    print(f"[fwid] trailer @ 0x{abs_addr:X} (base 0x{base:X}, off 0x{idx:X})")
    print(f"[fwid]   image_size = {len(flat)} B (0x{len(flat):X})")
    print(f"[fwid]   build #     = {build}")
    print(f"[fwid]   sha256      = {digest.hex()}")
    print(f"[fwid] firmware.hex aktualizovaný (pred .zip / .uf2)")


def _post(source, target, env):  # noqa: ANN001
    patch_hex(env.subst("$BUILD_DIR/${PROGNAME}.hex"))


# Zaveš na firmware.hex → beží po ElfToHex a PRED PackageDfu/uf2conv.
env.AddPostAction("$BUILD_DIR/${PROGNAME}.hex", _post)  # type: ignore
