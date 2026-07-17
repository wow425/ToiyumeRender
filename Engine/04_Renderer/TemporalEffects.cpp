//
// Created by saber on 2026/7/17.
//


#include "00_Core/PCH.h"
#include "TemporalEffects.h"

#include "TemporalEffects.h"
#include "BufferManager.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "02_RHI/Command/CommandContext.h"
#include "00_Core/SystemTime.h"
// #include "PostEffects.h"
#include <array>
// #include "TemporalBlendCS.h"
// #include "BoundNeighborhoodCS.h"
// #include "ResolveTAACS.h"
// #include "SharpenTAACS.h"

using namespace Graphics;
using namespace Math;
using namespace TemporalEffects;


namespace TemporalEffects
{
    bool EnableTAA = false;
    std::array<float, 4> TemporalMaxLerp = {1.0f, 0.0f, 1.0f, 0.01f}; // 控制历史帧融合比例

    bool TriggerReset = false; // 清空history

    ComputePSO s_TemporalBlendCS(L"TAA: Temporal Blend CS");

    uint32_t s_FrameIndex = 0;
    uint32_t s_FrameIndexMod2 = 0; // 双buffer历史需求

    float s_JitterX = 0.5f;
    float s_JitterY = 0.5f;

    float s_JitterDeltaX = 0.0f;
    float s_JitterDeltaY = 0.0f;

    void Initialize( void )
    {
#define CreatePSO( ObjName, ShaderByteCode ) \
ObjName.SetRootSignature(g_CommonRS); \
ObjName.SetComputeShader(ShaderByteCode, sizeof(ShaderByteCode) ); \
ObjName.Finalize();
//        CreatePSO( s_TemporalBlendCS, g_pTemporalBlendCS );
    }

    void Shutdown( void );

    // Call once per frame to increment the internal frame counter and, in the case of TAA, choosing the next
    // jittered sample position.
    // 每帧调用一次以增加内部帧计数器，并在TAA的情况下选择下一个抖动采样位置。
    void Update( uint64_t FrameIndex );

    // Returns whether the frame is odd or even, relevant to checkerboard rendering.
    // 返回帧是奇数还是偶数，与棋盘渲染相关。
    uint32_t GetFrameIndexMod2( void );

    // Jitter values are neutral at 0.5 and vary from [0, 1).  Jittering only occurs when temporal antialiasing
    // is enabled.  You can use these values to jitter your viewport or projection matrix.
    // 抖动值在0.5处为中性，并且变化范围为[0, 1)。 仅在启用时域抗锯齿时才会发生抖动。 您可以使用这些值来抖动视口或投影矩阵。
    void GetJitterOffset( float& JitterX, float& JitterY );

    void ClearHistory(CommandContext& Context);

    void ResolveImage(CommandContext& Context);
} // TemporalEffects