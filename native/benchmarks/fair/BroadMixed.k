fn gcd(a:i64,b:i64)->i64:
  return if b == 0:
    a
  else:
    gcd(b,a%b)
fn iterate(x:i64,seed:i64,rounds:i64)->f64:
  return if rounds == 0:
    f64(x)
  else:
    iterate((x*48271+gcd(x*13+7,seed*7+11)+seed)%2147483647,seed,rounds-1)
fn euclid_mixed(seed:i64,rounds:i64)->f64:
  return iterate(seed,seed,rounds)
fn main()->f64:
  return euclid_mixed(17,16)
