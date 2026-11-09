fn operation(values:Tensor[i16,?])->Tensor[i16,?]:
  return values
fn main()->Tensor[i16,3] ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    values:Tensor[i16,3]=tensor([1,2,3])
    resident=try await transfer(values,gpu)
    answer=try await spawn operation(resident) on gpu
    try await transfer(answer,devices.cpu())
