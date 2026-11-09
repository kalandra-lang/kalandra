fn overflow(values:Tensor[i8,?])->Tensor[i8,?]:
  return values + 1
fn main()->Tensor[i8,?] ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    values:Tensor[i8,1]=tensor([127])
    resident=try await transfer(values,gpu)
    outcome=parallel:
      result=try await spawn overflow(resident) on gpu
      try await transfer(result,devices.cpu())
    outcome catch fn(error):
      return match error:
        TaskError.Fault {kind:cause,message:detail}:
          match cause:
            FaultKind.Arithmetic: tensor([42])
            _: tensor([99])
        _: tensor([98])
