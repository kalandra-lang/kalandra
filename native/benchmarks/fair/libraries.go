package main

/*
#include <cblas.h>
*/
import "C"
import (
	"encoding/json"
	"os"
	"strconv"
	"time"
)

type Scalar = float64

func input(i int, right bool, pattern int) Scalar {
	mod, offset, mult, denom := 17, 8, 37, 37
	if right {
		mod, offset, mult, denom = 13, 6, 43, 29
	}
	if pattern != 0 {
		return Scalar((i*mult)%101-50) / Scalar(denom)
	}
	return Scalar(i%mod-offset) / 8
}

//go:noinline
func kernel(kind, n int, a, b, ones []Scalar) (Scalar, []Scalar) {
	if kind == 0 {
		return Scalar(C.cblas_ddot(C.int(n), (*C.double)(&a[0]), 1, (*C.double)(&b[0]), 1)), nil
	}
	out := make([]Scalar, len(a))
	if kind == 1 {
		C.cblas_dcopy(C.int(n), (*C.double)(&b[0]), 1, (*C.double)(&out[0]), 1)
		C.cblas_daxpy(C.int(n), 3, (*C.double)(&a[0]), 1, (*C.double)(&out[0]), 1)
	} else {
		C.cblas_dgemm(101, 111, 111, C.int(n), C.int(n), C.int(n), 1, (*C.double)(&a[0]), C.int(n), (*C.double)(&b[0]), C.int(n), 0, (*C.double)(&out[0]), C.int(n))
	}
	return Scalar(C.cblas_ddot(C.int(len(out)), (*C.double)(&out[0]), 1, (*C.double)(&ones[0]), 1)), out
}
func main() {
	if len(os.Args) != 8 {
		panic("args")
	}
	a := make([]int, 7)
	for i := range a {
		v, e := strconv.Atoi(os.Args[i+1])
		if e != nil {
			panic(e)
		}
		a[i] = v
	}
	kind, n, iterations, rounds, threads, pattern, dump := a[0], a[1], a[2], a[3], a[4], a[5], a[6]
	if kind < 0 || kind > 2 || n < 1 || n > 4194304 || (dump == 0 && (iterations < 1 || rounds < 1)) {
		panic("domain")
	}
	C.openblas_set_num_threads(C.int(threads))
	if int(C.openblas_get_num_threads()) != threads {
		panic("threads")
	}
	count := n
	if kind == 2 {
		count = n * n
	}
	left, right, ones := make([]Scalar, count), make([]Scalar, count), make([]Scalar, count)
	for i := range left {
		left[i] = input(i, false, pattern)
		right[i] = input(i, true, pattern)
		ones[i] = 1
	}
	enc := json.NewEncoder(os.Stdout)
	if dump != 0 {
		value, cells := kernel(kind, n, left, right, ones)
		if cells == nil {
			cells = []Scalar{}
		}
		enc.Encode(map[string]interface{}{"result": value, "threads": threads, "cells": cells})
		return
	}
	for round := -2; round < rounds; round++ {
		checksum := 0.0
		start := time.Now()
		for i := 0; i < iterations; i++ {
			value, _ := kernel(kind, n, left, right, ones)
			checksum += float64(value)
		}
		ns := time.Since(start).Nanoseconds()
		enc.Encode(map[string]interface{}{"round": round, "ns": ns, "checksum": checksum, "threads": threads})
	}
	for i := range left {
		if left[i] != input(i, false, pattern) || right[i] != input(i, true, pattern) {
			panic("mutated")
		}
	}
}
