#ifndef FLEAUX_COMMON_OVERLOADED_HPP
#define FLEAUX_COMMON_OVERLOADED_HPP

namespace fleaux::common {

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;
}  // namespace fleaux::common

#endif  // FLEAUX_COMMON_OVERLOADED_HPP