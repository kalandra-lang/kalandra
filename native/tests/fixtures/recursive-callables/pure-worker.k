fn count(n:int)->int:
  if n==0:
    22
  else:
    count(n-1)+1
fn main()->int ! TaskError:
  try parallel:
    try await spawn count(20)
