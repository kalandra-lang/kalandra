type Batch:
  captured:[int]
  left:[int]
  right:[int]
  independent:[int]
enum Result:
  Empty
  Ready:
    batch:Batch
fn collect(values:[int],enabled:Bool)->Result:
  if enabled:
    fresh=copy(values)
    Result.Ready {batch:Batch {captured:values,left:fresh,right:fresh,independent:copy(values)}}
  else:Result.Empty
fn main()->int ! TaskError:
  values=[2,3]
  try parallel:
    result=try await spawn collect(values,true) on devices.gpu()
    empty=try await spawn collect(values,false) on devices.gpu()
    values[0]=9
    first=match result:
      Result.Empty:0
      Result.Ready {batch}:
        batch.left[1]=8
        batch.captured[0]*100+batch.right[1]*10+batch.independent[1]
    second=match empty:
      Result.Empty:0
      Result.Ready {batch}:batch.captured[0]
    first+second
