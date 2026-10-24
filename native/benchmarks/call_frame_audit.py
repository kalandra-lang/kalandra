"""Audit emitted AArch64 source frames; never use instrumented timings."""
import argparse
import json
import platform
import re
import subprocess
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / 'fair'))
import protocol as p


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--directories', type=Path, nargs='+', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert sys.platform == 'darwin' and platform.machine() == 'arm64', 'AArch64 audit only'
    assert not args.output.exists()
    llvm = p.CLANG.parent
    artifacts, frames = [], []
    for directory in args.directories:
        for ir in sorted(directory.glob('*.ll')):
            if 'baseline' in ir.stem:
                continue
            binary = ir.with_suffix('')
            if not binary.is_file():
                continue
            symbols = ['_' + name for name in re.findall(
                r'^define [^\n]*@([a-zA-Z_][a-zA-Z0-9_.$]*)\(', ir.read_text(), re.M)]
            nm = subprocess.check_output([llvm / 'llvm-nm', '--defined-only', binary], text=True)
            addresses = {m[3]: m[1] for m in re.finditer(
                r'(?m)^([0-9a-f]+) ([A-Za-z]) (\S+)$', nm)}
            emitted = {symbol: addresses[symbol] for symbol in symbols if symbol in addresses}
            assert emitted, str(binary)
            command = [llvm / 'llvm-objdump', '--disassemble', '--no-show-raw-insn',
                       '--symbolize-operands', '--disassemble-symbols=' + ','.join(emitted), binary]
            assembly = subprocess.check_output(command, text=True)
            blocks = re.split(r'(?m)^([0-9a-f]+) <([^>]+)>:\n', assembly)
            visited = set()
            for offset in range(1, len(blocks), 3):
                address, symbol, body = blocks[offset:offset + 3]
                visited.add(address)
                size, changes = 0, []
                # Sanitizer prologues often reserve through x9/x25, then align
                # and assign SP. Counting only `sub sp, sp` misses that storage.
                derived = {}
                def distance(register):
                    return 0 if register == 'sp' else derived.get(register)
                def reserve(amount, line):
                    nonlocal size
                    if amount > 0:
                        size += amount
                        changes.append(line.strip())
                for line in body.splitlines():
                    push = re.search(r'\[sp, #-(0x[0-9a-f]+|\d+)\]!', line)
                    if push:
                        reserve(int(push[1], 0), line)
                    arithmetic = re.search(
                        r'\b(adds?|subs?)\s+(sp|x\d+), (sp|x\d+), #(0x[0-9a-f]+|\d+)(?:, lsl #(\d+))?', line)
                    move = re.search(r'\bmov\s+(sp|x\d+), (sp|x\d+)\b', line)
                    align = re.search(r'\band\s+(sp|x\d+), (sp|x\d+), #(0x[0-9a-f]+)', line)
                    if arithmetic:
                        operation, target, source, amount, shift = arithmetic.groups()
                        value = distance(source)
                        if value is not None:
                            delta = int(amount, 0) << int(shift or '0')
                            value += delta if operation.startswith('sub') else -delta
                            if target == 'sp': reserve(value, line)
                            else: derived[target] = value
                        else:
                            assert target != 'sp', ('unresolved SP base', symbol, line)
                            derived.pop(target, None)
                        continue
                    if move or align:
                        match = move or align
                        target, source = match[1], match[2]
                        value = distance(source)
                        if value is not None and align:
                            alignment = (-int(align[3], 0)) & ((1 << 64) - 1)
                            assert alignment and not alignment & (alignment - 1), line
                            value += alignment - 1
                        if value is not None:
                            if target == 'sp': reserve(value, line)
                            else: derived[target] = value
                        else:
                            assert target != 'sp', ('unresolved SP assignment', symbol, line)
                            derived.pop(target, None)
                        continue
                    assert not re.search(r'\b(sub|add|and)\s+sp,', line), (
                        'unresolved stack adjustment', symbol, line)
                    if re.search(r'\bblr?\s+', line):
                        for number in range(19): derived.pop('x' + str(number), None)
                    # Register writes invalidate a remembered stack expression.
                    # All selected source frames retain FP. Keep its prologue
                    # expression across epilogues on other linearized CFG paths.
                    write = re.search(r'\b([a-z][a-z0-9.]*)\s+([xw]\d+)(?:, ([xw]\d+))?', line)
                    if write and write[1] not in ('str', 'stur', 'stp', 'stnp', 'cmp', 'cmn', 'tst', 'cbz', 'cbnz', 'tbz', 'tbnz', 'br', 'blr', 'ret'):
                        if write[2][1:] != '29': derived.pop('x' + write[2][1:], None)
                        if write[1] in ('ldp', 'ldnp') and write[3]:
                            if write[3][1:] != '29': derived.pop('x' + write[3][1:], None)
                assert size <= 16384, (str(binary), symbol, size)
                frames.append(dict(binary=len(artifacts), symbol=symbol,
                    aliases=[name for name, value in emitted.items() if value == address],
                    conservative_frame_bytes=size, stack_adjustments=changes,
                    prologue=body.splitlines()[:12]))
            assert set(emitted.values()) <= visited, str(binary)
            artifacts.append(dict(binary=p.key(binary), binary_sha256=p.sha(binary),
                ir_sha256=p.sha(ir), command=list(map(str, command)),
                ir_symbols_not_emitted=[name for name in symbols if name not in addresses]))
    assert frames
    result = dict(scope='Physical Apple ARM64 machine code. Sum of negative static SP '
                  'adjustments and aligned sanitizer stack reservations, conservatively counting mutually exclusive paths. All emitted '
                  'functions defined by selected LLVM files, including checked entries, callbacks '
                  'and mode workers. Aliases share an address. This is evidence for these binaries, '
                  'not a general backend proof. Existing baseline artifacts are excluded.',
                  source_function_budget_bytes=16384, native_stack_reserve_bytes=131072,
                  max_frame_bytes=max(row['conservative_frame_bytes'] for row in frames),
                  artifacts=artifacts, frames=frames)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(len(artifacts), 'binaries;', len(frames), 'frame records; maximum', result['max_frame_bytes'])


if __name__ == '__main__':
    main()
