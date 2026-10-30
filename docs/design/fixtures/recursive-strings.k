enum Texts:
  Empty
  More:
    text: String
    next: Texts

fn join(items: Texts) -> String:
  match items:
    Texts.Empty: ""
    Texts.More { text, next }: text + join(next)

fn main() -> String:
  tail = Texts.More { text: "world", next: Texts.Empty }
  greeting = Texts.More { text: "Hello, ", next: tail }
  join(greeting)
