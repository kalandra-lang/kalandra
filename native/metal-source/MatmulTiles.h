#pragma once
// Independent strict register-tile entries; packaged with the scalar control.
inline constexpr const char *matmulRows2Shader = R"(module attributes {gpu.container_module,
  spirv.target_env = #spirv.target_env<#spirv.vce<v1.0, [Shader, Int64], [SPV_KHR_storage_buffer_storage_class]>, #spirv.resource_limits<>>} {
  gpu.module @kernels {
    gpu.func @kalandra_matmul_2x1(%a: memref<?xf32>, %b: memref<?xf32>, %output: memref<?xf32>, %parameters: memref<3xi32>)
      kernel attributes {spirv.entry_point_abi = #spirv.entry_point_abi<workgroup_size = [64, 1, 1]>} {
      %block = gpu.block_id x
      %thread = gpu.thread_id x
      %width = gpu.block_dim x
      %base = arith.muli %block, %width : index
      %tile = arith.addi %base, %thread : index
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
      %mr = arith.remui %m, %two : index
      %md = arith.divui %m, %two : index
      %nr = arith.remui %n, %two : index
      %nd = arith.divui %n, %two : index
      %tileRows = arith.addi %md, %mr : index
      %tileCols = arith.addi %n, %zero : index
      %tiles = arith.muli %tileRows, %tileCols : index
      %valid = arith.cmpi ult, %tile, %tiles : index
      scf.if %valid {
        %tr = arith.divui %tile, %tileCols : index
        %tc = arith.remui %tile, %tileCols : index
        %r0 = arith.muli %tr, %two : index
        %c0 = arith.addi %tc, %zero : index
        %r1 = arith.addi %r0, %one : index
        %c1 = arith.addi %c0, %one : index
        %hasRow = arith.cmpi ult, %r1, %m : index
        %hasCol = arith.cmpi ult, %c1, %n : index
        %hasBoth = arith.andi %hasRow, %hasCol : i1
        %ar0 = arith.muli %r0, %k : index
        %ar1 = arith.muli %r1, %k : index
        %stack0 = memref.alloca() : memref<32xf32, #spirv.storage_class<Function>>
        %stack1 = memref.alloca() : memref<32xf32, #spirv.storage_class<Function>>
        scf.for %i = %zero to %k step %one {
          %ai0 = arith.addi %ar0, %i : index
          %ai1 = arith.addi %ar1, %i : index
          %bi = arith.muli %i, %n : index
          %bi0 = arith.addi %bi, %c0 : index
          %bi1 = arith.addi %bi, %c1 : index
          %a0 = memref.load %a[%ai0] : memref<?xf32>
          %b0 = memref.load %b[%bi0] : memref<?xf32>
          %a1 = scf.if %hasRow -> f32 {
            %v = memref.load %a[%ai1] : memref<?xf32>
            scf.yield %v : f32
          } else { scf.yield %floatzero : f32 }
          %b1 = scf.if %hasCol -> f32 {
            %v = memref.load %b[%bi1] : memref<?xf32>
            scf.yield %v : f32
          } else { scf.yield %floatzero : f32 }
          %p0 = arith.mulf %a0, %b0 : f32
          %p1 = arith.mulf %a1, %b0 : f32
          %bits = arith.index_cast %i : index to i32
          %level, %remaining, %v0, %v1 = scf.while (%l = %zero, %r = %bits, %x0 = %p0, %x1 = %p1) : (index, i32, f32, f32) -> (index, i32, f32, f32) {
            %odd = arith.andi %r, %o32 : i32
            %occupied = arith.cmpi ne, %odd, %z32 : i32
            scf.condition(%occupied) %l, %r, %x0, %x1 : index, i32, f32, f32
          } do {
          ^bb0(%l: index, %r: i32, %x0: f32, %x1: f32):
            %old0 = memref.load %stack0[%l] : memref<32xf32, #spirv.storage_class<Function>>
            %sum0 = arith.addf %old0, %x0 : f32
            %old1 = memref.load %stack1[%l] : memref<32xf32, #spirv.storage_class<Function>>
            %sum1 = arith.addf %old1, %x1 : f32
            %nl = arith.addi %l, %one : index
            %nb = arith.shrui %r, %o32 : i32
            scf.yield %nl, %nb, %sum0, %sum1 : index, i32, f32, f32
          }
          memref.store %v0, %stack0[%level] : memref<32xf32, #spirv.storage_class<Function>>
          memref.store %v1, %stack1[%level] : memref<32xf32, #spirv.storage_class<Function>>
        }
        %result0, %result1, %seen = scf.for %l = %zero to %levels step %one iter_args(%acc0 = %floatzero, %acc1 = %floatzero, %used = %false) -> (f32, f32, i1) {
          %shift = arith.index_cast %l : index to i32
          %mask = arith.shli %o32, %shift : i32
          %occupied = arith.andi %k32, %mask : i32
          %present = arith.cmpi ne, %occupied, %z32 : i32
          %next0, %next1, %nextused = scf.if %present -> (f32, f32, i1) {
            %root0 = memref.load %stack0[%l] : memref<32xf32, #spirv.storage_class<Function>>
            %root1 = memref.load %stack1[%l] : memref<32xf32, #spirv.storage_class<Function>>
            %combined0, %combined1 = scf.if %used -> (f32, f32) {
              %sum0 = arith.addf %root0, %acc0 : f32
              %sum1 = arith.addf %root1, %acc1 : f32
              scf.yield %sum0, %sum1 : f32, f32
            } else {
              scf.yield %root0, %root1 : f32, f32
            }
            scf.yield %combined0, %combined1, %true : f32, f32, i1
          } else {
            scf.yield %acc0, %acc1, %used : f32, f32, i1
          }
          scf.yield %next0, %next1, %nextused : f32, f32, i1
        }
          %ob0 = arith.muli %r0, %n : index
          %out0 = arith.addi %ob0, %c0 : index
          memref.store %result0, %output[%out0] : memref<?xf32>
        scf.if %hasRow {
          %ob1 = arith.muli %r1, %n : index
          %out1 = arith.addi %ob1, %c0 : index
          memref.store %result1, %output[%out1] : memref<?xf32>
        }
      }
      gpu.return
    }
  }
}
)";
inline constexpr const char *matmulQuadShader = R"(module attributes {gpu.container_module,
  spirv.target_env = #spirv.target_env<#spirv.vce<v1.0, [Shader, Int64], [SPV_KHR_storage_buffer_storage_class]>, #spirv.resource_limits<>>} {
  gpu.module @kernels {
    gpu.func @kalandra_matmul_2x2(%a: memref<?xf32>, %b: memref<?xf32>, %output: memref<?xf32>, %parameters: memref<3xi32>)
      kernel attributes {spirv.entry_point_abi = #spirv.entry_point_abi<workgroup_size = [64, 1, 1]>} {
      %block = gpu.block_id x
      %thread = gpu.thread_id x
      %width = gpu.block_dim x
      %base = arith.muli %block, %width : index
      %tile = arith.addi %base, %thread : index
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
      %mr = arith.remui %m, %two : index
      %md = arith.divui %m, %two : index
      %nr = arith.remui %n, %two : index
      %nd = arith.divui %n, %two : index
      %tileRows = arith.addi %md, %mr : index
      %tileCols = arith.addi %nd, %nr : index
      %tiles = arith.muli %tileRows, %tileCols : index
      %valid = arith.cmpi ult, %tile, %tiles : index
      scf.if %valid {
        %tr = arith.divui %tile, %tileCols : index
        %tc = arith.remui %tile, %tileCols : index
        %r0 = arith.muli %tr, %two : index
        %c0 = arith.muli %tc, %two : index
        %r1 = arith.addi %r0, %one : index
        %c1 = arith.addi %c0, %one : index
        %hasRow = arith.cmpi ult, %r1, %m : index
        %hasCol = arith.cmpi ult, %c1, %n : index
        %hasBoth = arith.andi %hasRow, %hasCol : i1
        %ar0 = arith.muli %r0, %k : index
        %ar1 = arith.muli %r1, %k : index
        %stack0 = memref.alloca() : memref<32xf32, #spirv.storage_class<Function>>
        %stack1 = memref.alloca() : memref<32xf32, #spirv.storage_class<Function>>
        %stack2 = memref.alloca() : memref<32xf32, #spirv.storage_class<Function>>
        %stack3 = memref.alloca() : memref<32xf32, #spirv.storage_class<Function>>
        scf.for %i = %zero to %k step %one {
          %ai0 = arith.addi %ar0, %i : index
          %ai1 = arith.addi %ar1, %i : index
          %bi = arith.muli %i, %n : index
          %bi0 = arith.addi %bi, %c0 : index
          %bi1 = arith.addi %bi, %c1 : index
          %a0 = memref.load %a[%ai0] : memref<?xf32>
          %b0 = memref.load %b[%bi0] : memref<?xf32>
          %a1 = scf.if %hasRow -> f32 {
            %v = memref.load %a[%ai1] : memref<?xf32>
            scf.yield %v : f32
          } else { scf.yield %floatzero : f32 }
          %b1 = scf.if %hasCol -> f32 {
            %v = memref.load %b[%bi1] : memref<?xf32>
            scf.yield %v : f32
          } else { scf.yield %floatzero : f32 }
          %p0 = arith.mulf %a0, %b0 : f32
          %p1 = arith.mulf %a0, %b1 : f32
          %p2 = arith.mulf %a1, %b0 : f32
          %p3 = arith.mulf %a1, %b1 : f32
          %bits = arith.index_cast %i : index to i32
          %level, %remaining, %v0, %v1, %v2, %v3 = scf.while (%l = %zero, %r = %bits, %x0 = %p0, %x1 = %p1, %x2 = %p2, %x3 = %p3) : (index, i32, f32, f32, f32, f32) -> (index, i32, f32, f32, f32, f32) {
            %odd = arith.andi %r, %o32 : i32
            %occupied = arith.cmpi ne, %odd, %z32 : i32
            scf.condition(%occupied) %l, %r, %x0, %x1, %x2, %x3 : index, i32, f32, f32, f32, f32
          } do {
          ^bb0(%l: index, %r: i32, %x0: f32, %x1: f32, %x2: f32, %x3: f32):
            %old0 = memref.load %stack0[%l] : memref<32xf32, #spirv.storage_class<Function>>
            %sum0 = arith.addf %old0, %x0 : f32
            %old1 = memref.load %stack1[%l] : memref<32xf32, #spirv.storage_class<Function>>
            %sum1 = arith.addf %old1, %x1 : f32
            %old2 = memref.load %stack2[%l] : memref<32xf32, #spirv.storage_class<Function>>
            %sum2 = arith.addf %old2, %x2 : f32
            %old3 = memref.load %stack3[%l] : memref<32xf32, #spirv.storage_class<Function>>
            %sum3 = arith.addf %old3, %x3 : f32
            %nl = arith.addi %l, %one : index
            %nb = arith.shrui %r, %o32 : i32
            scf.yield %nl, %nb, %sum0, %sum1, %sum2, %sum3 : index, i32, f32, f32, f32, f32
          }
          memref.store %v0, %stack0[%level] : memref<32xf32, #spirv.storage_class<Function>>
          memref.store %v1, %stack1[%level] : memref<32xf32, #spirv.storage_class<Function>>
          memref.store %v2, %stack2[%level] : memref<32xf32, #spirv.storage_class<Function>>
          memref.store %v3, %stack3[%level] : memref<32xf32, #spirv.storage_class<Function>>
        }
        %result0, %result1, %result2, %result3, %seen = scf.for %l = %zero to %levels step %one iter_args(%acc0 = %floatzero, %acc1 = %floatzero, %acc2 = %floatzero, %acc3 = %floatzero, %used = %false) -> (f32, f32, f32, f32, i1) {
          %shift = arith.index_cast %l : index to i32
          %mask = arith.shli %o32, %shift : i32
          %occupied = arith.andi %k32, %mask : i32
          %present = arith.cmpi ne, %occupied, %z32 : i32
          %next0, %next1, %next2, %next3, %nextused = scf.if %present -> (f32, f32, f32, f32, i1) {
            %root0 = memref.load %stack0[%l] : memref<32xf32, #spirv.storage_class<Function>>
            %root1 = memref.load %stack1[%l] : memref<32xf32, #spirv.storage_class<Function>>
            %root2 = memref.load %stack2[%l] : memref<32xf32, #spirv.storage_class<Function>>
            %root3 = memref.load %stack3[%l] : memref<32xf32, #spirv.storage_class<Function>>
            %combined0, %combined1, %combined2, %combined3 = scf.if %used -> (f32, f32, f32, f32) {
              %sum0 = arith.addf %root0, %acc0 : f32
              %sum1 = arith.addf %root1, %acc1 : f32
              %sum2 = arith.addf %root2, %acc2 : f32
              %sum3 = arith.addf %root3, %acc3 : f32
              scf.yield %sum0, %sum1, %sum2, %sum3 : f32, f32, f32, f32
            } else {
              scf.yield %root0, %root1, %root2, %root3 : f32, f32, f32, f32
            }
            scf.yield %combined0, %combined1, %combined2, %combined3, %true : f32, f32, f32, f32, i1
          } else {
            scf.yield %acc0, %acc1, %acc2, %acc3, %used : f32, f32, f32, f32, i1
          }
          scf.yield %next0, %next1, %next2, %next3, %nextused : f32, f32, f32, f32, i1
        }
          %ob0 = arith.muli %r0, %n : index
          %out0 = arith.addi %ob0, %c0 : index
          memref.store %result0, %output[%out0] : memref<?xf32>
        scf.if %hasCol {
          %ob1 = arith.muli %r0, %n : index
          %out1 = arith.addi %ob1, %c1 : index
          memref.store %result1, %output[%out1] : memref<?xf32>
        }
        scf.if %hasRow {
          %ob2 = arith.muli %r1, %n : index
          %out2 = arith.addi %ob2, %c0 : index
          memref.store %result2, %output[%out2] : memref<?xf32>
        }
        scf.if %hasBoth {
          %ob3 = arith.muli %r1, %n : index
          %out3 = arith.addi %ob3, %c1 : index
          memref.store %result3, %output[%out3] : memref<?xf32>
        }
      }
      gpu.return
    }
  }
}
)";
