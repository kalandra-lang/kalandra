fn twice(values:Tensor[i64,?])->Tensor[i64,?]:
  return values+values
fn main()->i64 ! TaskError:
  return try parallel:
    values:[i64]=[]
    flags:[bool]=[]
    saved=values
    i=0
    total=0
    while i<100:
      values=generate(i%3,fn(j):return j)
      flags=map(values,fn(value):return value==0)
      total=total+length(values)+length(flags)
      i=i+1
    gpu=devices.gpu()
    resident=try await transfer(tensor([1,2,3]),gpu)
    work=spawn twice(resident) on gpu
    host=try await transfer(try await work,devices.cpu())
    reduceSum(host)+total+length(saved)
