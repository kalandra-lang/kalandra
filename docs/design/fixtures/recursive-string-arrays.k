enum Names:
  End
  Group:
    values: [String]
    next: Names

fn first(names: Names) -> [String]:
  match names:
    Names.End: []
    Names.Group { values }: values

fn main() -> [String]:
  tail = Names.Group { values: ["é", "", "👩"], next: Names.End }
  names = Names.Group { values: ["hello"], next: tail }
  match names:
    Names.End: []
    Names.Group { next }: first(next)
