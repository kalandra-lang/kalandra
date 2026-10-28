#include "TransformerBlock.h"
#include "../../runtime/Tensor.h"
using namespace kalandra::runtime;
using Matrix=Float32Tensor<2>;
extern "C" void _mlir_ciface_kalandra_test_entry(Matrix*,Matrix*,Matrix*,Matrix*,Matrix*,Matrix*,Matrix*,Matrix*,float,float,Invocation*);
int main(int argc,char**argv) {
  ffn::check(argc==8);
  const unsigned m=std::stoul(argv[1]),d=std::stoul(argv[2]),count=std::stoul(argv[3]),rounds=std::stoul(argv[4]),mode=std::stoul(argv[5]),dump=std::stoul(argv[6]),pattern=std::stoul(argv[7]);
  ffn::check(m && d && !mode && m<=512 && d<=2048);
  transformer::Inputs data(m,d,pattern);
  std::array<Matrix,7> input;
  for(unsigned i=0;i<7;++i) input[i]={data.values[i].data(),data.values[i].data(),0,data.shapes[i],{data.shapes[i][1],1}};
  auto call=[&](bool save) {
    Invocation context;Matrix out{};
    _mlir_ciface_kalandra_test_entry(&out,&input[0],&input[1],&input[2],&input[3],&input[4],&input[5],&input[6],transformer::epsilon,data.scale,&context);
    ffn::check(!context.status() && !context.callDepth() && out.sizes==data.shapes[0]);
    if(save) {
      std::vector<float> cells(m*d);
      for(unsigned r=0;r<m;++r) for(unsigned c=0;c<d;++c) cells[r*d+c]=out.aligned[out.offset+r*out.strides[0]+c*out.strides[1]];
      ffn::dump(cells,ffn::sum(cells));
    }
    context.settle();ffn::check(!context.live() && context.allocations()==context.releases());
    return double(m)*d*4;
  };
  if(dump) {call(true);return 0;}
  for(int round=-2;round<int(rounds);++round) {
    double checksum=0;auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<count;++i) checksum+=call(false);
    std::printf("{\"round\":%d,\"ns\":%.0f,\"checksum\":%.17g}\n",round,ffn::elapsed(start),checksum);
  }
}
