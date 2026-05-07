08_RenderFeatures
职责

渲染功能组合层。 负责：

延迟渲染
Forward+
SSAO
TAA
Bloom
PBR Feature

该层负责组合多个RenderPass形成完整功能。

允许依赖
06_RenderGraph
07_RenderPasses
05_ResourceSystem
被谁依赖
Renderer
禁止内容
底层RHI细节
