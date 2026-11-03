fn twice(values:Tensor[int,?])->Tensor[int,?]:
  values+values
fn flip(values:[Bool])->[Bool]:
  map(values,fn(flag):not flag)
fn wrapped(values:[Bool])->[Bool] ! TaskError:
  try parallel:
    result=try await spawn flip(values) on devices.cpu()
    result=copy(result)
    result
fn main()->int ! TaskError:
  try parallel:
    flags=[true,false,true]
    saved=flags
    flags=copy(flags)
    flags[0]=false
    worker=spawn wrapped(saved) on devices.cpu()
    flags=sorted(flags)
    fromTask=try await worker
    fromTask[0]=true
    i=0
    total=0
    while i<1000:
      flags=generate(i%5+1,fn(j):j%2==0)
      if flags[0]:
        total=total+length(flags)
      i=i+1
    result=if saved[0] and not saved[1] and fromTask[0] and fromTask[1]:total
    else:0
    gpu=devices.gpu()
    resident=try await transfer(tensor([1,2,3]),gpu)
    work=spawn twice(resident) on gpu
    host=try await transfer(try await work,devices.cpu())
    result+reduceSum(host)
