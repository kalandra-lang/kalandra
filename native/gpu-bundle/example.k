from "std/io" import println, IoError
from "std/fmt" import format

fn twice(values: Tensor[i32, ?], divisor: i32) -> Tensor[i32, ?]:
  return (values * 2) / divisor

fn main(args: [String]) -> unit ! (TaskError | IoError):
  if length(args) == 0:
    try println("cpu")
    return ()
  try println("dispatch")
  divisor: i32 = if args[0] == "fault":
    0
  else:
    1
  result = try parallel:
    gpu = devices.gpu()
    values: Tensor[i32, 3] = tensor([3, 7, 11])
    input = try await transfer(values, gpu)
    doubled = try await spawn twice(input, divisor) on gpu
    host = try await transfer(doubled, devices.cpu())
    reduceSum(host)
  try println(format("{}", result))
  return ()
