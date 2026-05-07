1_Application
职责

应用入口层。 负责：

WinMain/Main
Engine生命周期
初始化流程
主循环
输入派发
场景启动

本层是整个系统调度中心。

允许依赖
00_Core
09_Renderer
10_Scene
02_Camera
被谁依赖

通常不被依赖

禁止内容
具体GPU实现
RenderPass细节
Shader编译逻辑
