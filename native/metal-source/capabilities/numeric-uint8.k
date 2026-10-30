fn operation(values:Tensor[uint8,?])->Tensor[uint8,?]:
  values
fn main()->Tensor[uint8,3] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    values:Tensor[uint8,3]=tensor([1,2,3])
    resident=try await transfer(values,gpu)
    answer=try await spawn operation(resident) on gpu
    try await transfer(answer,devices.cpu())
