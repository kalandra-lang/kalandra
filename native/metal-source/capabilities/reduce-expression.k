fn operation(values:Tensor[float32,?])->float32:
  reduceSum(values+1)
fn main()->float32 ! TaskError:
  try parallel:
    gpu=devices.gpu()
    values:Tensor[float32,3]=tensor([1,2,3])
    resident=try await transfer(values,gpu)
    answer=try await spawn operation(resident) on gpu
    answer
