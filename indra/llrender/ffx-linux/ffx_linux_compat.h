// The Windows helper functions the FidelityFX SDK 1.1.4 calls and GCC's C library does not have (force included into every file of the Linux build of the SDK and of the files
// of llrender that use it: -include ffx_linux_compat.h). wchar_t is 4 bytes here, the SDK's own non Windows paths (ffx_vk.cpp ConvertUTF8ToUTF16) are used as they are.
#ifndef FFX_LINUX_COMPAT_H
#define FFX_LINUX_COMPAT_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <string>
#include <locale>       // std::wstring_convert (ffx_vk.cpp, its non Windows UTF-8 conversion)
#include <codecvt>

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

#ifndef FFX_UNUSED
#define FFX_UNUSED(x)               ((void)(x))
#endif

// wcscpy_s: the copy of at most size - 1 characters and the end mark (the Windows one also has a version that takes the size from an array)
inline int ffx_wcscpy_s(wchar_t* dst, size_t size, const wchar_t* src)
{
    if (!dst || size == 0)
    {
        return 22;
    }
    size_t i = 0;
    for (; src && i + 1 < size && src[i]; ++i)
    {
        dst[i] = src[i];
    }
    dst[i] = 0;
    return 0;
}
template <size_t N>
inline int ffx_wcscpy_s(wchar_t (&dst)[N], const wchar_t* src)
{
    return ffx_wcscpy_s(dst, N, src);
}
#define wcscpy_s ffx_wcscpy_s

inline int ffx_strcpy_s(char* dst, size_t size, const char* src)
{
    if (!dst || size == 0)
    {
        return 22;
    }
    size_t i = 0;
    for (; src && i + 1 < size && src[i]; ++i)
    {
        dst[i] = src[i];
    }
    dst[i] = 0;
    return 0;
}
#define strcpy_s ffx_strcpy_s

#define sprintf_s(buffer, size, ...) snprintf((buffer), (size), __VA_ARGS__)
#define swprintf_s(buffer, size, ...) swprintf((buffer), (size), __VA_ARGS__)

// wcstombs_s(&converted, out, out size, wide, max): the characters below 128 as they are, the others as '?': the names the SDK puts into its debug labels are ASCII
inline int ffx_wcstombs_s(size_t* converted, char* dst, size_t size, const wchar_t* src, size_t /*max*/)
{
    size_t i = 0;
    for (; dst && src && i + 1 < size && src[i]; ++i)
    {
        dst[i] = src[i] < 128 ? (char)src[i] : '?';
    }
    if (dst && size > 0)
    {
        dst[i] = 0;
    }
    if (converted)
    {
        *converted = i + 1;
    }
    return 0;
}
#define wcstombs_s ffx_wcstombs_s

#endif // FFX_LINUX_COMPAT_H
