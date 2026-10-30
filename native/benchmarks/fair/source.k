fn gcd(a:int,b:int)->int:
  if b == 0:
    a
  else:
    gcd(b,a%b)

fn polynomial(x:int,depth:int)->float64:
  if depth == 0:
    value=float64(x)
    (value*0.125+0.25)*value-0.5
  else:
    polynomial(x*2,depth-1)+polynomial(x*2+1,depth-1)

fn branches(x:int,depth:int)->float64:
  if depth == 0:
    if x%3 == 0:
      float64(x)*0.5
    else:
      if x%3 == 1:
        float64(x)*(-0.25)
      else:
        float64(x)+0.125
  else:
    branches(x*2,depth-1)+branches(x*2+1,depth-1)

fn euclid(x:int,depth:int)->float64:
  if depth == 0:
    float64(gcd(x*13+7,x*7+11))
  else:
    euclid(x*2,depth-1)+euclid(x*2+1,depth-1)

fn main()->float64:
  polynomial(17,4)
