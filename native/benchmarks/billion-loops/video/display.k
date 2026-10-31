fn sumRemainders(
  j: int32, count: int32,
  u: int32, total: int32
) -> int32:
  if j >= count: total
  else:
    sumRemainders(j+1, count, u, total+j%u)

fn main() -> int:
  u = args.int32(0)
  rows = int(args.int32(1))
  count = args.int32(2)
  r = random.int32(0, 10000)
  row = fn(index: int) -> int32:
    sumRemainders(0, count, u, 0) + r
  values = generate(rows, row)
  int(r)*4294967296 + int(values[int(r)%rows])
