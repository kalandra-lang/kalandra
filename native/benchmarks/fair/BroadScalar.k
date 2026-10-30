fn affineRounds(x:float64,offset:float64,rounds:int)->float64:
  if rounds == 0:
    x
  else:
    affineRounds(x*float64(0.5)+offset,offset,rounds-1)
fn affine(seed:int,rounds:int)->float64:
  affineRounds(float64(seed)*float64(0.25),float64(seed)*float64(0.0625),rounds)

fn polynomialRounds(x:float64,offset:float64,rounds:int)->float64:
  if rounds == 0:
    x
  else:
    polynomialRounds(x*float64(1.015625)+offset,offset,rounds-1)
fn polynomial(seed:int,rounds:int)->float64:
  polynomialRounds(float64(seed)*float64(0.015625),float64(seed)*float64(0.00390625),rounds)

fn gcd(a:int,b:int)->int:
  if b == 0:
    a
  else:
    gcd(b,a%b)
fn gcdRounds(x:int,seed:int,rounds:int)->float64:
  if rounds == 0:
    float64(x)
  else:
    gcdRounds(gcd(x*13+7,seed*7+11)+seed,seed,rounds-1)
fn euclid(seed:int,rounds:int)->float64:
  gcdRounds(seed,seed,rounds)
fn main()->float64:
  affine(17,16)
