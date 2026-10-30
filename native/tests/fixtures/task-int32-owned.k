fn arithmetic(value:int32) -> int32:
  value + 1
fn transform(values:Tensor[float32,?]) -> Tensor[float32,?]:
  values + 1
fn batch(values:Tensor[float32,?]) -> float32 ! TaskError:
  try parallel:
    sibling = spawn transform(values)
    work = spawn arithmetic(2147483647)
    try await work
    completed = try await sibling
    reduceSum(completed)
fn main() -> float32:
  values:Tensor[float32,3] = tensor([1,2,3])
  result = batch(values)
  recovered = result catch fn(error):
    match error:
      TaskError.Fault {kind:cause,message:detail}:
        match cause:
          FaultKind.Arithmetic: 10
          FaultKind.Allocation: 20
          _: 0
      _: 0
  recovered + reduceSum(values)
