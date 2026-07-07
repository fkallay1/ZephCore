import base64, json, hashlib, subprocess, sys, struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def test_export_pkg_roundtrip(tmp_path):
    old = tmp_path / "old.bin"; new = tmp_path / "new.bin"
    old.write_bytes(bytes(4096)); new.write_bytes(bytes(2048) + b"\x01" * 2048)
    out = tmp_path / "fw.fotapkg.json"
    r = subprocess.run([sys.executable, str(ROOT / "fota_export_pkg.py"),
        "--old", str(old), "--new", str(new), "--out", str(out)], capture_output=True)
    assert r.returncode == 0, r.stderr.decode()
    pkg = json.loads(out.read_text())
    assert pkg["format"] == "mc-fotanrf-fotapkg/1"
    patch = base64.b64decode(pkg["patch_b64"])
    assert pkg["fw"]["patch_len"] == len(patch)
    assert pkg["fw"]["patch_sha256"] == hashlib.sha256(patch).hexdigest()
    assert "signed" not in pkg  # no --privkey
