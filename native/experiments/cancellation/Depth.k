fn chain(x:int,depth:int)->float64:
  if depth == 0: float64(x)
  else:
    next=(x*13+7)%1000003
    child=chain(next,depth-1)
    child*0.5+float64(x)*0.25

fn main()->float64:
  chain(17,100)
