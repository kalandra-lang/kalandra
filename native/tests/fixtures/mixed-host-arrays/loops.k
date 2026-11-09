fn twice(values:Tensor[i64,?])->Tensor[i64,?]:
  return values+values
fn contains(values:[i64],wanted:i64)->bool:
  return for value in values:
    if value==wanted:
      break true
  else:
    false
fn edit(mut values:[i64])->[i64]:
  for index in range(length(values)):
    if index==1:
      continue
    values[index]=values[index]+1
  return values
fn sum(n:i64)->i64:
  return if n==0:
    0
  else:
    value=for index in range(1):
      break n
    else:
      0
    value+sum(n-1)
fn prepare(values:[i64])->Tensor[i64,?]:
  return while true:
    break tensor(values)
  else:
    tensor([0])
fn identity(value:i64)->i64:
  return value
fn main()->i64 ! TaskError:
  return try parallel:
    groups=0
    for index in range(3):
      try parallel:
        pending=spawn identity(index) on devices.cpu()
        observed=try await pending
        groups=groups+observed
        if index<2:
          continue
        break
    values=[1,2,3]
    result=edit(values)
    selected=for item in result:
      if item>2:
        break item
    else:
      0
    hit=if contains(result,2):1
    else:0
    gpu=devices.gpu()
    resident=try await transfer(prepare(result),gpu)
    work=spawn twice(resident) on gpu
    host=try await transfer(try await work,devices.cpu())
    i64(reduceSum(host))+selected*10+hit+sum(200)+groups
