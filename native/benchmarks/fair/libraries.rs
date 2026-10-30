use std::{env, hint::black_box, time::Instant};
type Scalar = f64;
#[link(name = "openblas")]
extern "C" {
    fn openblas_set_num_threads(n: i32);
    fn openblas_get_num_threads() -> i32;
    fn cblas_ddot(n: i32, a: *const Scalar, ia: i32, b: *const Scalar, ib: i32) -> Scalar;
    fn cblas_dcopy(n: i32, a: *const Scalar, ia: i32, b: *mut Scalar, ib: i32);
    fn cblas_daxpy(n: i32, alpha: Scalar, a: *const Scalar, ia: i32, b: *mut Scalar, ib: i32);
    fn cblas_dgemm(
        order: i32,
        ta: i32,
        tb: i32,
        m: i32,
        n: i32,
        k: i32,
        alpha: Scalar,
        a: *const Scalar,
        lda: i32,
        b: *const Scalar,
        ldb: i32,
        beta: Scalar,
        c: *mut Scalar,
        ldc: i32,
    );
}
fn input(i: usize, right: bool, pattern: i32) -> Scalar {
    if pattern != 0 {
        (((i * if right { 43 } else { 37 }) % 101) as i32 - 50) as Scalar
            / if right { 29.0 } else { 37.0 }
    } else {
        ((i % if right { 13 } else { 17 }) as i32 - if right { 6 } else { 8 }) as Scalar / 8.0
    }
}
#[inline(never)]
fn kernel(kind: i32, n: i32, a: &[Scalar], b: &[Scalar], ones: &[Scalar]) -> (Scalar, Vec<Scalar>) {
    unsafe {
        if kind == 0 {
            return (cblas_ddot(n, a.as_ptr(), 1, b.as_ptr(), 1), Vec::new());
        }
        let mut out: Vec<Scalar> = Vec::with_capacity(a.len());
        if kind == 1 {
            cblas_dcopy(n, b.as_ptr(), 1, out.as_mut_ptr(), 1);
            cblas_daxpy(n, 3.0, a.as_ptr(), 1, out.as_mut_ptr(), 1);
        } else {
            cblas_dgemm(
                101,
                111,
                111,
                n,
                n,
                n,
                1.0,
                a.as_ptr(),
                n,
                b.as_ptr(),
                n,
                0.0,
                out.as_mut_ptr(),
                n,
            );
        }
        out.set_len(a.len());
        let result = cblas_ddot(out.len() as i32, out.as_ptr(), 1, ones.as_ptr(), 1);
        (result, out)
    }
}
fn main() {
    let p: Vec<i64> = env::args().skip(1).map(|x| x.parse().unwrap()).collect();
    assert_eq!(p.len(), 7);
    let (kind, n, iterations, rounds, threads, pattern, dump) = (
        p[0] as i32,
        p[1] as i32,
        p[2],
        p[3] as i32,
        p[4] as i32,
        p[5] as i32,
        p[6] != 0,
    );
    assert!(
        (0..=2).contains(&kind)
            && n > 0
            && n <= 4194304
            && (dump || (iterations > 0 && rounds > 0))
    );
    unsafe {
        openblas_set_num_threads(threads);
        assert_eq!(openblas_get_num_threads(), threads);
    }
    let count = if kind == 2 {
        n as usize * n as usize
    } else {
        n as usize
    };
    let a: Vec<Scalar> = (0..count).map(|i| input(i, false, pattern)).collect();
    let b: Vec<Scalar> = (0..count).map(|i| input(i, true, pattern)).collect();
    let ones = vec![1.0; count];
    if dump {
        let (value, cells) = kernel(kind, n, &a, &b, &ones);
        println!(
            "{{\"result\":{},\"threads\":{},\"cells\":{:?}}}",
            value, threads, cells
        );
        return;
    }
    for round in -2..rounds {
        let mut checksum = 0.0;
        let start = Instant::now();
        for _ in 0..iterations {
            let (value, out) = black_box(kernel(
                kind,
                n,
                black_box(&a),
                black_box(&b),
                black_box(&ones),
            ));
            checksum += value as f64;
            drop(out);
        }
        println!(
            "{{\"round\":{},\"ns\":{},\"checksum\":{},\"threads\":{}}}",
            round,
            start.elapsed().as_nanos(),
            checksum,
            threads
        );
    }
    assert!((0..count).all(|i| a[i] == input(i, false, pattern) && b[i] == input(i, true, pattern)));
}
