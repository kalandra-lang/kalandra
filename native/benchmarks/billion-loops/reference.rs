use std::env;
extern "C" { fn arc4random_uniform(upper: u32) -> u32; }
fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() != 4 && args.len() != 5 { std::process::exit(2); }
    let u: i32 = args[1].parse().unwrap();
    let rows: i32 = args[2].parse().unwrap();
    let count: i32 = args[3].parse().unwrap();
    let r = unsafe { arc4random_uniform(10000) as i32 };
    let mut a = vec![0i32; rows as usize];
    for i in 0..rows as usize {
        for j in 0..count { a[i] = a[i] + j % u; }
        a[i] += r;
    }
    if args.get(4).map(|s| s == "--verify").unwrap_or(false) {
        print!("[{}", r);
        for value in &a { print!(", {}", value); }
        println!("]");
    } else { println!("{}", (r as i64)*4294967296i64 + (a[(r % rows) as usize] as i64)); }
}
