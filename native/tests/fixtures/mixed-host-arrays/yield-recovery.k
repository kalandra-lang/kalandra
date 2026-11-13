fn twice(values:Tensor[f32,?])->Tensor[f32,?]:
  return values+values
fn divide(value:i64)->i64:
  return 6/value
fn main()->i64 ! TaskError:
  return try parallel:
    values=array(for row in range(3):
      outcome=parallel:
        prefix=array(for i in range(3):
          child=spawn divide(2-i) on devices.cpu()
          yield f32(try await child)
        )
        length(prefix)
      recovered=outcome catch fn(error):return 7
      yield f32(recovered)
    )
    gpu=devices.gpu()
    resident=try await transfer(tensor(values),gpu)
    work=spawn twice(resident) on gpu
    host=try await transfer(try await work,devices.cpu())
    i64(reduceSum(host))
