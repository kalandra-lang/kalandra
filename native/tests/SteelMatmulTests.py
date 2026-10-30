"""Real compiler/runtime Steel regression, preserving an explicit fixed-tree mode."""
import argparse
from pathlib import Path
import subprocess

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--compiler',type=Path,required=True)
ap.add_argument('--clang',type=Path,required=True)
ap.add_argument('--output',type=Path,required=True)
ap.add_argument('--quick',action='store_true')
a=ap.parse_args();root=Path(__file__).resolve().parents[2]
compiler=a.compiler.resolve();clang=a.clang.absolute();llvm=clang.parent.parent
a.output.mkdir(parents=True,exist_ok=True)
def run(cmd,ok=True):
 r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=900)
 assert (r.returncode==0)==ok,(cmd,r.returncode,r.stdout,r.stderr)
 return r.stdout
source=root/'native/metal-source/post-matmul.k';bundle=a.output/'optimized'
run([compiler,source,'--target','metal','-O2','-o',bundle])
assert 'matmul_policy=optimized' in (bundle/'manifest.txt').read_text()
assert 'kalandra_matmul_steel_' in (bundle/'kernel0.metal').read_text()
assert 'kalandra_matmul_steel_fused_' in (bundle/'kernel0-post.metal').read_text()
assert (bundle/'THIRD_PARTY_NOTICES.txt').read_bytes()==(root/'native/third_party/mlx-steel/LICENSE').read_bytes()
for policy in ('fixed-tree','software'):
 path=a.output/policy
 flags=['--matmul-policy','fixed-tree'] if policy=='fixed-tree' else ['--arithmetic','software']
 run([compiler,source,'--target','metal','-O2','-o',path,*flags])
 assert 'matmul_policy=fixed-tree' in (path/'manifest.txt').read_text()
 assert 'kalandra_matmul_steel_' not in (path/'kernel0.metal').read_text()
 assert 'kalandra_matmul_steel_' not in (path/'kernel0-post.metal').read_text()
for invalid in (['--matmul-policy','invalid'],['--matmul-policy'],
                ['--matmul-policy','fixed-tree','--matmul-policy','optimized']):
 before=(bundle/'manifest.txt').read_bytes()
 run([compiler,source,'--target','metal','-o',bundle,*invalid],ok=False)
 assert (bundle/'manifest.txt').read_bytes()==before
variants=[('optimized',['-O2'],'libkalandra-task-core-optimized.a')]
if not a.quick:
 variants += [('debug',['-O0'],'libkalandra-native-runtime.a'),
              ('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'],'libkalandra-native-runtime-sanitized.a'),
              ('thread-sanitized',['-O1','-g','-fsanitize=thread','-fno-omit-frame-pointer'],'libkalandra-native-runtime-thread-sanitized.a')]
for variant,flags,core in variants:
 binary=a.output/('runtime-'+variant)
 run([clang,'-std=c++23','-ffp-contract=off','-pthread','-Wall','-Wextra','-Werror',*flags,
      '-isystem',llvm/'include',root/'native/tests/SteelMatmulRuntimeTests.cpp',
      compiler.parent/'metal'/f'libkalandra-metal-{variant}.a',compiler.parent/core,
      '-L'+str(llvm/'lib'),'-lLLVM','-Wl,-rpath,'+str(llvm/'lib'),
      '-framework','Foundation','-framework','Metal','-o',binary])
 print(variant+': '+run([binary,bundle,a.output/'fixed-tree']).strip(),flush=True)
