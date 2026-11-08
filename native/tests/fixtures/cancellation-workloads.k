fn tail(x:i64,n:i64)->f64:
  return if n == 0: f64(x)
  else: tail((x*13+7)%1000003,n-1)

fn odd(x:i64,n:i64)->f64:
  return if n == 0: f64(x)
  else: even((x*13+7)%1000003,n-1)
fn even(x:i64,n:i64)->f64:
  return if n == 0: f64(x)
  else: odd((x*7+11)%1000003,n-1)
fn mutual(x:i64,n:i64)->f64:
  return even(x,n)

fn churn(values:[i64],n:i64)->f64:
  return if n == 0: f64(values[2])
  else:
    x=(values[0]*13+values[1])%1000003
    churn([x,x+1,x+2],n-1)
fn owned(x:i64,n:i64)->f64:
  return churn([x,x+1,x+2],n)

fn main()->f64:
  return tail(17,100)
