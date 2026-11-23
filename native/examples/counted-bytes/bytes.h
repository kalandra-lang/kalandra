#include <stddef.h>
#include <stdint.h>
void copyPrefix(size_t n,
                uint8_t * _Nonnull __attribute__((counted_by(n))) dst __attribute__((noescape)),
                const uint8_t * _Nonnull __attribute__((counted_by(n))) src __attribute__((noescape)));
