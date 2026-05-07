二、Delt06 Renderer 的职责与定位
1. 项目定位

Delt06 Renderer 本质上是：

Pass-Based Modern Renderer

更偏向：

Render Pipeline Layer

其核心目标：

组织现代实时渲染流程
2. Delt06 主要负责的层次
（1）Render Pipeline / Render Pass（核心）

这是 Delt06 最核心部分。

例如：

GBuffer Pass
↓
SSAO
↓
SSR
↓
Deferred Lighting
↓
Bloom
↓
ToneMapping
↓
TAA
↓
Present

作用：

组织整帧渲染流程
（2）Rendering Features

例如：

Deferred Rendering
SSAO
SSR
Bloom
TAA
Auto Exposure
Skybox
PBR Lighting

作用：

实现具体视觉效果
（3）部分 RenderGraph 思想

虽然 Delt06 没有完整 RenderGraph。

但已经具备：

Pass Dependency Thinking

例如：

GBuffer 输出
→ Lighting 输入

Depth 输出
→ SSAO 输入

已经属于：

RenderGraph 思维雏形
3. Delt06 的核心思想

Delt06 关注：

WHAT GPU WORK IS DOING

即：

GPU 为什么执行这些 Pass

重点解决：

渲染流程组织
Pass Dependency
屏幕空间渲染
后处理链路
整帧 Frame Flow
4. Delt06 的优点
优点
（1）Render Pipeline 非常清晰

非常适合学习：

Deferred Rendering
Fullscreen Pass
Lighting Pipeline
Post Process
（2）接近现代 AAA Renderer

其结构非常接近：

Unreal Renderer
HDRP
Frostbite
RE Engine
（3）非常适合建立 Pass Thinking

即：

每个 Pass：

输入是什么？
输出是什么？
为什么存在？

这是现代图形程序员核心能力。

5. Delt06 的缺点
缺点
（1）底层 GPU Infrastructure 不够系统化

例如：

Descriptor System
Memory Allocator
Multi Queue
Resource Lifetime

不如 MiniEngine 完整。

（2）默认需要较强 DX12 基础

如果不懂：

CommandList
Barrier
RootSignature

阅读会比较困难。