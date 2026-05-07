#pragma once

// 没啃


class SystemTime
{
public:
    // 初始化计时器：查询硬件层面的性能计数器频率 (Performance Counter Frequency / パフォーマンスカウンタ周波数)
    static void Initialize(void);

    // 获取当前的硬件滴答数 (Tick Count / ティック数)：读取当前 CPU 计数器的瞬时值
    static int64_t GetCurrentTick(void);

    // 忙等睡眠 (Busy Loop Sleep / ビジーループ)：通过不断循环检查时间来实现高精度延迟，比 OS 的 Sleep 更精准但耗 CPU
    static void BusyLoopSleep(float SleepTime);

    // 将滴答数转换为秒：计算公式为 Tick * (1.0 / Frequency)
    static inline double TicksToSeconds(int64_t TickCount)
    {
        return TickCount * sm_CpuTickDelta; // sm_CpuTickDelta 是频率的倒数
    }

    // 将滴答数转换为毫秒 (Milliseconds / ミリ秒)
    static inline double TicksToMillisecs(int64_t TickCount)
    {
        return TickCount * sm_CpuTickDelta * 1000.0;
    }

    // 计算两个滴答点之间的时间差 (Time Delta / タイムデルタ)
    static inline double TimeBetweenTicks(int64_t tick1, int64_t tick2)
    {
        return TicksToSeconds(tick2 - tick1);
    }

private:
    // 性能计数器每滴答一次所经历的时间量 (单位：秒)
    // 它是静态变量，在 Initialize 中计算一次：sm_CpuTickDelta = 1.0 / Frequency
    static double sm_CpuTickDelta;
};


class CpuTimer
{
public:
    // 构造函数：初始化起始滴答和累计滴答为 0
    CpuTimer()
    {
        m_StartTick = 0ll;    // 0ll 表示 long long 类型的 0
        m_ElapsedTicks = 0ll; // 累计经过的滴答数
    }

    // 开始计时：记录当前的系统滴答数
    void Start()
    {
        if (m_StartTick == 0ll) // 简单的防重入保护
            m_StartTick = SystemTime::GetCurrentTick();
    }

    // 停止计时：计算从 Start 到现在的差值，并累加到总时间中
    void Stop()
    {
        if (m_StartTick != 0ll)
        {
            // 累加本次运行的滴答数
            m_ElapsedTicks += SystemTime::GetCurrentTick() - m_StartTick;
            m_StartTick = 0ll; // 重置起始点，标志计时结束
        }
    }

    // 重置计时器：清空累计时间
    void Reset()
    {
        m_ElapsedTicks = 0ll;
        m_StartTick = 0ll;
    }

    // 获取累计的时间（秒）：将内部存储的滴答数转为易读的 double 秒
    double GetTime() const
    {
        return SystemTime::TicksToSeconds(m_ElapsedTicks);
    }

private:
    int64_t m_StartTick;    // 记录计时的起始点
    int64_t m_ElapsedTicks; // 记录多次 Start/Stop 累计的总滴答数
};
