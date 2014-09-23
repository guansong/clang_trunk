// Test this without pch.
// RUN: %clang_cc1 -include %S/cxx-traits.h -std=c++11 -fsyntax-only -verify %s

// RUN: %clang_cc1 -x c++-header -std=c++11 -emit-pch -o %t %S/cxx-traits.h
// RUN: %clang_cc1 -std=c++11 -include-pch %t -DPCH -fsyntax-only -verify %s

#ifdef PCH
// expected-no-diagnostics
#endif

bool _Is_pod_comparator = n::__is_pod<int>::__value;
bool _Is_empty_check = n::__is_empty<int>::__value;

bool default_construct_int = n::is_trivially_constructible<int>::value;
bool copy_construct_int = n::is_trivially_constructible<int, const int&>::value;

// The built-ins should still work too:
bool _is_pod_result = __is_pod(int);
bool _is_empty_result = __is_empty(int);
