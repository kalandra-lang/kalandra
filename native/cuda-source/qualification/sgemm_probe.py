"""Explicit bounded physical correctness for pinned full-F32 warp-tile candidates.

No timing, production selection, reset, installation or primary-context access.
"""
import argparse
import ctypes as c
import json
from pathlib import Path
import struct
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--ptx',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--execute',action='store_true',required=True)
a=p.parse_args()
math=c.CDLL('libm.so.6');math.fmaf.argtypes=[c.c_float,c.c_float,c.c_float];math.fmaf.restype=c.c_float
lib=c.CDLL('libcuda.so.1');ptr=c.c_void_p;u64=c.c_uint64;size=c.c_size_t
signatures={'cuInit':[c.c_uint],'cuDeviceGet':[c.POINTER(c.c_int),c.c_int],
 'cuCtxCreate_v2':[c.POINTER(ptr),c.c_uint,c.c_int],'cuCtxDestroy_v2':[ptr],'cuCtxSynchronize':[],
 'cuModuleLoadDataEx':[c.POINTER(ptr),ptr,c.c_uint,ptr,ptr],'cuModuleUnload':[ptr],
 'cuModuleGetFunction':[c.POINTER(ptr),ptr,c.c_char_p],'cuFuncGetParamCount':[ptr,c.POINTER(size)],
 'cuFuncGetParamInfo':[ptr,size,c.POINTER(size),c.POINTER(size)],
 'cuMemAlloc_v2':[c.POINTER(u64),size],'cuMemFree_v2':[u64],
 'cuMemcpyHtoD_v2':[u64,ptr,size],'cuMemcpyDtoH_v2':[ptr,u64,size],
 'cuLaunchKernel':[ptr,c.c_uint,c.c_uint,c.c_uint,c.c_uint,c.c_uint,c.c_uint,c.c_uint,ptr,ptr,ptr]}
for name,args in signatures.items():fn=getattr(lib,name);fn.argtypes=args;fn.restype=c.c_int

def call(name,*args):
    status=getattr(lib,name)(*args)
    if status:raise RuntimeError(f'{name}: {status}')

def admission(tile,m,n,k):
    return tile in (64,128) and min(m,n,k)>0 and not m%tile and not n%tile and not k%16 and max(m*n,m*k,k*n)<2**31 and (m*k+k*n+m*n)*4<=64*1024*1024
for invalid in ((64,63,64,16),(64,64,65,16),(64,64,64,15),(64,0,64,16),(128,64,128,16),(64,2**31,64,16)):
    assert not admission(*invalid)
call('cuInit',0);device=c.c_int();call('cuDeviceGet',c.byref(device),0)
context=ptr();call('cuCtxCreate_v2',c.byref(context),4,device)
records=[]
try:
    code=c.create_string_buffer(a.ptx.read_bytes());module=ptr();call('cuModuleLoadDataEx',c.byref(module),code,0,None,None)
    try:
        for tile in (64,128):
            function=ptr();call('cuModuleGetFunction',c.byref(function),module,f'kalandra_sgemm_warp{tile}'.encode())
            count=size();call('cuFuncGetParamCount',function,c.byref(count));assert count.value==6
            for i,(expected_offset,expected_width) in enumerate(((0,4),(4,4),(8,4),(16,8),(24,8),(32,8))):
                offset=size();width=size();call('cuFuncGetParamInfo',function,i,c.byref(offset),c.byref(width))
                assert (offset.value,width.value)==(expected_offset,expected_width)
            for m,n,k,witness in ((tile,tile,16,True),(tile,2*tile,32,False),(2*tile,tile,64,False),(tile,tile,16,'fma')):
                assert admission(tile,m,n,k)
                if witness=='fma':
                    def value(i):
                        bits=((i%2)<<31)|((121+i%13)<<23)|((i*2654435761)&0x7fffff)
                        return struct.unpack('<f',struct.pack('<I',bits))[0]
                    left=[value(i*17+t*13) for i in range(m) for t in range(k)]
                    right=[value(t*7+j*11+5) for t in range(k) for j in range(n)]
                    expected=[]
                    for i in range(m):
                        for j in range(n):
                            accumulator=0.0
                            for t in range(k):accumulator=math.fmaf(left[i*k+t],right[t*n+j],accumulator)
                            expected.append(accumulator)
                elif witness:
                    left=[(1+(i%11)*2**-23) if t==0 else 0 for i in range(m) for t in range(k)]
                    right=[(1+(j%13)*2**-23) if t==0 else 0 for t in range(k) for j in range(n)]
                    expected=[left[i*k]*right[j] for i in range(m) for j in range(n)]
                else:
                    # Dyadic dot products fit exactly in float32. Integer dot
                    # sums are an independent all-output oracle, including
                    # non-square tiles and multiple contraction iterations.
                    aa=[(i*17+t*13)%19-9 for i in range(m) for t in range(k)]
                    bb=[(t*7+j*11)%23-11 for t in range(k) for j in range(n)]
                    left=[v/16 for v in aa];right=[v/16 for v in bb]
                    expected=[sum(aa[i*k+t]*bb[t*n+j] for t in range(k))/256 for i in range(m) for j in range(n)]
                payloads=[struct.pack('<'+'f'*len(v),*v) for v in (left,right)]
                expected_bytes=struct.pack('<'+'f'*len(expected),*expected)
                # The AB-only adapter must not consume prior NaN C values.
                payloads.append(struct.pack('<I',0x7fc00000)*(m*n))
                allocations=[]
                try:
                    for payload in payloads:
                        device_pointer=u64();call('cuMemAlloc_v2',c.byref(device_pointer),len(payload));allocations.append(device_pointer)
                        host=c.create_string_buffer(payload);call('cuMemcpyHtoD_v2',device_pointer,host,len(payload))
                    words=[c.c_int(m),c.c_int(n),c.c_int(k),*allocations]
                    arguments=(ptr*len(words))(*(c.addressof(v) for v in words))
                    call('cuLaunchKernel',function,n//tile,m//tile,1,128,1,1,0,None,arguments,None);call('cuCtxSynchronize')
                    output=c.create_string_buffer(len(expected_bytes));call('cuMemcpyDtoH_v2',output,allocations[2],len(expected_bytes))
                    assert output.raw==expected_bytes,(tile,m,n,k,'full output mismatch')
                    for pointer,payload in zip(allocations[:2],payloads[:2]):
                        original=c.create_string_buffer(len(payload));call('cuMemcpyDtoH_v2',original,pointer,len(payload));assert original.raw==payload
                    records.append({'tile':tile,'m':m,'n':n,'k':k,'exactCells':m*n,'precisionWitness':witness,'priorOutputNaN':True})
                    a.output.write_text(json.dumps({'timing':False,'activeLanguageKernel':False,'cases':records},indent=2)+'\n')
                finally:
                    call('cuCtxSynchronize')
                    for pointer in reversed(allocations):call('cuMemFree_v2',pointer)
    finally:call('cuModuleUnload',module)
finally:call('cuCtxDestroy_v2',context)
print('PASS:',sum(v['exactCells'] for v in records),'exact candidate outputs; immutable inputs, non-square grids, full-F32 witness, NaN output overwrite and invalid-shape admission')
