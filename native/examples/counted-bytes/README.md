# Borrow two byte buffers from Kalandra

The C header ties each buffer to the same byte count and declares that both borrows end when the call returns. The Kalandra program copies the first two bytes of a three-byte buffer. The third destination byte keeps its original value.

On macOS with a bounds-safety-capable Xcode, run these commands in this directory.

```sh
xcrun clang -std=c17 -fbounds-safety -Werror -c bytes.c -o bytes.o
ar rcs libbytes.a bytes.o
kalandra --project kalandra.toml -o demo
./demo
```

Expected output is `11 22 0`. Passing a count of four produces `BufferError.OutOfRange` before C executes.
