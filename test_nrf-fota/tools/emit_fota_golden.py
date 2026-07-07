#!/usr/bin/env python3
"""Emit byte-exact FOTA golden vectors for the Flutter Dart tests.
Uses FIXED synthetic inputs (no hdiffi needed). Run from MeshCore repo root:
    <penv>/python.exe test_nrf-fota/tools/emit_fota_golden.py <out_json>
"""
import json, struct, sys, hashlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # test_nrf-fota/
import fota_sender as S
from Crypto.PublicKey import ECC

SEED = bytes(range(32))                      # deterministic 00 01 02 … 1f
key = ECC.construct(curve='Ed25519', seed=SEED)
key_id = 1

patch = bytes((i * 7) % 256 for i in range(1234))      # synthetic staged patch
patch_size = len(patch)
patch_sha256 = hashlib.sha256(patch).digest()
new_sha256 = hashlib.sha256(b'NEW').digest()
old_sha256 = hashlib.sha256(b'OLD').digest()
old_fw_size = 442000
chunk_idx = 2
chunk_data = patch[chunk_idx*S.FOTA_CHUNK_DATA:(chunk_idx+1)*S.FOTA_CHUNK_DATA]
ts = 1000000
channel_idx = 1

meta = S.build_meta_payload(0, patch_size, patch_sha256, new_sha256, old_sha256)
sig = S.build_sig_payload(meta, key, key_id)
chunk = S.build_fota_chunk(chunk_idx, chunk_data, old_fw_size, old_sha256[:4])
apply = S.build_fota_apply(patch_sha256)

# full CMD_SEND_CHANNEL_DATA frame for META, zerohop, idx=1 (mirror fota_sender_mcpy)
data = struct.pack('<I', ts) + meta
frame = bytes([62, channel_idx, 0]) + struct.pack('<H', S.FOTA_MAGIC) + data

out = {
  "seed_hex": SEED.hex(),
  "inputs": {"patch_size": patch_size, "patch_sha256": patch_sha256.hex(),
             "new_sha256": new_sha256.hex(), "old_sha256": old_sha256.hex(),
             "key_id": key_id, "old_fw_size": old_fw_size, "chunk_idx": chunk_idx,
             "chunk_data_hex": chunk_data.hex(), "ts": ts, "channel_idx": channel_idx,
             "patch_hex": patch.hex()},
  "meta_hex": meta.hex(), "sig_hex": sig.hex(), "chunk_hex": chunk.hex(),
  "apply_hex": apply.hex(), "crc16_of_chunk_data": S.crc16(chunk_data),
  "channel_data_frame_hex": frame.hex(),
}
Path(sys.argv[1]).write_text(json.dumps(out, indent=2))
print(f"wrote {sys.argv[1]}: meta={len(meta)}B sig={len(sig)}B chunk={len(chunk)}B apply={len(apply)}B")
