#pragma once
// IWYU pragma: private, include "rlbox.hpp"
// IWYU pragma: friend "rlbox_.*\.hpp"

#include <cstdlib>
#include <map>
#include <mutex>
#include <type_traits>
#include <utility>

#include "rlbox_helpers.hpp"
#include "rlbox_struct_support.hpp"
#include "rlbox_typetraits.hpp"

namespace rlbox {

template<typename T_Sbx>
class RLBoxSandbox : protected T_Sbx
{
  KEEP_CLASSES_FRIENDLY

private:
  std::mutex func_ptr_cache_lock;
  std::map<std::string, void*> func_ptr_map;

  template<typename T>
  inline auto invoke_process_param(T&& param)
  {
    using T_NoRef = std::remove_reference_t<T>;

    if_constexpr_named(cond1, std::is_base_of_v<sandbox_wrapper_base, T_NoRef>)
    {
      return param.UNSAFE_Sandboxed();
    }
    else if_constexpr_named(cond2, detail::is_fundamental_or_enum_v<T_NoRef>)
    {
      // For unwrapped primitives, assign to a tainted var and then unwrap so
      // that we adjust for machine model
      tainted<T_NoRef, T_Sbx> copy = param;
      return copy.UNSAFE_Sandboxed();
    }
    else
    {
      constexpr auto unknownCase = !(cond1 || cond2);
      rlbox_detail_static_fail_because(
        unknownCase,
        "Arguments to a sandbox function call should be primitives  or wrapped "
        "types like tainted, callbacks etc.");
    }
  }

  template<typename T_Ret, typename... T_Args>
  static detail::convert_to_sandbox_equivalent_t<T_Ret, T_Sbx>
  sandbox_callback_interceptor(
    detail::convert_to_sandbox_equivalent_t<T_Args, T_Sbx>...)
  {
    std::pair<T_Sbx*, void*> context =
      T_Sbx::impl_get_executed_callback_sandbox_and_key();
    (void)context;
    throw "TODO: Not implemented";
  }

public:
  /***** Function to adjust for custom machine models *****/

  template<typename T>
  using convert_to_sandbox_equivalent_nonclass_t =
    detail::convert_base_types_t<T,
                                 typename T_Sbx::T_IntType,
                                 typename T_Sbx::T_LongType,
                                 typename T_Sbx::T_LongLongType,
                                 typename T_Sbx::T_PointerType>;

  T_Sbx* get_sandbox_impl() { return this; }

  template<typename... T_Args>
  inline auto create_sandbox(T_Args... args)
  {
    return this->impl_create_sandbox(std::forward<T_Args>(args)...);
  }

  inline auto destroy_sandbox() { return this->impl_destroy_sandbox(); }

  template<typename T>
  inline T* get_unsandboxed_pointer(
    convert_to_sandbox_equivalent_nonclass_t<T*> p) const
  {
    if (p == 0) {
      return nullptr;
    }
    auto ret = this->template impl_get_unsandboxed_pointer<T>(p);
    return reinterpret_cast<T*>(ret);
  }

  template<typename T>
  inline convert_to_sandbox_equivalent_nonclass_t<T*> get_sandboxed_pointer(
    const void* p) const
  {
    if (p == nullptr) {
      return 0;
    }
    return this->template impl_get_sandboxed_pointer<T>(p);
  }

  template<typename T>
  static inline T* get_unsandboxed_pointer_no_ctx(
    convert_to_sandbox_equivalent_nonclass_t<T*> p,
    const void* example_unsandboxed_ptr)
  {
    if (p == 0) {
      return nullptr;
    }
    auto ret = T_Sbx::template impl_get_unsandboxed_pointer_no_ctx<T>(
      p, example_unsandboxed_ptr);
    return reinterpret_cast<T*>(ret);
  }

  template<typename T>
  static inline convert_to_sandbox_equivalent_nonclass_t<T*>
  get_sandboxed_pointer_no_ctx(const void* p)
  {
    if (p == nullptr) {
      return 0;
    }
    return T_Sbx::template impl_get_sandboxed_pointer_no_ctx<T>(p);
  }

  template<typename T>
  inline tainted<T*, T_Sbx> malloc_in_sandbox()
  {
    const uint32_t defaultCount = 1;
    return malloc_in_sandbox<T>(defaultCount);
  }
  template<typename T>
  inline tainted<T*, T_Sbx> malloc_in_sandbox(uint32_t count)
  {
    detail::dynamic_check(count != 0, "Malloc tried to allocate 0 bytes");
    auto ptr_in_sandbox = this->impl_malloc_in_sandbox(sizeof(T) * count);
    auto ptr = get_unsandboxed_pointer<T>(ptr_in_sandbox);
    detail::dynamic_check(is_pointer_in_sandbox_memory(ptr),
                          "Malloc returned pointer outside the sandbox memory");
    auto ptr_end = reinterpret_cast<uintptr_t>(ptr + (count - 1));
    detail::dynamic_check(
      is_in_same_sandbox(ptr, reinterpret_cast<void*>(ptr_end)),
      "Malloc returned a pointer whose range goes beyond sandbox memory");
    auto cast_ptr = reinterpret_cast<T*>(ptr);
    return tainted<T*, T_Sbx>(cast_ptr);
  }

  template<typename T>
  inline void free_in_sandbox(tainted<T*, T_Sbx> ptr)
  {
    this->impl_free_in_sandbox(ptr.get_raw_sandbox_value());
  }

  static inline bool is_in_same_sandbox(const void* p1, const void* p2)
  {
    return T_Sbx::impl_is_in_same_sandbox(p1, p2);
  }

  inline bool is_pointer_in_sandbox_memory(const void* p)
  {
    return this->impl_is_pointer_in_sandbox_memory(p);
  }

  inline size_t get_total_memory() { return this->impl_get_total_memory(); }

  void* lookup_symbol(const char* func_name)
  {
    std::lock_guard<std::mutex> lock(func_ptr_cache_lock);

    auto func_ptr_ref = func_ptr_map.find(func_name);

    void* func_ptr;
    if (func_ptr_ref == func_ptr_map.end()) {
      func_ptr = this->impl_lookup_symbol(func_name);
      func_ptr_map[func_name] = func_ptr;
    } else {
      func_ptr = func_ptr_ref->second;
    }

    return func_ptr;
  }

  template<typename T, typename... T_Args>
  auto invoke_with_func_ptr(void* func_ptr, T_Args&&... params)
  {
    static_assert(
      std::is_invocable_v<T, detail::rlbox_remove_wrapper_t<T_Args>...>,
      "Mismatched arguments types for function");

    using T_Result = std::invoke_result_t<T, T_Args...>;

    if constexpr (std::is_void_v<T_Result>) {
      this->impl_invoke_with_func_ptr(reinterpret_cast<T*>(func_ptr),
                                      invoke_process_param(params)...);
      return;
    } else {
      auto raw_result = this->impl_invoke_with_func_ptr(
        reinterpret_cast<T*>(func_ptr), invoke_process_param(params)...);
      auto cast_result =
        reinterpret_cast<tainted_volatile<T_Result, T_Sbx>*>(&raw_result);
      tainted<T_Result, T_Sbx> wrapped_result = *cast_result;
      return wrapped_result;
    }
  }

  template<typename T_Ret, typename... T_Args>
  using T_Cb_no_wrap = detail::rlbox_remove_wrapper_t<T_Ret>(
    detail::rlbox_remove_wrapper_t<T_Args>...);

  template<typename T_Ret>
  sandbox_callback<T_Cb_no_wrap<T_Ret>*, T_Sbx> register_callback(T_Ret (*)())
  {
    rlbox_detail_static_fail_because(
      detail::true_v<T_Ret>,
      "Modify the callback to change the first parameter to a sandbox."
      "For instance if a callback has type\n\n"
      "int foo() {...}\n\n"
      "Change this to \n\n"
      "tainted<int, T_Sbx> foo(RLBoxSandbox<T_Sbx>& sandbox) {...}\n");

    // this is never executed, but we need it for the function to type-check
    std::abort();
  }

  template<typename T_RL, typename T_Ret, typename... T_Args>
  sandbox_callback<T_Cb_no_wrap<T_Ret, T_Args...>*, T_Sbx> register_callback(
    T_Ret (*func_ptr)(T_RL, T_Args...))
  {
    // Some branches don't use func_ptr, so make "use" it to get past linter
    (void)func_ptr;
    if_constexpr_named(cond1, !std::is_same_v<T_RL, RLBoxSandbox<T_Sbx>&>)
    {
      rlbox_detail_static_fail_because(
        cond1,
        "Modify the callback to change the first parameter to a sandbox."
        "For instance if a callback has type\n\n"
        "int foo(int a, int b) {...}\n\n"
        "Change this to \n\n"
        "tainted<int, T_Sbx> foo(RLBoxSandbox<T_Sbx>& sandbox,"
        "tainted<int, T_Sbx> a, tainted<int, T_Sbx> b) {...}\n");
    }
    else if_constexpr_named(
      cond2, !(std::is_base_of_v<sandbox_wrapper_base, T_Args> && ...))
    {
      rlbox_detail_static_fail_because(
        cond2,
        "Change all arguments to the callback have to be tainted."
        "For instance if a callback has type\n\n"
        "int foo(int a, int b) {...}\n\n"
        "Change this to \n\n"
        "tainted<int, T_Sbx> foo(RLBoxSandbox<T_Sbx>& sandbox,"
        "tainted<int, T_Sbx> a, tainted<int, T_Sbx> b) {...}\n");
    }
    else if_constexpr_named(cond3,
                            !(std::is_void_v<T_Ret> ||
                              std::is_base_of_v<sandbox_wrapper_base, T_Ret>))
    {
      rlbox_detail_static_fail_because(
        cond3,
        "Change the callback return type to be tainted if it is not void."
        "For instance if a callback has type\n\n"
        "int foo(int a, int b) {...}\n\n"
        "Change this to \n\n"
        "tainted<int, T_Sbx> foo(RLBoxSandbox<T_Sbx>& sandbox,"
        "tainted<int, T_Sbx> a, tainted<int, T_Sbx> b) {...}\n");
    }
    else
    {
      auto callback_interceptor =
        sandbox_callback_interceptor<detail::rlbox_remove_wrapper_t<T_Ret>,
                                     detail::rlbox_remove_wrapper_t<T_Args>...>;

      // Need unique key for each callback we register - just use the func addr
      void* unique_key = reinterpret_cast<void*>(func_ptr);

      auto callback_trampoline = this->template impl_register_callback<
        detail::rlbox_remove_wrapper_t<T_Ret>,
        detail::rlbox_remove_wrapper_t<T_Args>...>(
        unique_key, reinterpret_cast<void*>(callback_interceptor));

      auto ret = sandbox_callback<T_Cb_no_wrap<T_Ret, T_Args...>*, T_Sbx>(
        this, func_ptr, callback_interceptor, callback_trampoline);
      return ret;
    }
  }
};

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#elif defined(__GNUC__) || defined(__GNUG__)
// Can't turn off the variadic macro warning emitted from -pedantic
#  pragma GCC system_header
#elif defined(_MSC_VER)
// Doesn't seem to emit the warning
#else
// Don't know the compiler... just let it go through
#endif

#if defined(RLBOX_USE_STATIC_CALLS)
#  define sandbox_lookup_symbol_helper(prefix, sandbox, func_name)             \
    prefix(sandbox, func_name)

#  define sandbox_lookup_symbol(sandbox, func_name)                            \
    sandbox_lookup_symbol_helper(RLBOX_USE_STATIC_CALLS(), sandbox, func_name)
#else
#  define sandbox_lookup_symbol(sandbox, func_name)                            \
    sandbox.lookup_symbol(#func_name)
#endif

#define sandbox_invoke(sandbox, func_name, ...)                                \
  sandbox.invoke_with_func_ptr<decltype(func_name)>(                           \
    sandbox_lookup_symbol(sandbox, func_name), ##__VA_ARGS__)

#if defined(__clang__)
#  pragma clang diagnostic pop
#else
#endif

}