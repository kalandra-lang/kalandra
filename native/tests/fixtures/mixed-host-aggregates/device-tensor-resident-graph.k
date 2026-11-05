enum Tree:
  End
  Node:
    values:[int]
    tail:Tree
type Wrapped:
  tree:Tree
  adjustment:int
fn measure(grid:Tensor[int, ?, ?],first:Tree,scale:int,second:Wrapped)->int:
  match first:
    Tree.End:0
    Tree.Node {values,tail}:
      match second.tree:
        Tree.End:0
        Tree.Node {values:other,tail:unused}:
          values[0]*scale+other[1]+second.adjustment+grid[1,1]
fn main()->int ! TaskError:
  values=[7,11]
  tree=Tree.Node {values:values,tail:Tree.End}
  wrapped=Wrapped {tree:tree,adjustment:3}
  try parallel:
    gpu=devices.gpu()
    grid=tensor([1,2,3,4],[2,2])
    resident=try await transfer(grid,gpu)
    try await spawn measure(resident,tree,10,wrapped) on gpu
