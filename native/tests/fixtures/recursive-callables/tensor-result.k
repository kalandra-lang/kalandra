error E:
  Bad
fn descend(n:int)->Tensor[int,2] ! E:
  if n<0:fail E.Bad
  if n==0:
    tensor([20,22])
  else:
    try descend(n-1)
fn main()->int:
  callback=descend
  values=callback(20) catch fn(error):tensor([0,0])
  reduceSum(values)
