enum Batches:
  End
  Batch:
    values: [i64]
    enabled: [bool]
    next: Batches

fn sum(batches: Batches) -> i64:
  return match batches:
    Batches.End: 0
    Batches.Batch { values, enabled, next }:
      if enabled[0]: values[0] + values[1] + sum(next)
      else: sum(next)

fn main() -> i64:
  tail = Batches.Batch { values: [10, 20], enabled: [true], next: Batches.End }
  batches = Batches.Batch { values: [1, 2], enabled: [true], next: tail }
  return sum(batches)
