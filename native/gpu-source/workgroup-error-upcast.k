from "std/gpu" import dispatch, WorkItem
from "std/io" import println, IoError
from "std/fmt" import format

type Detail:
  negative: i8
  enabled: bool
  value: f64

error Problem:
  Empty
  Payload:
    detail: Detail
    counter: u64

error Other:
  Small:
    value: i16

fn first(kind: i64) -> f32 ! Problem:
  if kind == 1:
    fail Problem.Empty
  if kind == 2:
    fail Problem.Payload {detail: Detail {negative: i8(-7), enabled: true, value: f64(1.25)}, counter: 18446744073709551615}
  return f32(3)

fn work(kind: i64) -> Tensor[f32, ?] ! (Problem | Other):
  return try dispatch([1], [4], fn(item: WorkItem) -> f32 ! Problem:
    return try first(kind)
  )

fn example(kind: i64) -> String ! (TaskError | Problem | Other):
  return try parallel:
    result = try await spawn work(kind) on devices.gpu()
    cpu = try await transfer(result, devices.cpu())
    format("ok {}", array(cpu)[0])

fn main() -> unit ! IoError:
  for kind in [0, 1, 2]:
    text = example(kind) catch fn(error):
      return match error:
        Problem.Empty: "empty"
        Problem.Payload {detail: detail, counter: counter}: format("{} {} {} {}", detail.negative, detail.enabled, detail.value, counter)
        Other.Small {value: value}: format("other {}", value)
        _: "unexpected task fault"
    try println(text)
  return ()
