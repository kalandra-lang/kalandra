#pragma once
#include <fstream>
#include <iostream>
// Diagnostic only: retain loaded-module addresses before Vulkan unloads its
// dependencies. Does not suppress leaks or alter loader/library lifetimes.
inline bool writeSanitizerMaps() {
  std::ifstream maps("/proc/self/maps");
  if (!maps)
    return false;
  std::cerr << "BEGIN LOADED MODULE MAPS\n" << maps.rdbuf() << "END LOADED MODULE MAPS\n";
  return true;
}
