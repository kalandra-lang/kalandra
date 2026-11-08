enum Node:
  End
  Branch:
    value:i64
    children:[Node]
fn score(node:Node)->i64:
  return match node:
    Node.End:1
    Node.Branch {value,children}:value+length(children)
fn work(n:i64)->i64:
  children=[Node.End,Node.Branch {value:5,children:[Node.End]}]
  branch=Node.Branch {value:7,children:children}
  original=[branch,Node.End,branch]
  view=slice(original,1,3)
  joined=concat(view,original)
  total=0
  for i in range(n):
    unused=[branch,Node.Branch {value:i,children:joined}]
    total=total+score(unused[1])
  return total+score(joined[0])+score(joined[1])+score(joined[4])
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn work(300) on devices.gpu()
