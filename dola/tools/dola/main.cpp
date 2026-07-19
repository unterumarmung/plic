#include "dola/compiler_driver.h"
#include "dola/driver_options.h"

#include "llvm/Support/raw_ostream.h"

#include <iostream>

int main(int argc, char** argv) {
  auto result = dola::parseDriverOptions(argc, argv, llvm::errs());
  if (!result.options)
    return 2;
  return dola::runCompiler(*result.options, llvm::outs(), std::cerr);
}
