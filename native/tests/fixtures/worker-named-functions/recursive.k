fn count(value:int)->int:
  if value==0:
    22
  else:
    count(value-1)+1
fn apply(value:int,transform:fn(int)->int)->int:
  transform(value)
fn main()->int ! TaskError:
  try parallel:
    try await spawn apply(20,count)
