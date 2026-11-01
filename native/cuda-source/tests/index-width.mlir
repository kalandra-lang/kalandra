module attributes {gpu.container_module} {
  gpu.module @kernels {
    gpu.func @kalandra_cuda_index_width(%out: memref<2xi64>, %parameters: memref<2xi32>)
        kernel attributes {spirv.entry_point_abi = #spirv.entry_point_abi<workgroup_size = [1, 1, 1]>} {
      %zero = arith.constant 0 : index
      %one = arith.constant 1 : index
      %count = memref.load %parameters[%zero] : memref<2xi32>
      %signed = memref.load %parameters[%one] : memref<2xi32>
      %size = arith.index_cast %count : i32 to index
      %signedIndex = arith.index_cast %signed : i32 to index
      %wide = arith.index_cast %size : index to i64
      %signedWide = arith.index_cast %signedIndex : index to i64
      memref.store %wide, %out[%zero] : memref<2xi64>
      memref.store %signedWide, %out[%one] : memref<2xi64>
      gpu.return
    }
  }
}
