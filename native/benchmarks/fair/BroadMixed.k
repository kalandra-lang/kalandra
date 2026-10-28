fn gcd(a:int,b:int)->int:
  if b == 0:
    a
  else:
    gcd(b,a%b)
fn iterate(x:int,seed:int,rounds:int)->float64:
  if rounds == 0:
    float64(x)
  else:
    iterate((x*48271+gcd(x*13+7,seed*7+11)+seed)%2147483647,seed,rounds-1)
fn euclid_mixed(seed:int,rounds:int)->float64:
  iterate(seed,seed,rounds)
fn main()->float64:
  euclid_mixed(17,16)
