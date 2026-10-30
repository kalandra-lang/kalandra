#pragma once
#include "ResidualFfn.h"
#include <array>
#include <cstring>
namespace transformer {
inline constexpr float epsilon=0.00001f;
struct Inputs {
  std::array<std::vector<float>,7> values;
  std::array<std::array<std::int64_t,2>,7> shapes;
  float scale;
  Inputs(unsigned m,unsigned d,unsigned pattern):shapes{{{m,d},{d,d},{d,d},{d,d},{d,d},{d,4*d},{4*d,d}}},scale(float(1.0/std::sqrt(double(d)))) {
    for(unsigned stream=0;stream<values.size();++stream) {
      values[stream].resize(shapes[stream][0]*shapes[stream][1]);
      for(std::size_t i=0;i<values[stream].size();++i) values[stream][i]=ffn::input(i,stream,pattern);
    }
  }
};
}
