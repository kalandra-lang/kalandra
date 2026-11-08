enum Tree:
  End
  Node:
    values:[i64]
    tail:Tree
type Wrapped:
  tree:Tree
  adjustment:i64
fn measure(grid:Tensor[i64, ?, ?],first:Tree,scale:i64,second:Wrapped)->i64:
  return match first:
    Tree.End:0
    Tree.Node {values,tail}:
      match second.tree:
        Tree.End:0
        Tree.Node {values:other,tail:unused}:
          values[0]*scale+other[1]+second.adjustment+grid[1,1]
fn main()->i64 ! TaskError:
  values=[7,11]
  tree=Tree.Node {values:values,tail:Tree.End}
  wrapped=Wrapped {tree:tree,adjustment:3}
  return try parallel:
    gpu=devices.gpu()
    grid=tensor([1,2,3,4],[2,2])
    resident=try await transfer(grid,gpu)
    try await spawn measure(resident,tree,10,wrapped) on gpu
