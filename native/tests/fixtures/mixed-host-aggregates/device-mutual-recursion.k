fn even(n:int)->int:
  if n==0:
    1
  else:
    odd(n-1)+1
fn odd(n:int)->int:
  if n==0:
    0
  else:
    even(n-1)+2
fn main()->int ! TaskError:
  try parallel:
    try await spawn even(1000) on devices.gpu()
