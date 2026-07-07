import hashlib, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import fota_sender as S

def test_constants():
    assert S.FOTA_MAGIC == 0x07A0
    assert S.FOTA_PKT_HDR_SIG == 0x13
    assert S.FOTA_CHUNK_DATA == 144
    assert S.FOTA_CHANNEL_NAME == "#fkotanrf"

def test_channel_secret_matches_known():
    assert S.fota_channel_secret().hex() == "2382c5b811d390667e6a7800c03338ca"

def test_meta_payload_layout():
    ps = hashlib.sha256(b"p").digest(); ns = hashlib.sha256(b"n").digest(); os_ = hashlib.sha256(b"o").digest()
    meta = S.build_meta_payload(7, 1234, ps, ns, os_)
    assert len(meta) == 102
    assert meta[0] == S.FOTA_PKT_HEADER and meta[1] == S.FOTA_PROT_INF_V0
    assert int.from_bytes(meta[2:6], "little") == 1234
    assert meta[6:38] == ps and meta[38:70] == ns and meta[70:102] == os_
    assert 4 + len(meta) <= 165   # data_len limit

def test_sig_payload_layout_and_verify():
    from Crypto.PublicKey import ECC
    from Crypto.Signature import eddsa
    key = ECC.generate(curve="ed25519")
    ps = hashlib.sha256(b"p").digest(); ns = hashlib.sha256(b"n").digest(); os_ = hashlib.sha256(b"o").digest()
    meta = S.build_meta_payload(7, 1234, ps, ns, os_)
    sig = S.build_sig_payload(meta, key, key_id=1)
    assert len(sig) == 99 and 4 + len(sig) <= 165
    assert sig[0] == S.FOTA_PKT_HDR_SIG and sig[1] == S.FOTA_PROT_INF_V0
    assert sig[2:34] == os_ and sig[34] == 1
    eddsa.new(key.public_key(), "rfc8032").verify(meta, sig[35:99])  # raises on bad
