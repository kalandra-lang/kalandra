type Config:
  apply: fn(int)->int
fn make(offset:int)->Config:
  Config {apply:fn(value):value+offset}
fn main()->int:
  config=make(20)
  config.apply(22)
