03_RHI
职责

渲染硬件抽象层（Render Hardware Interface）。 负责：

Device
CommandQueue
CommandList
Fence
DescriptorHeap
SwapChain
PipelineState
RootSignature

本层屏蔽DX12/Vulkan等底层API差异。

允许依赖
00_Core
13_Math
12_Utility
被谁依赖
GPUInfrastructure
ResourceSystem
RenderGraph
Renderer
禁止内容
Scene逻辑
游戏逻辑
高层渲染策略
