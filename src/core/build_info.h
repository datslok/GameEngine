#pragma once

/*
* Which build this is, from the makefile: debug builds define ENGINE_DEBUG_BUILD, release builds do not. Code reads
* this constant instead of the macro, so both builds compile every branch (an if on a constexpr bool is still checked
* by the compiler, while #ifdef'd code is not). NDEBUG is never used for this: the tests rely on assert in both builds.
*/
#ifdef ENGINE_DEBUG_BUILD
inline constexpr bool isDebugBuild = true;
#else
inline constexpr bool isDebugBuild = false;
#endif
