enum Chain:
  End
  Link:
    values: [int]
    next: Chain

error ChainError:
  Invalid:
    chain: Chain

fn rejectChain() -> int ! ChainError:
  tail = Chain.Link { values: [11], next: Chain.End }
  fail ChainError.Invalid { chain: Chain.Link { values: [7], next: tail } }

fn sum(a: int, b: int) -> int:
  a + b

fn main() -> int ! TaskError:
  try parallel:
    result = await spawn rejectChain() on devices.gpu()
    handled = result catch fn(error):
      match error:
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
