fn twice(values:Tensor[int,?])->Tensor[int,?]:
  values+values
fn main()->int ! TaskError:
  try parallel:
    source=generate(8,fn(i):8-i)
    saved=source
    source=map(source,fn(value):value+1)
    source=slice(source,1,5)
    source=concat(source,[])
    sort(source)
    original=tensor(source)
    destination=source
    source[0]=if true:
      source=[90,91]
      80
    else:0
    restored=array(original)
    restored[0]=7
    gpu=devices.gpu()
    resident=try await transfer(original,gpu)
    work=spawn twice(resident) on gpu
    host=try await transfer(try await work,devices.cpu())
    reduceSum(host)+source[0]+restored[0]+saved[0]+length(source)+destination[0]
