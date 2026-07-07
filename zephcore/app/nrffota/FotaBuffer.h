#pragma once
// =====================================================================
//en: FotaBuffer.h — shared scratch buffer for FOTA (borrow / release).
//sk: FotaBuffer.h — zdieľaný scratch buffer pre FOTA (borrow / release).
//
//en: Purpose: separate "WHERE the memory comes from" from "HOW it is used".
//en: Today it returns a pointer to one static buffer in .bss — call-sites
//en: (e.g. the hpatch cache in fota_patch_to_file) just borrow/return it.
//en: If we ever want malloc/free (or to recycle another existing buffer),
//en: the change is ONLY here + in the .cpp; call-sites stay untouched.
//sk: Účel: oddeliť "ODKIAĽ" je pamäť od "AKO sa používa". Dnes vracia smerník
//sk: na jeden statický buffer v .bss — call-sites (napr. hpatch cache vo
//sk: fota_patch_to_file) ho len požičajú/vrátia. Ak raz budeme chcieť malloc/
//sk: free (alebo recyklovať iný existujúci buffer), zmena je IBA tu + v .cpp;
//sk: volajúce miesta sa nemenia.
//
//en: NOTE: FOTA operations are not reentrant — the buffer is borrowed at
//en: most once at any time. A double borrow is a bug (get returns NULL).
//sk: POZOR: FOTA operácie nie sú reentrantné — v jednom okamihu je buffer
//sk: požičaný max. raz. Dvojitá výpožička = bug (get vráti NULL).
// =====================================================================
#include <stdint.h>

//en: Capacity of the shared scratch [B]. Sized for the largest consumer:
//en: hpatch_lite_patch() read-cache in verify (required minimum =
//en: hpi_kMinCacheSize = 2 B; 512 = comfortable margin for dry-run
//en: throughput). The flasher has its own ~20 kB blob in the .bss of ITS
//en: relocatable image — unrelated to this.
//sk: Kapacita zdieľaného scratchu [B]. Dimenzované na najväčšieho konzumenta:
//sk: hpatch_lite_patch() read-cache vo verify (min. nutné = hpi_kMinCacheSize = 2 B;
//sk: 512 = pohodlná rezerva pre throughput dry-runu). Flasher má vlastný ~20 kB
//sk: blob v .bss SVOJHO relokovateľného obrazu — s týmto NESÚVISÍ.
#define FOTA_BUF_CAP   512u

//en: Borrow a scratch with capacity >= need bytes.
//en: Returns a pointer, or NULL if: need > FOTA_BUF_CAP, or the buffer is
//en: already borrowed. When done, ALWAYS call fota_put_buffer().
//sk: Požičaj scratch s kapacitou >= need bajtov.
//sk: Vráti smerník, alebo NULL ak: need > FOTA_BUF_CAP, alebo je buffer už
//sk: požičaný. Po dokončení práce VŽDY zavolaj fota_put_buffer().
uint8_t* fota_get_buffer(uint32_t need);

//en: Return a scratch borrowed via fota_get_buffer(). 'p' must be exactly
//en: the pointer that get returned (otherwise the call is ignored).
//sk: Vráť scratch požičaný cez fota_get_buffer(). 'p' musí byť presne ten
//sk: smerník, ktorý get vrátil (inak sa volanie ignoruje).
void fota_put_buffer(uint8_t* p);
