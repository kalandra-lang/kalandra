"""CPU-only CUDA artifact/ABI proof; never claims NVIDIA execution."""
import hashlib,json,pathlib,re,subprocess,sys,tempfile
compiler=pathlib.Path(sys.argv[1]).resolve()
fixture=pathlib.Path(__file__).with_name('elementwise.mlir').read_text()
with tempfile.TemporaryDirectory(prefix='kalandra cuda ABI λ ') as folder:
 root=pathlib.Path(folder)
 for chip in ('sm_80','sm_120'):
  for element in ('f32','f64'):
   for policy in ('hardware','software','hardware-with-software-fallback'):
    source=root/'input.mlir';source.write_text(fixture.replace('f32',element))
    output=root/'kernel.ptx'
    command=[str(compiler),str(source),str(output),policy,chip]
    subprocess.run(command,check=True,capture_output=True,text=True)
    ptx=output.read_text();metadata=json.loads(output.with_suffix('.ptx.json').read_text())
    assert metadata['ptxSHA256']==hashlib.sha256(output.read_bytes()).hexdigest()
    assert metadata['abi']=='memref1d-descriptor-v1' and metadata['chip']==chip
    assert '.version 8.7' in ptx and '.target '+chip in ptx
    assert len(re.findall(r'\.param \.u64',ptx))==20
    entry,=metadata['entries'];assert entry['workgroupX']==64 and len(entry['buffers'])==4
    assert entry['buffers'][-1]=={'elementBits':32,'extent':2}
    assert all(b=={'elementBits':int(element[1:]),'extent':-1} for b in entry['buffers'][:-1])
    assert '.extern .func' not in ptx and 'tf32' not in ptx and 'mma.sync' not in ptx
    if policy=='hardware' or (element=='f32' and policy=='hardware-with-software-fallback'):
     assert 'mul.rn.'+element in ptx and 'add.rn.'+element in ptx
     assert not re.search(r'\bfma\.[^\s]*'+element,ptx)
    else:
     assert not re.search(r'\b(?:add|sub|mul|div|fma|cvt)\.[^\s]*'+element,ptx)
    before=output.read_bytes();json_before=output.with_suffix('.ptx.json').read_bytes()
    subprocess.run(command,check=True,capture_output=True,text=True)
    assert output.read_bytes()==before and output.with_suffix('.ptx.json').read_bytes()==json_before
    for bad_source,bad_policy,bad_chip in [(fixture.replace('memref<?xf32>','memref<?x?xf32>'),'hardware',chip),
                                         ('not MLIR','hardware',chip),
                                         (fixture,'invalid',chip),(fixture,'hardware','sm_invalid')]:
     source.write_text(bad_source)
     result=subprocess.run([str(compiler),str(source),str(output),bad_policy,bad_chip],capture_output=True,text=True)
     assert result.returncode!=0
     assert output.read_bytes()==before and output.with_suffix('.ptx.json').read_bytes()==json_before
# Compile the existing fixed-tree matrix algorithm's private scratch, without
# a CUDA SDK or libdevice. This catches accidental SPIR-V address-space leakage.
with tempfile.TemporaryDirectory(prefix='kalandra CUDA scratch ') as folder:
 root=pathlib.Path(folder)
 header=pathlib.Path(__file__).parents[2].joinpath('metal-source/MatmulShader.h').read_text()
 source=root/'matmul.mlir';source.write_text(header.split('R"(',1)[1].rsplit(')";',1)[0])
 output=root/'matmul.ptx'
 subprocess.run([str(compiler),str(source),str(output),'hardware','sm_80'],check=True,capture_output=True)
 ptx=output.read_text()
 assert '.entry kalandra_matmul_cuda_desc_v1' in ptx and 'mul.rn.f32' in ptx and 'add.rn.f32' in ptx
 assert '.extern .func' not in ptx
print('PASS: twelve reproducible CUDA PTX artifacts; descriptor ABI, native precision, explicit floating software policy and fail-closed output protection; GPU execution NOT tested')
