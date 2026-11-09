fn twice(values:Tensor[i64,?])->Tensor[i64,?]:
  return values+values
fn bad(values:[i64],position:i64)->[i64]:
  return [values[position]]
fn recover(values:[i64])->[i64] ! TaskError:
  result=parallel:
    try await spawn bad(values,length(values)) on devices.cpu()
  return result catch fn(error):return copy(values)
fn main()->i64 ! TaskError:
  return try parallel:
    values=[3,1,2]
    saved=values
    values=try await spawn recover(values) on devices.cpu()
    values[0]=9
    gpu=devices.gpu()
    resident=try await transfer(tensor(saved),gpu)
    work=spawn twice(resident) on gpu
    host=try await transfer(try await work,devices.cpu())
    reduceSum(host)+values[0]+saved[0]+length(values)
