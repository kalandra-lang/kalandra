fn multiply(a: Tensor[f32, ?, ?], b: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return matmul(a, b)

fn project(input: Tensor[f32, ?, ?], weights: Tensor[f32, ?, ?], projection: Tensor[f32, ?, ?], count: f32) -> f32:
  hidden = multiply(input, weights)
  projected = multiply(hidden, projection)
  left = reduceSum(hidden)
  right = reduceSum(projected)
  return finish(left, right, count)

fn finish(left: f32, right: f32, count: f32) -> f32:
  return (left - right) / count

fn main() -> f32 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    input: Tensor[f32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[f32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
    projection: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
    a = try await transfer(input, gpu)
    b = try await transfer(weights, gpu)
    c = try await transfer(projection, gpu)
    work = spawn project(a, b, c, 2) on gpu
    result = try await work
    cached = try await work
    cached
