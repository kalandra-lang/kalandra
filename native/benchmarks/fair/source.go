package main

import (
	"encoding/json"
	"os"
	"strconv"
	"time"
)

func gcd(a, b int64) int64 {
	if b == 0 {
		return a
	}
	return gcd(b, a%b)
}
func polynomial(x, d int64) float64 {
	if d == 0 {
		v := float64(x)
		return (v*0.125+0.25)*v - 0.5
	}
	return polynomial(x*2, d-1) + polynomial(x*2+1, d-1)
}
func branches(x, d int64) float64 {
	if d == 0 {
		if x%3 == 0 {
			return float64(x) * 0.5
		}
		if x%3 == 1 {
			return float64(x) * (-0.25)
		}
		return float64(x) + 0.125
	}
	return branches(x*2, d-1) + branches(x*2+1, d-1)
}
func euclid(x, d int64) float64 {
	if d == 0 {
		return float64(gcd(x*13+7, x*7+11))
	}
	return euclid(x*2, d-1) + euclid(x*2+1, d-1)
}

//go:noinline
func kernel(kind, x, d int64) float64 {
	if kind == 0 {
		return polynomial(x, d)
	}
	if kind == 1 {
		return branches(x, d)
	}
	return euclid(x, d)
}
func main() {
	a := make([]int64, 5)
	if len(os.Args) != 6 {
		panic("args")
	}
	for i := range a {
		v, e := strconv.ParseInt(os.Args[i+1], 10, 64)
		if e != nil {
			panic(e)
		}
		a[i] = v
	}
	kind, depth, iterations, rounds, seed := a[0], a[1], a[2], a[3], a[4]
	if kind < 0 || kind > 2 || depth < 0 || depth > 14 || seed < 1 || seed > 31 || iterations < 1 || rounds < 1 {
		panic("domain")
	}
	enc := json.NewEncoder(os.Stdout)
	for round := int64(-2); round < rounds; round++ {
		checksum := 0.0
		start := time.Now()
		for i := int64(0); i < iterations; i++ {
			checksum += kernel(kind, seed, depth)
		}
		ns := time.Since(start).Nanoseconds()
		enc.Encode(map[string]interface{}{"round": round, "ns": ns, "checksum": checksum})
	}
}
