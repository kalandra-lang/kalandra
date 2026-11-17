"""Source-level cooperative admission regressions; device execution is a separate gate."""
import argparse
import json
from pathlib import Path
import subprocess
import textwrap


def kernel(body, *, size=4, parameters="", prefix="", result="f32"):
    return (
        'from "std/gpu" import dispatch, WorkItem, Shared\n'
        + prefix
        + f"\nfn test({parameters}) -> Tensor[{result}, ?]:\n"
        + f"  return dispatch([{size}], [{size}], fn(item: WorkItem) -> {result}:\n"
        + textwrap.indent(textwrap.dedent(body).strip(), "    ")
        + "\n  )\n\nfn main() -> unit:\n  return ()\n"
    )


CASES = {
    "owned-stencil": (kernel("""
        lane = item.localIndex(0)
        tile = item.shared[f32](6)
        tile[lane + 1] = f32(1)
        if lane == 0:
          tile[0] = f32(1)
        if lane == 3:
          tile[5] = f32(1)
        item.barrier()
        return tile[lane] + tile[lane + 1] + tile[lane + 2]
    """), None),
    "write-race": (kernel("""
        tile = item.shared[f32](1)
        tile[0] = f32(1)
        item.barrier()
        return tile[0]
    """), "conflicting writes"),
    "uninitialized": (kernel("""
        tile = item.shared[f32](4)
        item.barrier()
        return tile[item.localIndex(0)]
    """), "initialization"),
    "shared-bounds": (kernel("""
        tile = item.shared[f32](4)
        tile[4] = f32(1)
        item.barrier()
        return f32(0)
    """), "outside its allocation"),
    "negative-shared-index": (kernel("""
        tile = item.shared[f32](4)
        return tile[-1]
    """), "bounds"),
    "varying-barrier": (kernel("""
        if item.localIndex(0) == 0:
          item.barrier()
        return f32(0)
    """), "barrier"),
    "distinct-barriers": (kernel("""
        if item.localIndex(0) == 0:
          item.barrier()
        else:
          item.barrier()
        return f32(0)
    """), "barrier sites"),
    "conditional-own-write-read": (kernel("""
        lane = item.localIndex(0)
        tile = item.shared[f32](4)
        if values[lane] > f32(0):
          tile[lane] = f32(1)
          observed = tile[lane]
        item.barrier()
        return f32(0)
    """, parameters="values: Tensor[f32, ?]"), None),
    "conditional-uninitialized-publication": (kernel("""
        lane = item.localIndex(0)
        tile = item.shared[f32](4)
        if values[lane] > f32(0):
          tile[lane] = f32(1)
        item.barrier()
        return tile[lane]
    """, parameters="values: Tensor[f32, ?]"), "initialization"),
    "early-return-before-barrier": (kernel("""
        if item.localIndex(0) == 0:
          return f32(0)
        item.barrier()
        return f32(1)
    """), "barrier traces"),
    "early-return-after-barrier": (kernel("""
        item.barrier()
        if item.localIndex(0) == 0:
          return f32(0)
        return f32(1)
    """), None),
    "unknown-return-after-barrier": (kernel("""
        item.barrier()
        if values[item.localIndex(0)] > f32(0):
          return f32(0)
        return f32(1)
    """, parameters="values: Tensor[f32, ?]"), None),
    "helper-return": (kernel("""
        value = pick(item)
        item.barrier()
        return value
    """, prefix="""fn pick(item: WorkItem) -> f32:
  if item.localIndex(0) == 0:
    return f32(0)
  return f32(1)
"""), None),
    "helper-shared": (kernel("""
        tile = item.shared[f32](4)
        fill(item, tile)
        item.barrier()
        return tile[item.localIndex(0)]
    """, prefix="""fn fill(item: WorkItem, tile: Shared[f32]) -> unit:
  tile[item.localIndex(0)] = f32(1)
  return ()
"""), None),
    "helper-index": (kernel("""
        lane = item.localIndex(0)
        tile = item.shared[f32](4)
        tile[slot(lane)] = f32(1)
        item.barrier()
        return tile[lane]
    """, prefix="""fn slot(index: i64) -> i64:
  return (index + 1) % 4
"""), None),
    "range-nonzero-start": (kernel("""
        tile = item.shared[f32](4)
        if item.localIndex(0) == 0:
          for index in range(4, 8):
            tile[index] = f32(1)
        item.barrier()
        return f32(0)
    """), "outside its allocation"),
    "range-descending": (kernel("""
        tile = item.shared[f32](4)
        if item.localIndex(0) == 0:
          for index in range(3, -1, -1):
            tile[index] = f32(1)
        item.barrier()
        return tile[item.localIndex(0)]
    """), None),
    "helper-divergence": (kernel("""
        if item.localIndex(0) == 0:
          fence(item)
        return f32(0)
    """, prefix="""fn fence(item: WorkItem) -> unit:
  item.barrier()
  return ()
"""), "barrier"),
    "barrier-loop": (kernel("""
        lane = item.localIndex(0)
        tile = item.shared[f32](4)
        for round in range(3):
          tile[lane] = f32(round)
          item.barrier()
          observed = tile[(lane + 1) % 4]
          item.barrier()
        return tile[lane]
    """), None),
    "iteration-read-write-race": (kernel("""
        lane = item.localIndex(0)
        tile = item.shared[f32](4)
        for round in range(3):
          tile[lane] = f32(round)
          item.barrier()
          observed = tile[(lane + 1) % 4]
        return f32(0)
    """), "read and write require a barrier"),
    "record-control-dependence": (kernel("""
        lane = item.localIndex(0)
        choice = if values[lane] > f32(0):
          Choice {value: positive}
        else:
          Choice {value: negative}
        if choice.value > f32(0):
          item.barrier()
        return f32(0)
    """, parameters="values: Tensor[f32, ?], positive: f32, negative: f32",
        prefix="type Choice:\n  value: f32\n"), "barrier"),
    "array-control-dependence": (kernel("""
        lane = item.localIndex(0)
        choice = if values[lane] > f32(0): [positive]
        else: [negative]
        if choice[0] > f32(0):
          item.barrier()
        return f32(0)
    """, parameters="values: Tensor[f32, ?], positive: f32, negative: f32"), "barrier"),
    "shared-alias-control-dependence": (kernel("""
        lane = item.localIndex(0)
        a = item.shared[f32](4)
        b = item.shared[f32](4)
        a[lane] = positive
        b[lane] = negative
        item.barrier()
        selected = if values[lane] > f32(0): a
        else: b
        if selected[0] > f32(0):
          item.barrier()
        return f32(0)
    """, parameters="values: Tensor[f32, ?], positive: f32, negative: f32"), "barrier"),
    "item-capture": (kernel("""
        f = fn() -> i64:
          return item.localIndex(0)
        return f32(f())
    """), "cannot be captured"),
    "scratch-capture": (kernel("""
        tile = item.shared[f32](4)
        f = fn() -> f32:
          return tile[0]
        return f()
    """), "cannot be captured"),
    "private-item-field": (
        'from "std/gpu" import WorkItem\nfn bad(item: WorkItem) -> u64:\n'
        '  return item.__context\n', "opaque"),
    "forged-item": (
        'from "std/gpu" import WorkItem\nfn bad() -> WorkItem:\n'
        '  return WorkItem {__context: u64(0)}\n', "cannot be constructed"),
    "forged-scratch": (
        'from "std/gpu" import Shared\nfn bad() -> Shared[f32]:\n'
        '  return Shared[f32] {__storage: [f32(0)]}\n', "cannot be constructed"),
    "aggregate-item": (
        'from "std/gpu" import WorkItem\ntype Box:\n  item: WorkItem\n', "aggregate fields"),
    "zero-group": (kernel("return f32(0)", size=0), "positive constant"),
    "zero-scratch": (kernel("tile = item.shared[f32](0)\nreturn f32(0)"), "positive constant"),
    "bool-scratch": (kernel("tile = item.shared[bool](4)\nreturn f32(0)"), "numeric primitive"),
    "axis-outside-rank": (kernel("return f32(item.localIndex(1))"), "logical launch rank"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checker", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cases = dict(CASES)
    for element in ("i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64"):
        for rank in range(9):
            shape = ", ".join(["2"] * rank)
            group = ", ".join(["1"] * rank)
            dimensions = ", " + ", ".join(["?"] * rank) if rank else ""
            cases[f"{element}-rank-{rank}"] = (
                'from "std/gpu" import dispatch, WorkItem\n'
                f"fn test() -> Tensor[{element}{dimensions}]:\n"
                f"  return dispatch([{shape}], [{group}], fn(item: WorkItem) -> {element}:\n"
                f"    return {element}(1)\n  )\n", None)
    results = []
    for name, (source, expected) in cases.items():
        path = output / (name + ".k")
        path.write_text(source, encoding="utf-8")
        run = subprocess.run([str(args.checker.resolve()), "--diagnostics=json", str(path)],
                             capture_output=True, text=True, timeout=60)
        try:
            report = json.loads(run.stdout)
        except json.JSONDecodeError as error:
            raise AssertionError((name, run.returncode, run.stdout, run.stderr)) from error
        (output / (name + ".json")).write_text(json.dumps(report, indent=2) + "\n")
        if expected is None:
            assert run.returncode == 0 and report["ok"], (name, report, run.stderr)
        else:
            assert run.returncode == 1 and not report["ok"], (name, report, run.stderr)
            assert any(d["code"] == "S087" and expected in d["message"]
                       for d in report["diagnostics"]), (name, expected, report)
        results.append({"case": name, "expectedDiagnostic": expected, "passed": True})
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"Cooperative source admission: {len(results)} accepted/rejected cases passed")


if __name__ == "__main__":
    main()
