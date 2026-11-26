#include <iostream>
#include <source_location>
import utility;
int main() {
  utility::log(std::source_location::current(),"1");
  return 0;
}
