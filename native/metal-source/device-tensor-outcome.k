error TileError:
  Invalid

fn calculate(n: i64) -> Tensor[i16,2,2] ! TileError:
  if n == 0:
    fail TileError.Invalid
  return tensor([[1,2],[3,4]])

fn main() -> i64 ! (TileError | TaskError):
  return try parallel:
    good = spawn calculate(1) on devices.gpu()
    bad = spawn calculate(0) on devices.gpu()
    first = try await good
    recovered = await bad catch fn(error):
      return try await good
    second = try recovered
    hostFirst = try await transfer(first, devices.cpu())
    hostSecond = try await transfer(second, devices.cpu())
    i64(reduceSum(hostFirst + hostSecond))
