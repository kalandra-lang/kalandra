"""Generate real shared-memory shuffle cases for every numeric type and rank."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

TYPES = ("i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64")


def shuffle(element, rank):
    dimensions = ", " + ", ".join(["?"] * rank) if rank else ""
    shape = [3] + [2] * (rank - 1) if rank else []
    group = [2] * rank
    lanes = 1 << rank
    lane = " + ".join(f"item.localIndex({axis}) * {1 << (rank - axis - 1)}"
                      for axis in range(rank)) if rank else "0"
    other = f"{lanes - 1} - lane"
    source = f'''from "std/gpu" import dispatch, WorkItem

fn shuffle() -> Tensor[{element}{dimensions}]:
  return dispatch({shape}, {group}, fn(item: WorkItem) -> {element}:
    lane = {lane}
    tile = item.shared[{element}]({lanes})
    tile[lane] = {element}(lane % 3 + 1)
    item.barrier()
    return tile[{other}]
  )
'''
    return source, shape, group


def coordinates(rank):
    shape = [5 if axis == 0 else 7 if axis == 1 else 3 for axis in range(rank)]
    group = [3 if axis == 1 else 2 for axis in range(rank)]
    dimensions = ", " + ", ".join(["?"] * rank) if rank else ""
    body = "    offset = 0\n"
    for axis, extent in enumerate(shape):
        body += f"    offset = offset * {extent} + item.globalIndex({axis})\n"
    source = ('from "std/gpu" import dispatch, WorkItem\n'
              f'fn shuffle() -> Tensor[f32{dimensions}]:\n'
              f'  return dispatch({shape}, {group}, fn(item: WorkItem) -> f32:\n'
              + body + '    return f32(offset + 1)\n  )\n')
    return source, shape, group


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emitter", type=Path, required=True)
    parser.add_argument("--backend", choices=("metal", "vulkan", "cuda"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    cases = []
    for element in TYPES:
        for rank in range(9):
            name = f"{element}-rank-{rank}"
            source, shape, group = shuffle(element, rank)
            path = args.output / (name + ".k")
            path.write_text(source)
            result = subprocess.run([str(args.emitter.resolve()), str(path), "shuffle",
                str(args.output / (name + ".mlir")), args.backend], capture_output=True, text=True)
            (args.output / (name + ".log")).write_text(result.stdout + result.stderr)
            assert result.returncode == 0, (name, result.stdout, result.stderr)
            cases.append({"name": name, "type": element, "rank": rank, "shape": shape, "group": group})
    for element in TYPES:
        name = f"{element}-bit-patterns"
        source = f'''from "std/gpu" import dispatch, WorkItem
fn shuffle(values: Tensor[{element}, 8]) -> Tensor[{element}, ?]:
  return dispatch([9], [8], fn(item: WorkItem) -> {element}:
    lane = item.localIndex(0)
    tile = item.shared[{element}](8)
    tile[lane] = values[lane]
    item.barrier()
    return tile[7 - lane]
  )
'''
        path=args.output/(name+".k");path.write_text(source)
        result=subprocess.run([str(args.emitter.resolve()),str(path),"shuffle",
            str(args.output/(name+".mlir")),args.backend],capture_output=True,text=True)
        (args.output/(name+".log")).write_text(result.stdout+result.stderr)
        assert result.returncode==0,(name,result.stdout,result.stderr)
        cases.append(dict(name=name,type=element,rank=1,shape=[9],group=[8]))
    for rank in range(9):
        name = f"coordinates-rank-{rank}"
        source, shape, group = coordinates(rank)
        path = args.output / (name + ".k")
        path.write_text(source)
        result = subprocess.run([str(args.emitter.resolve()), str(path), "shuffle",
            str(args.output / (name + ".mlir")), args.backend], capture_output=True, text=True)
        (args.output / (name + ".log")).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, (name, result.stdout, result.stderr)
        cases.append(dict(name=name, type="f32", rank=rank, shape=shape, group=group))
    (args.output / "cases.json").write_text(json.dumps(cases, indent=2) + "\n")
    print(f"PASS: {len(cases)} numeric/shared-memory/coordinate kernels emitted for {args.backend}")
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
    from WorkgroupSourceTests import CASES
    controls = {
        "finite-numerical-helper-before-barrier": [3, 2, 1, 0],
        "caught-lane-error-before-barrier": [3, 2, 1, 7],
        "caught-error-union-before-barrier": [23,22,21,30],
        "caught-error-payload": [7, 1, 2, 3],
        "caught-conditional-error-value": [3, 7, 1, 7],
        "failed-argument-skips-shared-helper": [11, 10, 9, 8],
        "varying-failed-argument-skips-store": [106, 198, 198, 100],
        "nested-return-skips-divide-fault": [7, 5, 5, 5],
    }
    for name in controls:
        source, diagnostic = CASES[name]
        assert diagnostic is None
        path = args.output / (name + ".k")
        path.write_text(source)
        result = subprocess.run([str(args.emitter.resolve()), str(path), "test",
            str(args.output / (name + ".mlir")), args.backend], capture_output=True, text=True)
        (args.output / (name + ".log")).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, (name, result.stdout, result.stderr)
    (args.output / "control-cases.json").write_text(json.dumps(controls, indent=2) + "\n")
    tiled=args.output/'tiled.k'
    tiled.write_text((Path(__file__).parent/'workgroup-tiled.k').read_text())
    result=subprocess.run([str(args.emitter.resolve()),str(tiled),'tiled',
        str(args.output/'tiled.mlir'),args.backend],capture_output=True,text=True)
    (args.output/'tiled.log').write_text(result.stdout+result.stderr)
    assert result.returncode==0,('tiled',result.stdout,result.stderr)
    error_source = (Path(__file__).parent / "workgroup-error-export.k").read_text()
    error_source = error_source[:error_source.index('fn report')]
    uniform_source = error_source.replace('    lane = item.localIndex(0)', '''    if count >= 3:
      fail Bad.Failed {index: item.globalIndex(0), observed: f32(item.globalIndex(0) % 7)}
    lane = item.localIndex(0)''')
    fault_source = uniform_source.replace('    if count >= 3:',
        '    ignored = 1 / (count - count)\n    if count >= 3:')
    for name, source in {'error-export': error_source, 'error-uniform': uniform_source,
                         'error-fault-precedence': fault_source}.items():
        path=args.output/(name+'.k');path.write_text(source)
        result=subprocess.run([str(args.emitter.resolve()),str(path),'risky',
            str(args.output/(name+'.mlir')),args.backend],capture_output=True,text=True)
        (args.output/(name+'.log')).write_text(result.stdout+result.stderr)
        assert result.returncode==0,(name,result.stdout,result.stderr)
    dispatch = 'dispatch([4],[4],fn(item: WorkItem)->f32:return f32(1))'
    planning = {
        "transformed-dispatch": ('from "std/gpu" import dispatch, WorkItem\n'
            'fn test()->Tensor[f32,?]:\n  return '+dispatch+'+f32(1)\n', 'unsupported host operation'),
        "multiple-dispatches": ('from "std/gpu" import dispatch, WorkItem\n'
            'fn test()->Tensor[f32,?]:\n  first='+dispatch+'\n  return '+dispatch+'\n', 'one returned dispatch'),
        "returned-other-value": ('from "std/gpu" import dispatch, WorkItem\n'
            'fn test()->Tensor[f32,?]:\n  first='+dispatch+'\n  return tensor([1,2,3,4])\n', 'does not return'),
    }
    for name, (source, diagnostic) in planning.items():
        path = args.output / (name+'.k')
        path.write_text(source)
        result = subprocess.run([str(args.emitter.resolve()),str(path),'test',
            str(args.output/(name+'.mlir')),args.backend],capture_output=True,text=True)
        (args.output/(name+'.log')).write_text(result.stdout+result.stderr)
        assert result.returncode==1 and diagnostic in result.stderr,(name,result.stdout,result.stderr)
    print(f"PASS: caught application-error emission and launch-result provenance for {args.backend}")
