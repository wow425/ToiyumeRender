// ============================================================================
// 着色器：EnvSkyboxPS.hlsl（M6 天空盒球面映射 5687 s_slg_day02_sky）
// 对标：AK12-frame7206.rdc FS_7809_Skybox_Cubemap_FS
// 编译配置：compile_shader_bindless (ps_6_6, MainPS)
// ============================================================================
#include "08_Character/CharacterCommon.hlsli"

CharacterPSOutput MainPS(CharacterPSInput input)
{
    const uint debugView = (uint)(g_ToggleFlags0.x + 0.5f);
    CharacterPSOutput outPS;
    outPS.AuxMask = float4(0.5f, 0.5f, 1.0f, 0.50f); // 天空跳过 SSAO

    if (debugView != 0u && debugView != 1u)
    {
        outPS.LinearHDR = float4(0.02f, 0.03f, 0.04f, 1.0f);
        return outPS;
    }

    const float3 dirUE = normalize(input.WorldPos - g_CameraPosAndNear.xyz);
    const float3 dirU  = GFL2_UE5ToUnityDir(dirUE); // (x_u, y_u, z_u)

    const float u = atan2(dirU.x, dirU.z) * 0.15915494f + 0.5f;
    const float v = asin(clamp(-dirU.y, -1.0f, 1.0f)) * 0.31830989f + 0.5f;

    float3 skyCol = float3(0.05f, 0.08f, 0.12f);
    if (g_AuxTex0SrvIdx != GFL2_INVALID_BINDLESS && g_AuxTex0SrvIdx > 2u)
        skyCol = GFL2_SampleBindless2D(g_AuxTex0SrvIdx, g_SampLinearWrap, float2(u, v)).rgb;

    outPS.LinearHDR = float4(skyCol, 1.0f);
    return outPS;
}
