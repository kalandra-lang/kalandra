#include <exception>
#include <iostream>
void checkTypeCapabilities();
int main() {
  try {
    checkTypeCapabilities();
    std::cout << "Type-capability contracts passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
