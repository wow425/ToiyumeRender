#pragma once

#include "00_Core/PCH.h"

namespace Utility
{
    // ============================================================
    // R0.2：日志同时写入 debugger 与磁盘文件。
    // 原因：原来只调用 OutputDebugString，直接双击 exe 运行时看不到任何输出，
    // 崩溃/资产缺失只能表现为「黑屏 + 无信息」。这里补一条落盘通道，
    // 便于 CI、无调试器运行以及事后排查。
    // 日志文件：<exe 目录>/tooiyume_log.txt（追加写）
    // ============================================================
    inline std::wstring LogFilePath()
    {
        static std::wstring path;
        if (path.empty())
        {
            wchar_t exePath[MAX_PATH] = {};
            const DWORD len = ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring dir(exePath, len);
            const size_t slash = dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
                dir = dir.substr(0, slash + 1);
            else
                dir.clear();
            path = dir + L"tooiyume_log.txt";
        }
        return path;
    }

    inline void WriteLogFile(const wchar_t* text)
    {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, LogFilePath().c_str(), L"a, ccs=UTF-8") == 0 && fp != nullptr)
        {
            fputws(text, fp);
            fclose(fp);
        }
    }

    inline void Print(const char* msg) { OutputDebugStringA(msg); }
    inline void Print(const wchar_t* msg) { OutputDebugString(msg); }

    inline void Printf(const char* format, ...)
    {
        char buffer[2048];
        va_list ap;
        va_start(ap, format);
        vsprintf_s(buffer, format, ap);
        va_end(ap);
        OutputDebugStringA(buffer);

        wchar_t wide[2048] = {};
        ::MultiByteToWideChar(CP_ACP, 0, buffer, -1, wide, 2048);
        WriteLogFile(wide);
    }

    inline void Printf(const wchar_t* format, ...)
    {
        wchar_t buffer[2048];
        va_list ap;
        va_start(ap, format);
        // vswprintf_s 会根据 format 字符串的内容进行安全格式化
        vswprintf_s(buffer, format, ap);
        va_end(ap);
        OutputDebugStringW(buffer);
        WriteLogFile(buffer);
    }


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

#ifdef ERROR
#undef ERROR
#endif
#ifdef ASSERT
#undef ASSERT
#endif
#ifdef HALT
#undef HALT
#endif

#define HALT( ... ) ERROR( __VA_ARGS__ ) __debugbreak();

// ============================================================
// R0：分级启动日志
// ------------------------------------------------------------
// 目的：让「程序起不来 / 黑屏 / 资源没加载」这类问题在**没有调试器**的情况下
// 也能从 tooiyume_log.txt 里读出来。分级是为了控制噪声：
//   LOG_INFO  —— 默认输出，记录启动关键节点（设备、交换链、资源根、模型加载）
//   LOG_TRACE —— 只在 TOOIYUME_VERBOSE_LOG 定义时输出，用于函数级细粒度追踪
// 两者都写入 OutputDebugString 与日志文件（见 Printf）。
// ============================================================
// 注意：本块位于 `namespace Utility` 结束之后，所以必须写全限定名
// `Utility::Printf`；同文件上面的 ERROR/DEBUGPRINT 之所以能直接写 Printf，
// 是因为它们在命名空间内部。
#define LOG_INFO( fmt, ... )  Utility::Printf( fmt, ##__VA_ARGS__ )
#ifdef TOOIYUME_VERBOSE_LOG
#define LOG_TRACE( fmt, ... ) Utility::Printf( fmt, ##__VA_ARGS__ )
#else
#define LOG_TRACE( fmt, ... ) do {} while(0)
#endif

#ifdef RELEASE

// R0：Release 版断言宏必须自带分号。
// 原因：Debug 版 ASSERT 展开成 `if (...) { ... }`，本身就是一个完整语句，
// 所以全仓库大量调用点写成了不带分号的 `ASSERT(cond)`。改成 `(void)(isTrue)`
// 之后这些调用点后面紧跟的语句（例如 CommandContext.h 里的 `DynAlloc cb = ...`）
// 会被解析成 `(void)(x) Type name = ...`，直接语法错误。
// 在宏里补一个分号后：
//   调用点原本带 `;` 的 -> `;;`（合法空语句）
//   调用点原本不带 `;` 的 -> 被正确终止
// 这两种写法在 Debug 下都能编译，因此不会引入新的分支差异。
#define ASSERT( isTrue, ... ) (void)(isTrue);
#define WARN_ONCE_IF( isTrue, ... ) (void)(isTrue);
#define WARN_ONCE_IF_NOT( isTrue, ... ) (void)(isTrue);
#define ERROR( msg, ... ) ;
#define DEBUGPRINT( msg, ... ) do {} while(0)
#define ASSERT_SUCCEEDED( hr, ... ) (void)(hr);

#else	// !RELEASE

#define STRINGIFY(x) #x
#define STRINGIFY_BUILTIN(x) STRINGIFY(x)
#define ASSERT( isFalse, ... ) \
        if (!(bool)(isFalse)) { \
            Utility::Print("\nAssertion failed in " STRINGIFY_BUILTIN(__FILE__) " @ " STRINGIFY_BUILTIN(__LINE__) "\n"); \
            Utility::PrintSubMessage("\'" #isFalse "\' is false"); \
            Utility::PrintSubMessage(__VA_ARGS__); \
            Utility::Print("\n"); \
            __debugbreak(); \
        }

#define ASSERT_SUCCEEDED( hr, ... ) \
        if (FAILED(hr)) { \
            Utility::Print("\nHRESULT failed in " STRINGIFY_BUILTIN(__FILE__) " @ " STRINGIFY_BUILTIN(__LINE__) "\n"); \
            Utility::PrintSubMessage("hr = 0x%08X", hr); \
            Utility::PrintSubMessage(__VA_ARGS__); \
            Utility::Print("\n"); \
            __debugbreak(); \
        }


#define WARN_ONCE_IF( isTrue, ... ) \
    { \
        static bool s_TriggeredWarning = false; \
        if ((bool)(isTrue) && !s_TriggeredWarning) { \
            s_TriggeredWarning = true; \
            Utility::Print("\nWarning issued in " STRINGIFY_BUILTIN(__FILE__) " @ " STRINGIFY_BUILTIN(__LINE__) "\n"); \
            Utility::PrintSubMessage("\'" #isTrue "\' is true"); \
            Utility::PrintSubMessage(__VA_ARGS__); \
            Utility::Print("\n"); \
        } \
    }

#define WARN_ONCE_IF_NOT( isTrue, ... ) WARN_ONCE_IF(!(isTrue), __VA_ARGS__)

#define ERROR( ... ) \
        Utility::Print("\nError reported in " STRINGIFY_BUILTIN(__FILE__) " @ " STRINGIFY_BUILTIN(__LINE__) "\n"); \
        Utility::PrintSubMessage(__VA_ARGS__); \
        Utility::Print("\n");

#define DEBUGPRINT( msg, ... ) \
    Utility::Printf( msg "\n", ##__VA_ARGS__ );

#endif

#define BreakIfFailed( hr ) if (FAILED(hr)) __debugbreak()

void SIMDMemCopy(void* __restrict Dest, const void* __restrict Source, size_t NumQuadwords);
void SIMDMemFill(void* __restrict Dest, __m128 FillVector, size_t NumQuadwords);


