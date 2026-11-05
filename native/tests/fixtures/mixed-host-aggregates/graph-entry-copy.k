enum Node:
  End
  Branch:
    values:[int]
    next:Node
fn device(node:Node)->int:
  match node:
    Node.End:0
    Node.Branch {values,next}:
      independent=copy(values)
      independent[0]=9
      independent[0]+values[0]
fn main()->int ! TaskError:
  node=Node.Branch {values:[1,2],next:Node.End}
  try parallel:
    try await spawn device(node) on devices.gpu()
