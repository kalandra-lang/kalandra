fn overflow(values:Tensor[int8,?])->Tensor[int8,?]:
  values + 1
fn main()->Tensor[int8,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    values:Tensor[int8,1]=tensor([127])
    resident=try await transfer(values,gpu)
    outcome=parallel:
      result=try await spawn overflow(resident) on gpu
      try await transfer(result,devices.cpu())
    outcome catch fn(error):
      match error:
        TaskError.Fault {kind:cause,message:detail}:
          match cause:
            FaultKind.Arithmetic: tensor([42])
            _: tensor([99])
        _: tensor([98])
