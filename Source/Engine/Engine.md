# 📦 MiniEngine Core 模块结构指南

## 0. 数学矩阵规定
* 采用右手坐标系，CPP端跟shader端都采用行主序，上传前不转置，shader端用row_major，都采用左乘矩阵乘法
* FrontCounterClockwise = true，投影矩阵翻转z

* 现代 Renderer 分层架构

现代 AAA Renderer 通常可以抽象成如下层次：

Gameplay / Scene
        ↓
Rendering Features
        ↓
Render Pipeline / Render Pass
        ↓
RenderGraph
        ↓
GPU Infrastructure
        ↓
RHI
        ↓
Graphics API
        ↓
GPU Driver / Hardware

## 1. Engine职责
 作为Graphics API Layer（与 GPU 驱动直接通信） + GPU Infrastructure（组织 GPU Command 提交） + 部分RHI（降低直接操作 DX12 API 的复杂度）
 核心目标是：高效、安全、工程化地驱动 GPU。

 ## 2. Engine模块划分
 Command System、Resource System、Descriptor System、Synchronization System（？）、Memory Management System、Resource State Tracking（未实现）

  ## 3. Engine核心思想
  关注：HOW GPU WORK IS SUBMITTED，即：GPU 工作如何被正确、高效地提交。 重点解决：GPU 管线驱动、资源管理、DX12工程化、GPU执行效率，而不重点关注：画面是如何生成的。

  ## 4. Engine优缺点
  优点：
(1）工程化程度高
    非常适合学习：DX12 工程实践，GPU 同步，Descriptor 管理，Barrier 管理，多帧资源。
(2）非常接近工业底层框架
    适合理解：现代引擎底层 GPU 系统
（3）适合搭建自己的 Renderer 基础设施
例如：ResourceSystem、UploadSystem、DescriptorAllocator、ContextSystem
缺点：
(1）Render Pipeline 表达不清晰


推荐依赖方向

推荐保持如下单向依赖：

Application ↓ Renderer ↓ RenderFeatures ↓ RenderPasses ↓ RenderGraph ↓ ResourceSystem ↓ GPUInfrastructure ↓ RHI ↓ Core

Math 与 Utility 为横向基础模块。

严禁出现的反向依赖

禁止：

RenderPass 依赖 Renderer
ResourceSystem 依赖 Scene
RHI 依赖 RenderGraph
Utility 依赖 Renderer
Math 依赖 GPU模块

否则会导致：

循环依赖
编译膨胀
模块职责污染
架构不可维护
当前推荐演进方向

你当前已经非常接近现代渲染器架构：

MiniEngine → Renderer Framework → Modern RenderGraph Renderer

下一阶段建议重点：

RenderGraph资源生命周期
Pass自动Barrier
FrameGraph Compile阶段
Material系统
ECS化Scene
GPU Driven Rendering
Bindless
Async Compute



