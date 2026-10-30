fn count(n:int)->int:
  if n==0:
    22
  else:
    count(n-1)+1
fn main()->int:
  callback=count
  callback(20)
