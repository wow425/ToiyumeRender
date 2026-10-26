#pragma once

// ============================================================
// R4：显示编码链（Exposure -> ToneMap -> sRGB encode）
// ------------------------------------------------------------
// 为什么单独拆一个文件：
//   R3 之前，这条链只存在于 LightingPS 里，Skybox 完全不参与 ——
//   Skybox 直接把 HDR 采样值写进 R8G8B8A8_UNORM 的 SceneColor，
//   于是天空成为整幅画面里唯一「曝光/ACES 管不到」的区域（实测 42.5% 像素硬截断）。
//   同时 SceneColor 与 swapchain 都是 *_UNORM（非 _SRGB），
//   最后一步 gamma 编码必须由 Shader 自己做，否则线性值被当作 gamma 值显示，画面整体偏暗/偏灰。
//
//   把这三个函数集中到一个 .hlsli，是为了让「所有写 SceneColor 的 Pass」
//   引用同一份实现：任何一处漏用，就会重新出现两个 Pass 亮度/伽马不一致。
//
// 调用顺序必须是：hdr -> Exposure -> ToneMap(可选) -> [Debug View 在此之前返回]
//                 -> sRGB encode(可选) -> 写 UNORM RT
// ============================================================

#ifndef __COLOR_PIPELINE_HLSLI__
#define __COLOR_PIPELINE_HLSLI__

// sRGB 传递函数（IEC 61966-2-1）。
// 注意：不能用 pow(color, 1/2.2) 近似代替 —— 两者在暗部差异可达数个 8bit 级，
// 会让「改之前 / 改之后」的对比失去可比性。
float3 LinearToSRGB(float3 linearColor)
{
    float3 lo = linearColor * 12.92;
    float3 hi = 1.055 * pow(max(linearColor, float3(0.0, 0.0, 0.0)), 1.0 / 2.4) - 0.055;
    return lerp(hi, lo, step(linearColor, float3(0.0031308, 0.0031308, 0.0031308)));
}

// ACES Filmic 近似曲线（Krzysztof Narkowicz 拟合）。
float3 ACESFilm(float3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// 统一的场景色 -> 显示色转换。
//   exposure   线性乘性缩放，ACES 之前应用
//   tonemap    0 = 直接 saturate（线性裁剪，用作对照），1 = ACES
//   srgbEncode 0 = 原样输出（用于 A/B 取证，证明编码这一步确实在起作用），1 = sRGB
float3 EncodeDisplayColor(float3 hdrColor, float exposure, bool tonemap, bool srgbEncode)
{
    float3 exposed = hdrColor * max(exposure, 0.0);
    float3 mapped = tonemap ? ACESFilm(exposed) : saturate(exposed);
    return srgbEncode ? LinearToSRGB(mapped) : mapped;
}

#endif // __COLOR_PIPELINE_HLSLI__
