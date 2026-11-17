"""Audit raw cooperative stencil outputs using the existing Vulkan Decimal oracle."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

native = Path(__file__).resolve().parent.parent
sys.path[:0] = [str(native / "tests"), str(native / "vulkan/qualification")]
from TransformReference import D, divide, ulp


def audit(root):
    results = []
    worst = D(0)
    for path in sorted(root.glob("*.bin")):
        workers, count = map(int, path.stem.split("-"))
        data = path.read_bytes()
        assert len(data) == count * 4, path
        values = [D(i % 37 - 18) / 4 for i in range(count)]
        for index, (bits,) in enumerate(struct.iter_unpack("<I", data)):
            observed = D.from_float(struct.unpack("<f", struct.pack("<I", bits))[0])
            total = values[max(0, index - 1)] + values[index] + values[min(count - 1, index + 1)]
            low, high = divide((total, total), (D(3), D(3)))
            assert low <= observed <= high, (path.name, index, low, observed, high)
            exact = total / 3
            if exact:
                worst = max(worst, abs(observed - exact) / ulp(exact))
        results.append({"workers": workers, "elements": count,
                        "sha256": hashlib.sha256(data).hexdigest(), "passed": True})
    assert len(results) == 18, "expected all nine sizes with one and four workers"
    return {"oracle": "Vulkan TransformReference Decimal, exact additions and 2.5 ULP FDiv envelope",
            "runs": results, "valuesChecked": sum(run["elements"] for run in results),
            "maximumObservedUlps": str(worst)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.input)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"PASS: {len(result['runs'])} runs, {result['valuesChecked']} values; "
          f"maximum {result['maximumObservedUlps']} ULP from exact rational result")
