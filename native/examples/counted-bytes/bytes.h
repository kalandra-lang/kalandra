#include <stddef.h>
#include <stdint.h>

#define BYTE_EXTENT(p, n) __attribute__((diagnose_if( \
    (n) > __builtin_object_size((p), 0), "byte count exceeds buffer", "error")))

void copyPrefix(size_t n,
                uint8_t * _Nonnull dst __attribute__((noescape)),
                const uint8_t * _Nonnull src __attribute__((noescape)))
    BYTE_EXTENT(dst, n) BYTE_EXTENT(src, n);
