#pragma once


#include "../Resource/Buffer/ColorBuffer.h"
#include "../Resource/Buffer/DepthBuffer.h"
#include "../Resource/Buffer/ShadowBuffer.h"
#include "../Resource/Buffer/GpuBuffer.h"
#include "../RHI/GraphicsCore.h"

namespace Graphics
{
    // extern声明关键字，告知编译器该变量存在，等连接时去别处寻找
    // 打破文件重名，遵守ODR
    extern DepthBuffer g_SceneDepthBuffer;  // 场景深度缓冲区（Depth/Stencil Test）。D32_FLOAT_S8_UINT
    extern ColorBuffer g_SceneColorBuffer;  // 场景颜色缓冲区（色调映射Tone Mapping，HDR）。R11G11B10_FLOAT     HDR用
    extern ColorBuffer g_SceneNormalBuffer; // 场景法线缓冲区（延迟渲染/PBR，屏幕空间效果）。R16G16B16A16_FLOAT
    extern ColorBuffer g_PostEffectsBuffer; // 后处理缓冲区（后期特效计算中转站）。R32_UINT (to support Read-Modify-Write with a UAV)
    extern ColorBuffer g_OverlayBuffer;     // 覆盖层缓冲区（UI,文字，HUD等二维内容）。UI用。R8G8B8A8_UNORM
    extern ColorBuffer g_HorizontalBuffer;  // 水平模糊/缩放缓冲区（高斯模糊）。For separable (bicubic) upsampling

    extern ColorBuffer g_VelocityBuffer;    // 速度缓冲区（TAA,Motion Blur，SSR，DLSS3）R10G10B10  (3D velocity)
    extern ShadowBuffer g_ShadowBuffer;     // 阴影缓冲区（shadow mapping，深度对比，CSM）

    // 学过但不娴熟
    // SSAO
    extern ColorBuffer g_SSAOFullScreen;	// 最终生成遮蔽图。R8_UNORM
    extern ColorBuffer g_LinearDepth[2];	// 非线性深度转为线性的相机距离。Normalized planar distance (0 at eye, 1 at far plane) computed from the SceneDepthBuffer
    extern ColorBuffer g_MinMaxDepth8;		// nXn像素块中的最深/浅值。Min and max depth values of 8x8 tiles
    extern ColorBuffer g_MinMaxDepth16;		    // Min and max depth values of 16x16 tiles
    extern ColorBuffer g_MinMaxDepth32;		    // Min and max depth values of 16x16 tiles
    extern ColorBuffer g_DepthDownsize1;    // 降采样后的深度图
    extern ColorBuffer g_DepthDownsize2;
    extern ColorBuffer g_DepthDownsize3;
    extern ColorBuffer g_DepthDownsize4;
    extern ColorBuffer g_DepthTiled1;       // 平铺分块后的深度图
    extern ColorBuffer g_DepthTiled2;
    extern ColorBuffer g_DepthTiled3;
    extern ColorBuffer g_DepthTiled4;

    // 了解过
    // AO
    extern ColorBuffer g_AOMerged1;         // 合并后的AO图
    extern ColorBuffer g_AOMerged2;
    extern ColorBuffer g_AOMerged3;
    extern ColorBuffer g_AOMerged4;
    extern ColorBuffer g_AOSmooth1;         // 平滑后的AO图
    extern ColorBuffer g_AOSmooth2;
    extern ColorBuffer g_AOSmooth3;
    extern ColorBuffer g_AOHighQuality1;    // 高质量AO图（如双边滤波）。R8_UNORM
    extern ColorBuffer g_AOHighQuality2;
    extern ColorBuffer g_AOHighQuality3;
    extern ColorBuffer g_AOHighQuality4;

    // 没学过
    // DoF Depth of Field 景深,散景效果，基于分块，CS里实现
    extern ColorBuffer g_DoFTileClass[2];   //  分类缓冲区,屏幕分块，记录近，远景模糊，焦内清晰
    extern ColorBuffer g_DoFPresortBuffer;  // 预排序缓冲区，对颜色和深度进行预筛选
    extern ColorBuffer g_DoFPrefilter;
    extern ColorBuffer g_DoFBlurColor[2];   // 模糊缓冲区，分两轮，ping-pong交替使用，村粗模糊后的颜色和a通道
    extern ColorBuffer g_DoFBlurAlpha[2];
    extern StructuredBuffer g_DoFWorkQueue; // 间接绘制的工作队列。work处理复杂的散景计算
    extern StructuredBuffer g_DoFFastQueue; // fast处理简单的模糊
    extern StructuredBuffer g_DoFFixupQueue; // fixup处理边缘修复

    // 没学过
    // TAA Temporal Anti-Aliasing，时间抗锯齿，基于运动矢量和历史数据的抗锯齿技术
    extern ColorBuffer g_MotionPrepBuffer;		// 运动矢量预处理缓冲区。R10G10B10A2
    extern ColorBuffer g_LumaBuffer;            // 亮度缓冲区，存储亮度信息
    extern ColorBuffer g_TemporalColor[2];      // 历史颜色缓冲区，ping-pong交替使用，存储前一帧的颜色数据
    extern ColorBuffer g_TemporalMinBound;      // 历史邻域最值限制器.用于颜色裁剪
    extern ColorBuffer g_TemporalMaxBound;

    // 后处理
    // Bloom,Auto-Exposure,FXAA
    extern ColorBuffer g_aBloomUAV1[2];		// Bloom Pyramid缓冲区640x384 (1/3)
    extern ColorBuffer g_aBloomUAV2[2];		// 320x192 (1/6)  
    extern ColorBuffer g_aBloomUAV3[2];		// 160x96  (1/12)
    extern ColorBuffer g_aBloomUAV4[2];		// 80x48   (1/24)
    extern ColorBuffer g_aBloomUAV5[2];		// 40x24   (1/48)
    extern ColorBuffer g_LumaLR;            // 低分辨率亮度图
    extern ByteAddressBuffer g_Histogram;   // 亮度直方图缓冲区
    extern ByteAddressBuffer g_FXAAWorkQueue; // FXAA工作队列
    extern TypedBuffer g_FXAAColorQueue;

    void InitializeRenderingBuffers(uint32_t NativeWidth, uint32_t NativeHeight);
    void ResizeDisplayDependentBuffers(uint32_t NativeWidth, uint32_t NativeHeight);
    void DestroyRenderingBuffers();

} // namespace Graphics
