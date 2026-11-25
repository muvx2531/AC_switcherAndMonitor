#ifndef SSW_SHA256_H
#define SSW_SHA256_H
/* Small SHA-256 (FIPS 180-4). IDENTICAL COPY in the bootloader and the application. */
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  uint32_t n;
} SswSha256;

void SswSha256_Init(SswSha256 *c);
void SswSha256_Update(SswSha256 *c, const void *data, size_t len);
void SswSha256_Final(SswSha256 *c, uint8_t out[32]);
uint32_t SswCrc32(const void *data, size_t len);

#endif
