#pragma once

#include <type_traits>
#include <tuple>
#include <functional>
#include <string>
#include "jg_verify.h"
#include "jg_string.h"

//
// These mocking macros are defined and documented at the bottom of this file:
//
//   - JG_MOCK
//   - JG_MOCK_REF
//
// These compilation flags affect how jg::mock is built 
//
//   - JG_MOCK_ENABLE_SHORT_NAMES: Enables mocking macros named without the "JG_" prefix.
//

//
// Note that some MSVC versions require /Zc:__cplusplus even if /std:c++14 or higher is specified
//
#if (__cplusplus < 201402L)
#error jg::mock needs C++14 or newer
#endif

namespace jg
{
namespace detail 
{

template <typename T>
using base_t = std::remove_const_t<std::remove_reference_t<T>>; // C++20 has std::remove_cvref for this.

template <typename ...T>
using tuple_params_t = std::tuple<base_t<T>...>;

template<size_t N, typename... Params>
using nth_param_t = std::tuple_element_t<N, std::tuple<Params...>>;

template <size_t N, typename... Params>
using nth_param_base_t = base_t<nth_param_t<N, Params...>>;

template <size_t N, typename... Params>
nth_param_t<N, Params...> nth_param(Params&&... params)
{
    return std::get<N>(std::forward_as_tuple(params...));
}

// The auxiliary data for a mock function that takes N parameters has `param<1>(), ..., param<N>()` members
// holding the actual N parameters the function was last called with, for usage in tests.
template <size_t N, typename ...Params>
class mock_aux_parameters
{
public:
    template <size_t Number>
    auto param() const { return std::get<Number - 1>(m_params); }

protected:
    template <typename, typename>
    friend class mock_impl;

    template <typename ...Params2>
    void set_params(Params2&&... params) { m_params = std::make_tuple(params...); }

    tuple_params_t<Params...> m_params;
};

// The auxiliary data for a mock function that takes no parameters has no parameter-holding member.
template <>
class mock_aux_parameters<0>
{
};

// Verifies that the wrapped value is set before it gets used.
template <typename T, typename Enable = void>
class verified;

template <typename T>
class verified<T, std::enable_if_t<std::is_reference_v<T>>> final
{
public:
    verified& operator=(T other)
    {
        value = &other;
        assigned = true;
        return *this;
    }

    operator T()
    {
        verify(assigned);
        return *value;
    }

private:
    std::remove_reference_t<T>* value = nullptr;
    bool assigned = false;
};

template <typename T>
class verified<T, std::enable_if_t<!std::is_reference_v<T>>> final
{
public:
    verified& operator=(const T& other)
    {
        value = other;
        assigned = true;
        return *this;
    }

    verified& operator=(T&& other)
    {
        value = std::move(other);
        assigned = true;
        return *this;
    }

    operator T()
    {
        verify(assigned);
        return value;
    }

private:
    T value{};
    bool assigned = false;
};

// The auxiliary data for a mock function that returns non-`void` has a `result` member
// that can be set in tests. This is the simplest way to just return a specific value from a
// mock function. The other way is to assign a callable (like a lambda) to the `func` member,
// but if just a return value needs to be modeled, then `result` is the easier way.
template <typename T>
class mock_aux_return
{
public:
    // Verifying that a test doesn't use it without first setting it.
    verified<T> result;
};

// The auxiliary data for a mock function that returns `void` has no `result` member, and
// the function implementation can only be controlled by assigning a callable (like a lambda)
// to the `func` member.
template <>
class mock_aux_return<void>
{
};

template <typename T, typename ...Params>
class mock_aux final : public mock_aux_return<T>, public mock_aux_parameters<sizeof...(Params), Params...>
{
public:
    mock_aux(std::string prototype)
        : m_count(0)
        , m_prototype(trim(prototype, " "))
    {}

    std::function<T(Params...)> func;
    size_t                      count() const { return m_count; }
    bool                        called() const { return m_count > 0; }
    std::string                 prototype() const { return m_prototype; }

    void                        reset() { *this = mock_aux(m_prototype); }

private:
    template <typename, typename>
    friend class mock_impl;
    
    size_t m_count;
    std::string m_prototype;
};

template <typename T, typename TImpl, typename Enable = void>
class mock_impl_base;

// A mock function that returns `void` only calls `func` in its
// auxiliary data if it's set in the test. Nothing is done if it's not set.
template <typename T, typename TImpl>
class mock_impl_base<T, TImpl, std::enable_if_t<std::is_same<T, void>::value>>
{
public:
    template <typename... Params>
    void impl(Params&&... params)
    {        
        auto& aux = static_cast<TImpl*>(this)->aux;

        if (aux.func)
            aux.func(std::forward<Params>(params)...);
    }
};

// A mock function that returns non-`void` calls the `func` member
// in its auxiliary data if it's set in the test. If the `func` member isn't set, then the `result`
// member is used instead. If the `result` member isn't set, then by default an assertion failure is
// triggered and a stack trace is output, or the default value for the `result` member type is
// returned if the bahavior is non-default. The documentation for `jg::verify` has details on how to
// configure the assertion behavior at compile time.
// @see jg::verify
template <typename T, typename TImpl>
class mock_impl_base<T, TImpl, std::enable_if_t<!std::is_same<T, void>::value>>
{
public:
    template <typename... Params>
    T impl(Params&&... params)
    {
        auto& aux = static_cast<TImpl*>(this)->aux;
        
        if (aux.func)
            return aux.func(std::forward<Params>(params)...);
        
        return aux.result;
    }
};

// A mock function does 2 things: it makes sure that its auxiliary data state (call counter, etc.)
// has been updated when the function returns, and it calls the client supplied callable or returns
// the client supplied result.
template <typename T, typename TMockAux>
class mock_impl final : public mock_impl_base<T, mock_impl<T, TMockAux>>
{
public:
    TMockAux& aux;

    mock_impl(const TMockAux& aux)
        : aux(const_cast<TMockAux&>(aux)) // Minor hack to be able to use the same JG_MOCK macro for
                                          // both member functions and free functions. `mutable`
                                          // would otherwise be needed for some member functions,
                                          // and that would require separate macro implementations.
                                          // It's a "minor" hack because it's an implementation
                                          // detail and we know that the original instance is non-const.
    {}

    ~mock_impl()
    {
        aux.m_count++;
    }

    template <typename... Params>
    T impl(Params&&... params)
    {
        aux.set_params(std::forward<Params>(params)...);
        return mock_impl_base::impl(std::forward<Params>(params)...);
    }

    T impl()
    {
        return mock_impl_base::impl();
    }
};

} // namespace detail
} // namespace jg

// The variadic macro parameter count macros below are derived from these links:
//
// https://stackoverflow.com/questions/5530505/why-does-this-variadic-argument-count-macro-fail-with-vc
// https://stackoverflow.com/questions/26682812/argument-counting-macro-with-zero-arguments-for-visualstudio
// https://stackoverflow.com/questions/9183993/msvc-variadic-macro-expansion?rq=1

#define _JG_CONCAT3(x, y, z) x ## y ## z
#define _JG_CONCAT2(x, y) x ## y
#define _JG_CONCAT(x, y) _JG_CONCAT2(x, y)
#define _JG_EXPAND(x) x
#define _JG_GLUE(x, y) x y
#define _JG_VA_COUNT_2(x0,x1,x2,x3,x4,x5,x6,x7,x8,x9,x10,N,...) N
//
// Clang and GCC handles empty __VA_ARGS__ differently from MSVC.
//
#ifdef _MSC_VER
#define _JG_VA_COUNT_1(...) _JG_EXPAND(_JG_VA_COUNT_2(__VA_ARGS__,10,9,8,7,6,5,4,3,2,1,0))
#define _JG_AUGMENTER(...) unused, __VA_ARGS__
#define _JG_VA_COUNT(...) _JG_VA_COUNT_1(_JG_AUGMENTER(__VA_ARGS__))
#else
#define _JG_VA_COUNT_1(...) _JG_EXPAND(_JG_VA_COUNT_2(0, ## __VA_ARGS__,10,9,8,7,6,5,4,3,2,1,0))
#define _JG_VA_COUNT(...) _JG_VA_COUNT_1(__VA_ARGS__)
#endif

#define _JG_MOCK_FUNC_PARAMS_DECL_0()
#define _JG_MOCK_FUNC_PARAMS_DECL_1(T1) T1 p1
#define _JG_MOCK_FUNC_PARAMS_DECL_2(T1, T2) T1 p1, T2 p2
#define _JG_MOCK_FUNC_PARAMS_DECL_3(T1, T2, T3) T1 p1, T2 p2, T3 p3
#define _JG_MOCK_FUNC_PARAMS_DECL_4(T1, T2, T3, T4) T1 p1, T2 p2, T3 p3, T4 p4
#define _JG_MOCK_FUNC_PARAMS_DECL_5(T1, T2, T3, T4, T5) T1 p1, T2 p2, T3 p3, T4 p4, T5 p5
#define _JG_MOCK_FUNC_PARAMS_DECL_6(T1, T2, T3, T4, T5, T6) T1 p1, T2 p2, T3 p3, T4 p4, T5 p5, T6 p6
#define _JG_MOCK_FUNC_PARAMS_DECL_7(T1, T2, T3, T4, T5, T6, T7) T1 p1, T2 p2, T3 p3, T4 p4, T5 p5, T6 p6, T7 p7
#define _JG_MOCK_FUNC_PARAMS_DECL_8(T1, T2, T3, T4, T5, T6, T7, T8) T1 p1, T2 p2, T3 p3, T4 p4, T5 p5, T6 p6, T7 p7, T8 p8
#define _JG_MOCK_FUNC_PARAMS_DECL_9(T1, T2, T3, T4, T5, T6, T7, T8, T9) T1 p1, T2 p2, T3 p3, T4 p4, T5 p5, T6 p6, T7 p7, T8 p8, T9 p9
#define _JG_MOCK_FUNC_PARAMS_DECL_10(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10) T1 p1, T2 p2, T3 p3, T4 p4, T5 p5, T6 p6, T7 p7, T8 p8, T9 p9, T10 p10

#define _JG_MOCK_FUNC_PARAMS_CALL_0()
#define _JG_MOCK_FUNC_PARAMS_CALL_1(T1) p1
#define _JG_MOCK_FUNC_PARAMS_CALL_2(T1, T2) p1, p2
#define _JG_MOCK_FUNC_PARAMS_CALL_3(T1, T2, T3) p1, p2, p3
#define _JG_MOCK_FUNC_PARAMS_CALL_4(T1, T2, T3, T4) p1, p2, p3, p4
#define _JG_MOCK_FUNC_PARAMS_CALL_5(T1, T2, T3, T4, T5) p1, p2, p3, p4, p5
#define _JG_MOCK_FUNC_PARAMS_CALL_6(T1, T2, T3, T4, T5, T6) p1, p2, p3, p4, p5, p6
#define _JG_MOCK_FUNC_PARAMS_CALL_7(T1, T2, T3, T4, T5, T6, T7) p1, p2, p3, p4, p5, p6, p7
#define _JG_MOCK_FUNC_PARAMS_CALL_8(T1, T2, T3, T4, T5, T6, T7, T8) p1, p2, p3, p4, p5, p6, p7, p8
#define _JG_MOCK_FUNC_PARAMS_CALL_9(T1, T2, T3, T4, T5, T6, T7, T8, T9) p1, p2, p3, p4, p5, p6, p7, p8, p9
#define _JG_MOCK_FUNC_PARAMS_CALL_10(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10) p1, p2, p3, p4, p5, p6, p7, p8, p9, p10

#define _JG_MOCK_FUNC_PARAMS_DECL(...) \
    _JG_GLUE(_JG_CONCAT(_JG_MOCK_FUNC_PARAMS_DECL_, _JG_VA_COUNT(__VA_ARGS__)), (__VA_ARGS__))

#define _JG_MOCK_FUNC_PARAMS_CALL(...) \
    _JG_GLUE(_JG_CONCAT(_JG_MOCK_FUNC_PARAMS_CALL_, _JG_VA_COUNT(__VA_ARGS__)), (__VA_ARGS__))

// --------------------------- Implementation details go above this line ---------------------------

/// @macro JG_MOCK
///
/// Defines a free function, or a virtual member function override, with auxiliary data for use in testing.
///
/// @section Mocking a virtual member function
///
/// Assume that the virtual function `find_by_id` is a member of the base class `user_names`:
///
///     class user_names
///     {
///     public:
///         virtual const char* find_by_id(int id) = 0;
///         ...
///     };
///
/// We can mock that function in a test by deriving a mock class `mock_user_names` from `user_names`
/// and use the `JG_MOCK` macro in its body:
///
///     class mock_user_names final : public user_names
///     {
///     public:
///         JG_MOCK(,,, const char*, find_by_id, int);
///         ...
///     };
///
/// It's functionally equivalent to write this:
///
///     class mock_user_names final : public user_names
///     {
///     public:
///         JG_MOCK(virtual, override,, const char*, find_by_id, int);
///         ...
///     };
///
/// Since `virtual` and `override` aren't strictly mandatory to use when overriding virtual base class
/// functions, they can be omitted from the `JG_MOCK` declaration to make it easier to read. As the
/// `JG_MOCK` declaration shows, its first two parameters are named `prefix` and `suffix` and they'll
/// simply be pasted before and after the function declaration by the macro. The third parameter is named
/// `overload_suffix` and it's empty in this example, which just means that we're not mocking an overloaded
/// function. If we were, we could set a suffix here that would be added to the auxiliary data
/// name for the particular overload.
///
/// The mock class can be instantiated in a test and passed to a type or a function under test
/// that depends on the base class `user_names` for its functionality. By doing this, we can manipulate how
/// that tested entity behaves regarding how it uses its `user_names` dependency. The `JG_MOCK` macro sets
/// up some tools to help with that task in the test - the mock function auxiliary data. 
///
/// The `JG_MOCK` macro does two things. First, it sets up an auxiliary data structure that can be used
/// to 1) control what the mock function does when it's called, and 2) examine how it was called. Second,
/// it creates a function body that will use the auxiliary data in it's implementation.  
///
/// The mock function implementation can be controlled in a test 1) by intercepting calls to it by setting
/// the auxiliary data `func` to a callable (like a lambda) that does the actual implementation, or 2) by
/// setting the auxiliary data `result` to a value that will be returned by the function.
///
/// Typical usage of a the mock class we defined above:
///     
///     TEST("tested entity can do its job")
///     {
///         // The mock class instance.
///         mock_user_names names;
///     
///         // Whenever user_names::find_by_id is called by the tested entity, it always returns "Donald Duck".
///         names.find_by_id_.result = "Donald Duck";
///     
///         // Depends on user_names
///         some_tested_entity tested_entity(user_names);
///     
///         TEST_ASSERT(tested_entity.can_do_its_job());      // Allegedly uses user_names::find_by_id()
///         TEST_ASSERT(names.find_by_id_.called());          // Did the tested entity even call it?
///         TEST_ASSERT(names.find_by_id_.param<1>() < 4711); // Did the tested entity pass it a valid id?
///     }
///
/// The `func` auxiliary data member can be used instead of `result`, or when the mock function is
/// a void function and the auxiliary data simply doesn't have a `result` member.
///
///     TEST("some_tested_entity retries twice when failing")
///     {
///         // The mock class instance.
///         mock_user_names names;
///
///         // Make the mock fail twice.
///         names.find_by_id_.func = [] (int id)
///         {
///             switch (id)
///             {
///                 case 0:  return "Huey";
///                 case 1:  return "Dewey";
///                 case 2:  return "Louie";
///                 default: return nullptr;
///             }
///         };
///     
///         // Depends on user_names
///         nephew_reporter tested_entity(user_names);
///
///         const std::string expected_format = "Huey! Dewey? Louie!?";
///
///         TEST_ASSERT(tested_entity.format_nephews() == expected_format);
///         TEST_ASSERT(user_names.find_by_id_.count() == 3);
///     }
///
/// @section Mocking a free function
///
/// Assume that there is a free function `find_by_id` that we call in production code:
///
///     const char* find_by_id(int id);
///
/// We can mock it in a test, using the `JG_MOCK` macro in the same way as with the virtual function:
///
///     JG_MOCK(,,, const char*, find_by_id, int);
///
/// Typical usage of this mock:
///     
///     TEST("tested entity can do its job")
///     {
///         find_by_id_.reset();
///         find_by_id_.result = "Donald Duck";
///     
///         // Depends on find_by_id()
///         some_tested_entity tested_entity;
///
///         TEST_ASSERT(tested_entity.can_do_its_job()); // Allegedly uses find_by_id()
///         TEST_ASSERT(find_by_id_.called());           // Did the tested entity even call it?
///         TEST_ASSERT(find_by_id_.param<1>() < 4711);  // Did the tested entity pass it a valid id?
///     }
///
/// Note that, since this mock is global, its auxiliary data must be `reset()` in each test case.
/// That's not needed when virtual functions of a class is mocked, since their auxiliary data state
/// gets reset every time the mock class is instantiated. Regardless if a mock is global or not, its
/// auxiliary data _can_ be reset at any time in a test if needed.
///
/// @section Auxiliary data members
///
/// The auxiliary data members available for a mock function `foo` that returns void and takes 0 parameters are:
///
///     std::function<void()>            foo_.func;        // can be set in a test
///     ---------------------------------------------------------------------------
///     bool                             foo_.called();    // set by the mocking framework
///     size_t                           foo_.count();     // set by the mocking framework
///     std::string                      foo_.prototype(); // set by the mocking framework
///
/// The auxiliary data members available for a mock function `foo` that returns `T` and takes 0 parameters are:
///
///     std::function<T()>               foo_.func;        // can be set in a test
///     T                                foo_.result;      // can be set in a test
///     ---------------------------------------------------------------------------
///     bool                             foo_.called();    // set by the mocking framework
///     size_t                           foo_.count();     // set by the mocking framework
///     std::string                      foo_.prototype(); // set by the mocking framework
///
/// The auxiliary data members available for a mock function `foo` that returns void and takes N parameters of types T1..TN:
///
///     std::function<void(T1, ..., TN)> foo_.func;        // can be set in a test
///     ---------------------------------------------------------------------------
///     bool                             foo_.called();    // set by the mocking framework
///     size_t                           foo_.count();     // set by the mocking framework
///     std::string                      foo_.prototype(); // set by the mocking framework
///     T1                               foo_.param<1>();  // set by the mocking framework
///     .                                .
///     .                                .
///     TN                               foo_.param<N>()   // set by the mocking framework
///
/// The auxiliary data members available for a mock function `foo` that returns T and takes N parameters of types T1..TN:
///
///     std::function<void(T1, ..., TN)> foo_.func;        // can be set in a test
///     T                                foo_.result;      // can be set in a test
///     ---------------------------------------------------------------------------
///     bool                             foo_.called();    // set by the mocking framework
///     size_t                           foo_.count();     // set by the mocking framework
///     std::string                      foo_.prototype(); // set by the mocking framework
///     T1                               foo_.param<1>();  // set by the mocking framework
///     .                                .
///     .                                .
///     TN                               foo_.param<N>()   // set by the mocking framework
///
/// @param prefix "Things to the left in a function declaration". For instance `static`, `virtual`, etc. - often empty.
/// @param suffix "Things to the right in a function declaration". For instance `override`, `const`, `noexcept`, etc. - often empty.
/// @param overload_suffix An arbitrary suffix added to the auxiliary data name of overloaded functions to discriminate between them in tests - often empty.
/// @param return_type The type of the return value, or `void`.
/// @param function_name The name of the function to mock.
/// @param variadic Variadic parameter list of parameter types for the function, if any.
#define JG_MOCK(prefix, suffix, overload_suffix, return_type, function_name, ...) \
    jg::detail::mock_aux<return_type, __VA_ARGS__> _JG_CONCAT3(function_name, overload_suffix, _) {#return_type " " #function_name "(" #__VA_ARGS__ ") " #suffix}; \
    prefix return_type function_name(_JG_MOCK_FUNC_PARAMS_DECL(__VA_ARGS__)) suffix \
    { \
        return jg::detail::mock_impl<return_type, decltype(_JG_CONCAT3(function_name, overload_suffix, _))>(_JG_CONCAT3(function_name, overload_suffix, _)).impl(_JG_MOCK_FUNC_PARAMS_CALL(__VA_ARGS__)); \
    }

/// @macro JG_MOCK_REF
///
/// Makes an `extern` declaration of the auxiliary data defined by a corresponding usage of `JG_MOCK` in
/// a .cpp file. This makes it possible to only have one definition of a mock function in an entire test
/// program, and using its auxiliary data in other translation units.
/// 
/// Mocking the free function `foo* foolib_create(const char* id)` in one translation unit and using it in
/// two other translation units can be done like this:
/// 
///   * foolib_mocks.cpp
/// 
///         #include "flubber_mocks.h"
///     
///         JG_MOCK(,,, foo*, foolib_create, const char*);
/// 
///   * flubber_mocks.h
/// 
///         #include <foolib.h>
///         #include <jg/jg_mock.h>
///     
///         JG_MOCK_REF(,,, foo*, foolib_create, const char*);
///
///   * flubber_tests.cpp
/// 
///         #include <flubber.h>
///         #include "foolib_mocks.h"
///     
///         TEST("A flubber can do it")
///         {
///             foo dummy_result;
///             foolib_create_.reset();
///             foolib_create_.result = reinterpret_cast<foo*>(&dummy_result);
///     
///             flubber f; // System under test - depends on foolib
///             
///             TEST_ASSERT(f.do_it()); // If foolib_create succeeds, flubber can do it
///             TEST_ASSERT(foolib_create_.called());
///             TEST_ASSERT(foolib_create_.param<1>() != nullptr);
///         }
/// 
///   * fiddler_tests.cpp
/// 
///         #include <fiddler.h>
///         #include "foolib_mocks.h"
///     
///         TEST("A fiddler can't play")
///         {
///             foolib_create_.reset();
///             foolib_create_.result = nullptr;
///     
///             fiddler f; // System under test - depends on foolib
///             
///             TEST_ASSERT(!f.play()); // If foolib_create fails, fiddler can't play
///             TEST_ASSERT(foolib_create_.called());
///             TEST_ASSERT(foolib_create_.param<1>() != nullptr);
///         }
///
/// Mismatched `JG_MOCK` and `JG_MOCK_REF` declarations leads to compilation and linker errors.
#define JG_MOCK_REF(prefix, suffix, overload_suffix, return_type, function_name, ...) \
    extern jg::detail::mock_aux<return_type, __VA_ARGS__> _JG_CONCAT3(function_name, overload_suffix, _);

#ifdef JG_MOCK_ENABLE_SHORT_NAMES
#define MOCK     JG_MOCK
#define MOCK_REF JG_MOCK_REF
#endif
