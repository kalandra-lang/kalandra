fn divide(seed:int,denominator:int)->float64:
  float64((seed*65536-2147483648)/(denominator-1))
fn remainder(seed:int,denominator:int)->float64:
  float64((seed*65536-2147483648)%(denominator-1))
fn main()->float64:
  divide(0,0)
