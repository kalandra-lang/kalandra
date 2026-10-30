fn apply(base:int,callback:fn(int)->int,value:int)->int:
  callback(value)+base
fn main()->int:
  apply(2,fn(value):
    adjusted=value+1
    adjusted*2
  ,19)
