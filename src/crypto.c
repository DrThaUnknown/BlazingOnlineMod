// The game's HTTP body codec, identical to shared/src/codec.ts: AES-128-CBC with a fixed key and
// IV, zero padding (no PKCS7), then base64. Responses may be wrapped in single quotes.
#include "bo.h"
#include "crypto_impl.inc"
