#pragma once
#include <DirectXMath.h>

// 1. 底层数学函数：计算以 base 为底的第 index 个 Halton 序列值
float CreateHaltonSequence(int index, int base)
{
    float f = 1.0f;
    float r = 0.0f;
    int current = index;

    while (current > 0)
    {
        f = f / (float)base;
        r = r + f * (float)(current % base);
        current = (int)(current / base);
    }
    return r; // 返回值永远在 [0, 1] 之间
}

// 2. 业务函数：生成 TAA 所需的 Jitter 偏移
// 注意：这需要传入屏幕宽高，因为我们要把像素偏移转换到投影矩阵的 NDC 空间
DirectX::XMFLOAT2 GenerateHaltonJitter(int frameCount, int screenWidth, int screenHeight)
{
    // TAA 通常累积 8 帧或 16 帧为一个周期。这里以 16 帧为例。
    // 注意：index 必须从 1 开始，因为 CreateHaltonSequence(0, base) 会直接返回 0
    int index = (frameCount % 16) + 1;

    // X 轴通常使用底数 2，Y 轴使用底数 3
    float haltonX = CreateHaltonSequence(index, 2);
    float haltonY = CreateHaltonSequence(index, 3);

    // Halton 生成的值在 [0, 1] 之间。
    // 我们需要将其映射到 [-0.5, 0.5] 像素范围，即以当前像素中心为原点的偏移。
    float jitterX = haltonX - 0.5f;
    float jitterY = haltonY - 0.5f;

    // 将像素级别的偏移转换为 NDC（标准化设备坐标）空间的偏移量。
    // NDC 空间的 X 和 Y 范围都是 [-1, 1]，总跨度为 2。
    // 所以偏移量需要乘以 2，再除以屏幕分辨率。
    return DirectX::XMFLOAT2(
        jitterX * 2.0f / (float)screenWidth,
        jitterY * 2.0f / (float)screenHeight
    );
}