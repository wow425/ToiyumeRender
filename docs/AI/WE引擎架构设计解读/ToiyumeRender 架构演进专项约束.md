# ToiyumeRender 架构演进专项约束：以 Wicked Engine 为主要参考

## 一、总体目标

在保留 ToiyumeRender 现有可用功能的前提下，参考 Wicked Engine 的成熟架构设计，逐步改善项目的模块边界、渲染路径组织、公共渲染功能复用、资源生命周期、后处理调度与可扩展性。

参考项目：
- Wicked Engine：https://github.com/turanszkij/WickedEngine
- Microsoft MiniEngine：https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/MiniEngine

Wicked Engine 是主要的高层渲染架构参考；MiniEngine 是现有 DX12 底层实现与资源管理的辅助参考。

禁止仅复制目录名称、类名或接口形式。所有结构调整必须说明其解决的问题、设计依据、替代方案与实际收益。

## 二、先审计，再制定目标架构

第一阶段只进行调查和设计，不修改运行时代码。

1. 阅读 Wicked Engine 中与以下内容相关的实际源码：
   - MainComponent 与 RenderPath 生命周期；
   - Forward、Deferred、Tiled Rendering 和 Path Tracing 的组织方式；
   - 公共渲染功能与具体 RenderPath 的职责边界；
   - HDR/LDR 后处理链；
   - Scene、Material、Camera、GPU Resource 与渲染功能之间的关系。
2. 对照 ToiyumeRender 现有的 BaseRenderer、RendererRegistry、ForwardRenderer、DeferredRenderer、RHI、场景与材质系统。
3. 输出当前架构图、目标架构图、模块职责表、依赖关系图和迁移计划。
4. 对每一项建议说明：现有问题、Wicked Engine 的对应设计、拟采用方案、迁移风险、验收条件。
5. 不得假定 Wicked Engine 的具体实现适用于当前项目。必须阅读相关源码后再提出迁移建议。

## 三、目标架构原则

### 1. 应用层与渲染路径分离

应用层负责窗口、输入、主循环、场景选择和工具界面。

渲染路径负责组织一帧的渲染流程，包括 Pass 顺序、资源输入输出及最终画面合成。

避免应用主循环直接堆积大量管线专属渲染代码。

### 2. 渲染路径与公共渲染功能分离

Forward、Deferred、DXR Path Tracing 及未来的 Neural Rendering 应当具有明确的职责边界。

公共功能应根据真实复用需求设计，避免每条管线重复实现材质、阴影、后处理、调试视图等公共机制。

但不得强迫所有算法共用一个不合理的接口。对于确实不同的资源、执行模型和算法，允许保留专用实现。

### 3. 场景、材质与底层 RHI 解耦

场景与材质系统不应直接依赖某个具体渲染路径。

底层 RHI 负责明确的 GPU 资源、命令提交、描述符、同步与资源状态机制。

高层渲染模块通过受控接口使用 RHI，避免将 D3D12 细节无约束地泄漏到整个项目。

### 4. 后处理采用明确的输入输出约定

区分 HDR 阶段、Tonemapping 和 LDR 阶段。

每个效果必须明确输入纹理、输出纹理、分辨率、格式、资源状态及执行顺序。

根据需要使用 Ping-Pong 资源，避免读写冲突和隐式的全局状态依赖。

### 5. 资源生命周期必须与 GPU 执行同步

任何重构都必须检查资源创建、使用、状态转换、Fence、Resize、管线切换与释放的完整生命周期。

禁止为了追求抽象而隐藏重要的 GPU 同步关系。

### 6. 为未来管线保留合理扩展能力

未来计划支持：
- Forward Rendering；
- Deferred Rendering；
- DXR Path Tracing；
- Neural Rendering，例如 3D Gaussian Splatting。

这些路径可以共享场景描述、相机、资产和输出接口，但不要求共享完全相同的渲染算法。

Neural Rendering 初期优先采用独立 Demo 或独立渲染路径验证，不得提前将其强耦合进底层 RHI。

## 四、渐进式迁移规则

必须按依赖关系分阶段迁移，不允许一次性重写整个项目。

每个工作包必须：
1. 描述现状与目标差异；
2. 解释设计选择与关键取舍；
3. 限定修改文件和模块；
4. 保留现有功能或明确记录无法保留的原因；
5. 完成构建、适用测试和代码审查；
6. 生成独立施工文档；
7. 验证通过后才能进入依赖它的下一阶段。

若发现现有架构已合理解决某个问题，则保留原实现，不为追求形式统一而重构。

若 Wicked Engine 的设计不适合当前规模、学习目标或 DX12 实现，应明确说明并采用更合适的方案。

## 五、每次施工必须生成学习文档

每个工作包完成后，必须在 docs/AIWorkLogs/ 下创建独立报告，至少包含：

- 施工目标与原有问题；
- 修改前后的架构图；
- 模块职责及依赖关系；
- 关键类、函数与调用链；
- 关键算法、数据流和资源生命周期；
- 与 Wicked Engine 对应设计的比较；
- 为什么采用当前方案、有哪些替代方案；
- 构建、测试和运行验证证据；
- 已知缺陷与未验证内容；
- 建议的源码阅读顺序；
- 至少五个主动回忆问题及参考答案。

施工报告必须基于实际修改的代码，不得使用预期设计冒充已经实现的功能。

## 六、完成标准

最终目标不是让 ToiyumeRender 的目录结构看起来像 Wicked Engine，而是让项目逐步具备清晰的应用层、渲染路径、公共渲染功能、场景与资产系统、底层 RHI 及可验证的资源生命周期。

在架构迁移完成之前，禁止大规模新增与当前阶段无关的渲染特效。