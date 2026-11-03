fn twice(values:Tensor[int,?])->Tensor[int,?]:
  values+values
fn bad(values:[int],position:int)->[int]:
  [values[position]]
fn recover(values:[int])->[int] ! TaskError:
  result=parallel:
    try await spawn bad(values,length(values)) on devices.cpu()
  result catch fn(error):copy(values)
fn main()->int ! TaskError:
  try parallel:
    values=[3,1,2]
    saved=values
    values=try await spawn recover(values) on devices.cpu()
    values[0]=9
    gpu=devices.gpu()
    resident=try await transfer(tensor(saved),gpu)
    work=spawn twice(resident) on gpu
    host=try await transfer(try await work,devices.cpu())
    reduceSum(host)+values[0]+saved[0]+length(values)
