from "std/io" import println, IoError
from "std/fmt" import format

fn twice(values: Tensor[i32, ?]) -> Tensor[i32, ?]:
  return values * 2

fn main(args: [String]) -> unit ! (TaskError | IoError):
  if length(args) == 0:
    try println("cpu")
    return ()
  try println("dispatch")
  result = try parallel:
    gpu = devices.gpu()
    values: Tensor[i32, 3] = tensor([3, 7, 11])
    input = try await transfer(values, gpu)
    doubled = try await spawn twice(input) on gpu
    host = try await transfer(doubled, devices.cpu())
    reduceSum(host)
  try println(format("{}", result))
  return ()
