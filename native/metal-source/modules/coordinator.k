from "./kernels/public.k" import compute

export fn run(values: Tensor[float32, ?]) -> Tensor[float32, ?] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    input = try await transfer(values, gpu)
    result = try await spawn compute(input, input) on gpu
    try await transfer(result, devices.cpu())
