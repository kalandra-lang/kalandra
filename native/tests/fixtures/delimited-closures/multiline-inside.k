fn sum(a:i64,b:i64)->i64:
  return a+b
fn main()->i64:
  values=map([10,12],fn(value):
    total=sum(
              value,
       10
    )
    return total
  )
  return values[0]+values[1]
