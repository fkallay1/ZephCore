#!/usr/bin/env python3
"""Export a .fotapkg.json for mc_fotanrf_flutterapp (phase A).
Reuses fota_sender.make_patch / build_meta_payload / build_sig_payload."""
import argparse, base64, json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fota_sender as S
from fota_sender import (make_patch, build_meta_payload, build_sig_payload,
                        load_ed25519_privkey, FOTA_CHANNEL_NAME, FOTA_CHUNK_DATA)

def build_pkg(old, new, patch_path, *, channel_name=FOTA_CHANNEL_NAME, channel_idx=1,
              freq=869.618, bw=62.5, sf=8, cr=5, scope='zerohop', path='',
              privkey=None, privkey_hex=None, keyid=0, created="1970-01-01T00:00:00Z"):
    """Zostaví .fotapkg dict (old→new delta patch). privkey (cesta .der) alebo
    privkey_hex (128 hex, companion formát) → pridá 'signed' blok. keyid=0 =
    v0-prefix (nový formát, +signer_prefix), >=1 = legacy pre staré FW.
    Reuse-uje fota_sender.make_patch / build_meta_payload / build_sig_payload."""
    patch, patch_sha256, new_sha256, old_sha256, old_fw_size = \
        make_patch(Path(old), Path(new), Path(patch_path))
    pkg = {
        "format": "mc-fotanrf-fotapkg/1",
        "created": created,
        "channel": {"name": channel_name, "idx": channel_idx},
        "radio": {"freq": freq, "bw": bw, "sf": sf, "cr": cr},
        "scope": scope, "path": path,
        "fw": {"old_sha256": old_sha256.hex(), "new_sha256": new_sha256.hex(),
               "old_fw_size": old_fw_size, "patch_sha256": patch_sha256.hex(),
               "patch_len": len(patch)},
        "patch_b64": base64.b64encode(patch).decode(),
    }
    if privkey or privkey_hex:
        if privkey_hex:
            from fota_ed25519_expanded import key_from_hex
            pk = key_from_hex(privkey_hex)
        else:
            pk = load_ed25519_privkey(Path(privkey))
        total = (len(patch) + FOTA_CHUNK_DATA - 1) // FOTA_CHUNK_DATA
        meta = build_meta_payload(total, len(patch), patch_sha256, new_sha256, old_sha256)
        sig = build_sig_payload(meta, pk, keyid)
        pkg["signed"] = {"key_id": keyid,
                         "signer_prefix": pk.prefix.hex(),
                         "meta_b64": base64.b64encode(meta).decode(),
                         "sig_b64": base64.b64encode(sig).decode()}
    return pkg


def main():
    ap = argparse.ArgumentParser(description="Export .fotapkg.json for the Flutter FOTA app")
    ap.add_argument('--old', required=True); ap.add_argument('--new', required=True)
    ap.add_argument('--patch', default='fota_patch.bin')
    ap.add_argument('--out', required=True)
    ap.add_argument('--channel-name', default=FOTA_CHANNEL_NAME)
    ap.add_argument('--channel-idx', type=int, default=1)
    ap.add_argument('--scope', choices=['zerohop','flood','direct'], default='zerohop')
    ap.add_argument('--path', default='')
    # POZOR na defaulty rádia (idú DO .fotapkg.json → Flutter appka ich prevezme):
    # 869.618/62.5/SF8 = náš FK pracovný kanál; FOTA FW envy + e2e test = CZ 869.525/SF7.
    ap.add_argument('--freq', type=float, default=869.618); ap.add_argument('--bw', type=float, default=62.5)
    ap.add_argument('--sf', type=int, default=8); ap.add_argument('--cr', type=int, default=5)
    ap.add_argument('--privkey'); ap.add_argument('--privkey-hex')
    ap.add_argument('--keyid', type=int, default=0)
    args = ap.parse_args()

    pkg = build_pkg(args.old, args.new, args.patch, channel_name=args.channel_name,
                    channel_idx=args.channel_idx, freq=args.freq, bw=args.bw, sf=args.sf,
                    cr=args.cr, scope=args.scope, path=args.path,
                    privkey=args.privkey, privkey_hex=args.privkey_hex, keyid=args.keyid)
    Path(args.out).write_text(json.dumps(pkg, indent=2))
    print(f"[export] {args.out}: patch={pkg['fw']['patch_len']}B signed={'signed' in pkg}")

if __name__ == '__main__':
    main()
