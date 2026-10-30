use std::{env, hint::black_box, time::Instant};
fn gcd(a: i64, b: i64) -> i64 {
    if b == 0 {
        a
    } else {
        gcd(b, a % b)
    }
}
fn polynomial(x: i64, d: i64) -> f64 {
    if d == 0 {
        let v = x as f64;
        (v * 0.125 + 0.25) * v - 0.5
    } else {
        polynomial(x * 2, d - 1) + polynomial(x * 2 + 1, d - 1)
    }
}
fn branches(x: i64, d: i64) -> f64 {
    if d == 0 {
        if x % 3 == 0 {
            x as f64 * 0.5
        } else if x % 3 == 1 {
            x as f64 * (-0.25)
        } else {
            x as f64 + 0.125
        }
    } else {
        branches(x * 2, d - 1) + branches(x * 2 + 1, d - 1)
    }
}
fn euclid(x: i64, d: i64) -> f64 {
    if d == 0 {
        gcd(x * 13 + 7, x * 7 + 11) as f64
    } else {
        euclid(x * 2, d - 1) + euclid(x * 2 + 1, d - 1)
    }
}
#[inline(never)]
fn kernel(kind: i64, x: i64, d: i64) -> f64 {
    match kind {
        0 => polynomial(x, d),
        1 => branches(x, d),
        _ => euclid(x, d),
    }
}
fn main() {
    let a: Vec<i64> = env::args().skip(1).map(|x| x.parse().unwrap()).collect();
    assert_eq!(a.len(), 5);
    let (kind, depth, iterations, rounds, seed) = (a[0], a[1], a[2], a[3], a[4]);
    assert!(
        (0..=2).contains(&kind)
            && (0..=14).contains(&depth)
            && (1..=31).contains(&seed)
            && iterations > 0
            && rounds > 0
    );
    for round in -2..rounds {
        let mut checksum = 0.0;
        let start = Instant::now();
        for _ in 0..iterations {
            checksum += black_box(kernel(kind, black_box(seed), black_box(depth)));
        }
        println!(
            "{{\"round\":{},\"ns\":{},\"checksum\":{}}}",
            round,
            start.elapsed().as_nanos(),
            checksum
        );
    }
}
