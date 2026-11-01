"""Explicit, bounded Driver API launch-ABI probe; not the language runtime.

Requires only the system NVIDIA driver. Does not install tools, reset a device,
retain a primary context, or touch another program's context. No timings.
"""
import argparse,ctypes as c,hashlib,json,pathlib,struct
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--artifacts',type=pathlib.Path,required=True)
p.add_argument('--execute',action='store_true',required=True)
p.add_argument('--device',type=int,default=0)
p.add_argument('--output',type=pathlib.Path,required=True)
p.add_argument('--negative-api-controls',action='store_true')
a=p.parse_args()
lib=c.CDLL('libcuda.so.1')
ptr=c.c_void_p;u64=c.c_uint64;size=c.c_size_t
signatures={
 'cuInit':[c.c_uint], 'cuDriverGetVersion':[c.POINTER(c.c_int)],
 'cuDeviceGetCount':[c.POINTER(c.c_int)],'cuDeviceGet':[c.POINTER(c.c_int),c.c_int],
 'cuDeviceGetName':[ptr,c.c_int,c.c_int],
 'cuDeviceComputeCapability':[c.POINTER(c.c_int),c.POINTER(c.c_int),c.c_int],
 'cuCtxCreate_v2':[c.POINTER(ptr),c.c_uint,c.c_int], 'cuCtxDestroy_v2':[ptr],
 'cuCtxSynchronize':[], 'cuMemAlloc_v2':[c.POINTER(u64),size], 'cuMemFree_v2':[u64],
 'cuMemcpyHtoD_v2':[u64,ptr,size], 'cuMemcpyDtoH_v2':[ptr,u64,size],
 'cuModuleLoadDataEx':[c.POINTER(ptr),ptr,c.c_uint,ptr,ptr],
 'cuModuleUnload':[ptr], 'cuModuleGetFunction':[c.POINTER(ptr),ptr,c.c_char_p],
 'cuFuncGetParamInfo':[ptr,size,c.POINTER(size),c.POINTER(size)],
 'cuFuncGetParamCount':[ptr,c.POINTER(size)],
 'cuLaunchKernel':[ptr,c.c_uint,c.c_uint,c.c_uint,c.c_uint,c.c_uint,c.c_uint,c.c_uint,ptr,ptr,ptr],
}
for name,args in signatures.items():
 fn=getattr(lib,name);fn.argtypes=args;fn.restype=c.c_int

def call(name,*args):
 result=getattr(lib,name)(*args)
 if result:raise RuntimeError(f'{name}: CUDA error {result}')

call('cuInit',0)
version=c.c_int();call('cuDriverGetVersion',c.byref(version))
count=c.c_int();call('cuDeviceGetCount',c.byref(count));assert 0<=a.device<count.value
Device=c.c_int();call('cuDeviceGet',c.byref(Device),a.device)
name=c.create_string_buffer(256);call('cuDeviceGetName',name,len(name),Device)
major=c.c_int();minor=c.c_int();call('cuDeviceComputeCapability',c.byref(major),c.byref(minor),Device)
report={'device':name.value.decode(),'computeCapability':[major.value,minor.value],
        'driverVersion':version.value,'languageRuntime':False,'timing':False,
        'negativeApiControls':a.negative_api_controls,'cases':[]}
context=ptr();call('cuCtxCreate_v2',c.byref(context),4,Device)
try:
 for path in sorted(a.artifacts.glob('*.ptx')):
  data=path.read_bytes();meta=json.loads(path.with_suffix('.ptx.json').read_text())
  assert meta['ptxSHA256']==hashlib.sha256(data).hexdigest()
  assert meta['abi']=='memref1d-descriptor-v1'
  entry,=meta['entries'];buffers=entry['buffers'];assert len(buffers)==4
  assert buffers[-1]=={'elementBits':32,'extent':2}
  bits=buffers[0]['elementBits'];assert bits in (32,64)
  assert all(b=={'elementBits':bits,'extent':-1} for b in buffers[:3])
  code=c.create_string_buffer(data);module=ptr();call('cuModuleLoadDataEx',c.byref(module),code,0,None,None)
  try:
   function=ptr();call('cuModuleGetFunction',c.byref(function),module,entry['name'].encode())
   parameters=size();call('cuFuncGetParamCount',function,c.byref(parameters));assert parameters.value==20
   for index in range(20):
    offset=size();width=size();call('cuFuncGetParamInfo',function,index,c.byref(offset),c.byref(width))
    assert (offset.value,width.value)==(index*8,8)
   if a.negative_api_controls:
    offset=size();width=size()
    assert lib.cuFuncGetParamInfo(function,20,c.byref(offset),c.byref(width))==1
   fmt='f' if bits==32 else 'd'
   def rounded(x):return struct.unpack('<'+fmt,struct.pack('<'+fmt,x))[0]
   for n in (1,3,63,64,65,257,131073):
    left=[rounded((i%31-15)/16) for i in range(n)]
    right=[rounded((i%17-8)/32) for i in range(n)]
    # This multiply/add witness changes by one ulp if contracted to FMA.
    epsilon=2.0**(-23 if bits==32 else -52)
    left[0]=1+epsilon;right[0]=-1+epsilon
    left_bytes=struct.pack('<'+fmt*n,*left);right_bytes=struct.pack('<'+fmt*n,*right)
    expected=struct.pack('<'+fmt*n,*(rounded(rounded(x*y)+x) for x,y in zip(left,right)))
    allocations=[]
    try:
     for payload in (left_bytes,right_bytes,bytes(n*bits//8),struct.pack('<II',n,0)):
      device=u64();call('cuMemAlloc_v2',c.byref(device),len(payload));allocations.append(device)
      host=c.create_string_buffer(payload);call('cuMemcpyHtoD_v2',device,host,len(payload))
     words=[]
     for index,device in enumerate(allocations):
      words.extend(u64(v) for v in (device.value,device.value,0,n if index<3 else 2,1))
     args=(ptr*len(words))(*(c.addressof(v) for v in words))
     block=entry['workgroupX'];call('cuLaunchKernel',function,(n+block-1)//block,1,1,block,1,1,0,None,args,None)
     call('cuCtxSynchronize')
     result=c.create_string_buffer(len(expected));call('cuMemcpyDtoH_v2',result,allocations[2],len(expected))
     assert result.raw==expected,(path.name,n,'output differs')
     call('cuMemcpyDtoH_v2',result,allocations[0],len(left_bytes));assert result.raw==left_bytes
     call('cuMemcpyDtoH_v2',result,allocations[1],len(right_bytes));assert result.raw==right_bytes
     report['cases'].append({'artifact':path.name,'sha256':meta['ptxSHA256'],'elements':n,'bits':bits,'arithmetic':meta['arithmetic']})
    finally:
     call('cuCtxSynchronize')
     for device in reversed(allocations):call('cuMemFree_v2',device)
  finally:call('cuModuleUnload',module)
finally:call('cuCtxDestroy_v2',context)
assert len(report['cases']) in (56,84)
report['checkedOutputElements']=sum(v['elements'] for v in report['cases'])
a.output.write_text(json.dumps(report,indent=2)+'\n')
print(f"PASS: {len(report['cases'])} Driver API launches, {report['checkedOutputElements']} exact outputs, both FP32/FP64 policies and targets; immutable inputs and reflected ABI; language runtime NOT qualified",flush=True)
