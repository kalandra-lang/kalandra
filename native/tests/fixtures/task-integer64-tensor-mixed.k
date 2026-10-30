fn selectWideUnsigned(wideUnsigned:Tensor[uint64,?],wideSigned:Tensor[int64,?],narrowUnsigned:Tensor[uint32,?],narrowSigned:Tensor[int32,?],real:Tensor[float32,?],signedOffset:int64,unsignedOffset:uint64) -> Tensor[uint64,?]:
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  exactUnsigned
fn selectWideSigned(wideUnsigned:Tensor[uint64,?],wideSigned:Tensor[int64,?],narrowUnsigned:Tensor[uint32,?],narrowSigned:Tensor[int32,?],real:Tensor[float32,?],signedOffset:int64,unsignedOffset:uint64) -> Tensor[int64,?]:
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  exactSigned
fn selectNarrowUnsigned(wideUnsigned:Tensor[uint64,?],wideSigned:Tensor[int64,?],narrowUnsigned:Tensor[uint32,?],narrowSigned:Tensor[int32,?],real:Tensor[float32,?],signedOffset:int64,unsignedOffset:uint64) -> Tensor[uint32,?]:
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  narrowU
fn selectNarrowSigned(wideUnsigned:Tensor[uint64,?],wideSigned:Tensor[int64,?],narrowUnsigned:Tensor[uint32,?],narrowSigned:Tensor[int32,?],real:Tensor[float32,?],signedOffset:int64,unsignedOffset:uint64) -> Tensor[int32,?]:
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  narrowI
fn selectReal(wideUnsigned:Tensor[uint64,?],wideSigned:Tensor[int64,?],narrowUnsigned:Tensor[uint32,?],narrowSigned:Tensor[int32,?],real:Tensor[float32,?],signedOffset:int64,unsignedOffset:uint64) -> Tensor[float32,?]:
  exactSigned = wideSigned + (signedOffset - -9223372036854775808)
  exactUnsigned = wideUnsigned + (unsignedOffset - 18446744073709551615)
  narrowU = narrowUnsigned + 0
  narrowI = narrowSigned + 0
  floating = real + 0.5
  floating
fn wideUnsignedProbe() -> Tensor[uint64,?] ! TaskError:
  wideUnsigned:Tensor[uint64,4] = tensor([0,18446744073709551615,9223372036854775808,9007199254740993])
  wideSigned:Tensor[int64,4] = tensor([-9223372036854775808,9223372036854775807,9007199254740993,-5])
  narrowUnsigned:Tensor[uint32,4] = tensor([0,4294967295,2147483648,16777217])
  narrowSigned:Tensor[int32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[float32,4] = tensor([1.5,2.5,3.5,4.5])
  signedOffset:int64 = -9223372036854775808
  unsignedOffset:uint64 = 18446744073709551615
  try parallel:
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
fn wideSignedProbe() -> Tensor[int64,?] ! TaskError:
  wideUnsigned:Tensor[uint64,4] = tensor([0,18446744073709551615,9223372036854775808,9007199254740993])
  wideSigned:Tensor[int64,4] = tensor([-9223372036854775808,9223372036854775807,9007199254740993,-5])
  narrowUnsigned:Tensor[uint32,4] = tensor([0,4294967295,2147483648,16777217])
  narrowSigned:Tensor[int32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[float32,4] = tensor([1.5,2.5,3.5,4.5])
  signedOffset:int64 = -9223372036854775808
  unsignedOffset:uint64 = 18446744073709551615
  try parallel:
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
fn narrowUnsignedProbe() -> Tensor[uint32,?] ! TaskError:
  wideUnsigned:Tensor[uint64,4] = tensor([0,18446744073709551615,9223372036854775808,9007199254740993])
  wideSigned:Tensor[int64,4] = tensor([-9223372036854775808,9223372036854775807,9007199254740993,-5])
  narrowUnsigned:Tensor[uint32,4] = tensor([0,4294967295,2147483648,16777217])
  narrowSigned:Tensor[int32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[float32,4] = tensor([1.5,2.5,3.5,4.5])
  signedOffset:int64 = -9223372036854775808
  unsignedOffset:uint64 = 18446744073709551615
  try parallel:
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
fn narrowSignedProbe() -> Tensor[int32,?] ! TaskError:
  wideUnsigned:Tensor[uint64,4] = tensor([0,18446744073709551615,9223372036854775808,9007199254740993])
  wideSigned:Tensor[int64,4] = tensor([-9223372036854775808,9223372036854775807,9007199254740993,-5])
  narrowUnsigned:Tensor[uint32,4] = tensor([0,4294967295,2147483648,16777217])
  narrowSigned:Tensor[int32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[float32,4] = tensor([1.5,2.5,3.5,4.5])
  signedOffset:int64 = -9223372036854775808
  unsignedOffset:uint64 = 18446744073709551615
  try parallel:
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
fn realProbe() -> Tensor[float32,?] ! TaskError:
  wideUnsigned:Tensor[uint64,4] = tensor([0,18446744073709551615,9223372036854775808,9007199254740993])
  wideSigned:Tensor[int64,4] = tensor([-9223372036854775808,9223372036854775807,9007199254740993,-5])
  narrowUnsigned:Tensor[uint32,4] = tensor([0,4294967295,2147483648,16777217])
  narrowSigned:Tensor[int32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[float32,4] = tensor([1.5,2.5,3.5,4.5])
  signedOffset:int64 = -9223372036854775808
  unsignedOffset:uint64 = 18446744073709551615
  try parallel:
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
fn main() -> int64:
  0
