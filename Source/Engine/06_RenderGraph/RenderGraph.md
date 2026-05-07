06_RenderGraph
职责

RenderGraph系统。 负责：

Pass依赖分析
Resource生命周期
自动Barrier
Pass调度
Transient Resource

本层是现代渲染器核心调度系统。

允许依赖
00_Core
03_RHI
04_GPUInfrastructure
05_ResourceSystem
被谁依赖
RenderPasses
Renderer
禁止内容
具体场景逻辑
Camera控制
