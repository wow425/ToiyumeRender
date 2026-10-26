#pragma once

// 没啃

namespace CommandLineArgs
{
    void Initialize(int argc, wchar_t** argv);
    // 从 GetCommandLineW 解析（窗口应用拿不到 main 的 argc/argv 时使用）
    void InitializeFromSystemCommandLine();
    bool GetInteger(const wchar_t* key, uint32_t& value);
    bool GetFloat(const wchar_t* key, float& value);
    bool GetString(const wchar_t* key, std::wstring& value);
    // 无值开关：命令行里出现 `-RebuildModels` 即视为开启。
    // 区别于 GetString/GetInteger 这些「必须带值」的键。
    bool HasFlag(const wchar_t* key);
}
