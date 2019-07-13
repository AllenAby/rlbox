#pragma once
// IWYU pragma: private, include "rlbox.hpp"
// IWYU pragma: friend "rlbox_.*\.hpp"

#include <type_traits>

#include "rlbox_types.hpp"
#include "rlbox_typetraits.hpp"

namespace rlbox::detail {

template<typename T_Rhs>
inline auto unwrap_value(const T_Rhs& rhs) noexcept
{
  if constexpr (std::is_base_of_v<sandbox_wrapper_base, T_Rhs>) {
    return rhs.UNSAFE_Unverified();
  } else {
    return rhs;
  }
}

}