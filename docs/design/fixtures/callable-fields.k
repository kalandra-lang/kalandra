type Config:
  apply: fn(i64)->i64
fn make(offset:i64)->Config:
  return Config {apply:fn(value):return value+offset}
fn main()->i64:
  config=make(20)
  return config.apply(22)
