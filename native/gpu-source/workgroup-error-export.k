from "std/gpu" import dispatch, WorkItem
from "std/io" import println, IoError
from "std/fmt" import format

error Bad:
  Failed:
    index: i64
    observed: f32

fn risky(count: i64) -> Tensor[f32, ?] ! Bad:
  return try dispatch([count], [4], fn(item: WorkItem) -> f32 ! Bad:
    lane = item.localIndex(0)
    tile = item.shared[f32](4)
    tile[lane] = f32(lane + 1)
    item.barrier()
    if item.globalIndex(0) == 5:
      fail Bad.Failed {index: item.globalIndex(0), observed: tile[(lane + 1) % 4]}
    return tile[3 - lane]
  )

fn report(count: i64) -> String ! (TaskError | Bad):
  return try parallel:
    gpu = devices.gpu()
    output = try await spawn risky(count) on gpu
    cpu = try await transfer(output, devices.cpu())
    first = if count > 0: array(cpu)[0]
    else: f32(0)
    format("ok {} {}", shape(cpu)[0], first)

fn main() -> unit ! IoError:
  for count in [0, 4, 5, 6]:
    text = report(count) catch fn(error):
      return match error:
        Bad.Failed {index: index, observed: observed}: format("error {} {}", index, observed)
        _: "unexpected task fault"
    try println(text)
  return ()
