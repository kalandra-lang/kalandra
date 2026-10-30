// Private compute-task ABI: three strict binary32 operations over runtime-sized arrays.
// Generated and numerically qualified by the strict arithmetic lowering; not Kalandra source.
module attributes {gpu.container_module,
  spirv.target_env = #spirv.target_env<#spirv.vce<v1.0, [Shader, Int64], [SPV_KHR_storage_buffer_storage_class]>, #spirv.resource_limits<>>} {
  gpu.module @kernels {
    gpu.func @strict_binary32(%a: memref<?xf32>, %b: memref<?xf32>,
                             %out: memref<?xf32>, %parameters: memref<2xi32>)
        kernel attributes {spirv.entry_point_abi = #spirv.entry_point_abi<workgroup_size = [64, 1, 1]>} {
      %block = gpu.block_id x
      %thread = gpu.thread_id x
      %width = gpu.block_dim x
      %base = arith.muli %block, %width : index
      %i = arith.addi %base, %thread : index
      %zero = arith.constant 0 : index
      %one = arith.constant 1 : index
      %count = memref.load %parameters[%zero] : memref<2xi32>
      %mode = memref.load %parameters[%one] : memref<2xi32>
      %n = arith.index_cast %count : i32 to index
      %valid = arith.cmpi ult, %i, %n : index
      scf.if %valid {
        %x = memref.load %a[%i] : memref<?xf32>
        %y = memref.load %b[%i] : memref<?xf32>
        %c0 = arith.constant 0 : i32
        %c1 = arith.constant 1 : i32
        %add = arith.cmpi eq, %mode, %c0 : i32
        %result = scf.if %add -> f32 {
          %sum = arith.addf %x, %y : f32
          scf.yield %sum : f32
        } else {
          %product = arith.mulf %x, %y : f32
          %multiply = arith.cmpi eq, %mode, %c1 : i32
          %r = scf.if %multiply -> f32 {
            scf.yield %product : f32
          } else {
            %sum = arith.addf %product, %x : f32
            scf.yield %sum : f32
          }
          scf.yield %r : f32
        }
        memref.store %result, %out[%i] : memref<?xf32>
      }
      gpu.return
    }
  }
}
