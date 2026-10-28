#include "ResidualFfn.h"
#include <memory>
#ifdef KALANDRA_CPU
#include "../../runtime/Runtime.h"
using kalandra::runtime::Invocation;
struct Matrix { float *allocated,*aligned; std::int64_t offset,sizes[2],strides[2]; };
#ifdef DUMP_CELLS
extern "C" void _mlir_ciface_kalandra_test_entry(Matrix*,Matrix*,Matrix*,Matrix*,Invocation*);
#else
extern "C" float _mlir_ciface_kalandra_test_entry(Matrix*,Matrix*,Matrix*,Invocation*);
#endif
#elif defined(ACCELERATE)
#include <Accelerate/Accelerate.h>
#else
#include <cblas.h>
#include <dlfcn.h>
static_assert(sizeof(blasint)==4);
#endif

int main(int argc,char** argv) {
  if (argc!=8) return 2;
  const unsigned m=std::stoul(argv[1]),d=std::stoul(argv[2]),count=std::stoul(argv[3]),
    rounds=std::stoul(argv[4]),threads=std::stoul(argv[5]),pattern=std::stoul(argv[6]),dump=std::stoul(argv[7]);
  if (!m || m>128 || !d || d>2048 || !count || !rounds || pattern>1) return 2;
#ifdef ACCELERATE
  const auto mode=threads==1?BLAS_THREADING_SINGLE_THREADED:BLAS_THREADING_MULTI_THREADED;
  ffn::check(!BLASSetThreading(mode) && BLASGetThreading()==mode);
#elif !defined(KALANDRA_CPU)
  openblas_set_num_threads(threads);
  ffn::check(unsigned(openblas_get_num_threads())==threads);
  if (openblas_get_parallel()==2) {
    auto maximum=reinterpret_cast<int(*)()>(dlsym(RTLD_DEFAULT,"omp_get_max_threads"));
    auto dynamic=reinterpret_cast<int(*)()>(dlsym(RTLD_DEFAULT,"omp_get_dynamic"));
    ffn::check(maximum && dynamic && unsigned(maximum())==threads && !dynamic());
  }
#endif
  ffn::Inputs data(m,d,pattern);
#ifdef KALANDRA_CPU
  ffn::check(threads==1);
  Matrix x{data.x.data(),data.x.data(),0,{m,d},{d,1}},
    up{data.up.data(),data.up.data(),0,{d,4*d},{4*d,1}},
    down{data.down.data(),data.down.data(),0,{4*d,d},{d,1}};
#endif
  auto call=[&]() -> float {
#ifdef KALANDRA_CPU
    Invocation ctx;
#ifdef DUMP_CELLS
    Matrix result{};
    _mlir_ciface_kalandra_test_entry(&result,&x,&up,&down,&ctx);
    ffn::check(!ctx.status() && !ctx.callDepth() && result.sizes[0]==m && result.sizes[1]==d);
    std::vector<float> cells(m*d);
    for (unsigned r=0;r<m;++r) for(unsigned c=0;c<d;++c)
      cells[r*d+c]=result.aligned[result.offset+r*result.strides[0]+c*result.strides[1]];
    const float value=ffn::sum(cells);
    ffn::dump(cells,value);
#else
    const float value=_mlir_ciface_kalandra_test_entry(&x,&up,&down,&ctx);
#endif
    ffn::check(!ctx.status() && !ctx.callDepth());
    ctx.settle();
    ffn::check(!ctx.live() && ctx.allocations()==ctx.releases());
    return value;
#else
    auto hidden=std::make_unique_for_overwrite<float[]>(m*4*d);
    auto out=std::make_unique_for_overwrite<float[]>(m*d);
    cblas_sgemm(CblasRowMajor,CblasNoTrans,CblasNoTrans,m,4*d,d,1,data.x.data(),d,
                data.up.data(),4*d,0,hidden.get(),4*d);
    for (unsigned i=0;i<m*4*d;++i) hidden[i]=hidden[i]<0?0:hidden[i];
    cblas_sgemm(CblasRowMajor,CblasNoTrans,CblasNoTrans,m,d,4*d,1,hidden.get(),4*d,
                data.down.data(),d,0,out.get(),d);
    for (unsigned i=0;i<m*d;++i) out[i]+=data.x[i];
    std::vector<float> saved;
    if (dump) saved.assign(out.get(),out.get()+m*d);
    const float value=ffn::sumInPlace(out.get(),m*d);
    if (dump) ffn::dump(saved,value);
    return value;
#endif
  };
  if (dump) { call(); return 0; }
#ifdef DUMP_CELLS
  return 2;
#endif
  for(int round=-2;round<int(rounds);++round) {
    double checksum=0;
    auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<count;++i) checksum+=call();
    std::printf("{\"round\":%d,\"ns\":%.0f,\"checksum\":%.17g,\"threads\":%u}\n",
                 round,ffn::elapsed(start),checksum,threads);
  }
}
