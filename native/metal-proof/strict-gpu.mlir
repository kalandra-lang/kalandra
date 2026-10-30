// Six arithmetic probes, selected through an integer parameter, not source-language syntax.
module attributes {gpu.container_module,
  spirv.target_env = #spirv.target_env<#spirv.vce<v1.0, [Shader, Int64], [SPV_KHR_storage_buffer_storage_class]>, #spirv.resource_limits<>>} {
  gpu.module @kernels {
    gpu.func @strict_binary32(%a: memref<4096xf32>, %b: memref<4096xf32>,
                             %out: memref<4096xf32>, %parameters: memref<2xi32>)
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
        %x = memref.load %a[%i] : memref<4096xf32>
        %y = memref.load %b[%i] : memref<4096xf32>
        %c0 = arith.constant 0 : i32
        %c1 = arith.constant 1 : i32
        %add = arith.cmpi eq, %mode, %c0 : i32
        %result = scf.if %add -> f32 {
          %sum = arith.addf %x, %y : f32
          scf.yield %sum : f32
        } else {
          %c3 = arith.constant 3 : i32
          %c4 = arith.constant 4 : i32
          %subtract = arith.cmpi eq, %mode, %c3 : i32
          %other = scf.if %subtract -> f32 {
            %difference = arith.subf %x, %y : f32
            scf.yield %difference : f32
          } else {
            %negate = arith.cmpi eq, %mode, %c4 : i32
            %chosen = scf.if %negate -> f32 {
              %negative = arith.negf %x : f32
              scf.yield %negative : f32
            } else {
              %c5 = arith.constant 5 : i32
              %divide = arith.cmpi eq, %mode, %c5 : i32
              %selected = scf.if %divide -> f32 {
                %quotient = arith.divf %x, %y : f32
                scf.yield %quotient : f32
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
              scf.yield %selected : f32
            }
            scf.yield %chosen : f32
          }
          scf.yield %other : f32
        }
        memref.store %result, %out[%i] : memref<4096xf32>
      }
      gpu.return
    }
  }
}
