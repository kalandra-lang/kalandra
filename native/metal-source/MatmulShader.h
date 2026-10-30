#pragma once
// One independent output cell per GPU lane. Streaming binary-counter subtrees
// and a low-to-high final fold reproduce the adjacent-pair tree for odd K too.
inline constexpr const char *matmulShader = R"(module attributes {gpu.container_module,
  spirv.target_env = #spirv.target_env<#spirv.vce<v1.0, [Shader, Int64], [SPV_KHR_storage_buffer_storage_class]>, #spirv.resource_limits<>>} {
  gpu.module @kernels {
    gpu.func @kalandra_matmul(%a: memref<?xf32>, %b: memref<?xf32>, %output: memref<?xf32>, %parameters: memref<3xi32>)
      kernel attributes {spirv.entry_point_abi = #spirv.entry_point_abi<workgroup_size = [64, 1, 1]>} {
      %block = gpu.block_id x
      %thread = gpu.thread_id x
      %width = gpu.block_dim x
      %base = arith.muli %block, %width : index
      %cell = arith.addi %base, %thread : index
      %zero = arith.constant 0 : index
      %one = arith.constant 1 : index
      %two = arith.constant 2 : index
      %levels = arith.constant 32 : index
      %z32 = arith.constant 0 : i32
      %o32 = arith.constant 1 : i32
      %false = arith.constant false
      %true = arith.constant true
      %floatzero = arith.bitcast %z32 : i32 to f32
      %m32 = memref.load %parameters[%zero] : memref<3xi32>
      %k32 = memref.load %parameters[%one] : memref<3xi32>
      %n32 = memref.load %parameters[%two] : memref<3xi32>
      %m = arith.index_cast %m32 : i32 to index
      %k = arith.index_cast %k32 : i32 to index
      %n = arith.index_cast %n32 : i32 to index
      %cells = arith.muli %m, %n : index
      %valid = arith.cmpi ult, %cell, %cells : index
      scf.if %valid {
        %stack = memref.alloca() : memref<32xf32, #spirv.storage_class<Function>>
        %row = arith.divui %cell, %n : index
        %column = arith.remui %cell, %n : index
        %rowbase = arith.muli %row, %k : index
        scf.for %i = %zero to %k step %one {
          %ai = arith.addi %rowbase, %i : index
          %bi0 = arith.muli %i, %n : index
          %bi = arith.addi %bi0, %column : index
          %av = memref.load %a[%ai] : memref<?xf32>
          %bv = memref.load %b[%bi] : memref<?xf32>
          %product = arith.mulf %av, %bv : f32
          %bits = arith.index_cast %i : index to i32
          %level, %remaining, %carry = scf.while (%l = %zero, %r = %bits, %v = %product) : (index, i32, f32) -> (index, i32, f32) {
            %odd = arith.andi %r, %o32 : i32
            %occupied = arith.cmpi ne, %odd, %z32 : i32
            scf.condition(%occupied) %l, %r, %v : index, i32, f32
          } do {
          ^bb0(%l: index, %r: i32, %v: f32):
            %previous = memref.load %stack[%l] : memref<32xf32, #spirv.storage_class<Function>>
            %sum = arith.addf %previous, %v : f32
            %nextlevel = arith.addi %l, %one : index
            %nextbits = arith.shrui %r, %o32 : i32
            scf.yield %nextlevel, %nextbits, %sum : index, i32, f32
          }
          memref.store %carry, %stack[%level] : memref<32xf32, #spirv.storage_class<Function>>
        }
        %result, %seen = scf.for %l = %zero to %levels step %one iter_args(%acc = %floatzero, %used = %false) -> (f32, i1) {
          %shift = arith.index_cast %l : index to i32
          %mask = arith.shli %o32, %shift : i32
          %occupied = arith.andi %k32, %mask : i32
          %present = arith.cmpi ne, %occupied, %z32 : i32
          %next, %nextused = scf.if %present -> (f32, i1) {
            %root = memref.load %stack[%l] : memref<32xf32, #spirv.storage_class<Function>>
            %combined = scf.if %used -> f32 {
              %sum = arith.addf %root, %acc : f32
              scf.yield %sum : f32
            } else {
              scf.yield %root : f32
            }
            scf.yield %combined, %true : f32, i1
          } else {
            scf.yield %acc, %used : f32, i1
          }
          scf.yield %next, %nextused : f32, i1
        }
        memref.store %result, %output[%cell] : memref<?xf32>
      }
      gpu.return
    }
  }
}
)";
