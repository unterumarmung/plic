#include "dola/sema.h"
#include "dola/strong_id.h"

#include "gtest/gtest.h"

#include <concepts>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace dola {
namespace {

static_assert(!std::same_as<ModuleId, FunctionIndex>);
static_assert(!std::convertible_to<ModuleId, FunctionIndex>);
static_assert(!std::convertible_to<std::size_t, ModuleId>);
static_assert(!std::convertible_to<LocalId, TypeId>);

TEST(StrongIdTest, HashesAndIndexesWithoutLosingItsDomain) {
  std::unordered_map<ModuleId, const char*> modules;
  modules.emplace(ModuleId{2}, "two");
  EXPECT_STREQ(modules.at(ModuleId{2}), "two");

  const std::vector<int> values{4, 8, 15};
  EXPECT_EQ(at(values, FunctionIndex{1}), 8);
  EXPECT_THROW(at(values, FunctionIndex{3}), std::out_of_range);
}

} // namespace
} // namespace dola
