"""Copied runtime/bundle using general 2D dispatch and optional aligned tiles.

One extra PSO per eligible plain/fused entry, prepared at module load.
Aligned mode retains the original 1D entry for ragged tiles.
No exact shape whitelist, dynamic compilation or cache is introduced.
"""
import argparse
import json
import shutil
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent / 'fair'))
import protocol as p
from grid_shader import grid_kernel

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--mode', choices=('grid','aligned'), required=True)
a = parser.parse_args()
build = a.build.resolve()
build.mkdir(parents=True, exist_ok=False)
fair = Path(__file__).parent / 'fair'
source = p.ROOT / 'native/metal/Metal.mm'
text = source.read_text()
def replace(old, new):
    global text
    assert text.count(old) == 1, (old, text.count(old))
    text = text.replace(old, new)
replace('  std::array<id<MTLComputePipelineState>, 6> fusedMatrices{};', '''  std::array<id<MTLComputePipelineState>, 6> fusedMatrices{};
  std::array<id<MTLComputePipelineState>, 2> gridMatrices{}, gridFusedMatrices{};''')
needle = '      kernel.state_ = std::move(metadata);'
replace(needle, '''      for (unsigned fused = 0; fused < 2; ++fused) {
        if (!(fused ? bool(metadata->fusedMatrices[3]) : bool(metadata->registerMatrix)))
          continue;
        auto &pipelines = fused ? metadata->gridFusedMatrices : metadata->gridMatrices;
        const std::array<NSString *,2> names = fused
          ? std::array<NSString *,2>{@"kalandra_matmul_fused_register16_grid2d", @"kalandra_matmul_fused_register16_grid2d_aligned"}
          : std::array<NSString *,2>{@"kalandra_matmul_hardware_register16_grid2d", @"kalandra_matmul_hardware_register16_grid2d_aligned"};
        for (unsigned i = 0; i < names.size(); ++i) {
          if (![library.functionNames containsObject:names[i]]) continue;
          if (!state_->permit()) return std::unexpected(Error::Allocation);
          auto preparedGrid = prepare(names[i], fused, !fused);
          if (!preparedGrid) return std::unexpected(preparedGrid.error());
          if ((*preparedGrid).maxTotalThreadsPerThreadgroup < 256)
            return std::unexpected(Error::Unavailable);
          pipelines[i] = *preparedGrid;
        }
      }
'''+needle)
replace('''            [encoder setComputePipelineState:pipeline];
            if (!kernel->indirect()) {''', '''            bool grid2D = false;
            if (matrix && ((!fusePost && pipeline == kernel->registerMatrix) ||
                           (fusePost && fusedVariant == 3))) {
              const auto &grids = fusePost ? postKernel.state_->gridFusedMatrices : kernel->gridMatrices;
              const bool aligned = matrixShape[0] % 16 == 0 && matrixShape[1] % 16 == 0;
              if (aligned && grids[1]) { pipeline = grids[1]; grid2D = true; }
              else if (grids[0]) { pipeline = grids[0]; grid2D = true; }
            }
            [encoder setComputePipelineState:pipeline];
            if (!kernel->indirect()) {''')
replace('[encoder dispatchThreadgroups:MTLSizeMake((lanes + threads - 1) / threads, 1, 1)',
        '[encoder dispatchThreadgroups:(grid2D ? MTLSizeMake((std::uint64_t(parameters[2])+15)/16, (std::uint64_t(parameters[0])+15)/16, 1) : MTLSizeMake((lanes + threads - 1) / threads, 1, 1))')
(build / 'Metal.mm').write_text(text)
program = p.ROOT/'benchmarks/.build/gpu-closed-entry-2026-10-29/acceptance/TransformerProgram.h'
shutil.copy2(program, build/'TransformerProgram.h')
task = p.ROOT/'build/native/native/libkalandra-task-core-optimized.a'
flags = ['-std=c++23', '-O2', '-mmacosx-version-min=26.0']
p.run([p.CLANG,*flags,'-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs','-I'+str(p.ROOT/'native/metal'),'-c',build/'Metal.mm','-o',build/'metal.o'])
p.run([p.CLANG,*flags,'-pthread','-ffp-contract=off','-I'+str(build),'-I'+str(p.ROOT/'native/metal'),'-I'+str(fair),fair/'TransformerClosedMetal.cpp',build/'metal.o',task,'-framework','Foundation','-framework','Metal','-o',build/'gpu'])
original = p.ROOT/'benchmarks/.build/gpu-attention-2026-10-29/acceptance/bundle'
shutil.copytree(original, build/'bundle')
lock = json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text())
metal = ['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
changed = []
for shader in sorted((build/'bundle').glob('*.metal')):
    s = shader.read_text()
    additions = []
    for entry in ('kalandra_matmul_hardware_register16','kalandra_matmul_fused_register16'):
        if 'kernel void '+entry+'(' not in s: continue
        additions.append(grid_kernel(s,entry,a.mode=='aligned'))
    if not additions: continue
    shader.write_text(s+'\n'+'\n'.join(additions))
    p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',shader,'-o',shader.with_suffix('.air')])
    p.run([*metal,shader.with_suffix('.air'),'-o',shader.with_suffix('.metallib')])
    changed.append(shader.name)
sources = [Path(__file__),fair/'output_reuse_shader.py',fair/'grid_shader.py',source,p.ROOT/'native/metal/Metal.h',fair/'TransformerClosedMetal.cpp',fair/'TransformerBlock.h',program,task,p.ROOT/'native/metal-proof/toolchain.json',*original.glob('*.metal'),*original.glob('*.metallib')]
artifacts = [build/'Metal.mm',build/'TransformerProgram.h',build/'metal.o',build/'gpu',*(build/'bundle').glob('*.metal'),*(build/'bundle').glob('*.metallib')]
(build/'manifest.json').write_text(json.dumps(dict(signature={p.key(f.resolve()):p.sha(f) for f in sources},artifacts={p.key(f):p.sha(f) for f in artifacts},commands=p.COMMANDS,changed_shaders=changed,mode=a.mode),indent=2)+'\n')
print(build/'gpu')
