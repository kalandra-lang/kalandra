fn main()->i64 ! TaskError:
  return try parallel:
    work=spawn map([40],fn(offset):return fn(value:i64):return offset+value)
    first=try await work
    second=try await work
    first[0](1)+second[0](1)-40
