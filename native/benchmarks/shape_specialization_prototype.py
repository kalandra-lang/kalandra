"""Bounded, load-time shape PSOs in a copied runtime and transformer bundle.

Only six exact M/K/N triples specialize register16 and its fused epilogues.
No compilation, cache growth, environment lookup, or allocation occurs during
execution. Generic entries handle every other shape and kernel selection.
Production sources remain untouched; this is not a shipping shape policy.
"""
import argparse
import json
import shutil
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent / 'fair'))
import protocol as p
from output_reuse_shader import function

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
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
replace('struct KernelState {', '''static constexpr std::array<std::array<std::uint32_t, 3>, 6> specializedShapes{{
    {16,512,512}, {64,512,2048}, {64,2048,512},
    {16,512,2048}, {16,2048,512}, {64,512,512}}};
struct KernelState {''')
replace('  std::array<id<MTLComputePipelineState>, 6> fusedMatrices{};', '''  std::array<id<MTLComputePipelineState>, 6> fusedMatrices{};
  std::array<id<MTLComputePipelineState>, 6> specializedMatrices{};
  std::array<id<MTLComputePipelineState>, 6> specializedFusedMatrices{};''')
replace('''                             false) -> std::expected<id<MTLComputePipelineState>, Error> {
        id<MTLFunction> function = [library newFunctionWithName:entry];''', '''                             false, MTLFunctionConstantValues *constants = nil)
          -> std::expected<id<MTLComputePipelineState>, Error> {
        id<MTLFunction> function = constants
            ? [library newFunctionWithName:entry constantValues:constants error:&error]
            : [library newFunctionWithName:entry];''')
# Generic prepare() still validates every original ABI. Specializations retain
# the same validation; do not weaken it merely to make a prototype load.
needle = '      kernel.state_ = std::move(metadata);'
assert needle in text
replace(needle, '''      for (unsigned fused = 0; fused < 2; ++fused) {
        NSString *entry = fused ? @"kalandra_matmul_fused_specialized"
                                : @"kalandra_matmul_hardware_specialized";
        if (!(fused ? bool(metadata->fusedMatrices[3]) : bool(metadata->registerMatrix)) ||
            ![library.functionNames containsObject:entry])
          continue;
        auto &pipelines = fused ? metadata->specializedFusedMatrices
                                : metadata->specializedMatrices;
        for (unsigned i = 0; i < specializedShapes.size(); ++i) {
          if (!state_->permit()) return std::unexpected(Error::Allocation);
          auto constants = [MTLFunctionConstantValues new];
          for (unsigned j = 0; j < 3; ++j) {
            auto value = specializedShapes[i][j];
            [constants setConstantValue:&value type:MTLDataTypeUInt atIndex:j];
          }
          bool full = true;
          [constants setConstantValue:&full type:MTLDataTypeBool atIndex:3];
          auto specialized = prepare(entry, fused, !fused, constants);
          if (!specialized) return std::unexpected(specialized.error());
          if ((*specialized).maxTotalThreadsPerThreadgroup < 256)
            return std::unexpected(Error::Unavailable);
          pipelines[i] = *specialized;
        }
      }
'''+needle)
replace('''            [encoder setComputePipelineState:pipeline];
            if (!kernel->indirect()) {''', '''            if (matrix && ((!fusePost && pipeline == kernel->registerMatrix &&
                            kernel->specializedMatrices[0]) ||
                           (fusePost && fusedVariant == 3 &&
                            postKernel.state_->specializedFusedMatrices[0]))) {
              const std::array<std::uint32_t, 3> shape{
                  std::uint32_t(matrixShape[0]), std::uint32_t(source->dimensions[1]),
                  std::uint32_t(matrixShape[1])};
              for (unsigned i = 0; i < specializedShapes.size(); ++i)
                if (shape == specializedShapes[i]) {
                  pipeline = fusePost ? postKernel.state_->specializedFusedMatrices[i]
                                      : kernel->specializedMatrices[i];
                  break;
                }
            }
            [encoder setComputePipelineState:pipeline];
            if (!kernel->indirect()) {''')
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
    for old, new in [('kalandra_matmul_hardware_register16','kalandra_matmul_hardware_specialized'), ('kalandra_matmul_fused_register16','kalandra_matmul_fused_specialized')]:
        if 'kernel void '+old+'(' not in s: continue
        body = function(s, 'kernel void '+old+'(').replace(old+'(',new+'(')
        needle = 'const uint m=parameters.values[0], k=parameters.values[1], n=parameters.values[2];'
        assert body.count(needle) == 1
        additions.append(body.replace(needle, 'const uint m=kalandra_full_shape?kalandra_m:parameters.values[0], k=kalandra_k, n=kalandra_full_shape?kalandra_n:parameters.values[2];'))
    if not additions: continue
    constants = '\nconstant uint kalandra_m [[function_constant(0)]];\nconstant uint kalandra_k [[function_constant(1)]];\nconstant uint kalandra_n [[function_constant(2)]];\nconstant bool kalandra_full_shape [[function_constant(3)]];\n'
    shader.write_text(s+constants+'\n'.join(additions))
    p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',shader,'-o',shader.with_suffix('.air')])
    p.run([*metal,shader.with_suffix('.air'),'-o',shader.with_suffix('.metallib')])
    changed.append(shader.name)
sources = [Path(__file__),fair/'output_reuse_shader.py',source,p.ROOT/'native/metal/Metal.h',fair/'TransformerClosedMetal.cpp',fair/'TransformerBlock.h',program,task,p.ROOT/'native/metal-proof/toolchain.json',*original.glob('*.metal'),*original.glob('*.metallib')]
artifacts = [build/'Metal.mm',build/'TransformerProgram.h',build/'metal.o',build/'gpu',*(build/'bundle').glob('*.metal'),*(build/'bundle').glob('*.metallib')]
(build/'manifest.json').write_text(json.dumps(dict(signature={p.key(f.resolve()):p.sha(f) for f in sources},artifacts={p.key(f):p.sha(f) for f in artifacts},commands=p.COMMANDS,changed_shaders=changed),indent=2)+'\n')
print(build/'gpu')
