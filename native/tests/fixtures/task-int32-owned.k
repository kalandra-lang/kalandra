fn arithmetic(value:i32) -> i32:
  return value + 1
fn transform(values:Tensor[f32,?]) -> Tensor[f32,?]:
  return values + 1
fn batch(values:Tensor[f32,?]) -> f32 ! TaskError:
  return try parallel:
    sibling = spawn transform(values)
    work = spawn arithmetic(2147483647)
    try await work
    completed = try await sibling
    reduceSum(completed)
fn main() -> f32:
  values:Tensor[f32,3] = tensor([1,2,3])
  result = batch(values)
  recovered = result catch fn(error):
    return match error:
      TaskError.Fault {kind:cause,message:detail}:
        match cause:
          FaultKind.Arithmetic: 10
          FaultKind.Allocation: 20
          _: 0
      _: 0
  return recovered + reduceSum(values)
