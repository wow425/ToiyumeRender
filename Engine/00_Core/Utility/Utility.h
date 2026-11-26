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

    // [TD-07b / 统一落盘通道] 窄字符转宽字符并写入 tooiyume_log.txt。
    // 项目全局开启 /utf-8，窄字符串字面量为 UTF-8；优先按 CP_UTF8 解码，失败时回退 CP_ACP。
    inline void WriteLogFileA(const char* text)
    {
        if (text == nullptr || text[0] == '\0')
            return;

        wchar_t wide[2048] = {};
        int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, static_cast<int>(_countof(wide)));
        if (written == 0)
        {
            written = ::MultiByteToWideChar(CP_ACP, 0, text, -1, wide, static_cast<int>(_countof(wide)));
        }
        if (written > 0)
        {
            WriteLogFile(wide);
        }
    }

    // [TD-07b] Utility::Print 同步写入 OutputDebugString 与磁盘日志文件，确保无调试器运行时
    // ASSERT / ASSERT_SUCCEEDED / ERROR / WARN_ONCE_IF 的诊断信息也能完整落盘。
    inline void Print(const char* msg)
    {
        OutputDebugStringA(msg);
        WriteLogFileA(msg);
    }

    inline void Print(const wchar_t* msg)
    {
        OutputDebugStringW(msg);
        WriteLogFile(msg);
    }

    inline void Printf(const char* format, ...)
    {
        char buffer[2048];
        va_list ap;
        va_start(ap, format);
        vsnprintf(buffer, _countof(buffer), format, ap);
        va_end(ap);
        OutputDebugStringA(buffer);
        WriteLogFileA(buffer);
    }

    inline void Printf(const wchar_t* format, ...)
    {
        wchar_t buffer[2048];
        va_list ap;
        va_start(ap, format);
        // _vsnwprintf_s(..., _TRUNCATE, ...) 在超长时截断而非触发 CRT invalid-parameter abort
        _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, ap);
        va_end(ap);
        OutputDebugStringW(buffer);
        WriteLogFile(buffer);
    }


#ifndef RELEASE
    inline void PrintSubMessage(const char* format, ...)
    {
        Print("--> ");
        char buffer[1024];
        va_list ap;
        va_start(ap, format);
        vsnprintf(buffer, _countof(buffer), format, ap);
        va_end(ap);
        Print(buffer);
        Print("\n");
    }
    inline void PrintSubMessage(const wchar_t* format, ...)
    {
        Print(L"--> ");
        wchar_t buffer[1024];
        va_list ap;
        va_start(ap, format);
        _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, ap);
        va_end(ap);
        Print(buffer);
        Print(L"\n");
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
#define LOG_ERROR( fmt, ... ) Utility::Printf( fmt, ##__VA_ARGS__ )
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
// [TD-07b / 单次求值不变量] hr 表达式只求值一次，避免失败或 Release 下产生额外副作用；
// 同步记录失败的十六进制 HRESULT 到 tooiyume_log.txt，消除 Release 下静默吞错。
#define ASSERT_SUCCEEDED( hr, ... ) \
    { \
        const HRESULT _ty_hr = (hr); \
        if (FAILED(_ty_hr)) { \
            Utility::Printf("\n[ERROR] HRESULT failed in %s @ %d (hr = 0x%08X)\n", __FILE__, __LINE__, static_cast<unsigned int>(_ty_hr)); \
        } \
    }

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

// [TD-07b / 单次求值 + 同步落盘] 原宏把参数 hr 直接展开在 FAILED(hr) 与 PrintSubMessage(..., hr)
// 两处：当调用方传入带副作用的 DX12 创建调用（如 CreateGraphicsPipelineState）且首次失败时，
// 宏会二次执行同一 DX12 调用！改为先捕获进局部 const HRESULT _ty_hr，保证严格单次求值，
// 并经 Utility::Print 同步写入 OutputDebugString 与 tooiyume_log.txt。
#define ASSERT_SUCCEEDED( hr, ... ) \
        { \
            const HRESULT _ty_hr = (hr); \
            if (FAILED(_ty_hr)) { \
                Utility::Print("\nHRESULT failed in " STRINGIFY_BUILTIN(__FILE__) " @ " STRINGIFY_BUILTIN(__LINE__) "\n"); \
                Utility::PrintSubMessage("hr = 0x%08X", static_cast<unsigned int>(_ty_hr)); \
                Utility::PrintSubMessage(__VA_ARGS__); \
                Utility::Print("\n"); \
                __debugbreak(); \
            } \
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


