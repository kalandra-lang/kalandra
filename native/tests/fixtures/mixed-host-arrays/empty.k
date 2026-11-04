fn twice(values:Tensor[int,?])->Tensor[int,?]:
  values+values
fn main()->int ! TaskError:
  try parallel:
    values:[int]=[]
    flags:[Bool]=[]
    saved=values
    i=0
    total=0
    while i<100:
      values=generate(i%3,fn(j):j)
      flags=map(values,fn(value):value==0)
      total=total+length(values)+length(flags)
      i=i+1
    gpu=devices.gpu()
    resident=try await transfer(tensor([1,2,3]),gpu)
    work=spawn twice(resident) on gpu
    host=try await transfer(try await work,devices.cpu())
    reduceSum(host)+total+length(saved)
