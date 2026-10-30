fn double(value:int)->int:
  value*2
fn triple(value:int)->int:
  value*3
fn apply(value:int,transform:fn(int)->int)->int:
  transform(value)
fn main()->int ! TaskError:
  transform=double
  alias:fn(int)->int=transform
  try parallel:
    local=alias
    work=spawn apply(21,local)
    first=try await work
    second=try await work
    first+second-42
