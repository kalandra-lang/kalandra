// Toolchain fixture, not generated from Kalandra source.
module attributes {gpu.container_module,
  spirv.target_env = #spirv.target_env<
    #spirv.vce<v1.0, [Shader], [SPV_KHR_storage_buffer_storage_class]>, #spirv.resource_limits<>>} {
  gpu.module @kernels {
    gpu.func @multiply_add(%a: memref<4096xf32>, %b: memref<4096xf32>,
                           %out: memref<4096xf32>, %length: memref<1xi32>)
        kernel attributes {spirv.entry_point_abi = #spirv.entry_point_abi<workgroup_size = [64, 1, 1]>} {
      %block = gpu.block_id x
      %thread = gpu.thread_id x
      %width = gpu.block_dim x
      %base = arith.muli %block, %width : index
      %i = arith.addi %base, %thread : index
      %zero = arith.constant 0 : index
      %count = memref.load %length[%zero] : memref<1xi32>
      %n = arith.index_cast %count : i32 to index
      %valid = arith.cmpi ult, %i, %n : index
      scf.if %valid {
        %x = memref.load %a[%i] : memref<4096xf32>
        %y = memref.load %b[%i] : memref<4096xf32>
        %product = arith.mulf %x, %y : f32
        %result = arith.addf %product, %x : f32
        memref.store %result, %out[%i] : memref<4096xf32>
      }
      gpu.return
    }
  }
}
