#pragma once
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <source_location>
#include <string>
#include <vector>

namespace ffn {
inline void check(bool ok,const std::source_location where=std::source_location::current()) {
  if (!ok) throw std::runtime_error(std::string("Residual FFN validation/cleanup failed: ")+where.file_name()+":"+std::to_string(where.line()));
}
// Integer hash, rather than short-period data or repeated columns. Inputs are
// runtime arguments, not compiler constants. The same definition is in Python.
inline float input(std::uint32_t i, unsigned stream, unsigned pattern) {
  std::uint32_t z = i + 0x9e3779b9u * (stream + 1);
  z = (z ^ (z >> 16)) * 0x85ebca6bu;
  z = (z ^ (z >> 13)) * 0xc2b2ae35u;
  z ^= z >> 16;
  const int numerator = int(z % 17) - 8;
  return float(numerator) / float(pattern ? (stream ? 127 : 29) : (stream ? 128 : 32));
}
struct Inputs {
  std::vector<float> x, up, down;
  Inputs(unsigned m, unsigned d, unsigned pattern) : x(m*d), up(d*4*d), down(4*d*d) {
    for (std::size_t i=0;i<x.size();++i) x[i]=input(i,0,pattern);
    for (std::size_t i=0;i<up.size();++i) up[i]=input(i,1,pattern);
    for (std::size_t i=0;i<down.size();++i) down[i]=input(i,2,pattern);
  }
};
inline float sumInPlace(float* values,std::size_t n) {
  while (n>1) {
    for (std::size_t i=0;i<n/2;++i) values[i]=values[2*i]+values[2*i+1];
    if (n&1) values[n/2]=values[n-1];
    n=(n+1)/2;
  }
  return n?values[0]:0;
}
inline float sum(std::vector<float> values) {
  return sumInPlace(values.data(),values.size());
}
inline void dump(const std::vector<float>& cells, float value) {
  std::printf("{\"result\":%.17g,\"cells\":[",double(value));
  for (std::size_t i=0;i<cells.size();++i) std::printf("%s%.17g",i?",":"",double(cells[i]));
  std::puts("]}");
}
inline double elapsed(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
}
}
