#include "dola_runtime.h"

#include <cstdint>
#include <string_view>

namespace {
dola_value integer(int64_t value) {
  dola_value result{};
  result.tag = DOLA_VALUE_INT;
  result.payload.integer = value;
  return result;
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 2)
    return 64;
  const std::string_view scenario(argv[1]);
  dola_value output = integer(0);
  if (scenario == "unknown-operation")
    return static_cast<int>(
        dola_rt_codegen(UINT32_MAX, 0, 0, 0, nullptr, 0, &output));
  if (scenario == "null-output")
    return static_cast<int>(dola_rt_codegen(DOLA_CODEGEN_LIST_CREATE, 0, 0, 0,
                                            nullptr, 0, nullptr));
  if (scenario == "missing-input")
    return static_cast<int>(
        dola_rt_codegen(DOLA_CODEGEN_TUPLE_GET, 0, 0, 0, nullptr, 0, &output));
  if (scenario == "malformed-result")
    return dola_rt_result_main(&output);
  return 65;
}
