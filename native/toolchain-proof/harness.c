// Private fixed-shape test ABI. Not a public Kalandra FFI or tensor runtime.
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(float) == 4, "The proof requires binary32 storage");
_Static_assert(sizeof(intptr_t) == 8, "The proof requires 64-bit indices");

typedef struct {
  float *allocated;
  float *aligned;
  int64_t offset;
  int64_t sizes[2];
  int64_t strides[2];
} Matrix;

extern float _mlir_ciface_score(Matrix *a, Matrix *b);
static uint64_t allocations;
static uint64_t releases;
static void *live_buffer;

void *_mlir_memref_to_llvm_alloc(uint64_t size) {
  if (getenv("KALANDRA_PROOF_FAIL_ALLOC")) {
    fputs("AllocationFault: injected proof allocation failure\n", stderr);
    exit(86);
  }
  if (size > SIZE_MAX || live_buffer) abort();
  void *buffer = malloc((size_t)size);
  if (!buffer) {
    fputs("AllocationFault: proof allocation failed\n", stderr);
    exit(86);
  }
  live_buffer = buffer;
  ++allocations;
  return buffer;
}

void _mlir_memref_to_llvm_free(void *buffer) {
  if (!buffer || buffer != live_buffer) abort();
  live_buffer = NULL;
  ++releases;
  free(buffer);
}

int main(int argc, char **argv) {
  float a[6] = {1, 2, 3, 4, 5, 6};
  float b[6] = {1, 2, 3, 4, 5, 6};
  uint64_t repeats = 1;
  int bits_output = 0;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--bits")) {
      bits_output = 1;
    } else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
      FILE *input = fopen(argv[++i], "rb");
      if (!input) return 2;
      int valid = fread(a, sizeof(float), 6, input) == 6 &&
                  fread(b, sizeof(float), 6, input) == 6 &&
                  fgetc(input) == EOF && !ferror(input);
      fclose(input);
      if (!valid) return 2;
    } else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) {
      char *end;
      errno = 0;
      const char *text = argv[++i];
      repeats = strtoull(text, &end, 10);
      if (errno || end == text || *end || repeats == 0 || repeats > 1000000)
        return 2;
    } else {
      return 2;
    }
  }
  const float original_a[6] = {a[0], a[1], a[2], a[3], a[4], a[5]};
  const float original_b[6] = {b[0], b[1], b[2], b[3], b[4], b[5]};
  Matrix left = {a, a, 0, {2, 3}, {3, 1}};
  Matrix right = {b, b, 0, {3, 2}, {2, 1}};
  float result = 0;
  for (uint64_t i = 0; i < repeats; ++i) {
    result = _mlir_ciface_score(&left, &right);
    if (live_buffer || allocations != releases ||
        memcmp(a, original_a, sizeof(a)) || memcmp(b, original_b, sizeof(b)))
      abort();
  }
  if (bits_output) {
    uint32_t bits;
    memcpy(&bits, &result, sizeof(bits));
    printf("{\"bits\":%" PRIu32 ",\"allocations\":%" PRIu64
           ",\"releases\":%" PRIu64 "}\n", bits, allocations, releases);
  } else {
    printf("%.9g\n", (double)result);
  }
  return 0;
}
