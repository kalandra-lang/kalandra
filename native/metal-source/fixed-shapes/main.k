from "./library.k" import vector, selected, total, project, transitive, pinned as checkedKernel, pinnedResult, pinnedMatrix, empty

fn forward(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  values

fn matrixForward(values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  values

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    values: Tensor[float32, 3] = tensor([1, 2, 3])
    ignored: Tensor[float32, 2] = tensor([9, 9])
    input: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[float32, 3, 2] = tensor([[1, 0], [0, 1], [1, 1]])
    zeros: Tensor[float32, 0] = tensor([])
    a = try await transfer(forward(values), gpu)
    b = try await transfer(forward(ignored), gpu)
    c = try await transfer(matrixForward(input), gpu)
    d = try await transfer(matrixForward(weights), gpu)
    z = try await transfer(zeros, gpu)
    scaled = try await spawn vector(a) on gpu
    chosen = try await spawn selected(b, a) on gpu
    adjusted = try await spawn transitive(a) on gpu
    pinned = try await spawn pinnedResult(a) on gpu
    checked = try await spawn checkedKernel(a) on gpu
    projected = try await spawn project(c, d) on gpu
    inferred = try await spawn pinnedMatrix(c, d) on gpu
    sum = try await spawn total(c) on gpu
    nothing = try await spawn empty(z) on gpu
    hostScaled = try await transfer(scaled, devices.cpu())
    hostChosen = try await transfer(chosen, devices.cpu())
    hostAdjusted = try await transfer(adjusted, devices.cpu())
    hostPinned = try await transfer(pinned, devices.cpu())
    hostChecked = try await transfer(checked, devices.cpu())
    hostProjected = try await transfer(projected, devices.cpu())
    hostInferred = try await transfer(inferred, devices.cpu())
    hostNothing = try await transfer(nothing, devices.cpu())
    reduceSum(hostScaled) + reduceSum(hostChosen) + reduceSum(hostAdjusted) + reduceSum(hostPinned) + reduceSum(hostProjected) + reduceSum(hostInferred) + sum + reduceSum(hostNothing) + reduceSum(hostChecked)
