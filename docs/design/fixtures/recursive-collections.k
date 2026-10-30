enum Batches:
  End
  Batch:
    values: [int]
    enabled: [Bool]
    next: Batches

fn sum(batches: Batches) -> int:
  match batches:
    Batches.End: 0
    Batches.Batch { values, enabled, next }:
      if enabled[0]: values[0] + values[1] + sum(next)
      else: sum(next)

fn main() -> int:
  tail = Batches.Batch { values: [10, 20], enabled: [true], next: Batches.End }
  batches = Batches.Batch { values: [1, 2], enabled: [true], next: tail }
  sum(batches)
