fn twice(values:Tensor[float32,?])->Tensor[float32,?]:
  values+values
fn divide(value:int)->int:
  6/value
fn main()->int ! TaskError:
  try parallel:
    values=for row in range(3):
      outcome=parallel:
        prefix=for i in range(3):
          child=spawn divide(2-i) on devices.cpu()
          yield float32(try await child)
        length(prefix)
      recovered=outcome catch fn(error):7
      yield float32(recovered)
    gpu=devices.gpu()
    resident=try await transfer(tensor(values),gpu)
    work=spawn twice(resident) on gpu
    host=try await transfer(try await work,devices.cpu())
    int(reduceSum(host))
