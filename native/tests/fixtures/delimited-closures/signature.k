fn apply(callback:fn(fn(int)->int,int)->int)->int:
  callback(fn(value:int):value*2,21)
fn main()->int:
  apply(fn(
    callback:fn(int)->int,
    value:
           int
  )->int:
    total=callback(value)
    total
  )
