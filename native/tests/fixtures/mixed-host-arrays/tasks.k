fn twice(values:Tensor[int,?])->Tensor[int,?]:
  values+values
fn read(values:[int])->int:
  values[0]
fn privateWork(values:[int])->[int] ! TaskError:
  try parallel:
    old=spawn read(values) on devices.cpu()
    values=sorted(values)
    values[0]=9
    before=try await old
    concat(values,[before])
fn main()->int ! TaskError:
  try parallel:
    values=[3,1,2]
    saved=values
    worker=spawn privateWork(values) on devices.cpu()
    values=[7]
    values[0]=8
    first=try await worker
    cached=try await worker
    first[1]=6
    i=0
    total=0
    while i<1000:
      previous=try parallel:
        reader=spawn read(values) on devices.cpu()
        values=if i%2==0:[1,2,3]
        else:[4]
        try await reader
      total=total+previous
      i=i+1
    gpu=devices.gpu()
    resident=try await transfer(tensor(saved),gpu)
    work=spawn twice(resident) on gpu
    host=try await transfer(try await work,devices.cpu())
    reduceSum(host)+total+first[0]+cached[1]+saved[0]+values[0]
