#ifndef DOLA_DRIVER_COMPILER_DRIVER_H
#define DOLA_DRIVER_COMPILER_DRIVER_H

#include "dola/driver_options.h"

#include <iosfwd>

namespace llvm {
class raw_ostream;
}

namespace dola {
int runCompiler(const DriverOptions& options, llvm::raw_ostream& output,
                std::ostream& errorOutput);
} // namespace dola
#endif
