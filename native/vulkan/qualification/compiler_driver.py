#!/usr/bin/env python3
"""Private adapter for the shared source fixtures; not the release CLI.

Set KALANDRA_VULKAN_BUILD_ROOT and KALANDRA_VULKAN_LLVM_ROOT to already configured
project/toolchain directories. The adapter installs nothing and never substitutes
CPU numerical code for a GPU function.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--arithmetic', choices=('hardware', 'software'), default='hardware')
p.add_argument('--target', choices=('vulkan',), default='vulkan')
optimization = p.add_mutually_exclusive_group()
optimization.add_argument('-O0', dest='optimization', action='store_const', const='0')
optimization.add_argument('-O2', dest='optimization', action='store_const', const='2')
p.add_argument('-o', type=Path, required=True)
p.add_argument('source', type=Path)
a = p.parse_args()
for variable in ('KALANDRA_VULKAN_BUILD_ROOT', 'KALANDRA_VULKAN_LLVM_ROOT'):
    if not os.environ.get(variable):
        p.error('Set ' + variable + ' to the existing qualification directory')
command = [sys.executable, str(Path(__file__).with_name('source_program.py')),
    '--build', os.environ['KALANDRA_VULKAN_BUILD_ROOT'],
    '--llvm-root', os.environ['KALANDRA_VULKAN_LLVM_ROOT'],
    '--source', str(a.source), '--output', str(a.o), '--arithmetic', a.arithmetic,
    '--optimization', a.optimization or '2']
if os.environ.get('KALANDRA_VULKAN_CXX'):
    command += ['--cxx', os.environ['KALANDRA_VULKAN_CXX']]
raise SystemExit(subprocess.run(command).returncode)
