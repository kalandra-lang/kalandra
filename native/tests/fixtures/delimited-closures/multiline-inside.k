fn sum(a:int,b:int)->int:
  a+b
fn main()->int:
  values=map([10,12],fn(value):
    total=sum(
              value,
       10
    )
    total
  )
  values[0]+values[1]
