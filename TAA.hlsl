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
    
    
    float4 currColor = gCurrentColor[dtid.xy];

    // 【核心修改】：处理冷启动/相机瞬移 (Alpha == 1.0)
    // 提示：在 Shader 中比较浮点数，使用 >= 0.999f 比直接 == 1.0f 更安全
    if (gAlpha >= 0.999f)
    {
        // 此时完全不需要算速度、算 3x3 邻域、读历史纹理
        // 直接输出当前颜色，并结束该线程
        gOutputColor[dtid.xy] = currColor;
        return;
    }
    
    
    
    
    
    float2 uv = (dtid.xy + 0.5f) / gScreenSize;
    
    // 1. 获取速度矢量并计算重投影坐标
    // 采样时扣除抖动位移
    float2 velocity = gVelocity.SampleLevel(gsamPointClamp, uv, 0);
    float2 historyUV = uv - velocity;
    
    // 2. 边界检查，若历史坐标超出屏幕，则返回当前颜色
    if (any(historyUV < 0) || any(historyUV > 1))
    {
        gOutputColor[dtid.xy] = gCurrentColor[dtid.xy];
        return;
    }
    
    // 3. 采样当前颜色及其 3x3 邻域（用于 Clamping）
    float4 currColor = gCurrentColor[dtid.xy];
    float4 m1 = 0, m2 = 0; // 用于计算均值和方差
    
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
    float4 stddev = sqrt(max(0, (m2 / 9.0f) - (mean * mean)));
    
    // 4. 邻域钳制 (Neighborhood Clamping)
    // 防止历史颜色由于遮挡、移动产生“鬼影”
    float4 minColor = mean - 1.5f * stddev; // 这里的系数 1.5 可调
    float4 maxColor = mean + 1.5f * stddev;

    // 采样历史颜色（建议使用双线性或 Catmull-Rom 插值）
    float4 historyColor = gHistoryColor.SampleLevel(gsamLinearClamp, historyUV, 0);
    
    // 将历史颜色 Clamp 到当前帧邻域范围内
    historyColor = clamp(historyColor, minColor, maxColor);

    // 5. 混合输出 (Exponential Moving Average)
    float4 finalColor = lerp(historyColor, currColor, gAlpha);

    gOutputColor[dtid.xy] = finalColor;

}
	// Current, History, Depth, Velocity
	// taaRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 1, 1); // 占用t1-t4的s1 SRV
	// taaRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 1); // 占用u0的s1 UAV output