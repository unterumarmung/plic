#ifndef DOLA_SUPPORT_OVERLOADED_H
#define DOLA_SUPPORT_OVERLOADED_H

namespace dola {

template <class... Callables> struct Overloaded : Callables... {
  using Callables::operator()...;
};

template <class... Callables>
Overloaded(Callables...) -> Overloaded<Callables...>;

} // namespace dola

#endif
