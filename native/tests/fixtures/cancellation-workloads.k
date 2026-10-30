fn tail(x:int,n:int)->float64:
  if n == 0: float64(x)
  else: tail((x*13+7)%1000003,n-1)

fn odd(x:int,n:int)->float64:
  if n == 0: float64(x)
  else: even((x*13+7)%1000003,n-1)
fn even(x:int,n:int)->float64:
  if n == 0: float64(x)
  else: odd((x*7+11)%1000003,n-1)
fn mutual(x:int,n:int)->float64:
  even(x,n)

fn churn(values:[int],n:int)->float64:
  if n == 0: float64(values[2])
  else:
    x=(values[0]*13+values[1])%1000003
    churn([x,x+1,x+2],n-1)
fn owned(x:int,n:int)->float64:
  churn([x,x+1,x+2],n)

fn main()->float64:
  tail(17,100)
