error E:
  Bad
fn descend(n:i64)->Tensor[i64,2] ! E:
  if n<0:fail E.Bad
  return if n==0:
    tensor([20,22])
  else:
    try descend(n-1)
fn main()->i64:
  callback=descend
  values=callback(20) catch fn(error):return tensor([0,0])
  return reduceSum(values)
