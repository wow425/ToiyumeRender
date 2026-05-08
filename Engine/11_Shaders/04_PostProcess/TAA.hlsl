// 输入资源
Texture2D<float4> gCurrentColor : register(t1, space1);
Texture2D<float4> gHistoryColor : register(t2, space1);
Texture2D<float> gDepth : register(t3, space1);
Texture2D<float2> gVelocity : register(t4, space1);

// 输出资源
RWTexture2D<float4> gOutputColor : register(u0, space1);

// 参数
cbuffer TAAConstans : register(b2)
{
    float2 gScreenSize;
    float2 gJitter;
    float gAlpha;
    float3 pad;
}

SamplerState gsamPointWrap : register(s0);
SamplerState gsamPointClamp : register(s1);
SamplerState gsamLinearWrap : register(s2);
SamplerState gsamLinearClamp : register(s3);
SamplerState gsamAnisotropicWrap : register(s4);
SamplerState gsamAnisotropicClamp : register(s5);

[numthreads(8, 8, 1)]
void CS(uint3 dtid : SV_DispatchThreadID)
{
    
    // 【核心修复】：越界线程直接杀死 (Bounds Check)
    // 保护 UAV 写入不触发 GPU Page Fault
    if (dtid.x >= (uint) gScreenSize.x || dtid.y >= (uint) gScreenSize.y)
    {
        return;
    }
    
    
    
    // 1. 提取当前颜色，供早期判断使用
    float4 currColor = gCurrentColor[dtid.xy];

    // 【核心修改】：处理冷启动/相机瞬移 (Alpha == 1.0)
    // 提示：使用 >= 0.999f
    if (gAlpha >= 0.999f)
    {
        // 直接输出当前颜色，并结束该线程
        gOutputColor[dtid.xy] = currColor;
        return;
    }
    
    float2 uv = (dtid.xy + 0.5f) / gScreenSize;
    
    // 2. 获取速度矢量并计算重投影坐标
    float2 velocity = gVelocity.SampleLevel(gsamPointClamp, uv, 0);
    float2 historyUV = uv - velocity;
    
        // 3. 边界检查：注意这里使用 0.0f 和 1.0f，避免类型歧义
    if (any(historyUV < 0.0f) || any(historyUV > 1.0f))
    {
        gOutputColor[dtid.xy] = currColor; // 直接用上面声明好的 currColor
        return;
    }
    
    // （删除了这里原本重复声明的 currColor）
    float4 m1 = 0.0f, m2 = 0.0f; // 明确初始化为 float
    
    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            float4 c = gCurrentColor.Load(int3(dtid.xy + int2(x, y), 0));
            m1 += c;
            m2 += c * c;
        }
    }
    
    // 计算 3x3 邻域的均值和标准差
    float4 mean = m1 / 9.0f;
    // 4. 注意这里 max 的第一个参数必须是 0.0f
    float4 stddev = sqrt(max(0.0f, (m2 / 9.0f) - (mean * mean)));
    
    // 5. 邻域钳制 (Neighborhood Clamping)
    float4 minColor = mean - 1.5f * stddev;
    float4 maxColor = mean + 1.5f * stddev;

    // 采样历史颜色
    float4 historyColor = gHistoryColor.SampleLevel(gsamLinearClamp, historyUV, 0);
    
    // 将历史颜色 Clamp 到当前帧邻域范围内
    historyColor = clamp(historyColor, minColor, maxColor);

    // 6. 混合输出
    float4 finalColor = lerp(historyColor, currColor, gAlpha);

    gOutputColor[dtid.xy] = finalColor;
}