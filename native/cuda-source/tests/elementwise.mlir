module attributes {gpu.container_module} {
  gpu.module @kernels {
    gpu.func @kalandra_cuda_probe(%left: memref<?xf32>, %right: memref<?xf32>,
                                %out: memref<?xf32>, %parameters: memref<2xi32>)
        kernel attributes {spirv.entry_point_abi = #spirv.entry_point_abi<workgroup_size = [64, 1, 1]>} {
      %block = gpu.block_id x
      %thread = gpu.thread_id x
      %width = gpu.block_dim x
      %base = arith.muli %block, %width : index
      %index = arith.addi %base, %thread : index
      %zero = arith.constant 0 : index
      %count = memref.load %parameters[%zero] : memref<2xi32>
      %size = arith.index_cast %count : i32 to index
      %valid = arith.cmpi ult, %index, %size : index
      scf.if %valid {
        %a = memref.load %left[%index] : memref<?xf32>
        %b = memref.load %right[%index] : memref<?xf32>
        %product = arith.mulf %a, %b : f32
        %sum = arith.addf %product, %a : f32
        memref.store %sum, %out[%index] : memref<?xf32>
      }
      gpu.return
    }
  }
}
