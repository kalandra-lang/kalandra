enum Node:
  End
  Branch:
    values:[i64]
    next:Node
fn device(node:Node)->i64:
  return match node:
    Node.End:0
    Node.Branch {values,next}:
      independent=copy(values)
      independent[0]=9
      independent[0]+values[0]
fn main()->i64 ! TaskError:
  node=Node.Branch {values:[1,2],next:Node.End}
  return try parallel:
    try await spawn device(node) on devices.gpu()
