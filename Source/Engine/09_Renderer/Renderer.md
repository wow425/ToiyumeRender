09_Renderer
职责

渲染器总调度层。 负责：

Frame渲染
RenderFeature调度
Camera接入
Scene渲染入口
RenderGraph执行

本层是渲染系统总控制器。

允许依赖
02_Camera
03_RHI
04_GPUInfrastructure
05_ResourceSystem
06_RenderGraph
07_RenderPasses
08_RenderFeatures
10_Scene
被谁依赖
Application
禁止内容
WinMain
平台初始化细节
