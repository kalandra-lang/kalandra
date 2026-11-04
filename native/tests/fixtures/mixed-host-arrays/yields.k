fn twice(values:Tensor[float32,?])->Tensor[float32,?]:
  values+values
fn identity(value:int)->int:
  value
fn prepare(count:int)->[float32]:
  for index in range(count):
    yield float32(index+1)
fn main()->int ! TaskError:
  try parallel:
    values=for row in range(3):
      for index in range(2):
        child=spawn identity(row*2+index) on devices.cpu()
        value=try await child
        if value%2==0:
          yield float32(value+1)
    flags=for index in range(40):
      yield index%2==0
    flags[0]=false
    helpers=prepare(4)
    gpu=devices.gpu()
    resident=try await transfer(tensor(values),gpu)
    work=spawn twice(resident) on gpu
    host=try await transfer(try await work,devices.cpu())
    int(reduceSum(host))+length(flags)+int(helpers[3])
