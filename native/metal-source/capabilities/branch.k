fn operation(values:Tensor[float32,?],factor:float32)->Tensor[float32,?]:
  if factor>0:
    values+1
  else:
    values-1
fn main()->Tensor[float32,3] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    values:Tensor[float32,3]=tensor([1,2,3])
    resident=try await transfer(values,gpu)
    answer=try await spawn operation(resident,1) on gpu
    try await transfer(answer,devices.cpu())
