fn main() -> f32 ! TaskError:
  return try parallel:
    values:Tensor[f32,3] = tensor([1,2,3])
    left = spawn compute(forward(values),2)
    right = spawn compute(forward(values),3) on devices.cpu()
    first = try await left
    second = try await right
    reduceSum(first + second)

fn compute(values:Tensor[f32,?], factor:f32) -> Tensor[f32,?] ! TaskError:
  return try parallel:
    try await spawn transform(values,values,factor)

fn transform(left:Tensor[f32,3], right:Tensor[f32,3], factor:f32) -> Tensor[f32,?] ! TaskError:
  return try parallel:
    values = left + right
    ready = try await spawn forward(values)
    gpu = devices.gpu()
    input = try await transfer(ready,gpu)
    result = try await spawn scale(input,factor) on gpu
    try await transfer(result,devices.cpu())

fn scale(values:Tensor[f32,?], factor:f32) -> Tensor[f32,?]:
  return values * factor

fn forward(values:Tensor[f32,?]) -> Tensor[f32,?]:
  return values
