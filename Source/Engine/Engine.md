# 📦 MiniEngine Core 模块结构指南


---

## 1. Application Framework (应用程序框架与系统层)
* **This is**: 负责引擎生命周期、主循环、窗口与输入。
* **It does**: 负责管理引擎的生命周期、主循环（Main Loop）、窗口交换链（Swapchain）呈现、系统时间流逝以及操作系统级的输入（键鼠/手柄）接收。
* **Don't touch unless**: 你需要更改引擎的启动与关闭流程、添加对新输入设备的支持，或者修改主循环的帧率控制策略。
* **命名空间建议**: `namespace Core::Application`
* GameCore.h / GameCore.cpp
* Display.h / Display.cpp
* SystemTime.h / SystemTime.cpp
* GameInput.h / GameInput.cpp
* Input.cpp
* pch.h / pch.cpp (预编译头文件，通常放在核心/系统层)



## 2. RHI & DX12 Core (硬件渲染接口抽象)
* **This is**: 负责 GPU 显存分配、缓冲区和纹理的管理。
* **It does**: 隐藏直接调用 `d3d12.h` 的繁琐细节。管理图形设备（Device）、命令列表/上下文（CommandContext）、描述符堆（DescriptorHeap）、管线状态机（PSO）和根签名（RootSignature）。
* **Don't touch unless**: 你需要升级底层的渲染 API（例如引入 DXR 硬件光线追踪、Mesh Shader 接口），或者发现了核心命令提交、多线程录制时的同步机制 Bug。
* **命名空间建议**: `namespace Graphics::RHI`
* GraphicsCore.h / GraphicsCore.cpp
* GraphicsCommon.h / GraphicsCommon.cpp
* CommandContext.h / CommandContext.cpp
* CommandListManager.h / CommandListManager.cpp
* CommandAllocatorPool.h / CommandAllocatorPool.cpp
* CommandSignature.h / CommandSignature.cpp
* DescriptorHeap.h / DescriptorHeap.cpp
* DynamicDescriptorHeap.h / DynamicDescriptorHeap.cpp
* RootSignature.h / RootSignature.cpp
* PipelineState.h / PipelineState.cpp
* SamplerManager.h / SamplerManager.cpp
* d3dx12.h (DX12 官方辅助头文件)

## 3. Resource & Memory (资源与内存管理)
* **This is**: 显存的“大管家”。
* **It does**: 负责具体的 GPU 显存分配算法（如伙伴算法 BuddyAllocator、线性分配 LinearAllocator），并维护所有的显存对象生命周期，包括缓冲（Buffers）、纹理（Textures）和渲染目标（RT/DS）。
* **Don't touch unless**: 你需要优化显存的分配策略以减少碎片，或者需要引入一种全新格式的底层硬件资源。
* **命名空间建议**: `namespace Graphics::Resource`
* 内存分配器：BuddyAllocator.h/cpp, LinearAllocator.h/cpp, EsramAllocator.h
* 基类与通用：GpuResource.h, BufferManager.h/cpp
* 缓冲区 (Buffers)：GpuBuffer.h/cpp, ColorBuffer.h/cpp, DepthBuffer.h/cpp, ShadowBuffer.h/cpp, PixelBuffer.h/cpp, UploadBuffer.h/cpp, ReadbackBuffer.h/cpp
* 纹理 (Textures)：Texture.h/cpp, TextureManager.h/cpp, DDSTextureLoader.h/cpp, dds.h

## 4. Rendering Features (渲染特效与后处理)
* **This is**: 具体画面表现的算法实现层。
* **It does**: 通过调用底层的 RHI 命令和 Resource 资源，组合实现具体的图形学算法。包括 SSAO、TAA 抗锯齿、景深（DoF）、动态模糊、泛光（Bloom）等各种屏幕空间与后处理特效。
* **Don't touch unless**: 你正在开发、调试或优化特定的渲染算法，或者需要往后处理管线中插入一个新的画面特效通道。
* **命名空间建议**: `namespace Graphics::Features`
* PostEffects.h / PostEffects.cpp
* TemporalEffects.h / TemporalEffects.cpp
* SSAO.h / SSAO.cpp
* FXAA.h / FXAA.cpp
* DepthOfField.h / DepthOfField.cpp
* MotionBlur.h / MotionBlur.cpp
* ImageScaling.h / ImageScaling.cpp

## 5. Camera (摄像机与场景视图)
* **This is**: 观察虚拟世界的“眼睛”。
* **It does**: 维护视图矩阵（View Matrix）和投影矩阵（Projection Matrix），计算视锥体六个面用于剔除，并将玩家输入转化为摄像机的空间位移（漫游/飞行控制）。
* **Don't touch unless**: 你需要开发新的摄像机交互逻辑（例如第三人称越肩视角、轨道环绕视角），或者需要修改投影矩阵的底层推导。
* **命名空间建议**: `namespace Core::Camera`
* Camera.h / Camera.cpp
* CameraController.h / CameraController.cpp
* ShadowCamera.h / ShadowCamera.cpp

## 6. Subsystems (附加子系统)
* **This is**: 独立运作的高级业务及渲染模块。
* **It does**: 处理具有一定独立性和复杂逻辑的具体系统。例如 GPU 计算着色器驱动的粒子系统（Particle System）、文本与 UI 渲染器、以及用于 GPU 排序的双调排序算法（BitonicSort）。
* **Don't touch unless**: 你需要彻底更改粒子的发射/更新机制，或者需要引入一个新的大型非核心模块（如地形系统、贴花系统）。
* **命名空间建议**: `namespace Core::Subsystems`
* 粒子系统：ParticleEffect.h/cpp, ParticleEffectManager.h/cpp, ParticleEffectProperties.h, ParticleEmissionProperties.cpp, ParticleShaderStructs.h
*UI 与文本：TextRenderer.h/cpp, Fonts/consola24.h
*调试渲染与算法：GraphRenderer.h/cpp (图表绘制), Cube.h/cpp (基础测试立方体), BitonicSort.h/cpp (双调排序算法，供 GPU 粒子等使用)

## 7. Math Library (数学库)
* **This is**: 引擎的纯数学与几何基础。
* **It does**: 提供所有无状态的数学计算工具。涵盖线性代数（向量、矩阵、四元数）、空间几何测试（包围盒、包围球、视锥体相交）以及伪随机数生成。
* **Don't touch unless**: 你发现了底层数学运算的精度/性能瓶颈，或者需要添加一种全新的碰撞几何体相交测试公式。
* **命名空间建议**: `namespace Core::Math`

## 8. Utilities & Profiling (工具与调试分析)
* **This is**: 开发者的辅助工具箱。
* **It does**: 不直接参与画面渲染，但极其重要。提供 CPU/GPU 的时间打点与性能分析（Profiling）、用于实时调试变量的 UI 控制面板绑定、字符串哈希运算以及文件读写辅助。
* **Don't touch unless**: 你需要接入第三方性能分析工具（如对齐 PIX/RenderDoc 的底层事件），或者需要修改引擎配置文件的读取机制。
* **命名空间建议**: `namespace Core::Utility`
* EngineProfiling.h / EngineProfiling.cpp (性能打点)
* GpuTimeManager.h / GpuTimeManager.cpp (GPU 时间统计)
* EngineTuning.h / EngineTuning.cpp (变量实时调试)
* Utility.h / Utility.cpp (通用工具)
* FileUtility.h / FileUtility.cpp (文件读写)
* Util/CommandLineArg.h / Util/CommandLineArg.cpp (命令行解析)
