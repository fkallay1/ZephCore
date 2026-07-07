#!/usr/bin/env python3
"""
push_fotapkg.py — vyber viacero .fotapkg.json a pošli ich do telefónu (adb push).

Otvorí natívny Windows súborový dialóg (multi-select) defaultne v fotapkg_json/,
vybrané balíky pushne na /sdcard/Download/ pripojeného Android zariadenia.
Spúšťa sa cez push_fotapkg.bat (double-click), netreba zadávať cesty.

Bez závislostí: file dialog + message box cez ctypes (Win32 comdlg32/user32),
takže beží aj v PlatformIO pythone, ktorý nemá tkinter.
"""
import ctypes
import subprocess
import sys
from ctypes import wintypes
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_DIR = HERE / "fotapkg_json"
DEST = "/sdcard/Download/"

# adb: najprv konkrétna známa cesta (scrcpy), inak fallback na PATH ("adb").
ADB_CANDIDATES = [
    r"D:\FkDev\00_Downloads\scrcpy-win64-v4.0\adb.exe",
    "adb",
]

# --- Win32 message box ---
MB_OK = 0x0
MB_ICONERROR = 0x10
MB_ICONWARNING = 0x30
MB_ICONINFO = 0x40


def msgbox(title: str, text: str, icon: int = MB_ICONINFO) -> None:
    ctypes.windll.user32.MessageBoxW(0, text, title, MB_OK | icon)


# --- Win32 GetOpenFileName (multi-select) ---
OFN_ALLOWMULTISELECT = 0x00000200
OFN_EXPLORER = 0x00080000
OFN_FILEMUSTEXIST = 0x00001000
OFN_PATHMUSTEXIST = 0x00000800
OFN_HIDEREADONLY = 0x00000004
OFN_NOCHANGEDIR = 0x00000008


class OPENFILENAMEW(ctypes.Structure):
    _fields_ = [
        ("lStructSize", wintypes.DWORD),
        ("hwndOwner", wintypes.HWND),
        ("hInstance", wintypes.HINSTANCE),
        ("lpstrFilter", wintypes.LPCWSTR),
        ("lpstrCustomFilter", wintypes.LPWSTR),
        ("nMaxCustFilter", wintypes.DWORD),
        ("nFilterIndex", wintypes.DWORD),
        ("lpstrFile", wintypes.LPWSTR),
        ("nMaxFile", wintypes.DWORD),
        ("lpstrFileTitle", wintypes.LPWSTR),
        ("nMaxFileTitle", wintypes.DWORD),
        ("lpstrInitialDir", wintypes.LPCWSTR),
        ("lpstrTitle", wintypes.LPCWSTR),
        ("Flags", wintypes.DWORD),
        ("nFileOffset", wintypes.WORD),
        ("nFileExtension", wintypes.WORD),
        ("lpstrDefExt", wintypes.LPCWSTR),
        ("lCustData", wintypes.LPARAM),
        ("lpfnHook", wintypes.LPVOID),
        ("lpTemplateName", wintypes.LPCWSTR),
        ("pvReserved", wintypes.LPVOID),
        ("dwReserved", wintypes.DWORD),
        ("FlagsEx", wintypes.DWORD),
    ]


def pick_files(initial_dir: str) -> list[str]:
    """Natívny multi-select dialóg → zoznam absolútnych ciest (prázdny = zrušené)."""
    buf_len = 1 << 16  # 64K znakov: pohodlne pre desiatky súborov
    buf = ctypes.create_unicode_buffer(buf_len)
    flt = "FOTA balíky\0*.fotapkg.json\0JSON\0*.json\0Všetko\0*.*\0\0"

    ofn = OPENFILENAMEW()
    ofn.lStructSize = ctypes.sizeof(OPENFILENAMEW)
    ofn.lpstrFilter = flt
    ofn.lpstrFile = ctypes.cast(buf, wintypes.LPWSTR)
    ofn.nMaxFile = buf_len
    ofn.lpstrInitialDir = initial_dir
    ofn.lpstrTitle = "Vyber .fotapkg.json balíky na poslanie do telefónu"
    ofn.Flags = (OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST |
                 OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR)

    if not ctypes.windll.comdlg32.GetOpenFileNameW(ctypes.byref(ofn)):
        return []  # zrušené (alebo chyba)

    # Multi-select výsledok: "adresar\0subor1\0subor2\0\0".
    # Jeden súbor: buffer obsahuje rovno plnú cestu (bez vnútorných \0).
    parts = buf[:].split("\0")
    parts = [p for p in parts if p]
    if not parts:
        return []
    if len(parts) == 1:
        return parts
    base = parts[0]
    return [str(Path(base) / name) for name in parts[1:]]


def find_adb() -> str:
    for cand in ADB_CANDIDATES:
        if cand == "adb" or Path(cand).exists():
            try:
                subprocess.run([cand, "version"], capture_output=True, check=True)
                return cand
            except (OSError, subprocess.CalledProcessError):
                continue
    return ""


def main() -> int:
    adb = find_adb()
    if not adb:
        msgbox("adb", "adb sa nenašiel (ani scrcpy cesta, ani v PATH).", MB_ICONERROR)
        return 1

    out = subprocess.run([adb, "devices"], capture_output=True, text=True).stdout
    devices = [l.split("\t")[0] for l in out.splitlines()[1:] if l.strip().endswith("\tdevice")]
    if not devices:
        msgbox("adb", "Žiadne pripojené zariadenie (adb devices je prázdne).\n"
                      "Pripoj telefón a povoľ USB ladenie.", MB_ICONERROR)
        return 1

    initial = str(DEFAULT_DIR if DEFAULT_DIR.is_dir() else HERE)
    files = pick_files(initial)
    if not files:
        return 0

    results = []
    ok = 0
    for f in files:
        name = Path(f).name
        r = subprocess.run([adb, "push", f, DEST], capture_output=True, text=True)
        if r.returncode == 0:
            ok += 1
            results.append(f"OK    {name}")
        else:
            err = (r.stderr or r.stdout).strip().splitlines()
            results.append(f"CHYBA {name}: {err[-1] if err else '?'}")

    summary = (f"Zariadenie: {devices[0]}\nCieľ: {DEST}\n"
               f"Odoslané: {ok}/{len(files)}\n\n" + "\n".join(results))
    msgbox("Hotovo" if ok == len(files) else "Dokončené s chybami",
           summary, MB_ICONINFO if ok == len(files) else MB_ICONWARNING)
    return 0 if ok == len(files) else 1


if __name__ == "__main__":
    sys.exit(main())
