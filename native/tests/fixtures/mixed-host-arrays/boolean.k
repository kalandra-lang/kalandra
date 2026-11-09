fn twice(values:Tensor[i64,?])->Tensor[i64,?]:
  return values+values
fn flip(values:[bool])->[bool]:
  result=copy(values)
  for index in range(length(result)):
    result[index]=not result[index]
  return result
fn wrapped(values:[bool])->[bool] ! TaskError:
  return try parallel:
    result=try await spawn flip(values) on devices.cpu()
    result=copy(result)
    result
fn main()->i64 ! TaskError:
  return try parallel:
    flags=[true,false,true]
    saved=flags
    flags=copy(flags)
    flags[0]=false
    worker=spawn wrapped(saved) on devices.cpu()
    flags=sorted(flags)
    fromTask=try await worker
    fromTask[0]=true
    total=0
    for i in range(1000):
      flags=generate(i%5+1,fn(j):return j%2==0)
      if flags[0]:
        total=total+length(flags)
    result=if saved[0] and not saved[1] and fromTask[0] and fromTask[1]:total
    else:0
    gpu=devices.gpu()
    resident=try await transfer(tensor([1,2,3]),gpu)
    work=spawn twice(resident) on gpu
    host=try await transfer(try await work,devices.cpu())
    result+reduceSum(host)
