"""Build a private arena protocol fixture, not source-language array admission.

The GPU writes a growing arena, then reads every element back to compute its
checksum. Host code only admits storage and copies opaque device bytes.
"""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--llvm', type=Path, required=True)
p.add_argument('--backend', choices=('metal', 'vulkan', 'cuda'), required=True)
p.add_argument('--root', type=Path, required=True)
p.add_argument('--metal', type=Path)
p.add_argument('--sdk', type=Path)
p.add_argument('--cross', default='spirv-cross')
p.add_argument('--validator', default='spirv-val')
a = p.parse_args()
a.root.mkdir(parents=True, exist_ok=True)
lines = []
serial = 0


def emit(text):
    lines.append(text)


def value(expression):
    global serial
    name = f'%v{serial}'
    serial += 1
    emit(f'{name} = {expression}')
    return name


def constant(number, typ='i64'):
    return value(f'arith.constant {number} : {typ}')


def binary(op, left, right, typ='i64'):
    return value(f'arith.{op} {left}, {right} : {typ}')


def compare(pred, left, right):
    return value(f'arith.cmpi {pred}, {left}, {right} : i64')


def load(cell):
    low = value(f'memref.load %params[{constant(1 + cell * 2, "index")}] : memref<16xi32>')
    high = value(f'memref.load %params[{constant(2 + cell * 2, "index")}] : memref<16xi32>')
    lo = value(f'arith.extui {low} : i32 to i64')
    hi = value(f'arith.extui {high} : i32 to i64')
    return binary('ori', lo, binary('shli', hi, constant(32)))


def store(cell, raw):
    low = value(f'arith.trunci {raw} : i64 to i32')
    high = value(f'arith.trunci {binary("shrui", raw, constant(32))} : i64 to i32')
    emit(f'memref.store {low}, %params[{constant(1 + cell * 2, "index")}] : memref<16xi32>')
    emit(f'memref.store {high}, %params[{constant(2 + cell * 2, "index")}] : memref<16xi32>')


emit('module attributes {gpu.container_module, spirv.target_env = '
     '#spirv.target_env<#spirv.vce<v1.0, [Shader, Int64], '
     '[SPV_KHR_storage_buffer_storage_class]>, #spirv.resource_limits<>>} {')
emit('gpu.module @kernels { gpu.func @kalandra_numeric_v1_uint64('
     '%arena:memref<?xi64>, %out:memref<?xi64>, %params:memref<16xi32>) '
     'kernel attributes {spirv.entry_point_abi=#spirv.entry_point_abi<workgroup_size=[64,1,1]>} {')
thread = value('gpu.thread_id x')
zero_index = constant(0, 'index')
active = value(f'arith.cmpi eq, {thread}, {zero_index} : index')
emit(f'scf.if {active} {{')
zero, one, count = constant(0), constant(1), constant(16385)
fresh = compare('eq', load(0), zero)
emit(f'scf.if {fresh} {{')
store(0, one)
emit('}')
limit, step = constant(256, 'index'), constant(1, 'index')
emit(f'scf.for %iteration = {zero_index} to {limit} step {step} {{')
running = compare('eq', load(0), one)
emit(f'scf.if {running} {{')
writing = compare('eq', load(6), zero)
emit(f'scf.if {writing} {{')
index = load(4)
more = compare('ult', index, count)
emit(f'scf.if {more} {{')
capacity = load(1)
grow = compare('uge', index, capacity)
emit(f'scf.if {grow} {{')
store(2, binary('muli', capacity, constant(2)))
store(3, one)
store(0, constant(-2))
emit('} else {')
item = binary('addi', binary('muli', index, constant(17)), constant(23))
at = value(f'arith.index_cast {index} : i64 to index')
emit(f'memref.store {item}, %arena[{at}] : memref<?xi64>')
store(4, binary('addi', index, one))
emit('}')
emit('} else {')
store(6, one)
store(4, zero)
store(5, zero)
emit('}')
emit('} else {')
index = load(4)
more = compare('ult', index, count)
emit(f'scf.if {more} {{')
bounded = compare('ult', index, load(1))
emit(f'scf.if {bounded} {{')
at = value(f'arith.index_cast {index} : i64 to index')
item = value(f'memref.load %arena[{at}] : memref<?xi64>')
store(5, binary('addi', load(5), item))
store(4, binary('addi', index, one))
emit('} else {')
fault = constant(1, 'i32')
emit(f'memref.store {fault}, %params[{constant(15, "index")}] : memref<16xi32>')
store(0, constant(-1))
emit('}')
emit('} else {')
store(0, constant(-1))
emit('}')
emit('}')
emit('}')
emit('scf.yield')
emit('}')
result = load(5)
emit(f'memref.store {result}, %out[{zero_index}] : memref<?xi64>')
emit('}')
emit('gpu.return')
emit('}}}')
source = a.root/'arena.mlir'
source.write_text('\n'.join(lines)+'\n')


def run(args):
    subprocess.run(list(map(str, args)), check=True, timeout=180)


if a.backend == 'cuda':
    run([a.build/'native/cuda-source/kalandra-cuda-lower', source, a.root/'arena.ptx', 'hardware', 'sm_80'])
else:
    if a.backend == 'metal':
        if not a.metal or not a.sdk:
            p.error('Metal requires --metal and --sdk')
        run([a.build/'native/metal-proof/kalandra-metal-strict64', source, a.root/'lowered.mlir', '--hardware'])
    else:
        run([a.build/'native/gpu-source/kalandra-vulkan-lower', source, a.root/'lowered.mlir', 'hardware'])
    run([a.llvm/'bin/mlir-opt', a.root/'lowered.mlir', '--verify-each',
         '--pass-pipeline=builtin.module(canonicalize,cse,convert-gpu-to-spirv,spirv.module(spirv-lower-abi-attrs,spirv-update-vce))',
         '-o', a.root/'converted.mlir'])
    extractor = a.build/('native/metal-proof/kalandra-metal-extract' if a.backend == 'metal' else 'native/gpu-source/kalandra-vulkan-extract')
    run([extractor, a.root/'converted.mlir', a.root/'kernel.mlir'])
    run([a.llvm/'bin/mlir-translate', '--no-implicit-module', '--serialize-spirv',
         a.root/'kernel.mlir', '-o', a.root/'arena.spv'])
    run([a.validator, '--target-env', 'vulkan1.0', a.root/'arena.spv'])
    if a.backend == 'metal':
        run([a.cross, a.root/'arena.spv', '--msl', '--msl-version', '20300', '--output', a.root/'arena.metal'])
        run([a.metal, '-mmacosx-version-min=26.0', '-isysroot', a.sdk, '-std=macos-metal2.3',
             '-fno-fast-math', '-ffp-contract=off', '-fmodules-cache-path='+str(a.root/'module-cache'),
             '-c', a.root/'arena.metal', '-o', a.root/'arena.air'])
        run([a.metal, a.root/'arena.air', '-o', a.root/'arena.metallib'])
print('PASS private device arena fixture compiled; source-language arrays are not established by this probe')
