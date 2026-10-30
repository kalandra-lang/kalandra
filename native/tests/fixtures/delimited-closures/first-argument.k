fn apply(callback:fn(int)->int,value:int)->int:
  callback(value)
fn main()->int:
  apply(
    fn(value):
      adjusted=value+1
      adjusted*2
    ,20
  )
