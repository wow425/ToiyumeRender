
// 没啃

#include <windows.h>
#include <shellapi.h>
#include <unordered_map>
#include <string>
#include <sstream>
#include <cmath>
#include <climits>

namespace CommandLineArgs
{
    std::unordered_map<std::wstring, std::wstring> m_argumentMap;

    template<typename GetStringFunc>
    void GatherArgs(size_t numArgs, const GetStringFunc& pfnGet)
    {
        // [WP-1D / TD-07c] 每次重新初始化前清空旧参数表，保证重复调用 Initialize 时状态确定。
        m_argumentMap.clear();
        // argv[0] 永远是 exe 路径，从 1 开始扫描 key/value 对。
        //
        // R0 修复：原实现是
        //     const wchar_t* key = pfnGet(i);  i++;
        //     if (i < numArgs && key[0]=='-') { ... }
        // 也就是说「先自增、再判断」。当 -Flag 恰好是整条命令行的最后一个
        // token 时（很常见的写法：app.exe -RebuildModels），自增后 i == numArgs，
        // if 条件直接不成立，这个 flag 被静默丢弃。
        // 现在改成「先看当前 token 是不是 -key，再决定要不要吃掉下一个 token」，
        // 末尾 flag 也能被正确记录。
        for (size_t i = 1; i < numArgs; ++i)
        {
            const wchar_t* key = pfnGet(i);
            if (key == nullptr || key[0] != '-')
                continue;   // 不是 flag，跳过（例如孤立的位置参数）

            const wchar_t* strippedKey = key + 1;
            if (strippedKey[0] == L'\0')
                continue;   // 只有一个 "-"，忽略

            // 下一个 token 如果存在且不以 '-' 开头，视为本键的值；
            // 否则本键是「无值开关」，记录空串，交给 HasFlag 判定。
            if (i + 1 < numArgs)
            {
                const wchar_t* next = pfnGet(i + 1);
                if (next != nullptr && next[0] != '-')
                {
                    m_argumentMap[strippedKey] = next;
                    ++i;
                    continue;
                }
            }

            m_argumentMap[strippedKey] = L"";
        }
    }

    void Initialize(int argc, wchar_t** argv)
    {
        GatherArgs(size_t(argc), [argv](size_t i) -> const wchar_t* { return argv[i]; });
    }

    // 窗口应用（wWinMain）没有 argc/argv，但 GetCommandLineW 仍然能拿到完整命令行。
    // 这里用 CommandLineToArgvW 拆成 argv，复用同一套 GatherArgs 逻辑。
    void InitializeFromSystemCommandLine()
    {
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
        if (argv == nullptr)
            return;

        GatherArgs(size_t(argc), [argv](size_t i) -> const wchar_t* { return argv[i]; });
        ::LocalFree(argv);
    }

    template<typename FunctionType>
    bool Lookup(const wchar_t* key, const FunctionType& pfnFunction)
    {
        const auto found = m_argumentMap.find(key);
        if (found != m_argumentMap.end())
        {
            pfnFunction(found->second);
            return true;
        }

        return false;
    }

    bool GetInteger(const wchar_t* key, uint32_t& value)
    {
        // [WP-1D / TD-07c] 严格校验无符号 32 位整数字面量：拒绝空串、负数、非数字后缀与越界值，
        // 捕获所有解析异常且解析失败时不污染调用方传入的 value。
        const auto found = m_argumentMap.find(key);
        if (found == m_argumentMap.end())
            return false;

        const std::wstring& val = found->second;
        if (val.empty() || val[0] == L'-' || val[0] == L'+')
            return false;

        try
        {
            size_t pos = 0;
            unsigned long long parsed = std::stoull(val, &pos, 10);
            if (pos != val.size() || parsed > UINT_MAX)
                return false;
            value = static_cast<uint32_t>(parsed);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool GetFloat(const wchar_t* key, float& value)
    {
        // [WP-1D / TD-07c] 严格校验有限浮点字面量：拒绝空串、非数字尾缀与 NaN/Inf，
        // 解析失败返回 false 且不覆写 value。
        const auto found = m_argumentMap.find(key);
        if (found == m_argumentMap.end())
            return false;

        const std::wstring& val = found->second;
        if (val.empty())
            return false;

        try
        {
            size_t pos = 0;
            float parsed = std::stof(val, &pos);
            if (pos != val.size() || !std::isfinite(parsed))
                return false;
            value = parsed;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool HasFlag(const wchar_t* key)
    {
        return m_argumentMap.find(key) != m_argumentMap.end();
    }

    bool GetString(const wchar_t* key, std::wstring& value)
    {
        return Lookup(key, [&value](std::wstring& val)
            {
                value = val;
            });
    }
}
