enum Chain:
  End
  Link:
    values: [i64]
    next: Chain

error ChainError:
  Invalid:
    chain: Chain

fn rejectChain() -> i64 ! ChainError:
  tail = Chain.Link { values: [11], next: Chain.End }
  fail ChainError.Invalid { chain: Chain.Link { values: [7], next: tail } }

fn sum(a: i64, b: i64) -> i64:
  return a + b

fn main() -> i64 ! TaskError:
  return try parallel:
    result = await spawn rejectChain() on devices.gpu()
    handled = result catch fn(error):
      return match error:
        ChainError.Invalid { chain }:
          match chain:
            Chain.End: 0
            Chain.Link { values, next }:
              match next:
                Chain.End: 0
                Chain.Link { values: tailValues, next: end }:
                  try await spawn sum(values[0], tailValues[0]) on devices.gpu()
        _: 0
    try handled
