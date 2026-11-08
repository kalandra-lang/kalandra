fn divide(seed:i64,denominator:i64)->f64:
  return f64((seed*65536-2147483648)/(denominator-1))
fn remainder(seed:i64,denominator:i64)->f64:
  return f64((seed*65536-2147483648)%(denominator-1))
fn main()->f64:
  return divide(0,0)
