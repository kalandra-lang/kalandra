fn negate(value:Bool)->Bool:
  if value:false
  else:true
fn run(value:Bool)->Bool ! TaskError:
  try parallel:
    try await spawn negate(value)
fn main()->int ! TaskError:
  callback=run
  selected=try callback(false)
  if selected:42
  else:0
