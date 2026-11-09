# Bias-free single-head unmasked pre-RMSNorm encoder block, unit RMS gain.
# This is a complete block, not a tokenizer, KV cache, or generation engine.
fn block(x:Tensor[f32,?,?],wq:Tensor[f32,?,?],wk:Tensor[f32,?,?],wv:Tensor[f32,?,?],wo:Tensor[f32,?,?],up:Tensor[f32,?,?],down:Tensor[f32,?,?],epsilon:f32,scale:f32)->Tensor[f32,?,?]:
  norm=rmsNormRows(x,epsilon)
  q=matmul(norm,wq)
  k=matmul(norm,wk)
  v=matmul(norm,wv)
  probabilities=softmaxRows(matmul(q,transpose(k))*scale)
  residual=x+matmul(matmul(probabilities,v),wo)
  feed=rmsNormRows(residual,epsilon)
  return residual+matmul(relu(matmul(feed,up)),down)

fn main()->i64:
  return 0
