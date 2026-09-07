#ifndef NDNSF_TOKENIZER_BRIDGE_ABI_H
#define NDNSF_TOKENIZER_BRIDGE_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t* data;
  size_t size;
  int32_t code;
} NdiTokenResult;

NdiTokenResult ndi_token_create(const uint8_t* json, size_t size, void** handle);
NdiTokenResult ndi_token_encode(const void* handle, const uint8_t* text,
                                size_t size, uint8_t addSpecial);
NdiTokenResult ndi_token_decode(const void* handle, const uint32_t* ids,
                                size_t count, uint8_t skipSpecial);
NdiTokenResult ndi_token_decode_stable(const void* handle, const uint32_t* ids,
                                       size_t count, uint8_t skipSpecial,
                                       uint8_t final);
void ndi_token_free(uint8_t* data, size_t size);
void ndi_token_destroy(void* handle);

#ifdef __cplusplus
}
#endif

#endif // NDNSF_TOKENIZER_BRIDGE_ABI_H
