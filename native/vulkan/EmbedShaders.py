"""Embed validated built-in SPIR-V without paths, timestamps or host endianness."""
import argparse
from pathlib import Path
import struct


def generate(root, paths):
    expected = {'add.spv', 'mul_mm.spv'} | {
        f'transforms/transpose-{numeric}.spv' for numeric in
        ('int8', 'uint8', 'int16', 'uint16', 'int32', 'uint32', 'int64', 'uint64', 'float32', 'float64')
    } | {'transforms/softmaxRows-float32.spv', 'transforms/rmsNormRows-float32.spv'}
    resources = {}
    for path in paths:
        name = path.resolve().relative_to(root.resolve()).as_posix()
        if name not in expected or name in resources:
            raise ValueError(f'unexpected or repeated built-in: {name}')
        data = path.read_bytes()
        if not 20 <= len(data) <= 1024 * 1024 or len(data) % 4:
            raise ValueError(f'invalid SPIR-V length: {name}')
        words = struct.unpack(f'<{len(data) // 4}I', data)
        if words[0] != 0x07230203 or not words[3] or words[4]:
            raise ValueError(f'invalid SPIR-V header: {name}')
        resources[name] = words
    if set(resources) != expected:
        raise ValueError(f'missing built-ins: {sorted(expected - resources.keys())}')
    lines = ['// Generated from validated SPIR-V. Do not edit.', '#include "BuiltinShaders.h"',
             'namespace kalandra::runtime::vulkan {', 'namespace {']
    for index, name in enumerate(sorted(resources)):
        lines.append(f'constexpr std::uint32_t shader{index}[] = {{')
        words = resources[name]
        for offset in range(0, len(words), 8):
            lines.append('  ' + ', '.join(f'0x{word:08x}u' for word in words[offset:offset+8]) + ',')
        lines.append('};')
    lines += ['constexpr std::string_view names[] = {']
    lines += [f'  "{name}",' for name in sorted(resources)]
    lines += ['};', '}',
              'std::span<const std::string_view> builtinShaderNames() noexcept { return names; }',
              'std::span<const std::uint32_t> builtinShader(std::string_view name) noexcept {']
    for index, name in enumerate(sorted(resources)):
        lines.append(f'  if (name == names[{index}]) return shader{index};')
    lines += ['  return {};', '}', '}']
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('shaders', nargs='+', type=Path)
    args = parser.parse_args()
    args.output.write_text(generate(args.root, args.shaders))
