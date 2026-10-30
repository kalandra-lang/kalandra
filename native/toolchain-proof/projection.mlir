// Fixed-shape toolchain proof, not compiler-generated Kalandra IR.
// The three separately rounded products reduce as (p0 + p1) + p2.
module {
  func.func @project(%a: tensor<2x3xf32>, %b: tensor<3x2xf32>) -> tensor<2x2xf32> {
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c2 = arith.constant 2 : index
    %result = tensor.generate {
    ^bb0(%row: index, %column: index):
      %a0 = tensor.extract %a[%row, %c0] : tensor<2x3xf32>
      %a1 = tensor.extract %a[%row, %c1] : tensor<2x3xf32>
      %a2 = tensor.extract %a[%row, %c2] : tensor<2x3xf32>
      %b0 = tensor.extract %b[%c0, %column] : tensor<3x2xf32>
      %b1 = tensor.extract %b[%c1, %column] : tensor<3x2xf32>
      %b2 = tensor.extract %b[%c2, %column] : tensor<3x2xf32>
      %p0 = arith.mulf %a0, %b0 : f32
      %p1 = arith.mulf %a1, %b1 : f32
      %p2 = arith.mulf %a2, %b2 : f32
      %pair = arith.addf %p0, %p1 : f32
      %value = arith.addf %pair, %p2 : f32
      tensor.yield %value : f32
    } : tensor<2x2xf32>
    return %result : tensor<2x2xf32>
  }

  func.func @score(%a: memref<2x3xf32>, %b: memref<3x2xf32>) -> f32 attributes {llvm.emit_c_interface} {
    %at = bufferization.to_tensor %a restrict : memref<2x3xf32> to tensor<2x3xf32>
    %bt = bufferization.to_tensor %b restrict : memref<3x2xf32> to tensor<3x2xf32>
    %out = call @project(%at, %bt) : (tensor<2x3xf32>, tensor<3x2xf32>) -> tensor<2x2xf32>
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %v0 = tensor.extract %out[%c0, %c0] : tensor<2x2xf32>
    %v1 = tensor.extract %out[%c0, %c1] : tensor<2x2xf32>
    %v2 = tensor.extract %out[%c1, %c0] : tensor<2x2xf32>
    %v3 = tensor.extract %out[%c1, %c1] : tensor<2x2xf32>
    %s0 = arith.addf %v0, %v1 : f32
    %s1 = arith.addf %v2, %v3 : f32
    %total = arith.addf %s0, %s1 : f32
    return %total : f32
  }
}
