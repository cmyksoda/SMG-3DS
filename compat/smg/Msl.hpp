#pragma once
// The names Metrowerks' MSL added to <algorithm> and <functional> beside the standard ones, as the game calls
// them. libstdc++ supplies the standard ones.
#ifdef __cplusplus
#include <algorithm>
#include <functional>

// The adaptors are deprecated in C++17 and still what the game uses.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
namespace std {
// Each of MSL's mem_func overloads makes the adaptor that mem_fun makes for that member's shape.
template <class Result, class Type> inline mem_fun_t<Result, Type> mem_func(Result (Type::*function)()) {
    return mem_fun_t<Result, Type>(function);
}
template <class Result, class Type> inline const_mem_fun_t<Result, Type> mem_func(Result (Type::*function)() const) {
    return const_mem_fun_t<Result, Type>(function);
}
template <class Result, class Type, class Arg> inline mem_fun1_t<Result, Type, Arg> mem_func(Result (Type::*function)(Arg)) {
    return mem_fun1_t<Result, Type, Arg>(function);
}
template <class Result, class Type, class Arg>
inline const_mem_fun1_t<Result, Type, Arg> mem_func(Result (Type::*function)(Arg) const) {
    return const_mem_fun1_t<Result, Type, Arg>(function);
}

// The _array forms hand the function a pointer to each element rather than the element.
template <class Element, class Function> inline Function for_each_array(Element* first, Element* last, Function function) {
    for (; first != last; ++first) function(first);
    return function;
}
template <class Element, class Predicate> inline Element* find_if_array(Element* first, Element* last, Predicate predicate) {
    for (; first != last && !predicate(first); ++first) {}
    return first;
}
// Walks down from first, stopping at last or the first element the predicate accepts.
template <class Iterator, class Predicate> inline Iterator rfind_if(Iterator first, Iterator last, Predicate predicate) {
    for (; first != last && !predicate(*first); --first) {}
    return first;
}
} // namespace std
#pragma GCC diagnostic pop
#endif
