#pragma once
// =====================================================================
//en: FotaFs.h — FOTA filesystem glue (platform shim, dual-guarded).
//en:
//en: MESHCORE: dedicated CustomLFS at 0xD4000 (92kB) — separate from MeshCore
//en: InternalFS (which stays at the Adafruit default 0xED000-0xF4000 for
//en: identity/prefs/ACL). Same approach as companion_radio.
//en: ZEPHCORE: shared /lfs LittleFS partition (0xD4000, 128kB, automounted from
//en: DTS fstab) — FOTA files live under /lfs/fota/ (see FotaState.h paths).
//en:
//en: FotaFile mimics the Adafruit_LittleFS File subset the FOTA code uses
//en: (open/read/write/seek/size/close; FILE_O_WRITE = append semantics).
//en:
//en: Shared file — keep byte-identical between MeshCore and ZephCore
//en: (tools/fota_sync.py in ZephCore). Port of FK_lora-sniffer/src/fota_fs.h.
//sk: FotaFs.h — FOTA filesystem glue (platformový shim, duálne guardy).
//sk:
//sk: MESHCORE: dedikovaný CustomLFS na 0xD4000 (92kB) — oddelený od MeshCore
//sk: InternalFS (ten ostáva na Adafruit defaulte 0xED000-0xF4000 pre
//sk: identity/prefs/ACL). Rovnaký prístup ako companion_radio.
//sk: ZEPHCORE: zdieľaná /lfs LittleFS partícia (0xD4000, 128kB, automount z
//sk: DTS fstab) — FOTA súbory žijú pod /lfs/fota/ (cesty viď FotaState.h).
//sk:
//sk: FotaFile napodobňuje podmnožinu Adafruit_LittleFS File API, ktorú FOTA
//sk: kód používa (open/read/write/seek/size/close; FILE_O_WRITE = append).
//sk:
//sk: Zdieľaný súbor — drž byte-identický medzi MeshCore a ZephCore
//sk: (tools/fota_sync.py v ZephCore). Port z FK_lora-sniffer/src/fota_fs.h.
// =====================================================================

//en: Flash addresses (per-board, freestanding-safe) — single source
#include "flash_layout.h"

#if defined(FOTA_MESHCORE_BUILD)

#include <CustomLFS.h>
using namespace Adafruit_LittleFS_Namespace;

//en: Arduino: FotaFile is the Adafruit File itself (constructor File(FotaFS))
//sk: Arduino: FotaFile je priamo Adafruit File (konštruktor File(FotaFS))
#define FotaFile File

//en: Global instance — defined in FotaReceiver.cpp, extern elsewhere
extern CustomLFS FotaFS;

#elif defined(FOTA_ZEPHCORE_BUILD)

#include <zephyr/fs/fs.h>
#include <stdint.h>
#include <string.h>

//en: Adafruit-compatible open mode flags
#define FILE_O_READ  1
#define FILE_O_WRITE 2

//en: /lfs is automounted from the DTS fstab; begin() just ensures /lfs/fota exists.
//en: format() is a NO-OP — /lfs is shared with identity/prefs, FOTA must never
//en: format it; 'fota clear' removes individual files instead.
//sk: /lfs automountuje DTS fstab; begin() len zaistí existenciu /lfs/fota.
//sk: format() je NO-OP — /lfs je zdieľaný s identity/prefs, FOTA ho nesmie
//sk: formátovať; 'fota clear' maže jednotlivé súbory.
class FotaFsClass {
public:
    bool begin() { fs_mkdir(FOTA_FS_DIR_MK); return true; }   //en: idempotent (-EEXIST ok)
    void end() {}      //en: /lfs stays mounted (shared); the flasher never touches it
    void format() {}
    bool remove(const char* path) { return fs_unlink(path) == 0; }
private:
    //en: FOTA_FS_DIR from FotaState.h is not visible here (include order) — keep literal
    static constexpr const char* FOTA_FS_DIR_MK = "/lfs/fota";
};

extern FotaFsClass FotaFS;

class FotaFile {
    struct fs_file_t _f;
    bool _open = false;
public:
    explicit FotaFile(FotaFsClass&) { fs_file_t_init(&_f); }
    ~FotaFile() { close(); }
    bool open(const char* path, int mode) {
        fs_mode_t flags = (mode & FILE_O_WRITE) ? (fs_mode_t)(FS_O_CREATE | FS_O_RDWR)
                                                : (fs_mode_t)FS_O_READ;
        _open = (fs_open(&_f, path, flags) == 0);
        //en: Adafruit FILE_O_WRITE semantics = append (recv.log relies on it)
        //sk: Adafruit FILE_O_WRITE sémantika = append (spolieha sa na to recv.log)
        if (_open && (mode & FILE_O_WRITE)) fs_seek(&_f, 0, FS_SEEK_END);
        return _open;
    }
    operator bool() const { return _open; }
    int read(uint8_t* buf, size_t n) { return _open ? (int)fs_read(&_f, buf, n) : -1; }
    int read(void* buf, size_t n) { return read((uint8_t*)buf, n); }
    int write(const uint8_t* buf, size_t n) { return _open ? (int)fs_write(&_f, buf, n) : -1; }
    bool seek(uint32_t pos) { return _open && fs_seek(&_f, (off_t)pos, FS_SEEK_SET) == 0; }
    uint32_t size() {
        if (!_open) return 0;
        off_t cur = fs_tell(&_f);
        fs_seek(&_f, 0, FS_SEEK_END);
        off_t end = fs_tell(&_f);
        fs_seek(&_f, cur, FS_SEEK_SET);
        return (uint32_t)end;
    }
    void close() { if (_open) { fs_close(&_f); _open = false; } }
};

#else
  #error "FotaFs.h: define FOTA_MESHCORE_BUILD or FOTA_ZEPHCORE_BUILD"
#endif

#define FLASHER_META_MAGIC  0x464C5348u      //en: "FLSH"

//en: Flasher metadata (written before the jump, read by the flasher from FLASHER_META_ADDR)
typedef struct __attribute__((packed)) {
    uint32_t magic;          //en: FLASHER_META_MAGIC
    uint32_t src_addr;       //en: source address in RAM (new FW buffer)
    uint32_t fw_size;        //en: size of the new FW in bytes
    uint32_t dst_addr;       //en: destination flash address (APP_FLASH_START)
    uint8_t  new_sha256[32]; //en: SHA256 for verification after the write
} FlasherMeta;
