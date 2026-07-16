#!/usr/bin/env python3
"""
fota_sender.py  — MeshCore FOTA over LoRa / Serial sender

Závislosti:
    pip install pyserial pycryptodome

Použitie — cez LoRa gateway:
    python fota_sender.py --old old.bin --new new.bin --port COM5 --psk 9cd8... --mode meshcore
    python fota_sender.py --old old.bin --new new.bin --port COM5 --mode direct

Použitie — priamo cez serial na sniffer (bez LoRa!):
    python fota_sender.py --old old.bin --new new.bin --port COM5 --mode serial-direct

  serial-direct:  rámce [0xAB][0xCD][len][data] priamo na sniffer USB serial
                  sniffer ich detekuje v loop() cez serial_inject_try()
                  USB konzola stále funguje (text príkazy + výpisy súčasne)

Generovanie patchu:
  1. hdiffpatch (odporúčané, HPatchLite na zariadení):
       pip install hdiffpatch  (alebo stiahnuť hdiffz binary)
  2. bsdiff4 (záložné, pre budúcu kompatibilitu):
       pip install bsdiff4

Serial rámec od zariadenia (gateway reply): [0xCC][0xDD][len 2B LE][data]
"""

import argparse
import hashlib
import hmac as hmaclib
from collections import namedtuple
import random
import serial
import serial.threaded
import struct
import sys
import time
import threading
from pathlib import Path

# Windows konzola býva cp1250 — prepni na utf-8 (znaky →, š, č v print)
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

# ─────────────────────────────────────────────────────────────────────
# Protokol — synchronizované s nrffota/FotaProtocol.h
# ─────────────────────────────────────────────────────────────────────
FOTA_PKT_HEADER   = 0x10
FOTA_PKT_CHUNK   = 0x11
FOTA_PKT_APPLY   = 0x12
FOTA_PKT_HDR_SIG  = 0x13   # 2. časť HEADER — Ed25519 podpis (zjednotený formát v0)
FOTA_PKT_STATUS  = 0x20
FOTA_PKT_NACK    = 0x21

# Zjednotený FOTA formát v0 — viď fkclaude/docs/superpowers/specs/2026-06-23-fota-companion-mcpy-design.md
FOTA_MAGIC        = 0x07A0      # GRP_DATA data_type pre FOTA (gating diskriminátor)
FOTA_PROT_INF_V0  = 0x00       # verzia FOTA protokolu/štruktúr
FOTA_CHUNK_DATA   = 144        # bolo 150 — GRP_DATA limit data_len ≤165 (4B ts + 13B hdr + 144 = 161)
FOTA_CHANNEL_NAME = "#fkotanrf"
DIRECT_MAGIC    = b'\x4F\x54'   # 'OT'

# MeshCore route type (header bity 0-1, PH_ROUTE_MASK) — určuje LoRa šírenie
ROUTE_TYPE_TRANSPORT_FLOOD = 0   # flood + transport_codes (region scope)
ROUTE_TYPE_FLOOD           = 1   # flood (každý repeater re-flooduje)
ROUTE_TYPE_DIRECT          = 2   # direct/zero-hop (path_len=0 = zero-hop)

# scope = (mode, key, path_bytes, path_hashsize); key/path_* relevantné len pre region/direct.
# path_hashsize = 1/2/3 (bajty na hash), default 1. path_bytes = hop_count * path_hashsize bajtov.
Scope = namedtuple('Scope', ['mode', 'key', 'path_bytes', 'path_hashsize'],
                   defaults=(None, b'', 1))

FOTA_ST_VERIFIED = 0x04
FOTA_ST_ERROR    = 0x80

FRAME_SYNC_TX   = b'\xAB\xCD'  # PC → gateway / sniffer
FRAME_SYNC_RX   = b'\xCC\xDD'  # gateway → PC

# ─────────────────────────────────────────────────────────────────────
# CRC16/CCITT-FALSE
# ─────────────────────────────────────────────────────────────────────
def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc

# ─────────────────────────────────────────────────────────────────────
# Ed25519 podpisanie FOTA HEADER
# ─────────────────────────────────────────────────────────────────────
FOTA_KEY_ID_PREFIX = 0   # key_id=0 -> podpisovateľ identifikovaný 4B pubkey prefixom za podpisom

def load_ed25519_privkey(key_path: Path):
    """Načíta Ed25519 private key z DER súboru, vráti ExpandedKey (jednotný signer)."""
    from fota_ed25519_expanded import key_from_der
    return key_from_der(key_path)

def load_ed25519_privkey_hex(hexstr: str):
    """64 B expandovaný kľúč (companion 'dlhý hex') -> ExpandedKey."""
    from fota_ed25519_expanded import key_from_hex
    from fota_texts import T
    try:
        return key_from_hex(hexstr)
    except ValueError as e:
        sys.exit(T("sign_privkey_hex_err", msg=e))

def sign_fota_header(otbmsg: bytes, privkey) -> bytes:
    """Podpise message (102B META) a vráti 64B signature."""
    return privkey.sign(otbmsg)

# ─────────────────────────────────────────────────────────────────────
# Kryptografia (MeshCore GRP_DATA)
# ─────────────────────────────────────────────────────────────────────
def meshcore_encrypt(psk: bytes, plaintext: bytes) -> bytes:
    try:
        from Crypto.Cipher import AES
    except ImportError:
        sys.exit("[CHYBA] --mode meshcore potrebuje pycryptodome:\n"
                 "        <penv>/python.exe -m pip install pycryptodome")
    psk32 = psk.ljust(32, b'\x00')
    pad = (16 - len(plaintext) % 16) % 16
    padded = plaintext + b'\x00' * pad
    cipher = AES.new(psk32[:16], AES.MODE_ECB)
    ciphertext = cipher.encrypt(padded)
    mac = hmaclib.new(psk32, ciphertext, hashlib.sha256).digest()[:2]
    return mac + ciphertext

def calc_transport_code(scope_key16: bytes, payload_type: int, payload: bytes) -> int:
    """Replikuje TransportKey::calcTransportCode (TransportKeyStore.cpp).
    HMAC-SHA256(key16, type(1B) + payload), prvé 2B little-endian uint16."""
    d = hmaclib.new(scope_key16, bytes([payload_type]) + payload, hashlib.sha256).digest()
    code = d[0] | (d[1] << 8)
    if code == 0:        code = 1        # 0x0000 a 0xFFFF sú rezervované
    elif code == 0xFFFF: code = 0xFFFE
    return code

def wrap_meshcore_packet(payload_type: int, payload: bytes, scope: Scope) -> bytes:
    """Payload-type-agnostický wrapper: [hdr][transport_codes?][path_len][path][payload].
    'payload' je už hotové payload pole (pre GRP_DATA: ch_hash+MAC+ciphertext).
    Oddelené od typu, aby Fáza 2 (RAW_CUSTOM) iba zavolala s payload_type=0x0F."""
    if scope.mode == 'flood':
        route, codes, path = ROUTE_TYPE_FLOOD, b'', b''
    elif scope.mode == 'zerohop':
        route, codes, path = ROUTE_TYPE_DIRECT, b'', b''          # path_len=0 → zero-hop
    elif scope.mode == 'region':
        code1 = calc_transport_code(scope.key, payload_type, payload)
        route, codes, path = ROUTE_TYPE_TRANSPORT_FLOOD, struct.pack('<HH', code1, 0), b''
    elif scope.mode == 'direct':
        route, codes, path = ROUTE_TYPE_DIRECT, b'', scope.path_bytes
    else:
        raise ValueError(f'neznámy scope.mode: {scope.mode!r}')
    header = (payload_type << 2) | route
    # path_len: bity 0-5 = počet hopov, bity 6-7 = (hash_size - 1)
    hsz       = scope.path_hashsize
    hop_count = len(path) // hsz
    path_len  = ((hsz - 1) << 6) | (hop_count & 0x3F)
    return bytes([header]) + codes + bytes([path_len]) + path + payload

def fota_channel_secret(name: str = FOTA_CHANNEL_NAME) -> bytes:
    """FOTA kanál secret (16B PSK) = SHA256(name)[0:16] — MeshCore #-konvencia,
    zhodné s meshcore_py set_channel (device.py:216, hashuje meno vrátane '#')."""
    return hashlib.sha256(name.encode("utf-8")).digest()[:16]

def build_meta_payload(total_chunks: int, patch_size: int,
                       patch_sha256: bytes, new_sha256: bytes, old_sha256: bytes) -> bytes:
    """META (102B) = podpisovaná správa zjednoteného formátu. total_chunks sa NEposiela
    (odvodí sa z patch_size/FOTA_CHUNK_DATA), old_sha256_prefix sa NEposiela (= old_sha256[:4]).
    Argument total_chunks ponechaný pre kompatibilitu volajúceho/logu."""
    msg = (bytes([FOTA_PKT_HEADER, FOTA_PROT_INF_V0])
           + struct.pack('<I', patch_size)
           + patch_sha256 + new_sha256 + old_sha256)
    assert len(msg) == 102, f"META musi byt 102B, je {len(msg)}"
    return msg

def build_sig_payload(meta: bytes, privkey, key_id: int) -> bytes:
    """SIG = type+fota_prot_inf+old_sha256+key_id+signature[+signer_prefix].
    key_id=0 (nový formát): +4B prefix pubkey podpisovateľa -> 103 B; receiver
    hľadá prefix v s_authors a potom v ACL adminoch. key_id>=1 (legacy, 99 B):
    staré FW, s_authors[key_id-1]. Podpis vždy nad 102B META."""
    sig = sign_fota_header(meta, privkey) if privkey else bytes(64)
    old_sha256 = meta[70:102]
    out = bytes([FOTA_PKT_HDR_SIG, FOTA_PROT_INF_V0]) + old_sha256 + bytes([key_id]) + sig
    if key_id == FOTA_KEY_ID_PREFIX:
        if privkey is None:
            from fota_texts import T
            sys.exit(T("sign_keyid0_needs_priv"))
        out += privkey.pub[:4]
        assert len(out) == 103, f"SIG(v0-prefix) musi byt 103B, je {len(out)}"
    else:
        assert len(out) == 99, f"SIG musi byt 99B, je {len(out)}"
    return out

def grpdata_plaintext(fota_payload: bytes, ts: int) -> bytes:
    """Zjednotený GRP_DATA plaintext: [FOTA_MAGIC 2B LE][len 1B = 4+len][ts 4B LE][fota_payload].
    Bajt-identické s tým, čo companion sendGroupData zostaví z data=[ts4][fota_payload]."""
    data = struct.pack('<I', ts & 0xFFFFFFFF) + fota_payload
    return struct.pack('<HB', FOTA_MAGIC, len(data)) + data

def companion_grpdata_plaintext(data_type: int, data: bytes) -> bytes:
    """Replika BaseChatMesh::sendGroupData temp[] = [data_type 2B][len 1B][data] — pre testy ekvivalencie."""
    return struct.pack('<HB', data_type, len(data)) + data

def build_grpdata_payload(psk: bytes, fota_payload: bytes, ts: int | None = None) -> bytes:
    """GRP_DATA payload pole: [ch_hash][MAC+ciphertext]. plaintext = zjednotený formát (s FOTA_MAGIC).
    POZN: pri kanáli #fkotanrf je psk = fota_channel_secret(); ch_hash = sha256(secret)[0] = 0xA4."""
    ch_hash = hashlib.sha256(psk).digest()[0]
    if ts is None:
        ts = int(time.time()) & 0xFFFFFFFF
    plain = grpdata_plaintext(fota_payload, ts)
    return bytes([ch_hash]) + meshcore_encrypt(psk, plain)

def meshcore_grp_data_packet(psk: bytes, fota_payload: bytes, scope: Scope) -> bytes:
    return wrap_meshcore_packet(6, build_grpdata_payload(psk, fota_payload), scope)

def direct_fota_packet(fota_payload: bytes) -> bytes:
    return DIRECT_MAGIC + fota_payload

# ─────────────────────────────────────────────────────────────────────
# Generovanie patchu — hdiffpatch alebo bsdiff4
# ─────────────────────────────────────────────────────────────────────
def _hpatchi_extra_safe(raw: bytes):
    """extraSafeSize z 'hI' hlavičky HPatchLite inplace patchu, alebo None.

    Formát hlavičky (zrkadlí hpatchi_inplace_open v hpatch_lite.c):
      [0..1]='hI'  [2]=compressType  [3]=(inplaceCode<<6)|(uncompBytes<<3)|(newSizeBytes)
      [4]=extraSafeBytes  potom newSize, uncompSize, extraSafeSize (LE, dané počty bajtov).
    """
    if len(raw) < 5 or raw[0:2] != b'hI' or (raw[3] >> 6) != 2:
        return None
    lenn = raw[3] & 7
    lenu = (raw[3] >> 3) & 7
    lene = raw[4]
    off = 5 + lenn + lenu
    if len(raw) < off + lene:
        return None
    return int.from_bytes(raw[off:off + lene], 'little')

def make_patch(old_path: Path, new_path: Path, patch_path: Path):
    """Vracia (patch_bytes, patch_sha256, new_sha256, old_sha256, old_fw_size)."""
    old_data = old_path.read_bytes()
    new_data = new_path.read_bytes()

    new_sha256 = hashlib.sha256(new_data).digest()
    old_sha256 = hashlib.sha256(old_data).digest()
    print(f"[patch] OLD fw: {len(old_data)}B  sha256={old_sha256.hex()}")
    print(f"[patch] NEW fw: {len(new_data)}B  sha256={new_sha256.hex()}")

    # hdiffi -inplaceB: HPatchLite inplace formát — POVINNÉ pre in-place flasher na nRF52840.
    # Formát inplaceB zaručuje, že pri zápise na adresu X ešte nie sú potrebné staré dáta
    # z predchádzajúcich adries (extraSafeSize v hlavičke patch-u).
    # POZOR: hdiffi generuje iný formát ako hdiffz — nie sú vzájomne kompatibilné!
    #
    # Získaj hdiffi:
    #   git clone https://github.com/sisong/HPatchLite.git
    #   cd HPatchLite && g++ hdiffi.cpp HDiffPatch/libHDiffPatch/HDiff/*.cpp -O2 -o hdiffi
    # alebo stiahni binárku z: https://github.com/sisong/HPatchLite/releases
    import subprocess, os, tempfile, shutil, zlib, struct
    # Hľadaj hdiffi: vedľa skriptu (tools/), potom PATH
    _script_dir = Path(__file__).parent
    _hdiffi = None
    for candidate in [_script_dir / 'hdiffi.exe', _script_dir / 'hdiffi',
                      Path('hdiffi.exe'), Path('hdiffi')]:
        if candidate.exists():
            _hdiffi = str(candidate)
            break
    if _hdiffi is None:
        _hdiffi = shutil.which('hdiffi') or shutil.which('hdiffi.exe')
    if _hdiffi is None:
        print('[CHYBA] hdiffi.exe nenajdeny — skopiruj ho do tools/')
        sys.exit(1)
    print(f'[patch] hdiffi: {_hdiffi}')

    tmp = tempfile.mktemp(suffix='.hpatch')

    def _run_hdiffi(flags):
        """Spusti hdiffi s danými flagmi, vráti raw patch bytes alebo None."""
        try:
            ret = subprocess.run(
                [_hdiffi, *flags, str(old_path), str(new_path), tmp],
                capture_output=True
            )
        except FileNotFoundError:
            print(f'[CHYBA] hdiffi sa nedal spustit: {_hdiffi}')
            sys.exit(1)
        if ret.returncode != 0:
            return None
        raw = Path(tmp).read_bytes()
        if os.path.exists(tmp):
            os.unlink(tmp)
        return raw

    # ── Výber extraSafeSize: dvojkandidátna stratégia ────────────────────
    # -inplace-N: N = strop extraSafeSize pre hdiffi. extraSafeSize v hlavičke patchu
    # rastie s tým, o koľko sa obsah FW medzi buildmi POSUNUL (≈ o koľko FW narástol);
    # flasher patch s extra_safe > FOTA_MAX_EXTRA_SAFE (32768, flash_layout.h) odmietne
    # s 0xE5. Detailný rozbor: fkclaude/fcl_readme_fota_extrasafe.md.
    #
    # PREČO dva kandidáti: hdiffi si pokojne zvolí extra_safe > 4096 aj pri malej zmene,
    # keď mu to strop dovolí (napr. rast +144B → extra_safe 12kB) — a taký patch odmietnu
    # STARÉ flashery (limit 4kB, sensecap build <= 265). Preto:
    #   1. kandidát -inplace-4096  → kompatibilný so VŠETKÝMI flashermi (preferovaný),
    #   2. kandidát -inplace-32768 → záchrana pre veľké rasty FW (starý limit degeneroval
    #      patch na ~celý obraz, ~300kB — nezmestil by sa do FOTA FS ani RAM).
    # Kandidát 1 vyhráva, ak jeho komprimovaná veľkosť <= 32kB (norma je jednotky kB);
    # inak sa berie menší z dvojice a vypíše sa POZOR o nekompatibilite so starými flashermi.
    raw4  = _run_hdiffi(['-inplace-4096'])
    raw32 = _run_hdiffi(['-inplace-32768'])
    raw_patch = None
    if raw4 is not None or raw32 is not None:
        comp_len = lambda r: len(zlib.compress(r, level=9, wbits=-9)) if r is not None else None
        c4, c32 = comp_len(raw4), comp_len(raw32)
        if c4 is not None:
            print(f"[patch] kandidat -inplace-4096:  raw={len(raw4)}B comp={c4}B extraSafe={_hpatchi_extra_safe(raw4)}B")
        if c32 is not None:
            print(f"[patch] kandidat -inplace-32768: raw={len(raw32)}B comp={c32}B extraSafe={_hpatchi_extra_safe(raw32)}B")
        if c4 is not None and (c32 is None or c4 <= 32768 or c4 <= c32):
            raw_patch = raw4
        else:
            raw_patch = raw32
    else:
        # Fallback pre exotické verzie hdiffi CLI (-inplaceB = novšie, auto extraSafe)
        for flags in [['-inplaceB'], ['-inplace']]:
            raw_patch = _run_hdiffi(flags)
            if raw_patch is not None:
                print(f"[patch] hdiffi fallback {' '.join(flags)}: raw={len(raw_patch)}B")
                break
    if raw_patch is None:
        print(f"[CHYBA] hdiffi zlyhalo (skúšané -inplace-4096/-inplace-32768/-inplaceB/-inplace)")
        sys.exit(1)

    # extraSafeSize z 'hI' hlavičky zvoleného patchu — info + tvrdá kontrola voči flasheru.
    es = _hpatchi_extra_safe(raw_patch)
    if es is not None:
        print(f"[patch] extraSafeSize={es}B (limit flashera FOTA_MAX_EXTRA_SAFE=32768B)")
        if es > 32768:
            print(f"[CHYBA] extraSafeSize {es}B > 32768B — KAŽDÝ flasher tento patch odmietne (0xE5)!")
            sys.exit(1)
        if es > 4096:
            print(f"[POZOR] extraSafeSize {es}B > 4096B — flashery zo starých buildov (limit 4kB, "
                  f"sensecap <=265) tento patch odmietnu (0xE5). Cieľ musí bežať FW s 32kB flasherom.")

    # Komprimuj: raw DEFLATE, 512B okno (wbits=-9)
    # Flasher dekompresoruje pomocou puff.c (tiez wbits=9)
    comp_data = zlib.compress(raw_patch, level=9, wbits=-9)
    ratio = len(comp_data) * 100 // len(raw_patch) if raw_patch else 100
    print(f"[patch] puff kompresia: {len(raw_patch)}B -> {len(comp_data)}B ({ratio}%)")

    # Staged format: [magic 4B 'ZLIB'][uncomp_size 4B LE][new_fw_size 4B LE][deflate...]
    new_fw_size = len(new_data)
    staged = (b'ZLIB'
              + struct.pack('<II', len(raw_patch), new_fw_size)
              + comp_data)
    staged_sha = hashlib.sha256(staged).digest()
    print(f"[patch] staged (LittleFS): {len(staged)}B  (header=12B)")
    print(f"[patch] PATCH (staged) sha256={staged_sha.hex()}")

    patch_path.write_bytes(staged)
    return staged, staged_sha, new_sha256, old_sha256, len(old_data)

# ─────────────────────────────────────────────────────────────────────
# Serial framing
# ─────────────────────────────────────────────────────────────────────
def send_frame(ser: serial.Serial, data: bytes):
    frame = FRAME_SYNC_TX + struct.pack('<H', len(data)) + data
    try:
        ser.write(frame)
    except serial.SerialException as e:
        raise serial.SerialException(
            f"[CHYBA] Serial port stratený — zariadenie sa rebootlo?\n  ({e})"
        ) from e

def read_response_frame(buf: bytearray, magic: bytes = FRAME_SYNC_RX):
    """Hľadaj rámec v buffri, vráti (payload, zvyšok_bufra) alebo (None, buf)."""
    idx = buf.find(magic)
    if idx < 0:
        return None, buf
    if len(buf) < idx + 4:
        return None, buf
    length = struct.unpack_from('<H', buf, idx + 2)[0]
    if len(buf) < idx + 4 + length:
        return None, buf
    payload = bytes(buf[idx + 4: idx + 4 + length])
    return payload, bytearray(buf[idx + 4 + length:])

# ─────────────────────────────────────────────────────────────────────
# Vlákno pre čítanie sériového výstupu zariadenia
# ─────────────────────────────────────────────────────────────────────
class SerialReader(threading.Thread):
    """Číta riadky zo sériového portu a vypisuje ich. Thread-safe queue pre odpovede."""
    def __init__(self, ser: serial.Serial, response_queue):
        super().__init__(daemon=True)
        self.ser = ser
        self.q = response_queue
        self._stop = threading.Event()
        self._buf = bytearray()

    def stop(self): self._stop.set()

    def run(self):
        while not self._stop.is_set():
            try:
                chunk = self.ser.read(256)
                if chunk:
                    self._buf.extend(chunk)
                    # Hľadaj binárne rámce (odpovede od gateway)
                    while True:
                        payload, self._buf = read_response_frame(self._buf)
                        if payload is None:
                            break
                        self.q.put(payload)
                    # Vypiš čitateľné znaky
                    while b'\n' in self._buf:
                        line, self._buf = self._buf.split(b'\n', 1)
                        try:
                            print('  [sniffer]', line.decode('utf-8', errors='replace').rstrip())
                        except Exception:
                            pass
            except serial.SerialException:
                break

# ─────────────────────────────────────────────────────────────────────
# FOTA paket builders  (HEADER = META+SIG, viď build_meta_payload/build_sig_payload)
# ─────────────────────────────────────────────────────────────────────
def build_fota_chunk(idx: int, data: bytes, old_fw_size: int, old_sha256_prefix: bytes) -> bytes:
    """Vyrovi FOTA_CHUNK s base FW validáciou (+8B oproti pôvodnému)."""
    return (bytes([FOTA_PKT_CHUNK])
            + struct.pack('<HH', idx, crc16(data))
            + struct.pack('<I', old_fw_size)
            + old_sha256_prefix  # 4B
            + data)

def build_fota_apply(patch_sha256: bytes) -> bytes:
    return bytes([FOTA_PKT_APPLY]) + patch_sha256

# ─────────────────────────────────────────────────────────────────────
# Hlavná FOTA session
# ─────────────────────────────────────────────────────────────────────
def send_fota(ser: serial.Serial,
             patch: bytes, patch_sha256: bytes, new_sha256: bytes,
             old_sha256: bytes, old_fw_size: int,
             psk: bytes | None, mode: str,
             chunk_delay: float, nack_retries: int, do_reboot: bool = False,
             drop_prob: float = 0.0,
             privkey=None, key_id: int = 1, packetorder: str = 'normal',
             scope: Scope = Scope('zerohop', None, b''), header_every: int = 0):
    chunks = [patch[i:i+FOTA_CHUNK_DATA] for i in range(0, len(patch), FOTA_CHUNK_DATA)]
    total  = len(chunks)
    old_sha256_prefix = old_sha256[:4]  # 4B pre session izoláciu
    print(f"[FOTA] {len(patch)}B → {total} chunkov")
    print(f"[FOTA]   PATCH sha256 = {patch_sha256.hex()}")
    print(f"[FOTA]   OLD   sha256 = {old_sha256.hex()}  ({old_fw_size}B)")
    print(f"[FOTA]   NEW   sha256 = {new_sha256.hex()}")
    air_min = total * 1.2 / 60
    print(f"[FOTA] ~{air_min:.1f} min @ SF8/BW62.5  chunk_delay={chunk_delay}s")

    import queue
    resp_queue: queue.Queue = queue.Queue()
    reader = SerialReader(ser, resp_queue)
    reader.start()

    def send_pkt(payload: bytes):
        if mode == 'meshcore':
            lora_pkt = meshcore_grp_data_packet(psk, payload, scope)
        elif mode == 'direct':
            lora_pkt = direct_fota_packet(payload)
        else:  # serial-direct: inject priamo ako direct FOTA cez serial rámec
            lora_pkt = direct_fota_packet(payload)
        send_frame(ser, lora_pkt)

    # --- HEADER = META (102B, podpisované) + SIG (99B) — zjednotený formát v0 ---
    meta_payload = build_meta_payload(total, len(patch), patch_sha256, new_sha256, old_sha256)
    if privkey:
        print(f"[FOTA] Signujem META (key_id=0x{key_id:02X})...")
    else:
        print("[FOTA] WARNING: --privkey nie je zadany, podpis bude nulový!")
    sig_payload = build_sig_payload(meta_payload, privkey, key_id)

    _hdr_sent = [False]
    def send_header():
        if _hdr_sent[0]:
            return
        print(f"[FOTA] Posielam HEADER META+SIG (size={len(patch)}B, chunks={total}, order={packetorder})...")
        send_pkt(meta_payload)
        time.sleep(chunk_delay)
        send_pkt(sig_payload)
        _hdr_sent[0] = True
        time.sleep(1.2)

    # Kam vložiť HEADER v prvom pokuse (out-of-order test):
    #   normal/hbegin → pred chunkami | hmiddle → po total//2 chunkoch | hend → po všetkých
    if packetorder in ('normal', 'hbegin'):
        try:
            send_header()
        except serial.SerialException as e:
            print(e); reader.stop(); return False
        header_pos = -1
    elif packetorder == 'hmiddle':
        header_pos = total // 2
    else:  # hend
        header_pos = total

    # --- CHUNKS ---
    to_send = list(range(total))
    for attempt in range(nack_retries + 1):
        if not to_send:
            break
        print(f"[FOTA] Posielam {len(to_send)} chunkov (pokus {attempt+1}/{nack_retries+1})...")
        serial_lost = False
        dropped = 0
        for pos, idx in enumerate(to_send):
            # HEADER v strede (hmiddle) — vlož pred chunk na pozícii header_pos
            if attempt == 0 and not _hdr_sent[0] and pos == header_pos:
                try: send_header()
                except serial.SerialException: pass
            # Simulácia straty paketu (test kumulácie naprieč cyklami)
            if drop_prob > 0.0 and random.random() < drop_prob:
                dropped += 1
                continue
            try:
                send_pkt(build_fota_chunk(idx, chunks[idx], old_fw_size, old_sha256_prefix))
            except serial.SerialException as e:
                print(f"\n{e}")
                serial_lost = True
                break
            if (pos + 1) % 10 == 0 or pos == len(to_send) - 1:
                print(f"  → {pos+1}/{len(to_send)} (idx={idx})", end='\r', flush=True)
            time.sleep(chunk_delay)
            # Redundancia HEADER-a: pošli ho znova po každých 'header_every' chunkoch.
            # HEADER je jediný kritický paket (total=0 blokuje všetko) a nemá akumulačnú
            # výhodu ako 4 nezávislé chunky — viac pokusov/kolo zdvíha šancu doručenia
            # (najmä cez slabý/zarušený relay hop).
            if header_every > 0 and (pos + 1) % header_every == 0:
                try:
                    print(f"\n[FOTA] HEADER META+SIG (redundancia, po {pos+1} chunkoch)")
                    send_pkt(meta_payload)
                    time.sleep(chunk_delay)
                    send_pkt(sig_payload)
                    _hdr_sent[0] = True
                    time.sleep(chunk_delay)
                except serial.SerialException:
                    serial_lost = True; break
        print()
        # HEADER na konci (hend) — po odoslaní všetkých chunkov prvého pokusu
        if attempt == 0 and not _hdr_sent[0]:
            try: send_header()
            except serial.SerialException: pass
        if dropped:
            print(f"[FOTA] (simulácia straty: vynechaných {dropped} chunkov)")
        if serial_lost:
            reader.stop()
            return False

        # Čakaj na odpoveď od zariadenia
        print("[FOTA] Čakám na STATUS/NACK (5s)...")
        try:
            resp = resp_queue.get(timeout=5.0)
        except Exception:
            print("  (žiadna odpoveď — predpokladám OK)")
            to_send = []
            break

        if resp[0] == FOTA_PKT_NACK and len(resp) >= 2:
            count = resp[1]
            missing = list(struct.unpack_from(f'<{count}H', resp, 2))
            print(f"[FOTA] NACK: {count} chýba: {missing[:10]}...")
            to_send = missing
        elif resp[0] == FOTA_PKT_STATUS and len(resp) >= 6:
            recv_c, tot_c, status = struct.unpack_from('<HHB', resp, 1)
            print(f"[FOTA] STATUS: {recv_c}/{tot_c}  st=0x{status:02X}")
            if status & FOTA_ST_VERIFIED:
                to_send = []
                break
            elif status & FOTA_ST_ERROR:
                print("[FOTA] CHYBA na zariadení!")
                reader.stop()
                return False
        else:
            to_send = []

    if to_send:
        print(f"[FOTA] Stále chýba {len(to_send)} chunkov!")
        reader.stop()
        return False

    if not do_reboot:
        print("[FOTA] Vsetky chunky odoslane. Ovladaj manualme cez konzolu:")
        print("  f  = LittleFS zoznam suborov")
        print("  o  = OTA stav session")
        print("  Q  = test patch (dry-run SHA256, bez zapisu)")
        print("  e  = execute: flash + reboot")
        reader.stop()
        return True

    # --- APPLY (iba ak --reboot) ---
    print("[FOTA] Posielam APPLY (flash + reboot)...")
    try:
        send_pkt(build_fota_apply(patch_sha256))
    except serial.SerialException as e:
        print(e)
        reader.stop()
        return False
    time.sleep(2.0)
    print("[FOTA] Hotovo — zariadenie sa rebootu je.")
    reader.stop()
    return True

# ─────────────────────────────────────────────────────────────────────
# CLI
# ─────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description='MeshCore FOTA sender')
    ap.add_argument('--old',    required=True, help='Starý firmware .bin')
    ap.add_argument('--new',    required=True, help='Nový firmware .bin')
    ap.add_argument('--port',   required=True, help='Serial port (COM5 / /dev/ttyUSB0)')
    ap.add_argument('--baud',   type=int, default=115200)
    ap.add_argument('--psk',    help='Channel PSK hex (16B=32 znakov, pre mode=meshcore)')
    ap.add_argument('--mode',   choices=['meshcore', 'direct', 'serial-direct'],
                                default='serial-direct',
                                help=('meshcore=cez repeatre (šifrované) | '
                                      'direct=priamy LoRa gateway | '
                                      'serial-direct=USB serial bez LoRa (TEST)'))
    ap.add_argument('--patch',  default='fota_patch.bin', help='Medzisúbor patchu')
    ap.add_argument('--delay',  type=float, default=0.3,
                                help='Pauza medzi chunkmi [s] (serial-direct: môže byť 0.05)')
    ap.add_argument('--nack-retries', type=int, default=3)
    ap.add_argument('--reboot', action='store_true', default=False,
                                help='Po odoslaní chunkov pošli APPLY (flash+reboot). Default: len chunky, bez resetu.')
    ap.add_argument('--cycles', type=int, default=1,
                                help='Koľkokrát opakovať celý broadcast (HEADER+chunky+APPLY). '
                                     'Fire-and-forget model: prijímač si kumuluje chunky naprieč cyklami.')
    ap.add_argument('--drop',   type=float, default=0.0,
                                help='Pravdepodobnosť [0..1] zahodenia chunku (simulácia LoRa straty, test kumulácie).')
    ap.add_argument('--cycle-delay', type=float, default=2.0,
                                help='Pauza medzi cyklami [s].')
    from fota_texts import T
    ap.add_argument('--privkey', help=T('help_privkey'))
    ap.add_argument('--privkey-hex', help=T('help_privkey_hex'))
    ap.add_argument('--keyid',    type=int, default=0, help=T('help_keyid'))
    ap.add_argument('--packetorder', choices=['normal', 'hbegin', 'hmiddle', 'hend'],
                                default='normal',
                                help='Pozícia HEADER paketu (out-of-order test): '
                                     'normal/hbegin=na začiatku | hmiddle=v strede chunkov | '
                                     'hend=na konci po všetkých chunkoch.')
    ap.add_argument('--scope', choices=['flood', 'zerohop', 'region', 'direct'],
                                default='zerohop',
                                help='LoRa šírenie GRP_DATA (--mode meshcore): '
                                     'zerohop=len priami susedia, nikto nerepeatuje (DEFAULT) | '
                                     'flood=každý repeater re-flooduje | '
                                     'region=flood len v zhodnom regióne (--scope-name/--scope-key) | '
                                     'direct=cez menované hopy (--path).')
    ap.add_argument('--scope-name', help='Názov regiónu pre --scope region (key = SHA256(name)[:16]).')
    ap.add_argument('--scope-key',  help='16B hex scope key pre --scope region (alternatíva k --scope-name).')
    ap.add_argument('--path',       help='--scope direct: čiarkou oddelené hex hashe hopov v poradí, '
                                         'napr. 3f,a1 (1B) alebo 3fa1,b2c3 (2B). Veľkosť podľa --path-hashsize.')
    ap.add_argument('--path-hashsize', type=int, choices=[1, 2, 3], default=1,
                                help='Veľkosť path hashu v bajtoch pre --scope direct (default 1).')
    ap.add_argument('--header-every', type=int, default=0,
                                help='Pošli HEADER znova po každých N chunkoch (redundancia pre slabý/relay '
                                     'spoj; HEADER je jediný kritický paket). 0 = vyp (default).')
    args = ap.parse_args()
    if not (0.0 <= args.drop < 1.0):
        print('[CHYBA] --drop musí byť v [0..1)'); sys.exit(1)

    # Validácia
    psk = None
    if args.mode == 'meshcore':
        if not args.psk:
            print('[CHYBA] --psk je povinný pre --mode meshcore')
            sys.exit(1)
        psk = bytes.fromhex(args.psk)
        if len(psk) not in (16, 32):
            print('[CHYBA] PSK musí byť 16 alebo 32 bajtov')
            sys.exit(1)
        print(f'[init] Mode=MeshCore  ch_hash=0x{hashlib.sha256(psk).digest()[0]:02X}')
    elif args.mode == 'direct':
        print('[init] Mode=Direct (LoRa bez šifrovania, cez gateway)')
    else:
        print('[init] Mode=SerialDirect (USB serial, inject do sniffera)')
        if args.delay > 0.1:
            print(f'[init] TIP: pre serial-direct môžeš skúsiť --delay 0.05')

    # Zostav scope (LoRa šírenie) — relevantné len pre --mode meshcore
    scope = Scope('zerohop', None, b'', 1)
    if args.mode == 'meshcore':
        if args.scope in ('flood', 'zerohop'):
            scope = Scope(args.scope, None, b'', 1)
            print(f'[init] scope={args.scope}')
        elif args.scope == 'region':
            if bool(args.scope_name) == bool(args.scope_key):
                print('[CHYBA] --scope region vyžaduje práve jedno z --scope-name / --scope-key'); sys.exit(1)
            if args.scope_name:
                key = hashlib.sha256(args.scope_name.encode()).digest()[:16]
            else:
                key = bytes.fromhex(args.scope_key)
                if len(key) != 16:
                    print('[CHYBA] --scope-key musí byť 16 bajtov (32 hex znakov)'); sys.exit(1)
            scope = Scope('region', key, b'', 1)
            print(f'[init] scope=region name={args.scope_name or "(key)"} ')
        elif args.scope == 'direct':
            if not args.path:
                print('[CHYBA] --scope direct vyžaduje --path'); sys.exit(1)
            hsz = args.path_hashsize
            try:
                hops = [bytes.fromhex(tok.strip()) for tok in args.path.split(',') if tok.strip()]
            except ValueError:
                print('[CHYBA] --path obsahuje neplatný hex'); sys.exit(1)
            if not hops or any(len(h) != hsz for h in hops):
                print(f'[CHYBA] každý hop v --path musí byť {hsz}B (podľa --path-hashsize)'); sys.exit(1)
            if not (1 <= len(hops) <= 63) or len(hops) * hsz > 64:
                print('[CHYBA] --path: počet hopov 1..63 a hop_count*hashsize <= 64'); sys.exit(1)
            scope = Scope('direct', None, b''.join(hops), hsz)
            print(f'[init] scope=direct hops={len(hops)} hashsize={hsz}B')
    elif args.scope != 'zerohop':
        print(f'[init] POZOR: --scope {args.scope} sa ignoruje (platí len pre --mode meshcore)')

    # Generuj patch
    patch, patch_sha256, new_sha256, old_sha256, old_fw_size = \
        make_patch(Path(args.old), Path(args.new), Path(args.patch))
    total = (len(patch) + FOTA_CHUNK_DATA - 1) // FOTA_CHUNK_DATA

    print(f'[init] {total} chunkov x {FOTA_CHUNK_DATA}B = {len(patch)}B patch')

    # Nacitaj Ed25519 private key (hex > der > None s fallbackom na legacy unsigned)
    privkey = None
    if args.privkey_hex:
        privkey = load_ed25519_privkey_hex(args.privkey_hex)
        print(f'[init] Ed25519 privkey-hex (pub prefix {privkey.prefix.hex().upper()}, key_id=0x{args.keyid:02X})')
    elif args.privkey:
        privkey = load_ed25519_privkey(Path(args.privkey))
        print(f'[init] Ed25519 private key: {args.privkey} (pub prefix {privkey.prefix.hex().upper()}, key_id=0x{args.keyid:02X})')
    elif args.keyid == FOTA_KEY_ID_PREFIX:
        args.keyid = 1   # unsigned nejde s prefix formátom -> legacy zero-sig
        from fota_texts import T
        print(T("sign_no_privkey_legacy"))

    # Otvor serial
    print(f'[serial] {args.port} @ {args.baud}')
    if args.cycles > 1:
        print(f'[init] Broadcast {args.cycles}× '
              f'(drop={args.drop:.0%}) — fire-and-forget, prijímač kumuluje chunky')
    ok = False
    with serial.Serial(args.port, args.baud, timeout=0.05) as ser:
        # DTR pri otvorení resetne Adafruit nRF52 bridge → čerstvý boot rádia.
        # Daj mu čas nabehnúť (radio.begin ~2s), inak sa stratí HEADER.
        time.sleep(2.5)
        ser.reset_input_buffer()
        for cyc in range(args.cycles):
            if args.cycles > 1:
                print(f'\n========== CYKLUS {cyc+1}/{args.cycles} ==========')
            ok = send_fota(ser, patch, patch_sha256, new_sha256, old_sha256, old_fw_size,
                          psk, args.mode, args.delay, args.nack_retries, args.reboot,
                          drop_prob=args.drop,
                          privkey=privkey, key_id=args.keyid, packetorder=args.packetorder,
                          scope=scope, header_every=args.header_every)
            if not ok:
                print('[FOTA] cyklus zlyhal (serial?) — končím')
                break
            if cyc < args.cycles - 1:
                time.sleep(args.cycle_delay)

    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()
