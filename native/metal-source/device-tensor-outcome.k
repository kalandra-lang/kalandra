error TileError:
  Invalid

fn calculate(n: int) -> Tensor[int16,2,2] ! TileError:
  if n == 0:
    fail TileError.Invalid
  tensor([[1,2],[3,4]])

fn main() -> int ! (TileError | TaskError):
  try parallel:
    gpu = devices.gpu()
    good = spawn calculate(1) on gpu
    bad = spawn calculate(0) on gpu
    first = try await good
    recovered = await bad catch fn(error):
      try await good
    second = try recovered
    hostFirst = try await transfer(first, devices.cpu())
    hostSecond = try await transfer(second, devices.cpu())
    int(reduceSum(hostFirst + hostSecond))
