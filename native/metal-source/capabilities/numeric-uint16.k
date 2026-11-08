fn operation(values:Tensor[u16,?])->Tensor[u16,?]:
  return values
fn main()->Tensor[u16,3] ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    values:Tensor[u16,3]=tensor([1,2,3])
    resident=try await transfer(values,gpu)
    answer=try await spawn operation(resident) on gpu
    try await transfer(answer,devices.cpu())
