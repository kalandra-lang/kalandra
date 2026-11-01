// Reference-only executable: never linked into compiler/runtime distributions.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cublas_v2.h>
#include <cuda.h>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
template <class T> T symbol(void *library, const char *name) {
  auto value = reinterpret_cast<T>(dlsym(library, name));
  if (!value)
    throw std::runtime_error(std::string("missing ") + name);
  return value;
}
void check(CUresult status) {
  if (status != CUDA_SUCCESS)
    throw std::runtime_error("CUDA " + std::to_string(int(status)));
}
void blas(cublasStatus_t status) {
  if (status != CUBLAS_STATUS_SUCCESS)
    throw std::runtime_error("cuBLAS " + std::to_string(int(status)));
}
struct Driver {
  void *library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
#define F(field, type, name) type field = symbol<type>(library, name)
  using Init = CUresult (*)(unsigned);
  using Device = CUresult (*)(CUdevice *, int);
  using Create = CUresult (*)(CUcontext *, unsigned, CUdevice);
  using Destroy = CUresult (*)(CUcontext);
  using Allocate = CUresult (*)(CUdeviceptr *, size_t);
  using Free = CUresult (*)(CUdeviceptr);
  using CopyIn = CUresult (*)(CUdeviceptr, const void *, size_t);
  using CopyOut = CUresult (*)(void *, CUdeviceptr, size_t);
  using Load = CUresult (*)(CUmodule *, const void *, unsigned, CUjit_option *, void **);
  using Unload = CUresult (*)(CUmodule);
  using Function = CUresult (*)(CUfunction *, CUmodule, const char *);
  using Count = CUresult (*)(CUfunction, size_t *);
  using Launch = CUresult (*)(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned,
                              unsigned, unsigned, CUstream, void **, void **);
  using Sync = CUresult (*)();
  using EventCreate = CUresult (*)(CUevent *, unsigned);
  using EventRecord = CUresult (*)(CUevent, CUstream);
  using EventSync = CUresult (*)(CUevent);
  using EventTime = CUresult (*)(float *, CUevent, CUevent);
  using EventDestroy = CUresult (*)(CUevent);
  F(init, Init, "cuInit");
  F(device, Device, "cuDeviceGet");
  F(create, Create, "cuCtxCreate_v2");
  F(destroy, Destroy, "cuCtxDestroy_v2");
  F(allocate, Allocate, "cuMemAlloc_v2");
  F(free, Free, "cuMemFree_v2");
  F(copyIn, CopyIn, "cuMemcpyHtoD_v2");
  F(copyOut, CopyOut, "cuMemcpyDtoH_v2");
  F(load, Load, "cuModuleLoadDataEx");
  F(unload, Unload, "cuModuleUnload");
  F(function, Function, "cuModuleGetFunction");
  F(count, Count, "cuFuncGetParamCount");
  F(launch, Launch, "cuLaunchKernel");
  F(sync, Sync, "cuCtxSynchronize");
  F(eventCreate, EventCreate, "cuEventCreate");
  F(eventRecord, EventRecord, "cuEventRecord");
  F(eventSync, EventSync, "cuEventSynchronize");
  F(eventTime, EventTime, "cuEventElapsedTime");
  F(eventDestroy, EventDestroy, "cuEventDestroy_v2");
#undef F
};
int main(int argc, char **argv) {
  CUcontext context{};
  CUmodule module{}, legacyModule{};
  cublasHandle_t handle{};
  CUevent first{}, last{};
  std::vector<CUdeviceptr> allocations;
  Driver d;
  void *reference = nullptr;
  decltype(&cublasDestroy_v2) destroyReference = nullptr;
  try {
    if (argc != 4)
      throw std::runtime_error(
          "usage: catalog-resident catalog.ptx output.jsonl legacy-warp64.ptx");
    std::ofstream report(argv[2]);
    if (!report)
      throw std::runtime_error("output");
    check(d.init(0));
    CUdevice device{};
    check(d.device(&device, 0));
    check(d.create(&context, CU_CTX_SCHED_BLOCKING_SYNC, device));
    std::ifstream file(argv[1], std::ios::binary);
    std::string ptx(std::istreambuf_iterator<char>(file), {});
    if (ptx.empty())
      throw std::runtime_error("PTX");
    check(d.load(&module, ptx.c_str(), 0, nullptr, nullptr));
    std::ifstream legacy(argv[3], std::ios::binary);
    std::string legacyPtx(std::istreambuf_iterator<char>(legacy), {});
    check(d.load(&legacyModule, legacyPtx.c_str(), 0, nullptr, nullptr));
    const std::string catalog = "kalandra_numeric_v1_matmul_float32_catalog_cuda_desc_v1";
    std::array<CUfunction, 9> kernels{};
    std::array<unsigned, 9> tileM{64, 64, 64, 64, 64, 128, 128, 128, 128};
    std::array<unsigned, 9> tileN{64, 128, 128, 128, 128, 256, 256, 256, 256};
    std::array<unsigned, 9> blockK{16, 8, 8, 8, 8, 8, 8, 8, 8};
    std::array<unsigned, 9> threads{128, 128, 128, 128, 128, 256, 256, 256, 256};
    std::ofstream resources(std::string(argv[2]) + ".resources.jsonl");
    for (unsigned i : {0u, 3u, 7u}) {
      auto name = i == 0 ? std::string("kalandra_numeric_v1_matmul_float32_tile64_cuda_desc_v1")
                         : catalog + (i == 7 ? "_large" : "");
      check(d.function(&kernels[i], i == 0 ? legacyModule : module, name.c_str()));
      size_t count{};
      check(d.count(kernels[i], &count));
      if (count != 20)
        throw std::runtime_error("production descriptor ABI");
      auto attr = symbol<decltype(&cuFuncGetAttribute)>(d.library, "cuFuncGetAttribute");
      auto occupancy = symbol<decltype(&cuOccupancyMaxActiveBlocksPerMultiprocessor)>(
          d.library, "cuOccupancyMaxActiveBlocksPerMultiprocessor");
      int regs{}, shared{}, local{}, blocks{};
      check(attr(&regs, CU_FUNC_ATTRIBUTE_NUM_REGS, kernels[i]));
      check(attr(&shared, CU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES, kernels[i]));
      check(attr(&local, CU_FUNC_ATTRIBUTE_LOCAL_SIZE_BYTES, kernels[i]));
      check(occupancy(&blocks, kernels[i], threads[i], 0));
      resources << "{\"candidate\":" << i << ",\"registers\":" << regs
                << ",\"sharedBytes\":" << shared << ",\"localBytes\":" << local
                << ",\"residentBlocks\":" << blocks << "}\n";
    }
    std::array<CUfunction, 3> reducers{};
    std::array<unsigned, 9> parts{1, 1, 4, 5, 8, 1, 4, 5, 8};
    for (unsigned i : {1u}) {
      auto name = catalog + "_reduce";
      check(d.function(&reducers[i], module, name.c_str()));
      size_t count{};
      check(d.count(reducers[i], &count));
      if (count != 20)
        throw std::runtime_error("reduction descriptor ABI");
    }
    auto memset32 = symbol<decltype(&cuMemsetD32_v2)>(d.library, "cuMemsetD32_v2");
    auto preparedLaunch = [&](unsigned implementation, unsigned m, unsigned n,
                              CUdeviceptr workspace, auto &desc, auto &args) {
      if (parts[implementation] == 1) {
        check(d.launch(kernels[implementation],
                       (n + tileN[implementation] - 1) / tileN[implementation],
                       (m + tileM[implementation] - 1) / tileM[implementation], 1,
                       threads[implementation], 1, 1, 0, nullptr, args.data(), nullptr));
        return;
      }
      const auto count = uint64_t(m) * n;
      auto oldC = desc[10], oldCp = desc[11], oldCn = desc[13];
      desc[10] = desc[11] = workspace;
      desc[13] = count * parts[implementation];
      check(d.launch(kernels[implementation],
                     (n + tileN[implementation] - 1) / tileN[implementation],
                     (m + tileM[implementation] - 1) / tileM[implementation], parts[implementation],
                     threads[implementation], 1, 1, 0, nullptr, args.data(), nullptr));
      desc[10] = oldC;
      desc[11] = oldCp;
      desc[13] = oldCn;
      auto oldA = desc[0], oldAp = desc[1], oldAn = desc[3];
      desc[0] = desc[1] = workspace;
      desc[3] = count * parts[implementation];
      check(d.launch(reducers[parts[implementation] == 4   ? 0
                              : parts[implementation] == 5 ? 1
                                                           : 2],
                     unsigned((count + 255) / 256), 1, 1, 256, 1, 1, 0, nullptr, args.data(),
                     nullptr));
      desc[0] = oldA;
      desc[1] = oldAp;
      desc[3] = oldAn;
    };
    reference = dlopen("/usr/local/cuda/lib64/libcublas.so.13", RTLD_NOW | RTLD_LOCAL);
    if (!reference)
      throw std::runtime_error("cuBLAS reference unavailable");
    auto createReference = symbol<decltype(&cublasCreate_v2)>(reference, "cublasCreate_v2");
    destroyReference = symbol<decltype(&cublasDestroy_v2)>(reference, "cublasDestroy_v2");
    auto mathMode = symbol<decltype(&cublasSetMathMode)>(reference, "cublasSetMathMode");
    using Gemm = cublasStatus_t (*)(cublasHandle_t, cublasOperation_t, cublasOperation_t, int, int,
                                    int, const void *, const void *, cudaDataType_t, int,
                                    const void *, cudaDataType_t, int, const void *, void *,
                                    cudaDataType_t, int, cublasComputeType_t, cublasGemmAlgo_t);
    auto gemm = symbol<decltype(static_cast<Gemm>(&cublasGemmEx))>(reference, "cublasGemmEx");
    blas(createReference(&handle));
    blas(mathMode(handle, CUBLAS_PEDANTIC_MATH));
    int referenceVersion{};
    auto referenceVersionQuery =
        symbol<decltype(&cublasGetVersion_v2)>(reference, "cublasGetVersion_v2");
    blas(referenceVersionQuery(handle, &referenceVersion));
    check(d.eventCreate(&first, 0));
    check(d.eventCreate(&last, 0));
    // Before timing, verify row-major layout, beta=0 NaN independence and
    // non-TF32 precision with independent complete-output references.
    {
      const unsigned m = 128, n = 256,
                     k = std::getenv("KALANDRA_TUNING_K")
                             ? std::strtoul(std::getenv("KALANDRA_TUNING_K"), nullptr, 10)
                             : 64;
      std::vector<float> a(m * k), b(k * n), expected(m * n), out(m * n);
      std::array<unsigned, 4> words{m, k, n, UINT32_MAX};
      std::array<CUdeviceptr, 4> buffers{};
      for (unsigned i = 0; i < 4; ++i) {
        size_t count = i == 0 ? a.size() : i == 1 ? b.size() : i == 2 ? out.size() : words.size();
        check(d.allocate(&buffers[i], count * 4));
        allocations.push_back(buffers[i]);
      }
      CUdeviceptr workspace{};
      check(d.allocate(&workspace, out.size() * 5 * 4));
      allocations.push_back(workspace);
      check(memset32(workspace, 0x7fc00000, out.size() * 5));
      check(d.copyIn(buffers[3], words.data(), words.size() * 4));
      std::array<std::uint64_t, 20> descriptor{};
      std::array<void *, 20> arguments{};
      for (unsigned i = 0; i < 4; ++i) {
        descriptor[i * 5] = descriptor[i * 5 + 1] = buffers[i];
        descriptor[i * 5 + 3] = i == 0 ? a.size() : i == 1 ? b.size() : i == 2 ? out.size() : 4;
        descriptor[i * 5 + 4] = 1;
      }
      for (unsigned i = 0; i < 20; ++i)
        arguments[i] = &descriptor[i];
      for (bool precision : {false, true}) {
        for (unsigned i = 0; i < m; ++i)
          for (unsigned t = 0; t < k; ++t)
            a[i * k + t] = precision ? (t ? 0.0f : 1.0f + (i % 11) * 0x1p-23f)
                                     : float(int((i * 17 + t * 13) % 19) - 9) / 16;
        for (unsigned t = 0; t < k; ++t)
          for (unsigned j = 0; j < n; ++j)
            b[t * n + j] = precision ? (t ? 0.0f : 1.0f + (j % 13) * 0x1p-23f)
                                     : float(int((t * 7 + j * 11) % 23) - 11) / 16;
        for (unsigned i = 0; i < m; ++i)
          for (unsigned j = 0; j < n; ++j) {
            float sum = 0;
            for (unsigned t = 0; t < k; ++t)
              sum = std::fma(a[i * k + t], b[t * n + j], sum);
            expected[i * n + j] = sum;
          }
        check(d.copyIn(buffers[0], a.data(), a.size() * 4));
        check(d.copyIn(buffers[1], b.data(), b.size() * 4));
        for (unsigned implementation : {0u, 3u, 7u, 9u}) {
          if (implementation < 9 && k % blockK[implementation])
            continue;
          std::fill(out.begin(), out.end(), NAN);
          check(d.copyIn(buffers[2], out.data(), out.size() * 4));
          if (implementation < 9) {
            preparedLaunch(implementation, m, n, workspace, descriptor, arguments);
          } else {
            const float alpha = 1, beta = 0;
            blas(gemm(handle, CUBLAS_OP_N, CUBLAS_OP_N, n, m, k, &alpha,
                      reinterpret_cast<void *>(buffers[1]), CUDA_R_32F, n,
                      reinterpret_cast<void *>(buffers[0]), CUDA_R_32F, k, &beta,
                      reinterpret_cast<void *>(buffers[2]), CUDA_R_32F, n,
                      CUBLAS_COMPUTE_32F_PEDANTIC, CUBLAS_GEMM_DEFAULT));
          }
          check(d.sync());
          check(d.copyOut(out.data(), buffers[2], out.size() * 4));
          if (out != expected)
            throw std::runtime_error("full-f32 layout/precision/reference control");
        }
      }
      check(d.sync());
      for (auto pointer : allocations)
        check(d.free(pointer));
      allocations.clear();
    }
    report << "{\"cublasVersion\":" << referenceVersion
           << ",\"precisionControls\":\"complete dyadic, non-TF32 and prior-NaN controls "
              "passed\",\"scope\":\"resident production descriptor kernels and prepared launches; "
              "excludes "
              "allocation, transfers, task scheduling and compilation\",\"reference\":\"cuBLAS "
              "CUBLAS_COMPUTE_32F_PEDANTIC, beta=0, row-major via operand "
              "swap\",\"tf32\":false,\"controlTolerance\":0.05}\n";
    for (auto shape : std::array<std::array<unsigned, 3>, 12>{{{512, 512, 512},
                                                               {1024, 1024, 1024},
                                                               {2048, 2048, 2048},
                                                               {512, 1024, 768},
                                                               {64, 64, 64},
                                                               {128, 256, 128},
                                                               {256, 128, 512},
                                                               {384, 768, 256},
                                                               {768, 768, 768},
                                                               {1536, 1536, 1536},
                                                               {1024, 256, 512},
                                                               {256, 1024, 1536}}}) {
      if (std::getenv("KALANDRA_TUNING_CORRECTNESS_ONLY"))
        break;
      const auto [m, n, k] = shape;
      std::array<unsigned, 4> words{m, k, n, UINT32_MAX};
      auto allocate = [&](std::span<const std::byte> bytes) {
        CUdeviceptr pointer{};
        check(d.allocate(&pointer, bytes.size()));
        allocations.push_back(pointer);
        check(d.copyIn(pointer, bytes.data(), bytes.size()));
        return pointer;
      };
      std::vector<float> a(size_t(m) * k, 1), b(size_t(k) * n, 1), nan(size_t(m) * n, NAN);
      const std::array<CUdeviceptr, 4> buffers{
          allocate(std::as_bytes(std::span(a))), allocate(std::as_bytes(std::span(b))),
          allocate(std::as_bytes(std::span(nan))), allocate(std::as_bytes(std::span(words)))};
      CUdeviceptr workspace{};
      check(d.allocate(&workspace, size_t(m) * n * 5 * 4));
      allocations.push_back(workspace);
      check(memset32(workspace, 0x7fc00000, size_t(m) * n * 5));
      std::array<std::uint64_t, 20> descriptors{};
      std::array<void *, 20> arguments{};
      for (unsigned i = 0; i < 4; ++i) {
        auto offset = i * 5;
        descriptors[offset] = descriptors[offset + 1] = buffers[i];
        descriptors[offset + 3] = i == 0   ? size_t(m) * k
                                  : i == 1 ? size_t(k) * n
                                  : i == 2 ? size_t(m) * n
                                           : 4;
        descriptors[offset + 4] = 1;
      }
      for (unsigned i = 0; i < 20; ++i)
        arguments[i] = &descriptors[i];
      const float alpha = 1, beta = 0;
      auto invoke = [&](unsigned implementation) {
        if (implementation < 9) {
          preparedLaunch(implementation, m, n, workspace, descriptors, arguments);
        } else
          blas(gemm(handle, CUBLAS_OP_N, CUBLAS_OP_N, n, m, k, &alpha,
                    reinterpret_cast<void *>(buffers[1]), CUDA_R_32F, n,
                    reinterpret_cast<void *>(buffers[0]), CUDA_R_32F, k, &beta,
                    reinterpret_cast<void *>(buffers[2]), CUDA_R_32F, n,
                    CUBLAS_COMPUTE_32F_PEDANTIC, CUBLAS_GEMM_DEFAULT));
      };
      for (unsigned implementation : {0u, 3u, 7u, 9u}) {
        if (implementation < 9 && k % blockK[implementation])
          continue;
        for (unsigned j = 0; j < 32; ++j)
          invoke(implementation);
        check(d.sync());
        std::vector<float> out(nan.size());
        check(d.copyOut(out.data(), buffers[2], out.size() * 4));
        for (float value : out)
          if (value != float(k))
            throw std::runtime_error("complete output validation");
      }
      // Verify nonuniform complete outputs independently before timing.
      for (unsigned i = 0; i < m; ++i)
        for (unsigned t = 0; t < k; ++t)
          a[size_t(i) * k + t] = float(int(i % 7) - 3) * (t % 2 ? 1.0f : -1.0f) / 8;
      for (unsigned t = 0; t < k; ++t)
        for (unsigned j = 0; j < n; ++j)
          b[size_t(t) * n + j] = float(int(j % 11) - 5) * (t % 2 ? 1.0f : -1.0f) / 8;
      check(d.copyIn(buffers[0], a.data(), a.size() * 4));
      check(d.copyIn(buffers[1], b.data(), b.size() * 4));
      for (unsigned implementation : {0u, 3u, 7u, 9u}) {
        invoke(implementation);
        check(d.sync());
        std::vector<float> out(nan.size());
        check(d.copyOut(out.data(), buffers[2], out.size() * 4));
        for (unsigned i = 0; i < m; ++i)
          for (unsigned j = 0; j < n; ++j)
            if (out[size_t(i) * n + j] !=
                float(int(i % 7) - 3) * float(int(j % 11) - 5) * float(k) / 64)
              throw std::runtime_error("large nonuniform full-output validation");
      }
      const unsigned repeats = m >= 1536 ? 64 : m >= 768 ? 128 : 256;
      auto measure = [&](unsigned implementation) {
        check(d.eventRecord(first, nullptr));
        auto wall = std::chrono::steady_clock::now();
        for (unsigned j = 0; j < repeats; ++j)
          invoke(implementation);
        check(d.eventRecord(last, nullptr));
        check(d.eventSync(last));
        auto elapsed =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - wall)
                .count() /
            repeats;
        float ms{};
        check(d.eventTime(&ms, first, last));
        return std::array<double, 2>{double(ms) * 1000 / repeats, elapsed};
      };
      std::vector<std::array<unsigned, 2>> pairs{{0, 0}, {3, 3}, {7, 7}, {9, 9},
                                                 {0, 3}, {3, 9}, {0, 7}, {7, 9}};
      for (auto pair : pairs)
        for (unsigned block = 0; block < 4; ++block) {
          for (unsigned position = 0; position < 4; ++position) {
            const unsigned side = position == 0 || position == 3 ? 0 : 1;
            auto value = measure(pair[side]);
            report << "{\"m\":" << m << ",\"n\":" << n << ",\"k\":" << k << ",\"a\":" << pair[0]
                   << ",\"b\":" << pair[1] << ",\"block\":" << block << ",\"position\":" << position
                   << ",\"side\":" << side << ",\"implementation\":" << pair[side]
                   << ",\"repeats\":" << repeats << ",\"gpuUs\":" << value[0]
                   << ",\"wallUs\":" << value[1] << "}\n";
            report.flush();
          }
        }
      check(d.sync());
      for (auto pointer : allocations)
        check(d.free(pointer));
      allocations.clear();
      std::cout << "PASS: " << m << "x" << n << "x" << k
                << " complete-output check and frozen ABBA/control blocks\n"
                << std::flush;
    }
    check(d.eventDestroy(first));
    first = nullptr;
    check(d.eventDestroy(last));
    last = nullptr;
    blas(destroyReference(handle));
    handle = nullptr;
    check(d.unload(legacyModule));
    legacyModule = nullptr;
    check(d.unload(module));
    module = nullptr;
    check(d.destroy(context));
    context = nullptr;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    if (context) {
      d.sync();
      for (auto pointer : allocations)
        d.free(pointer);
      if (first)
        d.eventDestroy(first);
      if (last)
        d.eventDestroy(last);
      if (handle && destroyReference)
        destroyReference(handle);
      if (legacyModule)
        d.unload(legacyModule);
      if (module)
        d.unload(module);
      d.destroy(context);
    }
    return 1;
  }
}
