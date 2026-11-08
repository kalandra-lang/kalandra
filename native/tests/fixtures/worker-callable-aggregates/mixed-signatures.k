type Job:
  numbers:[fn(i64)->i64]
  strings:[fn(String)->i64]
fn apply(job:Job)->i64:
  return job.numbers[0](2)+job.strings[0]("hi")
fn main()->i64 ! TaskError:
  values=[38]
  job=Job {numbers:[fn(value):return values[0]+value],strings:[fn(value):return length(value)]}
  return try parallel:
    try await spawn apply(job)
