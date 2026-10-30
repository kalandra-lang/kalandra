type Tree:
  name: String
  children: [Tree]

enum Forest:
  End
  More:
    roots: [Tree]
    next: Forest

fn first(forest: Forest) -> String:
  match forest:
    Forest.End: ""
    Forest.More { roots }: roots[0].children[0].name

fn main() -> String:
  leaf = Tree { name: "hello", children: [] }
  tree = Tree { name: "root", children: [leaf] }
  inner = Forest.More { roots: [tree], next: Forest.End }
  outer = Forest.More { roots: [], next: inner }
  match outer:
    Forest.End: ""
    Forest.More { next }: first(next)
