enum Node:
  End
  Branch:
    value:i64
    children:[Node]
fn score(node:Node)->i64:
  return match node:
    Node.End:1
    Node.Branch {value,children}:value+length(children)
fn device(node:Node, index:i64)->i64:
  return match node:
    Node.End:0
    Node.Branch {value,children}:value+score(children[index])+score(children[0])
fn make()->Node:
  leaf=Node.Branch {value:7,children:[Node.End]}
  values=generate(500,fn(i):return leaf)
  return Node.Branch {value:11,children:values}
fn main()->i64 ! TaskError:
  root=make()
  return try parallel:
    try await spawn device(root,499) on devices.gpu()
