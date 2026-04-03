#pragma once

#include <corecrt.h>

#undef assert

#ifdef NDEBUG
    // Release 模式下，assert 不产生任何机器码
    #define assert(expression) ((void)0)
#else

    _ACRTIMP void __cdecl _wassert(
        _In_z_ wchar_t const* _Message,
        _In_z_ wchar_t const* _File,
        _In_   unsigned       _Line
    );

    #define assert(expression) (void)(                                                       \
                (!!(expression)) ||                                                              \
                (_wassert(_CRT_WIDE(#expression), _CRT_WIDE(__FILE__), (unsigned)(__LINE__)), 0) \
            )

#endif


