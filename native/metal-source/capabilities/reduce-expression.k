fn operation(values:Tensor[f32,?])->f32:
  return reduceSum(values+1)
fn main()->f32 ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    values:Tensor[f32,3]=tensor([1,2,3])
    resident=try await transfer(values,gpu)
    answer=try await spawn operation(resident) on gpu
    answer
