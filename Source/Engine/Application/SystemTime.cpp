#include "PCH.h"
#include "SystemTime.h"


// 静态成员变量初始化：存储每个“滴答”代表的秒数。初始值为 0.0。
double SystemTime::sm_CpuTickDelta = 0.0;

// 初始化计时器系统：查询硬件的频率
void SystemTime::Initialize(void)
{
    // LARGE_INTEGER 是 Windows 定义的 64 位联合体，用于存储高精度频率和计数
    LARGE_INTEGER frequency;

    // 查询性能频率 (QueryPerformanceFrequency / クエリパフォーマンス周波数)：
    // 获取硬件计数器每秒跳动多少次 (Ticks per Second)。如果硬件不支持，ASSERT 报错。
    ASSERT(TRUE == QueryPerformanceFrequency(&frequency), "Unable to query performance counter frequency");

    // 计算滴答增量：频率的倒数。
    // 公式：sm_CpuTickDelta = 1.0 / frequency
    // 得到的单位是 “秒/滴答”，用于后续将计数转为时间。
    sm_CpuTickDelta = 1.0 / static_cast<double>(frequency.QuadPart);
}

// 获取当前的硬件计数值
int64_t SystemTime::GetCurrentTick(void)
{
    LARGE_INTEGER currentTick;

    // 查询性能计数器 (QueryPerformanceCounter / クエリパフォーマンスカウンタ)：
    // 获取从系统启动到现在的总滴答数。这是一个极低延迟的硬件读取操作。
    ASSERT(TRUE == QueryPerformanceCounter(&currentTick), "Unable to query performance counter value");

    // 返回 64 位整数值
    return static_cast<int64_t>(currentTick.QuadPart);
}

// 忙等睡眠：一种高精度的等待方式
void SystemTime::BusyLoopSleep(float SleepTime)
{
    // 计算目标停止点：当前滴答数 + (目标秒数 / 每滴答秒数)
    // 注意：这里强制转换为 int64_t 以便进行精确的整数比较
    int64_t finalTick = (int64_t)((double)SleepTime / sm_CpuTickDelta) + GetCurrentTick();

    // 紧凑循环 (Tight Loop)：CPU 会在这里全力运转，直到计数器达到或超过目标点。
    // 这能提供微秒级的精度，但代价是该核心的 CPU 使用率会瞬间飙升到 100%。
    while (GetCurrentTick() < finalTick);
}
