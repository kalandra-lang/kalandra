fn main()->int ! TaskError:
  try parallel:
    work=spawn map([40],fn(offset):fn(value:int):offset+value)
    first=try await work
    second=try await work
    first[0](1)+second[0](1)-40
