#include "bytes.h"

void copyPrefix(size_t n, uint8_t * _Nonnull dst __attribute__((noescape)),
                const uint8_t * _Nonnull src __attribute__((noescape))) {
  for (size_t i = 0; i < n; ++i)
    dst[i] = src[i];
}
