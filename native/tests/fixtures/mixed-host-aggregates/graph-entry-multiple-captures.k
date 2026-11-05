enum Tree:
  End
  Node:
    values:[int]
    tail:Tree
type Wrapped:
  tree:Tree
  adjustment:int
fn measure(first:Tree,scale:int,second:Wrapped)->int:
  match first:
    Tree.End:0
    Tree.Node {values,tail}:
      match second.tree:
        Tree.End:0
        Tree.Node {values:other,tail:unused}:
          values[0]*scale+other[1]+second.adjustment
fn main()->int ! TaskError:
  values=[7,11]
  tree=Tree.Node {values:values,tail:Tree.End}
  wrapped=Wrapped {tree:tree,adjustment:3}
  try parallel:
    try await spawn measure(tree,10,wrapped) on devices.gpu()
