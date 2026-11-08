fn selectWideUnsigned(wideUnsigned:Tensor[u64,?],wideSigned:Tensor[i64,?],narrowUnsigned:Tensor[u32,?],narrowSigned:Tensor[i32,?],real:Tensor[f32,?],signedOffset:i64,unsignedOffset:u64) -> Tensor[u64,?]:
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  return exactUnsigned
fn selectWideSigned(wideUnsigned:Tensor[u64,?],wideSigned:Tensor[i64,?],narrowUnsigned:Tensor[u32,?],narrowSigned:Tensor[i32,?],real:Tensor[f32,?],signedOffset:i64,unsignedOffset:u64) -> Tensor[i64,?]:
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  return exactSigned
fn selectNarrowUnsigned(wideUnsigned:Tensor[u64,?],wideSigned:Tensor[i64,?],narrowUnsigned:Tensor[u32,?],narrowSigned:Tensor[i32,?],real:Tensor[f32,?],signedOffset:i64,unsignedOffset:u64) -> Tensor[u32,?]:
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  return narrowU
fn selectNarrowSigned(wideUnsigned:Tensor[u64,?],wideSigned:Tensor[i64,?],narrowUnsigned:Tensor[u32,?],narrowSigned:Tensor[i32,?],real:Tensor[f32,?],signedOffset:i64,unsignedOffset:u64) -> Tensor[i32,?]:
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  return narrowI
fn selectReal(wideUnsigned:Tensor[u64,?],wideSigned:Tensor[i64,?],narrowUnsigned:Tensor[u32,?],narrowSigned:Tensor[i32,?],real:Tensor[f32,?],signedOffset:i64,unsignedOffset:u64) -> Tensor[f32,?]:
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  return floating
fn wideUnsignedProbe() -> Tensor[u64,?] ! TaskError:
  wideUnsigned:Tensor[u64,4] = tensor([0,18446744073709551615,9223372036854775808,9007199254740993])
  wideSigned:Tensor[i64,4] = tensor([-9223372036854775808,9223372036854775807,9007199254740993,-5])
  narrowUnsigned:Tensor[u32,4] = tensor([0,4294967295,2147483648,16777217])
  narrowSigned:Tensor[i32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[f32,4] = tensor([1.5,2.5,3.5,4.5])
  signedOffset:i64 = -9223372036854775808
  unsignedOffset:u64 = 18446744073709551615
  return try parallel:
    wideUnsignedWork = spawn selectWideUnsigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    wideSignedWork = spawn selectWideSigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    narrowUnsignedWork = spawn selectNarrowUnsigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    narrowSignedWork = spawn selectNarrowSigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    realWork = spawn selectReal(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    wideUnsignedResult = try await wideUnsignedWork
    wideSignedResult = try await wideSignedWork
    narrowUnsignedResult = try await narrowUnsignedWork
    narrowSignedResult = try await narrowSignedWork
    realResult = try await realWork
    wideUnsignedResult
fn wideSignedProbe() -> Tensor[i64,?] ! TaskError:
  wideUnsigned:Tensor[u64,4] = tensor([0,18446744073709551615,9223372036854775808,9007199254740993])
  wideSigned:Tensor[i64,4] = tensor([-9223372036854775808,9223372036854775807,9007199254740993,-5])
  narrowUnsigned:Tensor[u32,4] = tensor([0,4294967295,2147483648,16777217])
  narrowSigned:Tensor[i32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[f32,4] = tensor([1.5,2.5,3.5,4.5])
  signedOffset:i64 = -9223372036854775808
  unsignedOffset:u64 = 18446744073709551615
  return try parallel:
    wideUnsignedWork = spawn selectWideUnsigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    wideSignedWork = spawn selectWideSigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    narrowUnsignedWork = spawn selectNarrowUnsigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    narrowSignedWork = spawn selectNarrowSigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    realWork = spawn selectReal(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    wideUnsignedResult = try await wideUnsignedWork
    wideSignedResult = try await wideSignedWork
    narrowUnsignedResult = try await narrowUnsignedWork
    narrowSignedResult = try await narrowSignedWork
    realResult = try await realWork
    wideSignedResult
fn narrowUnsignedProbe() -> Tensor[u32,?] ! TaskError:
  wideUnsigned:Tensor[u64,4] = tensor([0,18446744073709551615,9223372036854775808,9007199254740993])
  wideSigned:Tensor[i64,4] = tensor([-9223372036854775808,9223372036854775807,9007199254740993,-5])
  narrowUnsigned:Tensor[u32,4] = tensor([0,4294967295,2147483648,16777217])
  narrowSigned:Tensor[i32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[f32,4] = tensor([1.5,2.5,3.5,4.5])
  signedOffset:i64 = -9223372036854775808
  unsignedOffset:u64 = 18446744073709551615
  return try parallel:
    wideUnsignedWork = spawn selectWideUnsigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    wideSignedWork = spawn selectWideSigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    narrowUnsignedWork = spawn selectNarrowUnsigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    narrowSignedWork = spawn selectNarrowSigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    realWork = spawn selectReal(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    wideUnsignedResult = try await wideUnsignedWork
    wideSignedResult = try await wideSignedWork
    narrowUnsignedResult = try await narrowUnsignedWork
    narrowSignedResult = try await narrowSignedWork
    realResult = try await realWork
    narrowUnsignedResult
fn narrowSignedProbe() -> Tensor[i32,?] ! TaskError:
  wideUnsigned:Tensor[u64,4] = tensor([0,18446744073709551615,9223372036854775808,9007199254740993])
  wideSigned:Tensor[i64,4] = tensor([-9223372036854775808,9223372036854775807,9007199254740993,-5])
  narrowUnsigned:Tensor[u32,4] = tensor([0,4294967295,2147483648,16777217])
  narrowSigned:Tensor[i32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[f32,4] = tensor([1.5,2.5,3.5,4.5])
  signedOffset:i64 = -9223372036854775808
  unsignedOffset:u64 = 18446744073709551615
  return try parallel:
    wideUnsignedWork = spawn selectWideUnsigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    wideSignedWork = spawn selectWideSigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    narrowUnsignedWork = spawn selectNarrowUnsigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    narrowSignedWork = spawn selectNarrowSigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    realWork = spawn selectReal(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    wideUnsignedResult = try await wideUnsignedWork
    wideSignedResult = try await wideSignedWork
    narrowUnsignedResult = try await narrowUnsignedWork
    narrowSignedResult = try await narrowSignedWork
    realResult = try await realWork
    narrowSignedResult
fn realProbe() -> Tensor[f32,?] ! TaskError:
  wideUnsigned:Tensor[u64,4] = tensor([0,18446744073709551615,9223372036854775808,9007199254740993])
  wideSigned:Tensor[i64,4] = tensor([-9223372036854775808,9223372036854775807,9007199254740993,-5])
  narrowUnsigned:Tensor[u32,4] = tensor([0,4294967295,2147483648,16777217])
  narrowSigned:Tensor[i32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[f32,4] = tensor([1.5,2.5,3.5,4.5])
  signedOffset:i64 = -9223372036854775808
  unsignedOffset:u64 = 18446744073709551615
  return try parallel:
    wideUnsignedWork = spawn selectWideUnsigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    wideSignedWork = spawn selectWideSigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    narrowUnsignedWork = spawn selectNarrowUnsigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    narrowSignedWork = spawn selectNarrowSigned(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    realWork = spawn selectReal(wideUnsigned,wideSigned,narrowUnsigned,narrowSigned,real,signedOffset,unsignedOffset)
    wideUnsignedResult = try await wideUnsignedWork
    wideSignedResult = try await wideSignedWork
    narrowUnsignedResult = try await narrowUnsignedWork
    narrowSignedResult = try await narrowSignedWork
    realResult = try await realWork
    realResult
fn main() -> i64:
  return 0
