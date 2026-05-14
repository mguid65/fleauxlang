#pragma once

#include <cstdlib>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace fleaux::utility {
// structured_visit only expands tuple-like values. In this codebase, that means
// a type participates in the tuple protocol via std::tuple_size,
// std::tuple_element, and an ADL-visible get<I>(value) overload (or std::get
// for standard tuple-like types). Plain aggregates are not expanded unless they
// opt into that protocol.

namespace detail {

template <class...>
inline constexpr bool always_false_v = false;

[[noreturn]] inline void unreachable() {
#if defined(__cpp_lib_unreachable) && __cpp_lib_unreachable >= 202202L
  std::unreachable();
#elif defined(_MSC_VER) && !defined(__clang__)
  __assume(false);
#elif defined(__GNUC__) || defined(__clang__)
  __builtin_unreachable();
#else
  std::abort();
#endif
}

template <std::size_t I, class T>
constexpr decltype(auto) adl_get(T&& value) {
  using std::get;
  return get<I>(std::forward<T>(value));
}

template <class T, class = void>
struct is_tuple_like : std::false_type {};

template <class T>
struct is_tuple_like<T, std::void_t<decltype(std::tuple_size<std::remove_cvref_t<T>>::value)>> : std::true_type {};

template <class T>
inline constexpr bool is_tuple_like_v = is_tuple_like<T>::value;

template <class T, class = void>
struct is_variant_like : std::false_type {};

template <class T>
struct is_variant_like<T, std::void_t<decltype(std::variant_size<std::remove_cvref_t<T>>::value)>> : std::true_type {};

template <class T>
inline constexpr bool is_variant_like_v = is_variant_like<T>::value;

template <class R, class Visitor, class Tuple, std::size_t... Is>
constexpr auto tuple_invocable_impl(std::index_sequence<Is...>) -> bool {
  if constexpr (std::is_void_v<R>) {
    return std::is_invocable_v<Visitor, decltype(std::get<Is>(std::declval<Tuple>()))...>;
  } else {
    return std::is_invocable_r_v<R, Visitor, decltype(std::get<Is>(std::declval<Tuple>()))...>;
  }
}

template <class R, class Visitor, class Tuple>
inline constexpr bool tuple_invocable_v = tuple_invocable_impl<R, Visitor, Tuple>(
    std::make_index_sequence<std::tuple_size_v<std::remove_reference_t<Tuple>>>{});

template <class Tuple, std::size_t... Is>
constexpr auto tuple_tail_impl(Tuple&& tuple, std::index_sequence<Is...>) {
  return std::forward_as_tuple(std::get<Is + 1>(std::forward<Tuple>(tuple))...);
}

template <class Tuple>
constexpr auto tuple_tail(Tuple&& tuple) {
  constexpr auto kTupleSize = std::tuple_size_v<std::remove_reference_t<Tuple>>;
  static_assert(kTupleSize > 0, "tuple_tail requires a non-empty tuple.");
  return tuple_tail_impl(std::forward<Tuple>(tuple), std::make_index_sequence<kTupleSize - 1>{});
}

template <class T, std::size_t... Is>
constexpr auto expand_value_as_tuple_impl(T&& value, std::index_sequence<Is...>) {
  return std::forward_as_tuple(adl_get<Is>(std::forward<T>(value))...);
}

template <class T>
constexpr auto expand_value_as_tuple(T&& value) {
  static_assert(is_tuple_like_v<T>, "expand_value_as_tuple requires a tuple-like value.");
  return expand_value_as_tuple_impl(std::forward<T>(value),
                                    std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<T>>>{});
}

template <class T, class Indices>
struct expanded_argument_tuple;

template <class T, std::size_t... Is>
struct expanded_argument_tuple<T, std::index_sequence<Is...>> {
  using type = std::tuple<decltype(adl_get<Is>(std::declval<T>()))...>;
};

template <class T>
using expanded_argument_tuple_t =
    typename expanded_argument_tuple<T, std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<T>>>>::type;

template <class LeftTuple, class RightTuple>
struct tuple_concat_types;

template <class... Left, class... Right>
struct tuple_concat_types<std::tuple<Left...>, std::tuple<Right...>> {
  using type = std::tuple<Left..., Right...>;
};

template <class LeftTuple, class RightTuple>
using tuple_concat_types_t =
    typename tuple_concat_types<std::remove_reference_t<LeftTuple>, std::remove_reference_t<RightTuple>>::type;

template <class... Ts>
struct type_pack {};

template <class Tuple>
struct tuple_to_pack;

template <class... Ts>
struct tuple_to_pack<std::tuple<Ts...>> {
  using type = type_pack<Ts...>;
};

template <class Tuple>
using tuple_to_pack_t = typename tuple_to_pack<std::remove_reference_t<Tuple>>::type;

template <class Pack, class T>
struct pack_append;

template <class... Ts, class T>
struct pack_append<type_pack<Ts...>, T> {
  using type = type_pack<Ts..., T>;
};

template <class Pack, class T>
using pack_append_t = typename pack_append<Pack, T>::type;

template <class Pack, class Tuple>
struct pack_append_tuple;

template <class... Ts, class... Us>
struct pack_append_tuple<type_pack<Ts...>, std::tuple<Us...>> {
  using type = type_pack<Ts..., Us...>;
};

template <class Pack, class Tuple>
using pack_append_tuple_t = typename pack_append_tuple<Pack, std::remove_reference_t<Tuple>>::type;

template <class R, class Visitor, class Pack>
struct pack_invocable;

template <class R, class Visitor, class... Args>
struct pack_invocable<R, Visitor, type_pack<Args...>>
    : std::bool_constant<std::is_void_v<R> ? std::is_invocable_v<Visitor, Args...>
                                           : std::is_invocable_r_v<R, Visitor, Args...>> {};

template <class R, class Visitor, class Pack>
inline constexpr bool pack_invocable_v = pack_invocable<R, Visitor, Pack>::value;

template <class Visitor, class Pack>
struct pack_invocable_exact;

template <class Visitor, class... Args>
struct pack_invocable_exact<Visitor, type_pack<Args...>> : std::bool_constant<std::is_invocable_v<Visitor, Args...>> {};

template <class Visitor, class Pack>
inline constexpr bool pack_invocable_exact_v = pack_invocable_exact<Visitor, Pack>::value;

template <class R, class Visitor, class BoundPack, class RemainingPack>
struct can_structured_pack_match;

template <bool IsTupleLike, class R, class Visitor, class BoundPack, class Next, class RemainingPack>
struct can_structured_pack_expand_next : std::false_type {};

template <class R, class Visitor, class BoundPack, class Next, class RemainingPack>
struct can_structured_pack_expand_next<true, R, Visitor, BoundPack, Next, RemainingPack> {
  static constexpr bool value =
      can_structured_pack_match<R, Visitor, pack_append_tuple_t<BoundPack, expanded_argument_tuple_t<Next>>,
                                RemainingPack>::value;
};

template <class R, class Visitor, class BoundPack>
struct can_structured_pack_match<R, Visitor, BoundPack, type_pack<>>
    : std::bool_constant<pack_invocable_v<R, Visitor, BoundPack>> {};

template <class R, class Visitor, class BoundPack, class Next, class... Rest>
struct can_structured_pack_match<R, Visitor, BoundPack, type_pack<Next, Rest...>> {
private:
  using keep_bound_pack = pack_append_t<BoundPack, Next>;
  using rest_pack = type_pack<Rest...>;

  static constexpr bool kKeepWhole = can_structured_pack_match<R, Visitor, keep_bound_pack, rest_pack>::value;
  static constexpr bool kExpand =
      can_structured_pack_expand_next<is_tuple_like_v<Next>, R, Visitor, BoundPack, Next, rest_pack>::value;

public:
  static constexpr bool value = kKeepWhole || kExpand;
};

template <class R, class Visitor, class BoundTuple>
constexpr auto invoke_from_tuple(Visitor&& visitor, BoundTuple&& bound) -> R {
  return std::apply(
      [&visitor]<class... Args>(Args&&... args) -> R {
        if constexpr (std::is_void_v<R>) {
          std::invoke(std::forward<Visitor>(visitor), std::forward<Args>(args)...);
          return;
        } else {
          return std::invoke(std::forward<Visitor>(visitor), std::forward<Args>(args)...);
        }
      },
      std::forward<BoundTuple>(bound));
}

template <class BoundTuple, class Next>
using append_whole_tuple_t = tuple_concat_types_t<BoundTuple, std::tuple<Next>>;

template <class BoundTuple, class Next>
constexpr auto append_whole(BoundTuple&& bound, Next&& next) {
  return std::tuple_cat(std::forward<BoundTuple>(bound), std::forward_as_tuple(std::forward<Next>(next)));
}

template <class BoundTuple, class Next>
using append_expanded_tuple_t = tuple_concat_types_t<BoundTuple, expanded_argument_tuple_t<Next>>;

template <class BoundTuple, class Next>
constexpr auto append_expanded(BoundTuple&& bound, Next&& next) {
  return std::tuple_cat(std::forward<BoundTuple>(bound), expand_value_as_tuple(std::forward<Next>(next)));
}

template <class R, class Visitor, class BoundTuple, class RemainingTuple>
constexpr auto structured_dispatch_recursive_impl(Visitor&& visitor, BoundTuple&& bound, RemainingTuple&& remaining) -> R;

template <class Visitor, class BoundTuple, class RemainingTuple>
constexpr decltype(auto) structured_dispatch_recursive_impl_exact(Visitor&& visitor, BoundTuple&& bound,
                                                                  RemainingTuple&& remaining);

template <class R, class Visitor, class BoundTuple>
constexpr auto structured_dispatch_case_0(Visitor&& visitor, BoundTuple&& bound) -> R {
  static_assert(pack_invocable_v<R, Visitor, tuple_to_pack_t<BoundTuple>>,
                "structured_visit could not find a matching visitor signature for the selected variant alternatives.");
  return invoke_from_tuple<R>(std::forward<Visitor>(visitor), std::forward<BoundTuple>(bound));
}

template <class Visitor, class BoundTuple>
constexpr decltype(auto) structured_dispatch_case_0_exact(Visitor&& visitor, BoundTuple&& bound) {
  static_assert(pack_invocable_exact_v<Visitor, tuple_to_pack_t<BoundTuple>>,
                "structured_visit could not find a matching visitor signature for the selected variant alternatives.");
  return std::apply(
      [&visitor]<class... Args>(Args&&... args) -> decltype(auto) {
        return std::invoke(std::forward<Visitor>(visitor), std::forward<Args>(args)...);
      },
      std::forward<BoundTuple>(bound));
}

template <class R, class Visitor, class BoundTuple, class A>
constexpr auto structured_dispatch_case_1(Visitor&& visitor, BoundTuple&& bound, A&& a) -> R {
  using bound_pack = tuple_to_pack_t<BoundTuple>;
  using whole_pack = pack_append_t<bound_pack, A&&>;

  if constexpr (pack_invocable_v<R, Visitor, whole_pack>) {
    auto next_bound = append_whole(std::forward<BoundTuple>(bound), std::forward<A>(a));
    return invoke_from_tuple<R>(std::forward<Visitor>(visitor), std::move(next_bound));
  } else if constexpr (can_structured_pack_expand_next<is_tuple_like_v<A&&>, R, Visitor, bound_pack, A&&,
                                                       type_pack<>>::value) {
    if constexpr (std::tuple_size_v<std::remove_reference_t<BoundTuple>> == 0) {
      auto expanded = expand_value_as_tuple(std::forward<A>(a));
      return invoke_from_tuple<R>(std::forward<Visitor>(visitor), std::move(expanded));
    } else {
      auto expanded_bound = append_expanded(std::forward<BoundTuple>(bound), std::forward<A>(a));
      return invoke_from_tuple<R>(std::forward<Visitor>(visitor), std::move(expanded_bound));
    }
  } else {
    static_assert(
        always_false_v<BoundTuple>,
        "structured_visit could not find a matching visitor signature for the selected variant alternatives.");

    if constexpr (std::is_void_v<R>) {
      return;
    } else {
      unreachable();
    }
  }
}

template <class Visitor, class BoundTuple, class A>
constexpr decltype(auto) structured_dispatch_case_1_exact(Visitor&& visitor, BoundTuple&& bound, A&& a) {
  using bound_pack = tuple_to_pack_t<BoundTuple>;
  using whole_pack = pack_append_t<bound_pack, A&&>;

  if constexpr (pack_invocable_exact_v<Visitor, whole_pack>) {
    auto next_bound = append_whole(std::forward<BoundTuple>(bound), std::forward<A>(a));
    return structured_dispatch_case_0_exact(std::forward<Visitor>(visitor), std::move(next_bound));
  } else if constexpr (is_tuple_like_v<A&&> &&
                       pack_invocable_exact_v<Visitor, pack_append_tuple_t<bound_pack, expanded_argument_tuple_t<A&&>>>) {
    auto expanded_bound = append_expanded(std::forward<BoundTuple>(bound), std::forward<A>(a));
    return structured_dispatch_case_0_exact(std::forward<Visitor>(visitor), std::move(expanded_bound));
  } else {
    static_assert(always_false_v<BoundTuple>,
                  "structured_visit could not find a matching visitor signature for the selected variant alternatives.");
  }
}

template <class R, class Visitor, class BoundTuple, class A, class B>
constexpr auto structured_dispatch_case_2(Visitor&& visitor, BoundTuple&& bound, A&& a, B&& b) -> R {
  using bound_pack = tuple_to_pack_t<BoundTuple>;
  using tail_pack = type_pack<B&&>;
  using whole_pack = pack_append_t<bound_pack, A&&>;

  if constexpr (can_structured_pack_match<R, Visitor, whole_pack, tail_pack>::value) {
    auto next_bound = append_whole(std::forward<BoundTuple>(bound), std::forward<A>(a));
    return structured_dispatch_case_1<R>(std::forward<Visitor>(visitor), std::move(next_bound), std::forward<B>(b));
  } else if constexpr (can_structured_pack_expand_next<is_tuple_like_v<A&&>, R, Visitor, bound_pack, A&&,
                                                       tail_pack>::value) {
    auto expanded_bound = append_expanded(std::forward<BoundTuple>(bound), std::forward<A>(a));
    return structured_dispatch_case_1<R>(std::forward<Visitor>(visitor), std::move(expanded_bound), std::forward<B>(b));
  } else {
    static_assert(
        always_false_v<BoundTuple>,
        "structured_visit could not find a matching visitor signature for the selected variant alternatives.");

    if constexpr (std::is_void_v<R>) {
      return;
    } else {
      unreachable();
    }
  }
}

template <class Visitor, class BoundTuple, class A, class B>
constexpr decltype(auto) structured_dispatch_case_2_exact(Visitor&& visitor, BoundTuple&& bound, A&& a, B&& b) {
  using bound_pack = tuple_to_pack_t<BoundTuple>;
  using tail_pack = type_pack<B&&>;
  using whole_pack = pack_append_t<bound_pack, A&&>;

  if constexpr (can_structured_pack_match<void, Visitor, whole_pack, tail_pack>::value &&
                pack_invocable_exact_v<Visitor, whole_pack>) {
    auto next_bound = append_whole(std::forward<BoundTuple>(bound), std::forward<A>(a));
    return structured_dispatch_case_1_exact(std::forward<Visitor>(visitor), std::move(next_bound), std::forward<B>(b));
  } else if constexpr (is_tuple_like_v<A&&> &&
                       can_structured_pack_match<void, Visitor, pack_append_tuple_t<bound_pack, expanded_argument_tuple_t<A&&>>,
                                                 tail_pack>::value) {
    auto expanded_bound = append_expanded(std::forward<BoundTuple>(bound), std::forward<A>(a));
    return structured_dispatch_case_1_exact(std::forward<Visitor>(visitor), std::move(expanded_bound), std::forward<B>(b));
  } else {
    static_assert(always_false_v<BoundTuple>,
                  "structured_visit could not find a matching visitor signature for the selected variant alternatives.");
  }
}

template <class R, class Visitor, class BoundTuple, class A, class B, class C>
constexpr auto structured_dispatch_case_3(Visitor&& visitor, BoundTuple&& bound, A&& a, B&& b, C&& c) -> R {
  using bound_pack = tuple_to_pack_t<BoundTuple>;
  using tail_pack = type_pack<B&&, C&&>;
  using whole_pack = pack_append_t<bound_pack, A&&>;

  if constexpr (can_structured_pack_match<R, Visitor, whole_pack, tail_pack>::value) {
    auto next_bound = append_whole(std::forward<BoundTuple>(bound), std::forward<A>(a));
    return structured_dispatch_case_2<R>(std::forward<Visitor>(visitor), std::move(next_bound), std::forward<B>(b),
                                         std::forward<C>(c));
  } else if constexpr (can_structured_pack_expand_next<is_tuple_like_v<A&&>, R, Visitor, bound_pack, A&&,
                                                       tail_pack>::value) {
    auto expanded_bound = append_expanded(std::forward<BoundTuple>(bound), std::forward<A>(a));
    return structured_dispatch_case_2<R>(std::forward<Visitor>(visitor), std::move(expanded_bound), std::forward<B>(b),
                                         std::forward<C>(c));
  } else {
    static_assert(
        always_false_v<BoundTuple>,
        "structured_visit could not find a matching visitor signature for the selected variant alternatives.");

    if constexpr (std::is_void_v<R>) {
      return;
    } else {
      unreachable();
    }
  }
}

template <class Visitor, class BoundTuple, class A, class B, class C>
constexpr decltype(auto) structured_dispatch_case_3_exact(Visitor&& visitor, BoundTuple&& bound, A&& a, B&& b, C&& c) {
  using bound_pack = tuple_to_pack_t<BoundTuple>;
  using tail_pack = type_pack<B&&, C&&>;
  using whole_pack = pack_append_t<bound_pack, A&&>;

  if constexpr (can_structured_pack_match<void, Visitor, whole_pack, tail_pack>::value &&
                pack_invocable_exact_v<Visitor, whole_pack>) {
    auto next_bound = append_whole(std::forward<BoundTuple>(bound), std::forward<A>(a));
    return structured_dispatch_case_2_exact(std::forward<Visitor>(visitor), std::move(next_bound), std::forward<B>(b),
                                            std::forward<C>(c));
  } else if constexpr (is_tuple_like_v<A&&> &&
                       can_structured_pack_match<void, Visitor, pack_append_tuple_t<bound_pack, expanded_argument_tuple_t<A&&>>,
                                                 tail_pack>::value) {
    auto expanded_bound = append_expanded(std::forward<BoundTuple>(bound), std::forward<A>(a));
    return structured_dispatch_case_2_exact(std::forward<Visitor>(visitor), std::move(expanded_bound), std::forward<B>(b),
                                            std::forward<C>(c));
  } else {
    static_assert(always_false_v<BoundTuple>,
                  "structured_visit could not find a matching visitor signature for the selected variant alternatives.");
  }
}

template <class R, class Visitor, class BoundTuple, class A, class B, class C, class D>
constexpr auto structured_dispatch_case_4(Visitor&& visitor, BoundTuple&& bound, A&& a, B&& b, C&& c, D&& d) -> R {
  using bound_pack = tuple_to_pack_t<BoundTuple>;
  using tail_pack = type_pack<B&&, C&&, D&&>;
  using whole_pack = pack_append_t<bound_pack, A&&>;

  if constexpr (can_structured_pack_match<R, Visitor, whole_pack, tail_pack>::value) {
    auto next_bound = append_whole(std::forward<BoundTuple>(bound), std::forward<A>(a));
    return structured_dispatch_case_3<R>(std::forward<Visitor>(visitor), std::move(next_bound), std::forward<B>(b),
                                         std::forward<C>(c), std::forward<D>(d));
  } else if constexpr (can_structured_pack_expand_next<is_tuple_like_v<A&&>, R, Visitor, bound_pack, A&&,
                                                       tail_pack>::value) {
    auto expanded_bound = append_expanded(std::forward<BoundTuple>(bound), std::forward<A>(a));
    return structured_dispatch_case_3<R>(std::forward<Visitor>(visitor), std::move(expanded_bound), std::forward<B>(b),
                                         std::forward<C>(c), std::forward<D>(d));
  } else {
    static_assert(
        always_false_v<BoundTuple>,
        "structured_visit could not find a matching visitor signature for the selected variant alternatives.");

    if constexpr (std::is_void_v<R>) {
      return;
    } else {
      unreachable();
    }
  }
}

template <class Visitor, class BoundTuple, class A, class B, class C, class D>
constexpr decltype(auto) structured_dispatch_case_4_exact(Visitor&& visitor, BoundTuple&& bound, A&& a, B&& b, C&& c,
                                                          D&& d) {
  using bound_pack = tuple_to_pack_t<BoundTuple>;
  using tail_pack = type_pack<B&&, C&&, D&&>;
  using whole_pack = pack_append_t<bound_pack, A&&>;

  if constexpr (can_structured_pack_match<void, Visitor, whole_pack, tail_pack>::value &&
                pack_invocable_exact_v<Visitor, whole_pack>) {
    auto next_bound = append_whole(std::forward<BoundTuple>(bound), std::forward<A>(a));
    return structured_dispatch_case_3_exact(std::forward<Visitor>(visitor), std::move(next_bound), std::forward<B>(b),
                                            std::forward<C>(c), std::forward<D>(d));
  } else if constexpr (is_tuple_like_v<A&&> &&
                       can_structured_pack_match<void, Visitor, pack_append_tuple_t<bound_pack, expanded_argument_tuple_t<A&&>>,
                                                 tail_pack>::value) {
    auto expanded_bound = append_expanded(std::forward<BoundTuple>(bound), std::forward<A>(a));
    return structured_dispatch_case_3_exact(std::forward<Visitor>(visitor), std::move(expanded_bound), std::forward<B>(b),
                                            std::forward<C>(c), std::forward<D>(d));
  } else {
    static_assert(always_false_v<BoundTuple>,
                  "structured_visit could not find a matching visitor signature for the selected variant alternatives.");
  }
}

template <class R, class Visitor, class BoundTuple, class RemainingTuple>
constexpr auto structured_dispatch_recursive_impl(Visitor&& visitor, BoundTuple&& bound, RemainingTuple&& remaining) -> R {
  if constexpr (std::tuple_size_v<std::remove_reference_t<RemainingTuple>> == 0) {
    static_assert(
        pack_invocable_v<R, Visitor, tuple_to_pack_t<BoundTuple>>,
        "structured_visit could not find a matching visitor signature for the selected variant alternatives.");
    return invoke_from_tuple<R>(std::forward<Visitor>(visitor), std::forward<BoundTuple>(bound));
  } else {
    using bound_pack = tuple_to_pack_t<BoundTuple>;
    using next_type = std::tuple_element_t<0, std::remove_reference_t<RemainingTuple>>;
    using tail_tuple = decltype(tuple_tail(std::declval<RemainingTuple>()));
    using tail_pack = tuple_to_pack_t<tail_tuple>;
    using whole_pack = pack_append_t<bound_pack, next_type>;

    auto&& next = std::get<0>(std::forward<RemainingTuple>(remaining));
    auto tail = tuple_tail(std::forward<RemainingTuple>(remaining));

    if constexpr (can_structured_pack_match<R, Visitor, whole_pack, tail_pack>::value) {
      auto next_bound = append_whole(std::forward<BoundTuple>(bound), std::forward<decltype(next)>(next));
      return structured_dispatch_recursive_impl<R>(std::forward<Visitor>(visitor), std::move(next_bound),
                                                   std::move(tail));
    } else if constexpr (can_structured_pack_expand_next<is_tuple_like_v<next_type>, R, Visitor, bound_pack, next_type,
                                                         tail_pack>::value) {
      auto expanded_bound = append_expanded(std::forward<BoundTuple>(bound), std::forward<decltype(next)>(next));
      return structured_dispatch_recursive_impl<R>(std::forward<Visitor>(visitor), std::move(expanded_bound),
                                                   std::move(tail));
    } else {
      static_assert(
          always_false_v<BoundTuple>,
          "structured_visit could not find a matching visitor signature for the selected variant alternatives.");
    }

    if constexpr (std::is_void_v<R>) {
      return;
    } else {
      unreachable();
    }
  }
}

template <class Visitor, class BoundTuple, class RemainingTuple>
constexpr decltype(auto) structured_dispatch_recursive_impl_exact(Visitor&& visitor, BoundTuple&& bound,
                                                                  RemainingTuple&& remaining) {
  if constexpr (std::tuple_size_v<std::remove_reference_t<RemainingTuple>> == 0) {
    return structured_dispatch_case_0_exact(std::forward<Visitor>(visitor), std::forward<BoundTuple>(bound));
  } else {
    using bound_pack = tuple_to_pack_t<BoundTuple>;
    using next_type = std::tuple_element_t<0, std::remove_reference_t<RemainingTuple>>;
    using tail_tuple = decltype(tuple_tail(std::declval<RemainingTuple>()));
    using tail_pack = tuple_to_pack_t<tail_tuple>;
    using whole_pack = pack_append_t<bound_pack, next_type>;

    auto&& next = std::get<0>(std::forward<RemainingTuple>(remaining));
    auto tail = tuple_tail(std::forward<RemainingTuple>(remaining));

    if constexpr (can_structured_pack_match<void, Visitor, whole_pack, tail_pack>::value &&
                  pack_invocable_exact_v<Visitor, whole_pack>) {
      auto next_bound = append_whole(std::forward<BoundTuple>(bound), std::forward<decltype(next)>(next));
      return structured_dispatch_recursive_impl_exact(std::forward<Visitor>(visitor), std::move(next_bound),
                                                      std::move(tail));
    } else if constexpr (is_tuple_like_v<next_type> &&
                         can_structured_pack_match<void, Visitor,
                                                   pack_append_tuple_t<bound_pack, expanded_argument_tuple_t<next_type>>,
                                                   tail_pack>::value) {
      auto expanded_bound = append_expanded(std::forward<BoundTuple>(bound), std::forward<decltype(next)>(next));
      return structured_dispatch_recursive_impl_exact(std::forward<Visitor>(visitor), std::move(expanded_bound),
                                                      std::move(tail));
    } else {
      static_assert(always_false_v<BoundTuple>,
                    "structured_visit could not find a matching visitor signature for the selected variant alternatives.");
    }
  }
}

template <class R, class Visitor, class BoundTuple, class RemainingTuple>
constexpr auto structured_dispatch_impl(Visitor&& visitor, BoundTuple&& bound, RemainingTuple&& remaining) -> R {
  constexpr auto kArity = std::tuple_size_v<std::remove_reference_t<RemainingTuple>>;
  if constexpr (kArity <= 4) {
    if constexpr (kArity == 0) {
      return structured_dispatch_case_0<R>(std::forward<Visitor>(visitor), std::forward<BoundTuple>(bound));
    } else if constexpr (kArity == 1) {
      auto&& a = std::get<0>(std::forward<RemainingTuple>(remaining));
      return structured_dispatch_case_1<R>(std::forward<Visitor>(visitor), std::forward<BoundTuple>(bound),
                                           std::forward<decltype(a)>(a));
    } else if constexpr (kArity == 2) {
      auto&& a = std::get<0>(std::forward<RemainingTuple>(remaining));
      auto&& b = std::get<1>(std::forward<RemainingTuple>(remaining));
      return structured_dispatch_case_2<R>(std::forward<Visitor>(visitor), std::forward<BoundTuple>(bound),
                                           std::forward<decltype(a)>(a), std::forward<decltype(b)>(b));
    } else if constexpr (kArity == 3) {
      auto&& a = std::get<0>(std::forward<RemainingTuple>(remaining));
      auto&& b = std::get<1>(std::forward<RemainingTuple>(remaining));
      auto&& c = std::get<2>(std::forward<RemainingTuple>(remaining));
      return structured_dispatch_case_3<R>(std::forward<Visitor>(visitor), std::forward<BoundTuple>(bound),
                                           std::forward<decltype(a)>(a), std::forward<decltype(b)>(b),
                                           std::forward<decltype(c)>(c));
    } else {
      auto&& a = std::get<0>(std::forward<RemainingTuple>(remaining));
      auto&& b = std::get<1>(std::forward<RemainingTuple>(remaining));
      auto&& c = std::get<2>(std::forward<RemainingTuple>(remaining));
      auto&& d = std::get<3>(std::forward<RemainingTuple>(remaining));
      return structured_dispatch_case_4<R>(std::forward<Visitor>(visitor), std::forward<BoundTuple>(bound),
                                           std::forward<decltype(a)>(a), std::forward<decltype(b)>(b),
                                           std::forward<decltype(c)>(c), std::forward<decltype(d)>(d));
    }
  } else {
    return structured_dispatch_recursive_impl<R>(std::forward<Visitor>(visitor), std::forward<BoundTuple>(bound),
                                                 std::forward<RemainingTuple>(remaining));
  }
}

template <class Visitor, class BoundTuple, class RemainingTuple>
constexpr decltype(auto) structured_dispatch_impl_exact(Visitor&& visitor, BoundTuple&& bound, RemainingTuple&& remaining) {
  constexpr auto kArity = std::tuple_size_v<std::remove_reference_t<RemainingTuple>>;
  if constexpr (kArity <= 4) {
    if constexpr (kArity == 0) {
      return structured_dispatch_case_0_exact(std::forward<Visitor>(visitor), std::forward<BoundTuple>(bound));
    } else if constexpr (kArity == 1) {
      auto&& a = std::get<0>(std::forward<RemainingTuple>(remaining));
      return structured_dispatch_case_1_exact(std::forward<Visitor>(visitor), std::forward<BoundTuple>(bound),
                                              std::forward<decltype(a)>(a));
    } else if constexpr (kArity == 2) {
      auto&& a = std::get<0>(std::forward<RemainingTuple>(remaining));
      auto&& b = std::get<1>(std::forward<RemainingTuple>(remaining));
      return structured_dispatch_case_2_exact(std::forward<Visitor>(visitor), std::forward<BoundTuple>(bound),
                                              std::forward<decltype(a)>(a), std::forward<decltype(b)>(b));
    } else if constexpr (kArity == 3) {
      auto&& a = std::get<0>(std::forward<RemainingTuple>(remaining));
      auto&& b = std::get<1>(std::forward<RemainingTuple>(remaining));
      auto&& c = std::get<2>(std::forward<RemainingTuple>(remaining));
      return structured_dispatch_case_3_exact(std::forward<Visitor>(visitor), std::forward<BoundTuple>(bound),
                                              std::forward<decltype(a)>(a), std::forward<decltype(b)>(b),
                                              std::forward<decltype(c)>(c));
    } else {
      auto&& a = std::get<0>(std::forward<RemainingTuple>(remaining));
      auto&& b = std::get<1>(std::forward<RemainingTuple>(remaining));
      auto&& c = std::get<2>(std::forward<RemainingTuple>(remaining));
      auto&& d = std::get<3>(std::forward<RemainingTuple>(remaining));
      return structured_dispatch_case_4_exact(std::forward<Visitor>(visitor), std::forward<BoundTuple>(bound),
                                              std::forward<decltype(a)>(a), std::forward<decltype(b)>(b),
                                              std::forward<decltype(c)>(c), std::forward<decltype(d)>(d));
    }
  } else {
    return structured_dispatch_recursive_impl_exact(std::forward<Visitor>(visitor), std::forward<BoundTuple>(bound),
                                                    std::forward<RemainingTuple>(remaining));
  }
}

template <class VisitorRef>
struct structured_visit_result_visitor {
  template <class... Alternatives>
  constexpr auto operator()(Alternatives&&...) const
      -> decltype(structured_dispatch_impl_exact(std::declval<VisitorRef>(), std::tuple<>{},
                                                 std::forward_as_tuple(std::declval<Alternatives>()...)));
};

template <class VisitorRef, class... Variants>
using structured_visit_result_t =
    decltype(std::visit(std::declval<structured_visit_result_visitor<VisitorRef>>(), std::declval<Variants>()...));

}  // namespace detail

template <class Visitor, class... Variants>
constexpr auto structured_visit(Visitor&& v, Variants&&... values)
    -> detail::structured_visit_result_t<Visitor&, Variants&&...> {
  static_assert((detail::is_variant_like_v<Variants> && ...), "structured_visit requires std::variant-like inputs.");

  auto&& visitor_ref = v;
  return std::visit(
      [&visitor_ref]<class... Alternatives>(Alternatives&&... alternatives) -> decltype(auto) {
        return detail::structured_dispatch_impl_exact(visitor_ref, std::tuple<>{},
                                                      std::forward_as_tuple(std::forward<Alternatives>(alternatives)...));
      },
      std::forward<Variants>(values)...);
}

template <class R, class Visitor, class... Variants>
constexpr auto structured_visit(Visitor&& v, Variants&&... values) -> R {
  static_assert((detail::is_variant_like_v<Variants> && ...), "structured_visit requires std::variant-like inputs.");

  // Whole selected alternatives are preferred. If that does not match the
  // visitor, tuple-like selected alternatives are expanded according to the
  // tuple protocol described above.

  auto&& visitor_ref = v;
  return std::visit(
      [&visitor_ref]<class... Alternatives>(Alternatives&&... alternatives) -> R {
        return detail::structured_dispatch_impl<R>(visitor_ref, std::tuple<>{},
                                                   std::forward_as_tuple(std::forward<Alternatives>(alternatives)...));
      },
      std::forward<Variants>(values)...);
}

}  // namespace fleaux::utility