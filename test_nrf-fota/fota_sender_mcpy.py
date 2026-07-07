#!/usr/bin/env python3
"""fota_sender_mcpy.py — FOTA sender cez MeshCore companion (meshcore_py, serial).

Companion (Xiao_nrf52_companion_radio_usb na COM3) šifruje a smeruje sám cez
CMD_SEND_CHANNEL_DATA. Tento sender NEROBÍ AES/HMAC — len zostaví FOTA payload
(META/SIG/chunk) a pošle ho ako GRP_DATA `data = [ts4][fota_payload]`.

Závislosti:
    pip install meshcore pycryptodome

Použitie:
    python fota_sender_mcpy.py --old old.bin --new new.bin --port COM3 \
          --channel-name "#fkotanrf" --channel-idx 1 --scope zerohop \
          --privkey test_nrf-fota/test_key.der --keyid 1 --reboot
"""
import argparse, asyncio, struct, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fota_sender as S
from fota_sender import (make_patch, build_meta_payload, build_sig_payload,
                        build_fota_chunk, build_fota_apply, load_ed25519_privkey,
                        FOTA_MAGIC, FOTA_CHUNK_DATA, FOTA_CHANNEL_NAME)

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

CMD_SEND_CHANNEL_DATA = 62
OUT_PATH_FLOOD        = 0xFF


def companion_chan_data_frame(channel_idx, path_len, path, data_type, data):
    """Companion serial frame pre CMD_SEND_CHANNEL_DATA:
    [62][channel_idx][path_len][path][data_type 2B LE][data]."""
    return (bytes([CMD_SEND_CHANNEL_DATA, channel_idx & 0xFF, path_len & 0xFF])
            + path + struct.pack('<H', data_type) + data)


def scope_to_path(scope, path_bytes=b""):
    """Scope → (path_len, path) pre sendGroupData: zerohop=0, flood=0xFF, direct=N hashov."""
    if scope == "zerohop":
        return (0, b"")
    if scope == "flood":
        return (OUT_PATH_FLOOD, b"")
    if scope == "direct":
        return (len(path_bytes), path_bytes)   # 1B hashe (path_hashsize=1)
    raise ValueError(f"scope {scope} nepodporovaný cez companion (zatiaľ)")


async def _send_data(mc, channel_idx, path_len, path, fota_payload, ts):
    from meshcore.events import EventType
    data = struct.pack('<I', ts & 0xFFFFFFFF) + fota_payload
    if len(data) > 165:
        raise ValueError(f"data_len {len(data)} > 165 (GRP_DATA limit)")
    frame = companion_chan_data_frame(channel_idx, path_len, path, FOTA_MAGIC, data)
    res = await mc.commands.send(frame, [EventType.OK, EventType.ERROR])
    if res is not None and res.type == EventType.ERROR:
        print(f"[mcpy] WARN companion ERROR: {getattr(res, 'payload', None)}")
    return res


async def run_mcpy(args):
    from meshcore import MeshCore

    patch, patch_sha256, new_sha256, old_sha256, old_fw_size = \
        make_patch(Path(args.old), Path(args.new), Path(args.patch))
    chunks = [patch[i:i+FOTA_CHUNK_DATA] for i in range(0, len(patch), FOTA_CHUNK_DATA)]
    total = len(chunks)
    old_prefix = old_sha256[:4]
    privkey = load_ed25519_privkey(Path(args.privkey)) if args.privkey else None
    meta = build_meta_payload(total, len(patch), patch_sha256, new_sha256, old_sha256)
    sig  = build_sig_payload(meta, privkey, args.keyid)
    path_len, path = scope_to_path(args.scope, bytes.fromhex(args.path) if args.path else b"")

    print(f"[mcpy] {len(patch)}B -> {total} chunkov x {FOTA_CHUNK_DATA}B; scope={args.scope} path_len={path_len}")
    if not privkey:
        print("[mcpy] WARNING: --privkey nezadaný — podpis nulový (repeater odmietne, ak nemá FOTA_ALLOW_UNSIGNED)")

    mc = await MeshCore.create_serial(args.port, args.baud)
    if mc is None:
        sys.exit("[CHYBA] companion neodpovedá na COM porte (je to serial companion?)")
    await mc.commands.set_radio(args.freq, args.bw, args.sf, args.cr)
    await mc.commands.set_channel(args.channel_idx, args.channel_name)
    print(f"[mcpy] radio={args.freq}/{args.bw}/SF{args.sf}/CR{args.cr}  kanal[{args.channel_idx}]={args.channel_name}")

    ts = int(time.time())

    async def snd(payload, label=""):
        nonlocal ts
        ts += 1                       # rastúci ts → každý paket unikátny (anti-dedup)
        await _send_data(mc, args.channel_idx, path_len, path, payload, ts)
        if label:
            print(f"[mcpy] {label}")
        await asyncio.sleep(args.delay)

    async def send_hdr():
        await snd(meta, "META")
        await snd(sig, "SIG")

    if args.packetorder in ("normal", "hbegin"):
        await send_hdr()
    for idx in range(total):
        await snd(build_fota_chunk(idx, chunks[idx], old_fw_size, old_prefix))
        if (idx + 1) % 10 == 0 or idx == total - 1:
            print(f"[mcpy]   chunk {idx+1}/{total}")
    if args.packetorder == "hend":
        await send_hdr()
    if args.reboot:
        await snd(build_fota_apply(patch_sha256), "APPLY")

    await mc.disconnect()
    print("[mcpy] hotovo")


def main():
    ap = argparse.ArgumentParser(description="FOTA sender cez MeshCore companion (meshcore_py)")
    ap.add_argument('--old', required=True)
    ap.add_argument('--new', required=True)
    ap.add_argument('--port', default="COM3")
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--patch', default='fota_patch.bin')
    ap.add_argument('--channel-name', default=FOTA_CHANNEL_NAME)
    ap.add_argument('--channel-idx', type=int, default=1)
    ap.add_argument('--scope', choices=['zerohop', 'flood', 'direct'], default='zerohop')
    ap.add_argument('--path', help='direct: hex hashe hopov (1B), napr. 6368')
    ap.add_argument('--delay', type=float, default=0.3)
    ap.add_argument('--packetorder', choices=['normal', 'hbegin', 'hend'], default='hend')
    ap.add_argument('--reboot', action='store_true')
    ap.add_argument('--privkey')
    ap.add_argument('--keyid', type=int, default=1)
    # POZOR na defaulty rádia: 869.618/62.5/SF8 = náš FK pracovný kanál. FOTA FW envy
    # (ProMicro/SenseCap *_fota) a e2e test ale stavajú CZ test preset 869.525/62.5/SF7
    # — pri ručnom použití zadaj --freq/--sf explicitne podľa repeatera, inak sa minú.
    ap.add_argument('--freq', type=float, default=869.618)
    ap.add_argument('--bw', type=float, default=62.5)
    ap.add_argument('--sf', type=int, default=8)
    ap.add_argument('--cr', type=int, default=5)
    args = ap.parse_args()
    asyncio.run(run_mcpy(args))


if __name__ == '__main__':
    main()
