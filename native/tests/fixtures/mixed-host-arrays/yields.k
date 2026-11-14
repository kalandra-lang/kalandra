fn twice(values:Tensor[f32,?])->Tensor[f32,?]:
  return values+values
fn identity(value:i64)->i64:
  return value
fn prepare(count:i64)->[f32]:
  return array(for index in range(count):
    yield f32(index+1)
  )
fn main()->i64 ! TaskError:
  return try parallel:
    values=try array(for row in range(3):
      for index in range(2):
        value=try parallel:
          child=spawn identity(row*2+index) on devices.cpu()
          try await child
        if value%2==0:
          yield f32(value+1)
    )
    flags=array(for index in range(40):
      yield index%2==0
    )
    flags[0]=false
    helpers=prepare(4)
    gpu=devices.gpu()
    resident=try await transfer(tensor(values),gpu)
    work=spawn twice(resident) on gpu
    host=try await transfer(try await work,devices.cpu())
    i64(reduceSum(host))+length(flags)+i64(helpers[3])
