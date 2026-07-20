#include "dola_runtime.h"
#include <type_traits>
static_assert(std::is_same_v<decltype(dola_rt_abi_version()), uint32_t>);
int main() { return 0; }
