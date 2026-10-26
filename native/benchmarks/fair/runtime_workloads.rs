fn array_rounds(values: Vec<i64>, rounds: i64) -> f64 {
    if rounds == 0 { return values.iter().sum::<i64>() as f64; }
    let next = values.iter().map(|value| (value * 13 + 7) % 1000003).collect();
    array_rounds(next, rounds - 1)
}
fn apply_rounds<F: Fn(i64) -> i64>(transform: F, value: i64, rounds: i64) -> f64 {
    if rounds == 0 { return value as f64; }
    let next = transform(value);
    apply_rounds(transform, next, rounds - 1)
}
#[unsafe(no_mangle)]
pub extern "C" fn runtime_workload(kind: i64, seed: i64, rounds: i64) -> f64 {
    if kind == 0 { return array_rounds((seed..seed+8).collect(), rounds); }
    let offset = seed + 7;
    apply_rounds(|value| (value * 13 + offset) % 1000003, seed, rounds)
}
