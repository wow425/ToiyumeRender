04_GPUInfrastructure
职责

GPU基础设施层。 负责：

Upload管理
DynamicBuffer
RingBuffer
Context管理
FrameResource
GPU同步封装
DescriptorAllocator

该层是RHI上的GPU运行时支持层。

允许依赖
00_Core
03_RHI
12_Utility
被谁依赖
ResourceSystem
RenderGraph
Renderer
禁止内容
RenderPass逻辑
Scene逻辑
