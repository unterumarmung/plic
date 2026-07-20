#ifndef DOLA_SUPPORT_STRONG_ID_H
#define DOLA_SUPPORT_STRONG_ID_H

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

namespace dola {

template <typename Tag, std::integral Representation = std::size_t>
class StrongId {
public:
  using representation_type = Representation;

  constexpr explicit StrongId(Representation value) noexcept : value_(value) {}

  [[nodiscard]] constexpr Representation value() const noexcept {
    return value_;
  }

  friend constexpr bool operator==(StrongId, StrongId) noexcept = default;
  friend constexpr auto operator<=>(StrongId, StrongId) noexcept = default;

private:
  Representation value_;
};

template <typename Value, typename Tag, std::integral Representation>
Value& at(std::vector<Value>& values, StrongId<Tag, Representation> id) {
  return values.at(static_cast<std::size_t>(id.value()));
}

template <typename Value, typename Tag, std::integral Representation>
const Value& at(const std::vector<Value>& values,
                StrongId<Tag, Representation> id) {
  return values.at(static_cast<std::size_t>(id.value()));
}

} // namespace dola

template <typename Tag, std::integral Representation>
struct std::hash<dola::StrongId<Tag, Representation>> {
  std::size_t
  operator()(dola::StrongId<Tag, Representation> id) const noexcept {
    return std::hash<Representation>{}(id.value());
  }
};

#endif
