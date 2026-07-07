import sys, struct
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import fota_sender_mcpy as M

def test_chan_data_frame_zerohop():
    data = struct.pack('<I', 0x11223344) + b"\x10payload"
    f = M.companion_chan_data_frame(channel_idx=1, path_len=0, path=b"", data_type=0x07A0, data=data)
    assert f[0] == 62 and f[1] == 1 and f[2] == 0
    assert f[3:5] == struct.pack('<H', 0x07A0)
    assert f[5:] == data

def test_scope_to_path():
    assert M.scope_to_path("zerohop") == (0, b"")
    assert M.scope_to_path("flood") == (0xFF, b"")
    assert M.scope_to_path("direct", b"\x63\x68") == (2, b"\x63\x68")
