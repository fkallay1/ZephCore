#pragma once
// =====================================================================
//en: FotaRepeaterMesh.h — FOTA member variables and method declarations of
//en: the RepeaterMesh class (ZephCore port of nrffota/FotaMyMesh.h).
//en:
//en: WARNING: do NOT include standalone! This file is pasted INSIDE the body
//en: of `class RepeaterMesh { ... }` in app/RepeaterMesh.h (protected
//en: section) — it contains raw member declarations without a class wrapper.
//en: Purpose: a single guarded #include hook in RepeaterMesh.h instead of a
//en: ~38-line block (minimal diff vs stock). Method bodies:
//en: FotaRepeaterMesh.cpp. Everything gated by WITH_LORA_FOTA at the
//en: include site.
//sk: FotaRepeaterMesh.h — FOTA členy a deklarácie metód triedy RepeaterMesh
//sk: (ZephCore port nrffota/FotaMyMesh.h).
//sk:
//sk: POZOR: NEinclude-ovať samostatne! Tento súbor sa vkladá VNÚTRI tela
//sk: `class RepeaterMesh { ... }` v app/RepeaterMesh.h (protected sekcia) —
//sk: obsahuje surové member deklarácie bez obalu triedy. Zmysel: jediný
//sk: guardovaný #include hook v RepeaterMesh.h namiesto ~38-riadkového bloku
//sk: (minimálny diff vs stock). Telá metód: FotaRepeaterMesh.cpp. Všetko
//sk: gated WITH_LORA_FOTA na mieste include.
//
//en: Key designs preserved from the Arduino port (details at definitions):
//en: deferred handling of both packets and CLI (heavy FS I/O runs from
//en: loop() after the radio is re-armed), deferred flash (the ACK must
//en: leave the outbound queue before the reboot).
//sk: Kľúčové návrhy zachované z Arduino portu (detaily pri definíciách):
//sk: deferred spracovanie paketov aj CLI (ťažké FS I/O beží až z loop() po
//sk: re-arme rádia), deferred flash (ACK musí odísť z outbound queue pred
//sk: rebootom).
// =====================================================================

  mesh::GroupChannel _fota_channel;
  bool _fota_ready;
  uint8_t _fota_pending[MAX_PACKET_PAYLOAD];  //en: deferred FOTA packet for loop()
  int     _fota_pending_len;                  //en: 0 = nothing pending
  float   _fota_pending_rssi, _fota_pending_snr;
  volatile uint32_t _fota_raw_rx;             //en: RAW frames received (before decode/decrypt)
  volatile uint32_t _fota_raw_tx;             //en: RAW frames sent (handed to the radio)
  uint32_t          _fota_raw_last_len;
  float             _fota_raw_last_rssi, _fota_raw_last_snr;
  bool     _fota_cli_pending;                 //en: deferred LoRa CLI command
  uint8_t* _fota_cli_buf;                     //en: borrowed FotaBuffer with the snapshot
  unsigned long _fota_apply_deadline;         //en: safety net for the deferred flash
  int  searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) override;
  void onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel, uint8_t* data, size_t len) override;
  void logTxRaw(const uint8_t raw[], int len) override;
  void fotaLogRxRaw(float snr, float rssi, const uint8_t raw[], int len);
  void fotaLogTxRaw(const uint8_t raw[], int len);
  void fotaEarlyInit();
  void fotaBegin();
  void fotaLoop();
  bool fotaHandleCliCommand(const char* command, char* reply);
#if FOTA_DEBUG
  //en: Serial-only debug CLI (getacl / getpath|setpath by pub_key prefix)
  //sk: Serial-only debug CLI (getacl / getpath|setpath cez pub_key prefix)
  bool fotaHandleSerialPathCli(const char* fargs, char* reply);
#endif
  //en: non-const client — getpath/setpath/missall-with-path write ACL out_path
  //sk: non-const client — getpath/setpath/missall-s-cestou zapisujú ACL out_path
  bool fotaHandleLoRaCli(ClientInfo* client, const uint8_t* secret,
                         const char* command, char* reply,
                         uint8_t path_hash_size, uint32_t sender_timestamp);
  void runFotaCli(const char* fargs, char* reply);
  bool deferFotaCli(const ClientInfo* client, const uint8_t* secret,
                    const char* fargs, uint8_t path_hash_size,
                    uint32_t sender_timestamp, const char* tag);
  void sendDeferredCliReply(const uint8_t* dest_pub, const uint8_t* secret,
                            const uint8_t* out_path, uint8_t out_path_len,
                            uint8_t path_hash_size, const char* text,
                            uint32_t sender_timestamp);
