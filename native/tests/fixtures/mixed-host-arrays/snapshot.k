fn twice(values:Tensor[i64,?])->Tensor[i64,?]:
  return values+values
fn change(mut values:[i64])->[i64]:
  sort(values)
  values[0]=9
  return values
fn main()->i64 ! TaskError:
  return try parallel:
    values=[3,1,2]
    alias=values
    original=tensor(values)
    edited=change(values)
    values=sorted(values)
    values=concat(values,[4])
    values[0]=7
    target=devices.gpu(0)
    resident=try await transfer(original,target)
    work=spawn twice(resident) on target
    result=try await work
    host=try await transfer(result,devices.cpu())
    reduceSum(host)+alias[0]*100+edited[0]*10+values[0]+length(values)
