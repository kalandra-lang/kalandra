fn operation(values:Tensor[f32,?],factor:f32)->Tensor[f32,?]:
  return if factor>0:
    values+1
  else:
    values-1
fn main()->Tensor[f32,3] ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    values:Tensor[f32,3]=tensor([1,2,3])
    resident=try await transfer(values,gpu)
    answer=try await spawn operation(resident,1) on gpu
    try await transfer(answer,devices.cpu())
