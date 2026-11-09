from "./kernels/public.k" import compute

export fn run(values: Tensor[f32, ?]) -> Tensor[f32, ?] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    input = try await transfer(values, gpu)
    result = try await spawn compute(input, input) on gpu
    try await transfer(result, devices.cpu())
