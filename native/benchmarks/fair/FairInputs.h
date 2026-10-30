#pragma once
#include <cstddef>
#include <cstdlib>
inline const bool fairPattern = std::getenv("KALANDRA_FAIR_PATTERN") != nullptr;
template <class F> F fairA(std::size_t i) {
  return fairPattern ? F(int((i * 37) % 101) - 50) / F(37) : F(int(i % 17) - 8) / F(8);
}
template <class F> F fairB(std::size_t i) {
  return fairPattern ? F(int((i * 43) % 101) - 50) / F(29) : F(int(i % 13) - 6) / F(8);
}
