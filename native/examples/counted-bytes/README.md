# Borrow two byte buffers from Kalandra

The C header ties each buffer to the same byte count and declares that both borrows end when the call returns. The Kalandra program copies the first two bytes of a three-byte buffer. The third destination byte keeps its original value.

Build the C object with Clang in this directory. On Windows, use a Visual Studio developer shell with the Windows SDK available.

```sh
clang -std=c17 -Werror -c bytes.c -o bytes.o
kalandra --project kalandra.toml -o demo.exe
./demo.exe
```

Expected output is `11 22 0`. Passing a count of four produces `BufferError.OutOfRange` before C executes.
