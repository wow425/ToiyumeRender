07_RenderPasses
职责

渲染Pass集合。 负责：

GBufferPass
ShadowPass
LightingPass
SSAOPass
PostProcessPass
SkyboxPass

每个Pass只关注自己的输入输出。

允许依赖
00_Core
03_RHI
05_ResourceSystem
06_RenderGraph
13_Math
被谁依赖
RenderFeatures
Renderer
禁止内容
Engine生命周期
Scene加载
