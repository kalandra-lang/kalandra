fn makeAdder(offset:int)->fn(int)->int:
  fn(value):
    adjusted=value+offset
    adjusted*2
export fn main()->int:
  addOne=makeAdder(1)
  addOne(20)
