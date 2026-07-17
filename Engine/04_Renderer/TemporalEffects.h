//
// Created by saber on 2026/7/17.
//

#ifndef FOLDER_TEMPORALEFFECTS_H
#define FOLDER_TEMPORALEFFECTS_H

class CommandContext;


namespace TemporalEffects
{
    // Temporal antialiasing involves jittering sample positions and accumulating color over time to
    // effectively supersample the image.
    // 时域抗锯齿包含抖动采样位置和随时间累积颜色以有效地对图像进行超采样。
    extern bool EnableTAA;

    void Initialize( void );

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

#endif //FOLDER_TEMPORALEFFECTS_H
