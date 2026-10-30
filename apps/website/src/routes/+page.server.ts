import matrix from "../../../../native/metal-source/matrix-graph.k?raw";
import arithmetic from "../../../../native/metal-source/pre-matmul.k?raw";
import placement from "../../../../native/benchmarks/gpu.k?raw";

const normalize = (source: string) => source.replaceAll("\r\n", "\n");

// Display and copy source from qualified native fixtures, never an invented dialect.
export function load() {
  return {
    examples: [
      {
        title: "Matrix product",
        file: "matrix-graph.k",
        path: "native/metal-source/matrix-graph.k",
        source: normalize(matrix)
          .split("\n\n")[0]
          .replace("fn multiply(a:", "fn multiply(\n  a:")
          .replace(", b:", ",\n  b:")
          .replace("]) ->", "]\n) ->")
          .trim(),
        description:
          "Compose tensor operations as ordinary functions. Keep intermediate results on the GPU.",
      },
      {
        title: "Tensor arithmetic",
        file: "pre-matmul.k",
        path: "native/metal-source/pre-matmul.k",
        source: normalize(arithmetic).split("\nfn main")[0].trim(),
        description:
          "Combine tensor arithmetic, matrix products and typed parameters in a resident numerical graph.",
      },
      {
        title: "CPU and GPU placement",
        file: "gpu.k",
        path: "native/benchmarks/gpu.k",
        source: normalize(placement).trim(),
        description:
          "Transfer tensors explicitly, place a typed task on the GPU, and await its result.",
      },
    ],
  };
}
