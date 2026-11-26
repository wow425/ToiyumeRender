// ============================================================================
// 着色器：GFL2SSAOCS.hlsl（M6 UE5 Reverse-Z Compute SSAO + AuxMask 角色豁免）
// 对标：AK12-frame7206.rdc EID 3645 CS_711
// 编译配置：compile_shader_bindless (cs_6_6, MainCS)
// ============================================================================
#include "08_Character/CharacterCommon.hlsli"

static const float3 kHemisphereKernel[12] = {
    float3( 0.0497f,  0.0315f, 0.1450f),
    float3(-0.1120f,  0.0884f, 0.2120f),
    float3( 0.1980f, -0.1430f, 0.2750f),
    float3(-0.0840f, -0.2510f, 0.3420f),
    float3( 0.3410f,  0.1980f, 0.3910f),
    float3(-0.3720f,  0.1420f, 0.4480f),
    float3( 0.1540f,  0.4320f, 0.5120f),
    float3(-0.2610f, -0.4210f, 0.5780f),
    float3( 0.5410f, -0.3120f, 0.6410f),
    float3(-0.5120f,  0.4120f, 0.7150f),
    float3( 0.2890f,  0.6140f, 0.7850f),
    float3(-0.1420f, -0.6840f, 0.8920f),
};

float3 ReconstructWorldPosReverseZ(float2 uv, float depth)
{
    const float4 clip = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    const float4 ws   = mul(clip, g_InvViewProj);
    return ws.xyz / max(ws.w, 1e-5f);
}

[numthreads(8, 8, 1)]
void MainCS(uint3 dtid : SV_DispatchThreadID)
{
    uint outW = 1u, outH = 1u;
    RWTexture2D<float> ssaoOut = ResourceDescriptorHeap[g_AuxUavIdx];
    ssaoOut.GetDimensions(outW, outH);
    if (dtid.x >= outW || dtid.y >= outH)
        return;

    const float2 uv = (float2(dtid.xy) + 0.5f) / float2(outW, outH);

    Texture2D<float>  depthTex = ResourceDescriptorHeap[g_AuxTex0SrvIdx];
    Texture2D<float4> auxTex   = ResourceDescriptorHeap[g_AuxTex1SrvIdx];

    const float  depth = depthTex.SampleLevel(g_SampPointClamp, uv, 0.0f);
    const float4 aux   = auxTex.SampleLevel(g_SampPointClamp, uv, 0.0f);
    const bool bypassExempt = (g_PassSubMode != 0u);

    // Reverse-Z 天空 (depth <= 1e-5) 或角色像素 (AuxMask.a >= 0.495) 直接豁免输出 1.0
    if (depth <= 1e-5f || (!bypassExempt && aux.a >= 0.495f))
    {
        ssaoOut[dtid.xy] = 1.0f;
        return;
    }

    const float3 posWS = ReconstructWorldPosReverseZ(uv, depth);
    float3 N_ws = aux.xyz * 2.0f - 1.0f;
    const float nLenSq = dot(N_ws, N_ws);
    N_ws = (nLenSq > 1e-6f) ? (N_ws * rsqrt(nLenSq)) : float3(0.0f, 0.0f, 1.0f);

    // 像素哈希旋转基底
    const float angle = frac(sin(dot(float2(dtid.xy), float2(12.9898f, 78.233f))) * 43758.5453f) * 6.2831853f;
    float s, c;
    sincos(angle, s, c);

    const float3 upRef = (abs(N_ws.z) < 0.99f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 T     = normalize(cross(upRef, N_ws));
    const float3 B     = cross(N_ws, T);
    const float3 rotT  = T * c + B * s;
    const float3 rotB  = -T * s + B * c;

    const float radius = max(g_PassParam0, 0.25f); // 默认 1.2m
    const float bias   = max(g_PassParam1, 0.01f); // 默认 0.04m
    const float viewDist = length(posWS - g_CameraPosAndNear.xyz);

    float occlusion = 0.0f;
    [unroll]
    for (int i = 0; i < 12; ++i)
    {
        const float3 k = kHemisphereKernel[i];
        const float3 samplePosWS = posWS + (rotT * k.x + rotB * k.y + N_ws * k.z) * radius;
        const float4 sClip = mul(float4(samplePosWS, 1.0f), g_ViewProj);
        if (sClip.w <= 1e-4f)
            continue;
        const float3 sNdc = sClip.xyz / sClip.w;
        const float2 sUV  = float2(sNdc.x * 0.5f + 0.5f, -sNdc.y * 0.5f + 0.5f);
        if (sUV.x <= 0.0f || sUV.x >= 1.0f || sUV.y <= 0.0f || sUV.y >= 1.0f)
            continue;

        const float sceneDepth = depthTex.SampleLevel(g_SampPointClamp, sUV, 0.0f);
        if (sceneDepth <= 1e-5f)
            continue;
        const float3 scenePosWS = ReconstructWorldPosReverseZ(sUV, sceneDepth);
        const float  sampleDist = length(samplePosWS - g_CameraPosAndNear.xyz);
        const float  occDist    = length(scenePosWS - g_CameraPosAndNear.xyz);

        // Reverse-Z / 世界空间距离：真实表面更靠近相机且落入 radius 范围内即产生遮蔽
        const float rangeCheck = smoothstep(0.0f, 1.0f, radius / max(abs(viewDist - occDist), 1e-3f));
        occlusion += ((occDist + bias) < sampleDist ? 1.0f : 0.0f) * rangeCheck;
    }

    const float ao = saturate(1.0f - (occlusion / 12.0f));
    ssaoOut[dtid.xy] = ao;
}
