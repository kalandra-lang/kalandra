fn makeAdder(offset:i64)->fn(i64)->i64:
  return fn(value):
    adjusted=value+offset
    return adjusted*2
export fn main()->i64:
  addOne=makeAdder(1)
  return addOne(20)
