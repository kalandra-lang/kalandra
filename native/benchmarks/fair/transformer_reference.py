"""Independent full-block oracle and propagated float32 forward error bounds.

The nominal reference uses separately rounded products and an adjacent-pair
tree, without BLAS. The float64 mathematical reference and per-cell bounds
permit float32 library FMA/reassociation and documented exp error, then propagate
each stage's error through normalization, softmax, ReLU, and residuals.
"""
import functools
import math
import numpy as np
from ffn_reference import tree,strict_matmul
U=2.0**-24
def gamma(n):return n*U/(1-n*U)
def inputs(m,d,pattern):
    arrays=[]
    for stream,shape in enumerate([(m,d)]+[(d,d)]*4+[(d,4*d),(4*d,d)]):
        z=np.arange(math.prod(shape),dtype=np.uint32)+np.uint32((0x9e3779b9*(stream+1))&0xffffffff)
        z=(z^(z>>16))*np.uint32(0x85ebca6b);z=(z^(z>>13))*np.uint32(0xc2b2ae35);z^=z>>16
        denominator=(127 if stream else 29) if pattern else (128 if stream else 32)
        arrays.append(((z%17).astype(np.int32)-8).astype(np.float32).reshape(shape)/np.float32(denominator))
    return arrays
def rms32(x,epsilon):
    denominator=np.sqrt((tree(x*x,axis=1)/np.float32(x.shape[1]))+epsilon)
    return x/denominator[:,None]
def softmax32(x):
    shifted=x-np.max(x,axis=1,keepdims=True)
    exponent=np.exp(shifted.astype(np.float64)).astype(np.float32)
    return exponent/tree(exponent,axis=1)[:,None]
def rms(value,error,epsilon):
    n=value.shape[1];squares=value*value
    mean=np.mean(squares,axis=1,keepdims=True)
    propagated=np.mean((2*np.abs(value)+error)*error,axis=1,keepdims=True)
    total_error=propagated+gamma(2*n+4)*(mean+propagated+epsilon)
    denominator=np.sqrt(mean+epsilon)
    lower=np.sqrt(np.maximum(epsilon,mean+epsilon-total_error))
    upper=np.sqrt(mean+epsilon+total_error)
    low=np.minimum((value-error)/lower,(value-error)/upper)
    high=np.maximum((value+error)/lower,(value+error)/upper)
    result=value/denominator
    bound=np.maximum(np.abs(result-low),np.abs(high-result))+gamma(4)*np.abs(result)
    return result,bound
def softmax(value,error):
    maximum=np.max(value,axis=1,keepdims=True)
    exponent=np.exp(value-maximum);result=exponent/np.sum(exponent,axis=1,keepdims=True)
    # Outward input/max/subtraction intervals, plus four-ULP exp uncertainty.
    rounding=gamma(3)*(np.abs(value)+np.abs(maximum))
    low=np.exp(value-error-np.max(value+error,axis=1,keepdims=True)-rounding)*(1-8*U)
    high=np.exp(value+error-np.max(value-error,axis=1,keepdims=True)+rounding)*(1+8*U)
    n=value.shape[1]
    lower=low/(np.sum(high,axis=1,keepdims=True)*(1+gamma(2*n+3)))
    upper=high/(np.sum(low,axis=1,keepdims=True)*(1-gamma(2*n+3)))
    return result,np.maximum(np.abs(result-lower),np.abs(upper-result))
def mm(a,b):
    x,ex=a;y,ey=b
    magnitude=np.abs(x)@np.abs(y)
    propagated=np.abs(x)@ey+ex@np.abs(y)+ex@ey
    return x@y,propagated+gamma(2*x.shape[1]+4)*(magnitude+propagated)
def add(a,b):
    x,ex=a;y,ey=b;result=x+y
    return result,ex+ey+gamma(2)*(np.abs(x)+np.abs(y)+ex+ey)
@functools.lru_cache(maxsize=1)
def oracle(m,d,pattern):
    x,wq,wk,wv,wo,up,down=inputs(m,d,pattern)
    epsilon=np.float32(.00001);scale=np.float32(1/math.sqrt(d))
    n=rms32(x,epsilon);q=strict_matmul(n,wq);k=strict_matmul(n,wk);v=strict_matmul(n,wv)
    p=softmax32(strict_matmul(q,k.T)*scale)
    residual=x+strict_matmul(strict_matmul(p,v),wo)
    hidden=np.maximum(strict_matmul(rms32(residual,epsilon),up),np.float32(0))
    nominal=residual+strict_matmul(hidden,down)
    arrays=[(a.astype(np.float64),np.zeros(a.shape,np.float64)) for a in (x,wq,wk,wv,wo,up,down)]
    x,wq,wk,wv,wo,up,down=arrays
    n=rms(*x,float(epsilon));q=mm(n,wq);k=mm(n,wk);v=mm(n,wv)
    score,error=mm(q,(k[0].T,k[1].T));error=error*abs(float(scale))+gamma(2)*np.abs(score*float(scale));score*=float(scale)
    probability=softmax(score,error)
    residual=add(x,mm(mm(probability,v),wo))
    hidden,error=mm(rms(*residual,float(epsilon)),up);hidden=np.maximum(hidden,0)
    high,bound=add(residual,mm((hidden,error),down))
    # Also cover the float64 oracle's much smaller arithmetic error.
    return nominal,high,2*bound+np.finfo(np.float32).tiny
def assess(m,d,pattern,row):
    nominal,high,bound=oracle(m,d,pattern)
    actual=np.asarray(row['cells'],np.float32).reshape(m,d)
    assert np.isfinite(actual).all()
    error=np.abs(actual.astype(np.float64)-high)
    assert np.all(error<=bound),(m,d,pattern,float(np.max(error/bound)))
    assert float(tree(actual.ravel()))==row['result'],(row['result'],float(tree(actual.ravel())))
    return dict(cells=m*d,max_abs_error=float(np.max(error)),max_forward_bound=float(np.max(bound)),
                max_error_bound_ratio=float(np.max(error/bound)),nominal_matching_cells=int(np.sum(actual.view(np.uint32)==nominal.view(np.uint32))),
                max_difference_nominal=float(np.max(np.abs(actual.astype(np.float64)-nominal.astype(np.float64))))),actual
