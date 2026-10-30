module attributes {gpu.container_module,
  spirv.target_env = #spirv.target_env<#spirv.vce<v1.0, [Shader, Int64], [SPV_KHR_storage_buffer_storage_class]>, #spirv.resource_limits<>>} {
  gpu.module @kernels {
    gpu.func @strict_binary64(%a: memref<4096xi64>, %b: memref<4096xi64>,
                             %out: memref<4096xi64>, %parameters: memref<2xi32>)
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
        %xbits = memref.load %a[%i] : memref<4096xi64>
        %ybits = memref.load %b[%i] : memref<4096xi64>
        %x = arith.bitcast %xbits : i64 to f64
        %y = arith.bitcast %ybits : i64 to f64
        %c0 = arith.constant 0 : i32
        %eq0 = arith.cmpi eq, %mode, %c0 : i32
        %r0 = scf.if %eq0 -> i64 {
          %v0 = arith.addf %x, %y : f64
          %b0 = arith.bitcast %v0 : f64 to i64
          scf.yield %b0 : i64
        } else {
          %c1 = arith.constant 1 : i32
          %eq1 = arith.cmpi eq, %mode, %c1 : i32
          %r1 = scf.if %eq1 -> i64 {
            %v1 = arith.mulf %x, %y : f64
            %b1 = arith.bitcast %v1 : f64 to i64
            scf.yield %b1 : i64
          } else {
            %c2 = arith.constant 2 : i32
            %eq2 = arith.cmpi eq, %mode, %c2 : i32
            %r2 = scf.if %eq2 -> i64 {
              %product = arith.mulf %x, %y : f64
              %v2 = arith.addf %product, %x : f64
              %b2 = arith.bitcast %v2 : f64 to i64
              scf.yield %b2 : i64
            } else {
              %c3 = arith.constant 3 : i32
              %eq3 = arith.cmpi eq, %mode, %c3 : i32
              %r3 = scf.if %eq3 -> i64 {
                %v3 = arith.subf %x, %y : f64
                %b3 = arith.bitcast %v3 : f64 to i64
                scf.yield %b3 : i64
              } else {
                %c4 = arith.constant 4 : i32
                %eq4 = arith.cmpi eq, %mode, %c4 : i32
                %r4 = scf.if %eq4 -> i64 {
                  %v4 = arith.negf %x : f64
                  %b4 = arith.bitcast %v4 : f64 to i64
                  scf.yield %b4 : i64
                } else {
                  %v5 = arith.divf %x, %y : f64
                  %b5 = arith.bitcast %v5 : f64 to i64
                  scf.yield %b5 : i64
                }
                scf.yield %r4 : i64
              }
              scf.yield %r3 : i64
            }
            scf.yield %r2 : i64
          }
          scf.yield %r1 : i64
        }
        memref.store %r0, %out[%i] : memref<4096xi64>
      }
      gpu.return
    }
  }
}
