"""Freeze the actual production compiler/runtime and transformer driver for acceptance."""
import argparse
import json
import shutil
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import prepared_program
from grid_shader import grid_kernel
from output_reuse_shader import function
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build',type=Path,required=True)
a=parser.parse_args();b=a.build.resolve();b.mkdir(parents=True,exist_ok=False)
fair=Path(__file__).parent/'fair'
compiler=p.ROOT/'build/native/native/kalandra'
source=p.ROOT/'benchmarks/.build/gpu-attention-2026-10-29/acceptance/block-gpu.k'
p.run([compiler,source,'--target','metal','--arithmetic','hardware','-O2','-o',b/'bundle'])
(b/'TransformerProgram.h').write_text(prepared_program.header((b/'bundle/host.cpp').read_text()))
matched=[]
for shader in sorted((b/'bundle').glob('*.metal')):
    text=shader.read_text()
    for entry in ('kalandra_matmul_hardware_register16','kalandra_matmul_fused_register16'):
        if 'kernel void '+entry+'(' not in text: continue
        actual=function(text,'kernel void '+entry+'_grid2d_aligned(')
        assert actual.strip()==grid_kernel(text,entry,True).strip(),shader
        matched.append([shader.name,entry])
assert matched

metal=p.ROOT/'build/native/native/metal/libkalandra-metal-optimized.a'
task=p.ROOT/'build/native/native/libkalandra-task-core-optimized.a'
p.run([p.CLANG,'-std=c++23','-O2','-mmacosx-version-min=26.0','-pthread','-ffp-contract=off','-I'+str(b),'-I'+str(p.ROOT/'native/metal'),'-I'+str(fair),fair/'TransformerClosedMetal.cpp',metal,task,'-framework','Foundation','-framework','Metal','-o',b/'gpu'])
# Persist all generated shaders, including optional fused epilogues, as artifacts.
sources=[Path(__file__),fair/'grid_shader.py',fair/'output_reuse_shader.py',Path(prepared_program.__file__),source,fair/'TransformerClosedMetal.cpp',fair/'TransformerBlock.h',p.ROOT/'native/metal/Metal.mm',p.ROOT/'native/metal/Metal.h',p.ROOT/'native/metal-source/AlignedGridMatmul.h',p.ROOT/'native/metal-source/Build.cpp',p.ROOT/'native/metal-source/MatmulEpilogue.h',p.ROOT/'native/metal-source/HardwareShaders.h',p.ROOT/'native/metal-proof/toolchain.json']
artifacts=[compiler,metal,task,b/'gpu',b/'TransformerProgram.h',b/'bundle/host.cpp',*(b/'bundle').glob('*.metal'),*(b/'bundle').glob('*.metallib')]
(b/'manifest.json').write_text(json.dumps(dict(signature={p.key(f.resolve()):p.sha(f) for f in sources},artifacts={p.key(f):p.sha(f) for f in artifacts},commands=p.COMMANDS,matched_kernel_bodies=matched),indent=2)+'\n')
print(b/'gpu')
