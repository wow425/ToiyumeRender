#pragma once

#include <cstdint>

namespace Display
{
    void Initialize(void);
    void Shutdown(void);
    void Resize(uint32_t width, uint32_t height);
    void Present(void);
}

namespace Graphics
{
    extern uint32_t g_DisplayWidth;
    extern uint32_t g_DisplayHeight;


    // 返回帧数
    uint64_t GetFrameCount(void);

    //上一个已完成帧所消耗的时间。在这一帧的部分时间内，CPU 和 GPU 可能会处于空闲状态。
    // 帧耗时（Frame Time）衡量的是两次“呈现（Present）”调用之间的时间间隔。
    float GetFrameTime(void);

    // 每秒总帧数
    float GetFrameRate(void);


}
