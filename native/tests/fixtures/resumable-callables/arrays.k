fn double(value:int)->int:
  value*2
fn run(value:int)->int ! TaskError:
  try parallel:
    try await spawn double(value)
fn other(value:int)->int ! TaskError:
  value*2
fn main()->int ! TaskError:
  callbacks=[run,other]
  first=try callbacks[0](10)
  second=try callbacks[1](11)
  first+second
