#pragma once

#include "PCH.h"

namespace Utility
{
// 窗口
#ifdef _CONSOLE
	inline void Print(const char* msg) { printf("%s", msg); }
	inline void Print(const wchar_t* msg) { wprintf(L"%s", msg); }
#else // 非窗口
	inline void Print(const char* msg) { OutputDebugStringA(msg); }
	inline void Print(const wchar_t* msg) { OutputDebugString(msg); }
#endif

#ifndef RELEASE
    inline void PrintSubMessage(const char* format, ...)
    {
        Print("--> ");
        char buffer[256];
        va_list ap;
        va_start(ap, format);
        vsprintf_s(buffer, 256, format, ap);
        va_end(ap);
        Print(buffer);
        Print("\n");
    }
    inline void PrintSubMessage(const wchar_t* format, ...)
    {
        Print("--> ");
        wchar_t buffer[256];
        va_list ap;
        va_start(ap, format);
        vswprintf(buffer, 256, format, ap);
        va_end(ap);
        Print(buffer);
        Print("\n");
    }
    inline void PrintSubMessage(void)
    {
    }
#endif

    std::wstring UTF8ToWideString(const std::string& str);
    std::string WideStringToUTF8(const std::wstring& wstr);
    std::string ToLower(const std::string& str);
    std::wstring ToLower(const std::wstring& str);
    std::string GetBasePath(const std::string& str);
    std::wstring GetBasePath(const std::wstring& str);
    std::string RemoveBasePath(const std::string& str);
    std::wstring RemoveBasePath(const std::wstring& str);
    std::string GetFileExtension(const std::string& str);
    std::wstring GetFileExtension(const std::wstring& str);
    std::string RemoveExtension(const std::string& str);
    std::wstring RemoveExtension(const std::wstring& str);


} // namespace Utility



// 开放版
#ifdef RELEASE
#define ASSERT_SUCCEEDED(hr,..) (void)(hr)

#else // 非开放版

    // 双重宏展开，先展开为数字，再转换为字符串
#define STRINGIFY(x) #x
#define STRINGIFY_BUILTIN(x) STRINGIFY(x) 
// __FILE__当前源代码文件的完整路径。__LINE__当前代码所在的行号。
// 变长参数与错误消息 (__VA_ARGS__) 允许在检查 hr 的同时，传入一段自定义的文字描述。
// __debugbreak()向CPU发送中断指令，停在报错的那行代码上
#define ASSERT_SUCCEEDED(hr,...) \
		if (FAILED(hr)) {\
	            Utility::Print("\nHRESULT failed in " STRINGIFY_BUILTIN(__FILE__) " @ " STRINGIFY_BUILTIN(__LINE__) "\n"); \
            Utility::PrintSubMessage("hr = 0x%08X", hr); \
            Utility::PrintSubMessage(__VA_ARGS__); \
            Utility::Print("\n"); \
            __debugbreak(); \
		}

#endif
