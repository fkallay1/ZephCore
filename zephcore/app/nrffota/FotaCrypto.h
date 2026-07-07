#pragma once
// =====================================================================
//en: FotaCrypto.h — platform shim for SHA256 + Ed25519 used by FOTA code.
//en: MESHCORE: rweather/Crypto (same dep as mesh::Utils on Arduino).
//en: ZEPHCORE: PSA Crypto (same backend as mesh::Utils) + Monocypher Ed25519.
//en: Shared file — keep byte-identical between MeshCore and ZephCore
//en: (tools/fota_sync.py in ZephCore).
//sk: FotaCrypto.h — platformový shim pre SHA256 + Ed25519 vo FOTA kóde.
//sk: MESHCORE: rweather/Crypto (rovnaká dep ako mesh::Utils na Arduino).
//sk: ZEPHCORE: PSA Crypto (rovnaký backend ako mesh::Utils) + Monocypher Ed25519.
//sk: Zdieľaný súbor — drž byte-identický medzi MeshCore a ZephCore
//sk: (tools/fota_sync.py v ZephCore).
// =====================================================================
#include <stdint.h>
#include <stddef.h>

#if defined(FOTA_MESHCORE_BUILD)
  #include <SHA256.h>
  #include <Ed25519.h>
  typedef SHA256 FotaSha256;
  static inline bool fota_ed25519_verify(const uint8_t sig[64], const uint8_t pub[32],
                                         const void* msg, size_t len) {
    return Ed25519::verify(sig, pub, msg, len);
  }
#elif defined(FOTA_ZEPHCORE_BUILD)
  #include <psa/crypto.h>
  #include "monocypher-ed25519.h"
  //en: rweather-compatible streaming SHA256 API over PSA (reset/update/finalize)
  //sk: streaming SHA256 s rweather-kompatibilným API nad PSA (reset/update/finalize)
  class FotaSha256 {
    psa_hash_operation_t _op = PSA_HASH_OPERATION_INIT;
    bool _active = false;
  public:
    FotaSha256() { reset(); }
    ~FotaSha256() { if (_active) psa_hash_abort(&_op); }
    void reset() {
      if (_active) psa_hash_abort(&_op);
      _op = PSA_HASH_OPERATION_INIT;
      _active = (psa_hash_setup(&_op, PSA_ALG_SHA_256) == PSA_SUCCESS);
    }
    void update(const void* data, size_t len) {
      if (_active) psa_hash_update(&_op, (const uint8_t*)data, len);
    }
    void finalize(void* out, size_t len) {
      uint8_t digest[32]; size_t olen = 0;
      if (_active && psa_hash_finish(&_op, digest, sizeof(digest), &olen) == PSA_SUCCESS) {
        for (size_t i = 0; i < len && i < sizeof(digest); i++) ((uint8_t*)out)[i] = digest[i];
      }
      _active = false;
    }
  };
  static inline bool fota_ed25519_verify(const uint8_t sig[64], const uint8_t pub[32],
                                         const void* msg, size_t len) {
    return crypto_ed25519_check(sig, pub, (const uint8_t*)msg, len) == 0;
  }
#else
  #error "FotaCrypto.h: define FOTA_MESHCORE_BUILD or FOTA_ZEPHCORE_BUILD"
#endif
