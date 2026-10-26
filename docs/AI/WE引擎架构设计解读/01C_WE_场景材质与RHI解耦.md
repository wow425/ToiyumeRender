# 01C WickedEngine 源码精读：场景/材质与底层 RHI 解耦（源码级证据报告）

> 调查对象：`D:/CS-Self-Study/Computer_Graphics/DX12/工程实现参考/WickedEngine-master/WickedEngine/`（下称 WE，全文相对路径均以该目录为基准）。
> 调查方式：只读源码 + 逐文件定位读取，未运行构建、未修改任何文件。
> 证据标注约定：**【实测】**=由读文件/命令输出直接看到；**【推断】**=由上下文推测、尚需读指定文件确认。

---

## 0. 结论速览（TL;DR）

1. WE 的 Scene 是标准 ECS（`wi::ecs::ComponentManager<T>` 线性数组 + 实体句柄），**没有 RenderableComponent**；可渲染/可见性是 `ObjectComponent::_flags` 与 `MaterialComponent` 的位移标志。
2. Scene **直接持有 GPU 资源**：`wiScene.h:86-167` 有 `Texture`/`GPUBuffer`/`QueryHeap`/`RaytracingAccelerationStructure` 成员，但类型一律是 `wi::graphics::*`，**不出现 D3D12 类型**。
3. 组件同样直接持有 GPU 资源：`MeshComponent::generalBuffer/streamoutBuffer/BLASes`（`wiScene_Components.h:728-770`）、`ObjectComponent::lightmap/vb_ao/wetmap`（`:1203-1208`），以及**裸 `int` bindless 描述符索引**（`:255-256, 267, 737-738`）。
4. 材质参数经 `MaterialComponent::WriteShaderMaterial()`（`wiScene_Components.cpp:282`）打包成 384 字节 `ShaderMaterial`（`shaders/ShaderInterop_Renderer.h:393,513`），memcpy 进 `Scene::materialArrayMapped`（`wiScene.cpp:4352`），**shader 用 bindless 索引取材质**，而非逐材质常量缓冲。
5. PSO 不在场景层创建：渲染器持有 `wi::unordered_map<uint32_t, PipelineState> PSO_object[渲染通道][着色类型][mesh shader]`（`wiRenderer.cpp:366`），变体 key 是位域 union `ObjectRenderingVariant`（`wiRenderer.cpp:348-362`）；整个变体空间在 `Initialize()` 里**后台线程预热**（`wiRenderer.cpp:1945-2224`），未就绪时 `RenderMeshes` 跳过该 draw（`:3333-3334`）。
6. 延迟销毁在 RHI 内部：`AllocationHandler::Update()` 用「入队帧号 + BUFFERCOUNT < 当前帧号」的判据批量释放 10 个 `destroyer_*` 队列（`wiGraphicsDevice_DX12.h:565-638`），未暴露逐资源 fence。
7. 3DGS 是**独立模块**：`wi::GaussianSplatModel` + `wi::GaussianSplatScene`（`wiGaussianSplatModel.h:15-67`）自带 `GPUBuffer`，只被 Scene 持有（`wiScene.h:68` 组件管理器、`wiScene.cpp:5405-5419` 更新），**未塞进 MeshComponent 体系**，且自持 shader/PSO（`wiGaussianSplatModel.cpp:18-24, 236-243, 493-505`）。
8. root signature **内嵌在 shader 字节码**：DX12 `CreateShader` 反序列化 shader 里的 root signature 并 `CreateRootSignature`（`wiGraphicsDevice_DX12.cpp:3899-3916`），PSO 直接复用 shader 的 root signature（`:4102-4189`）——RHI 层不暴露该概念。
9. 模型格式导入（OBJ/GLTF/GLB/VRM/FBX/PLY）**不在引擎运行时**：导入器全在 `Editor/ModelImporter_*.cpp`，运行时 `LoadModel2` 只读序列化场景（`wiScene.cpp:9230-9237`、`Editor/ModelImporter.h:3-7`）。

---

## 1. Scene 的数据组织：ECS、遍历与脏标记

### 1.1 实体与组件存储【实测】
- `wiECS.h:25` `using Entity = uint64_t;`；`:26` `INVALID_ENTITY = 0`；`:28-32` `CreateEntity()` 用全局 `std::atomic<Entity> next` 自增发号，**实体是全局唯一句柄而非索引**。
- `wiECS.h:147-705` `ComponentManager<Component>`：`wi::vector<Component> components`（`:461`，“linear array of alive components”）与 `wi::vector<Entity> entities`（`:463`）**平行数组**，外加 `LookupTable lookup`（`:470`，默认 `LOOKUP_BUCKET_HASH`，`:616-671` 按 `entity>>6` 分桶、`entity&63` 桶内槽，注释 `:618` “hashes per 64-item bucket, resulting in less hashed entries”）。
- `Create()`（`:300-321`）断言“同一 entity 同类型组件唯一”；`Remove()`（`:324-344`）用 `components[index] = std::move(components.back())` 做 **swap-and-pop**，会打乱顺序；`Remove_KeepSorted()`（`:347-372`）才保序 O(n)。
- `ComponentLibrary`（`:709-916`）用 `std::unordered_map<std::string, LibraryEntry>`（`:717`）按**类型名字符串**注册组件管理器，名字参与序列化（`:719-721`）。

### 1.2 Scene 的组件管理器清单【实测】
`wiScene.h:25-68`：`Scene` 持有 `wi::ecs::ComponentLibrary componentLibrary`（`:29`）与约 40 个 `ComponentManager<T>&`：materials(35)/meshes(36)/objects(38)/lights(42)/cameras(43)/…/gaussian_splats(68) 等。**没有独立的 RenderableComponent**：可渲染性由 `ObjectComponent`（`wiScene_Components.h:1181` `_flags = RENDERABLE|CAST_SHADOW`）与 `MeshComponent` 共同表达。

### 1.3 可见性判据【实测】
- `ObjectComponent::IsRenderable()`（`wiScene_Components.h:1238`）= `(_flags & RENDERABLE) && (GetTransparency() < 0.99f)`；`GetTransparency()`（`:1249`）= `1 - color.w`。**可见性判据里混进了透明度**。
- `filterMask/filterMaskDynamic`（`:1186, 1201, 1250`）与 `lod`/`draw_distance`（`:1189-1190`）供渲染器做通道过滤与距离淡出。
- `ObjectComponent::meshID`（`:1184`）是到 `MeshComponent` 的实体引用——组件间用 **Entity 而非指针**互联（可序列化）。

### 1.4 渲染器如何遍历【实测 + 待补】
- `wiRenderer.h:150` `wi::vector<uint32_t> visibleObjects;`（`:174` 每帧 `clear()`）——可见对象列表是**索引数组**，不是指针数组。
- 写入点：`wiRenderer.cpp:3727` `vis.visibleObjects.resize(object_loop)`、`:3805` `vis.visibleObjects[prev_count + i] = groupOffset + stream_compaction.list[i]`、`:3991` `vis.visibleObjects.resize((size_t)vis.object_counter.load())`——由 **stream compaction（GPU 侧）产生压缩后的索引**再拷回。
- 消费点：`wiRenderer.cpp:5849-5853` `for (uint32_t objectIndex : vis.visibleObjects)`、`:7223` `for (uint32_t instanceIndex : vis.visibleObjects)`。
- `renderQueue/renderQueue_transparent`（`wiRenderer.cpp:338-339`，`thread_local`）：每帧 `init()`（`:7222`）→ `add(mesh_index, instanceIndex, distance, sort_bits)`（`:7243`）→ `sort_opaque()/sort_transparent()`（`:7168`、`:6816`）→ `RenderMeshes(vis, renderQueue, renderPass, filterMask, cmd)`（定义 `:3137`，注释 `:3425` "RenderQueue is sorted based on mesh index"）遍历 `renderQueue.batches`（`:3427`）成实例化批次。
- `DrawScene` 定义在 `wiRenderer.cpp:7179`（`device->EventBegin("DrawScene", cmd)` `:7196`）。
- **【未确认】** `RenderQueue` 类型定义与 `add/sort_opaque` 的排序键细节（本次 grep `wiRenderer.h` 未命中，需在别的头文件确认）。

### 1.5 层次变换与脏标记【部分实测】
- `Scene` 提供 `StartBuildTopDownHierarchy()/WaitBuildTopDownHierarchy()/RefreshHierarchyTopdownFromParent()`（`wiScene.h:316-318`）与 `ComputeEntityMatrixRecursive/ComputeParentMatrixRecursive`（`:583,586`）。
- `MaterialComponent` 有显式脏标志：`SetDirty()`（`wiScene_Components.h:280`）、`IsDirty()`（`:281`），所有 `Set*` 都先 `SetDirty()`（`:309-383`）。
- **【待补】**`HierarchyComponent` 的脏传播与 `RunTransformUpdateSystem/RunHierarchyUpdateSystem`（`wiScene.h:494-495`）实现细节，需读 `wiScene.cpp` 对应函数。

---

## 2. 「场景数据」与「GPU 资源」的边界

### 2.1 边界在哪：组件既存 CPU 数据也存 GPU 句柄【实测】
`MeshComponent`（`wiScene_Components.h:727-792`）同一个 struct 里同时有：
- CPU 端：`wi::vector<XMFLOAT3> vertex_positions` 等几何数组、`Subset` 子集表；
- GPU 端：`wi::graphics::GPUBuffer generalBuffer; // index buffer + all static vertex buffers`（`:728`）、`GPUBuffer streamoutBuffer; // all dynamic vertex buffers`（`:729`）、`wi::allocator::PageAllocator::Allocation generalBufferOffsetAllocation`（`:730`）；
- 光线追踪：`wi::vector<wi::graphics::RaytracingAccelerationStructure> BLASes; // one BLAS per LOD`（`:770`）+ `BLAS_STATE{NEEDS_REBUILD,NEEDS_REFIT,COMPLETE}`（`:786-791`）；
- 绑定细节：`struct BufferView{offset,size,subresource_srv,descriptor_srv,subresource_uav,descriptor_uav}`（`:732-745`）与 20 个 `vb_*/ib_*/so_*` 视图（`:746-761`）。
**结论【实测】**：Entity 的 mesh **不是**通过“中间层资源池”间接寻址 GPU 资源，而是 `MeshComponent` 本身就持有 `GPUBuffer` 句柄与 bindless 描述符索引。中间确实有一层——`Scene` 全场景共享的 `generalBuffer` 大缓冲（用 suballocation 塞进每个 mesh 的数据），但**这一层没有类型封装**，只是“在同一个 GPUBuffer 里用 offset 区分”。

### 2.2 全局几何池：suballocation 注释原文【实测】
`wiScene_Components.cpp:1217-1219` 注释：
> “The suballocation strategy is used to have all mesh buffers reside in a global buffer / With this we can avoid rebinding the index buffer for every mesh and can work with purely offsets / Though the index buffer will still need to be rebound if the index format changes, but that happens less frequently”
实现：`SuballocateGPUBuffer(bd.size)`（`:1220`）→ `device->CreateBuffer2(&bd, init_callback, &generalBuffer, &suballoc.alias, suballoc.allocation.byte_offset)`（`:1223`），失败则退化为独立 buffer（`:1232`）。随后每个 `BufferView` 用 `CreateSubresource(..., SubresourceType::SRV, offset, size, &format)` + `GetDescriptorIndex(...)` 拿到裸 `int`（`:1237-1297`）。

### 2.3 每帧 CPU→GPU：mapped 指针直写【实测】
`wiScene.h:126-150`：“上传缓冲 + 映射指针 + GPU 缓冲”三件套（双缓冲 `[BUFFERCOUNT]`）：
| 数据 | upload 缓冲 | 映射指针 | 设备缓冲 |
| --- | --- | --- | --- |
| 实例 | `instanceUploadBuffer[BUFFERCOUNT]`（`:126`） | `instanceArrayMapped`（`:127`） | `instanceBuffer[BUFFERCOUNT]`（`:129`） |
| 几何 | `geometryUploadBuffer[BUFFERCOUNT]`（`:137`） | `geometryArrayMapped`（`:138`） | `geometryBuffer[BUFFERCOUNT]`（`:140`） |
| 材质 | `materialUploadBuffer[BUFFERCOUNT]`（`:144`） | `materialArrayMapped`（`:145`） | `materialBuffer[BUFFERCOUNT]`（`:147`） |
写入点【实测】：`wiScene.cpp:4352`（材质）、`:4237`（subset 几何）、`:4784`（实例）、`:5268/5371`（实例）均为 `std::memcpy(xxxArrayMapped + index, &struct, sizeof(struct))`，注释 “memcpy whole structure into mapped pointer to avoid read from uncached memory”。
消费侧描述符【实测】：`wiScene.cpp:930-932` `shaderscene.instancebuffer/geometrybuffer/materialbuffer = device->GetDescriptorIndex(&...UploadBuffer[...], SubresourceType::SRV)`。

### 2.4 Entity → DrawIndexed 链路（已实证部分）【实测】
```
Scene::objects (ComponentManager<ObjectComponent>)
  └─ ObjectComponent::meshID                      wiScene_Components.h:1184
Scene::meshes (ComponentManager<MeshComponent>)
  ├─ generalBuffer (GPUBuffer)                    wiScene_Components.h:728
  ├─ BufferView {offset,size,descriptor_srv}      wiScene_Components.h:732-761
  └─ Subset {indexOffset,indexCount,materialID}   → 每 subset 一次 Draw
每帧：memcpy → instance/geometry/materialArrayMapped  wiScene.cpp:4237/4352/4784
     → Scene::instanceBuffer 等（DEFAULT 堆）      wiScene.h:126-150
可见性：GPU stream compaction → visibleObjects    wiRenderer.cpp:3727/3805/3991
提交：BindVertexBuffers/BindIndexBuffer + DrawIndexedInstanced
      wiRenderer.cpp:3406/3418（实例化）, 8596/9739（单实例）
```
- 上传→设备缓冲【实测】`wiScene.cpp:1084-1128`：注释 `:1084` "Note: with the CopyBufferAsync we also have to double buffer the dedicated GPU memory buffers as the async copy will be kicked with frame overlap"；`StackVector<GPUBufferCopyCommand,8> async_buffer_uploads`（`:1085`）逐项填 `command.src = &instanceUploadBuffer[cpu_gpu_mapped_resource_index]`、`command.dst = &instanceBuffer[...]`（`:1086-1104`），最后 `device->CopyBufferAsync(..., "Scene async buffer updates")`（`:1127`）。
- 实例批次【实测】`wiRenderer.cpp:3424-3450`：注释 `:3425` "RenderQueue is sorted based on mesh index, so when a new mesh or stencil request is encountered, we need to flush the batch"；遇到新 `meshIndex`/stencil/lod 就 `batch_flush()` 开新批（`:3436-3444`），实例数累积在 `instancedBatch.instanceCount`。
- 索引偏移【实测】`:3385-3394`：若走了 suballocation，`indexOffset = ((generalBufferOffsetAllocation.byte_offset + ibv.offset) / ib_stride) + subset.indexOffset`，否则 `ibv.offset / ib_stride + subset.indexOffset`——**这是 2.2 节全局缓冲策略在绘制侧的直接后果**。
- 提交【实测】`:3418` `device->DrawIndexedInstanced(subset.indexCount, instancedBatch.instanceCount, indexOffset, 0, 0, cmd)`；mesh shader 变体走 `DispatchMesh`（`:3414`）。

---

## 3. Material 与 shader/PSO 的绑定

### 3.1 材质参数结构与打包【实测】
- GPU 结构 `ShaderMaterial`（`shaders/ShaderInterop_Renderer.h:393-511`）`alignas(32)`，**全部字段用半精度打包成 `uint2`** 以省空间；`static_assert(sizeof(ShaderMaterial) == 384)`（`:513`）。
- 打包函数 `MaterialComponent::WriteShaderMaterial(ShaderMaterial*)`（`wiScene_Components.cpp:282-445`）：`pack_half4(...)` 逐字段写入，末尾 `CombineStencilrefs(...) << 24u`（`:391`）与 `std::memcpy(dest, &material, sizeof(ShaderMaterial))`（`:444`）。
- 纹理槽：`TEXTURESLOT` 共 15 个（`wiScene_Components.h:220-239`）；`material.textures[i].texture_descriptor = device->GetDescriptorIndex(...)`（`wiScene_Components.cpp:400-418`），为空则 `-1`（`:422`）。→ **纹理槽与 shader 的对应关系就是数组下标 + bindless 索引**，没有额外的槽位映射层。

### 3.2 材质如何决定 PSO【实测】
变体 key = 位域 union `ObjectRenderingVariant`（`wiRenderer.cpp:348-362`）：renderpass:4 / shadertype:8 / blendmode:4 / cullmode:2 / tessellation:1 / alphatest:1 / sample_count:4 / mesh_shader:1，`static_assert(sizeof(...) == sizeof(uint32_t))`（`:363`）。
缓存 = `static wi::unordered_map<uint32_t, PipelineState> PSO_object[RENDERPASS_COUNT][MaterialComponent::SHADERTYPE_COUNT][OBJECT_MESH_SHADER_PSO_COUNT]`（`wiRenderer.cpp:366`），经 `GetObjectPSO(variant)`（`:364-368`）取用。
**注意：不是懒创建，而是 `Initialize()` 全变体预热**【实测】`wiRenderer.cpp:1945-2224`，注释 `:1940-1942` "We don't wait for these here, because then it can slow down the init time a lot / We compile PipelineStates on backround threads, but we will add them to be useable by GetObjectPSO on the main thread (wi::eventhandler::EVENT_THREAD_SAFE_POINT) / The RenderMeshes that uses these pipeline states will be checking the PipelineState.IsValid() and skip draws if the pipeline is not yet valid"；嵌套循环 `renderPass × shaderType × mesh_shader × blendMode × cullMode × tessellation × alphatest × msaa{1,2,4,8}`（`:1945-1956, 2150-2160`），每个 `device->CreatePipelineState(&desc, &pso, &renderpass_info)` 建好后在**线程安全点回填** `*GetObjectPSO(variant) = pso`（`:2156-2159` 等 5 处）。运行时 PSO 若尚未编译完，`RenderMeshes` 直接 `continue`（`:3333-3334`）。
【判断】收益是运行时零编译停顿，代价是启动期编译量大（注释即为此把编译放后台）；TooiyumeRender 的 Deferred 变体维度少得多，可改为按需创建 + 首帧跳过，避免几秒启动抖动。
→ **不是“shader 名字字符串查表”，而是把渲染状态压成 32 位整数 key**；`shadertype` 只占 8 位，来自 `MaterialComponent::SHADERTYPE`。

### 3.3 shader 文件绑定【实测】
`wiRenderer.cpp:791` `bool LoadShader(...)`、`:895` `void LoadShaders()`，逐个 `LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_COMMON], "objectVS_common.cso")`（`:910`），并大量用 `wi::jobsystem::Execute(ctx, ...)` 并行加载（`:944-998`）。→ **shader 是“编译期枚举槽位 + 固定 .cso 文件名”**，运行时没有字符串查找。
内部机制【实测】`wiRenderer.cpp:791-892`：`shaderbinaryfilename = SHADERPATH + filename`（`:800`）；有 permutation defines 时把每个 define 拼进文件名（`:802-811`）；`#ifdef SHADERDUMP_ENABLED` 先查 `wiShaderDump::shaderdump`（`:815-832`）；否则 `wi::shadercompiler::RegisterShader(...)`（`:835`）+ `IsShaderOutdated(...)`（`:837`）命中时用 `CompilerInput{entrypoint, format = device->GetShaderFormat(), stage, minshadermodel, defines}` 与 `SHADERSOURCEPATH` include 目录就地编译 HLSL（`:839-864`），失败 `backlog::post` + `SHADER_ERRORS` 累加（`:869`）；否则 `FileRead(*.cso)` → `device->CreateShader(stage, data, size, &shader, entrypoint)` + `SetName`（`:875-883`）；文件缺失返回 `SHADER_MISSING`（`:887`）。→ **shader 编译/加载是渲染器职责**；连 `GaussianSplatModel` 也反过来调 `wi::renderer::LoadShader`（`wiGaussianSplatModel.cpp:209-212`）。

---

## 4. wiGraphics 抽象暴露了多少 D3D12 细节

条目清单（全部【实测】，见 `wiGraphics.h` / `wiGraphicsDevice.h`）：
| # | 暴露的 D3D12 概念 | WE 抽象 | 判定 |
| --- | --- | --- | --- |
| 1 | 资源状态 | `enum ResourceState`（`wiGraphics.h:458-487`，`SHADER_RESOURCE`/`RENDERTARGET`/`DEPTHSTENCIL_READONLY`/`COPY_DST`…19 个位标志） | **必要物理事实**（但命名几乎照抄 D3D12） |
| 2 | 屏障 | `GPUBarrier`（`:683-762`）`Type{MEMORY,IMAGE,BUFFER,ALIASING}` + 静态工厂 `Image/Buffer/Memory/Aliasing`；`CommandList::Barrier(...)`（`wiGraphicsDevice.h:256`） | **必要物理事实**（与 `D3D12_RESOURCE_BARRIER` 同构） |
| 3 | 描述符堆/句柄 | 只暴露 `int GetDescriptorIndex(const GPUResource*, SubresourceType, int subresource)`（`wiGraphicsDevice.h:109`）与 `CreateSubresource`（`:104-107`）；堆本身在 DX12 后端私有（`DescriptorHeapGPU`，`wiGraphicsDevice_DX12.h:474-498`） | **抽象基本成立**，但把 `int` 写进 ECS 组件（`wiScene_Components.h:255,267,737`）是**抽象泄漏** |
| 4 | root signature | RHI 层不可见；DX12 后端内部 `ID3D12RootSignature*`（`wiGraphicsDevice_DX12.h:202-203`）与 `multidraw_signatures`（`:63-71`）。**它来自 shader 字节码**：`CreateShader` 用 `D3D12CreateVersionedRootSignatureDeserializer` + `GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1)` + `device->CreateRootSignature(...)`（`wiGraphicsDevice_DX12.cpp:3899-3916`）；`CreatePipelineState` 再复用 `shader_internal->rootSignature/rootsig_desc`（`:4102-4189`） | **良好封装**（代价：RHI 使用者的 HLSL 必须自带 root signature 声明，这本身是 D3D12 事实） |
| 5 | 队列 | `enum QUEUE_TYPE{QUEUE_GRAPHICS,QUEUE_COMPUTE,QUEUE_COPY,QUEUE_VIDEO_DECODE}`（`wiGraphicsDevice.h:59-67`），但上层只用 `BeginCommandList()/SubmitCommandLists()`（`:122,125`） | 半暴露；**上层无感** |
| 6 | 围栏/fence | RHI 层只有 `WaitForGPU()`（`:128`）与 `GetFrameCount()`（`:139`）；`ID3D12Fence` 在 `GraphicsDevice_DX12.h:93-97,134-135` 私有 | **良好封装**（同步语义被收进 RHI） |
| 7 | 显式堆类型/上传 | `enum Usage{UPLOAD,DEFAULT,READBACK}`（`wiGraphics.h:160-165`）+ `GPUResource::mapped_data`（`:868`，注释限定仅 UPLOAD/READBACK 有效）+ `AllocateGPU/CreateBuffer2(init_callback)`（`wiGraphicsDevice.h:93,355`） | **必要物理事实**（上传/回读必须显式） |
| 8 | 描述符表绑定 | `DESCRIPTORBINDER_CBV_COUNT=14/SRV=16/UAV=16/SAMPLER=8`（`:26-30`）+ `DescriptorBindingTable`（`:31-40`）+ `BINDLESS_RESOURCE_CAPACITY=500000`（`:44`） | **需注意**：把“根参数槽位数”写进公共头，属实现细节外露（但对教学项目是明确约束） |
| 9 | push constant / root CBV | `PUSH_CONSTANT_COUNT=22`（`:49`）、`ROOT_CBV_COUNT=3`（`:53`） | 同上 |
| 10 | 交换链 HDR/色彩空间 | `GetSwapChainColorSpace()`、`IsSwapChainSupportsHDR()`（`:172,175`） | 必要 |
| 11 | 显存预算 | `struct MemoryUsage{budget,usage}` + `GetMemoryUsage()`（`:184-190`） | 可选（D3D12 QueryVideoMemoryInfo 的直译） |
| 12 | 内存别名 | `AliasingAllocator`（`:449-505`）+ `ResourceMiscFlag::ALIASING_*` | 可选/高级（D3D12 别名堆直译） |
| 13 | 稀疏/虚拟纹理 | `SparseTextureProperties`、`SparseUpdate`（`:196`）、`SetTextureVirtual`（`wiResourceManager.h:40`） | **规模不匹配**（WE 面向大世界流送） |
| 14 | 光线追踪 | `RaytracingAccelerationStructure`/`RaytracingPipelineState`/`ShaderTable`/`DispatchRays`（`wiGraphics.h:1188-1368`） | 视目标而定（WE 有 DXR 路径） |
| 15 | 视频解码 | `VideoDecoder`/`VideoDecodeOperation`（`:921-931`）、`QUEUE_VIDEO_DECODE` | **规模不匹配**（应砍） |

**对照约束原则③【实测+判断】**：WE 的 RHI **没有**把 `ID3D12*` 类型泄漏到场景/材质头文件（`wiScene.h`/`wiScene_Components.h` 内只出现 `wi::graphics::`），但把 **bindless 描述符 `int`、`SubresourceType`、`Usage`、`BindFlag/ResourceMiscFlag`、`Format`、`ResourceState`** 泄漏到了场景层。前者是“必要物理事实”，后者（把描述符索引存进组件字段）是典型**抽象泄漏**。

---

## 5. GPU 资源生命周期

### 5.1 资源句柄语义【实测】
- `GPUResource`（`wiGraphics.h:862-891`）：`wi::allocator::shared_ptr<void> internal_state`（`:864`）+ `IsValid()`（`:865`）；**禁用 `new/delete`、只允许 placement new**（`:884-890`，注释 “virtual table is not used”）。→ 句柄是值类型，拷贝即引用计数。
- `Texture`/`GPUBuffer`（`:893-919`）在句柄上再加 `desc` 与映射信息。

### 5.2 延迟销毁【实测】
`wiGraphicsDevice_DX12.h:565-574` 有 10 个延迟销毁队列（元素 `std::pair<ComPtr<...>, uint64_t frame>`）：`destroyer_allocations/resources/queryheaps/pipelines/rootSignatures/stateobjects/video_decoder_heaps/video_decoders/bindless_res/bindless_sam`。
`~AllocationHandler()`（`:576-579`）调用 `Update(~0ull, 0)` 清空全部。
`AllocationHandler::Update(FRAMECOUNT, BUFFERCOUNT)`（`:581-638`）判据为「入队帧号 + BUFFERCOUNT < 当前帧号」即释放（析构 `ComPtr`，注释 “comptr auto delete”），bindless 索引队列额外把索引推回 `free_bindless_res/free_bindless_sam`（`:626-637`）。
→ **【实测】采用“帧计数 + 双缓冲深度”而非逐资源 fence；上层不需要知道，也无法影响**。

### 5.3 上传/回读与 ring buffer【实测】
- `wiGraphicsDevice.h:331-341` `GPULinearAllocator{buffer, offset, alignment, reset()}` + `GetFrameAllocator(cmd)`；`AllocateGPU()`（`:355-389`）注释 “only alive for one frame”，容量不足时按 `align((old_size + dataSize) * 2, alignment)` **几何增长**新建 buffer（即 ring/linear bump 分配器）。
- `UpdateBuffer()`（`:391-404`）注释 “Updates a Usage::DEFAULT buffer data / Since it uses a GPU Copy operation, appropriate synchronization is expected”；`BindDynamicConstantBuffer<T>()`（`:406-415`）注释 “done on the CPU to an UPLOAD buffer… only visible on the command list it was bound to”。
- 回读：`Scene::textureStreamingFeedbackBuffer_readback[BUFFERCOUNT]` 与 `textureStreamingFeedbackMapped`（`wiScene.h:149-150`）；`CopyBufferAsync`（`wiGraphicsDevice.h:198-200`）注释 “submitted immediately to a GPU COPY QUEUE… so this can straddle frame boundaries”。

### 5.4 上传流程【实测+待补】
`Scene::Update` 里对每个上传缓冲发出 `command.src = &instanceUploadBuffer[cpu_gpu_mapped_resource_index]`、`command.dst = &instanceBuffer[...]`（`wiScene.cpp:1090,1097,1104`），随后 `device->CopyBufferAsync(async_buffer_uploads.data(), size, "Scene async buffer updates")`（`:1127`）——**提交到 COPY 队列、不阻塞当前 command list**（`wiGraphicsDevice.h:198-200` 注释："submitted immediately to a GPU COPY QUEUE independently from the SubmitCommandLists, so this can straddle frame boundaries"）。注释原文（`:1084`）："Note: with the CopyBufferAsync we also have to double buffer the dedicated GPU memory buffers as the async copy will be kicked with frame overlap"——**双缓冲 `[BUFFERCOUNT]` 正是为配合跨帧异步拷贝**。
**【未确认】** `cpu_gpu_mapped_resource_index` 的赋值/翻转点本次未定位（读写成该变量的行即可确认）。

---

## 6. 资产系统（wiResourceManager）

### 6.1 接口【实测】
- `wi::Resource`（`wiResourceManager.h:20-50`）：`wi::allocator::shared_ptr<void> internal_state`（`:22`）+ `GetTexture()/GetSound()/GetScript()/GetVideo()`（`:26-31`）+ `SetOutdated()`（`:45`）+ `SetTextureVirtual(...)`（`:40`，注释 “for virtual texture, bind additional resources to have same lifetime”）。
- `resourcemanager::Load(name, flags, filedata, filesize, container_filename, container_fileoffset)`（`:84-98`）；`flags` 含 `IMPORT_NORMALMAP/IMPORT_BLOCK_COMPRESSED/IMPORT_DELAY/STREAMING`（`:72-82`）——**导入策略由调用方通过 flags 决定**。
- 全局更新入口：`UpdateStreamingResources(float dt)`（`:114-116`，注释 “call it once per frame on the main thread / Launching or finalizing background streaming jobs is attempted here”）、`CheckResourcesOutdated()`、`ReloadOutdatedResources()`（`:118-122`）。

### 6.2 缓存与引用计数【实测】
`wiResourceManager.cpp:236` `static wi::unordered_map<std::string, wi::allocator::weak_ptr<ResourceInternal>> resources;`——**缓存键是路径字符串，值是 `weak_ptr`**：资源是否存活由使用方持有的 `Resource`（shared_ptr）决定，缓存不阻止回收。`Load()` 起点 `:1036`，命中查询 `:1150-1151`，未命中则 `resources.erase`（`:1286`）。
`Load()` 内部机制【实测】`wiResourceManager.cpp:1036-1144`：加锁后取 `resources[name]`（weak_ptr）并 `lock()`；用 `wi::helper::FileTimestamp(container_filename 或 name)` 取时间戳（`:1045-1057`）；**仅当 `resource == nullptr || resource->timestamp < timestamp` 才新建 `make_shared<ResourceInternal>` 写回缓存**（`:1059-1085`）——同一路径 + 文件未变时返回**同一个 shared_ptr**，日志 "\tResource reused: %s"（`:1094-1101`）；若调用方本次不带 `IMPORT_DELAY` 而缓存项带，则清标志继续加载（`:1088-1093`）；无内存数据则 `FileRead(container_filename, ..., container_fileoffset)`（`:1105-1117`）；`IMPORT_DELAY` 只登记不加载（`:1119-1121`），否则 `LoadResourceDirectly(...)`（`:1123-1131`）。→ 失效判据是**文件时间戳**（另有手动 `Resource::SetOutdated()`，`wiResourceManager.h:45`）。

### 6.3 后台加载与流送【实测】
`wiResourceManager.cpp:1167` `wi::jobsystem::context streaming_ctx;`、`:1295-1296` `streaming_ctx.priority = wi::jobsystem::Priority::Streaming; wi::jobsystem::Execute(streaming_ctx, ...)`、`:1256` `if (wi::jobsystem::IsBusy(streaming_ctx))`。→ 加载/流送走 WE 自己的 `jobsystem`，不是独立线程池。

### 6.4 延迟 MIP 生成【实测】
- **`AddDeferredMIPGen` 不在 `wiTextureHelper.h`**（该头 90 行只有 `Initialize()`、若干默认纹理 getter、`CreateTexture/CreateGradientTexture/...`，`:8-86`）；它定义在 `wiRenderer.cpp:19360` `void AddDeferredMIPGen(const Texture& texture, bool preserve_coverage)`。
- 唯一调用点：`wiResourceManager.cpp:942` `wi::renderer::AddDeferredMIPGen(resource->texture, true);`。→ **资产层调用渲染层函数**，是“资产↔渲染”之间的一处反向依赖（对原则③有意义）。
- 底层设施：`wiGraphicsDevice.h:426-437` `CreateMipgenSubresources(Texture&)` 注释 “each number directly refers to either SRV or UAV for the same numbered mip level”，即 **MIP 生成靠 UAV 逐级下发**。
调度机制【实测】`wiRenderer.cpp:191-193`：`wi::SpinLock deferredMIPGenLock` + `wi::vector<std::pair<Texture,bool>> deferredMIPGens` + `wi::vector<std::pair<Texture,Texture>> deferredBCQueue`；`AddDeferredMIPGen`（`:19360`）与 `AddDeferredBlockCompression`（`:19366-19371`）只**入队**；真正执行在每帧渲染函数 `ProcessDeferredTextureRequests(CommandList cmd)`（`:3531`）末尾（`:3603-3616`）：逐项 `GenerateMipChain(it.first, MIPGENFILTER_LINEAR, cmd, mipopt)`（`mipopt.preserve_coverage = it.second`）与 `BlockCompress(it.first, it.second, cmd)`，随后 `clear()`。
BC 压缩触发条件【实测】`wiResourceManager.cpp:944-982`：`IMPORT_BLOCK_COMPRESSED` 时按用途选 `bc_format`（普通 BC1/BC3；`IMPORT_NORMALMAP` 强制 `BC5_UNORM` + swizzle R/G/ONE/ONE，`:952-956`），按 block size 对齐宽高重建纹理（`:960-965`），最后 `wi::renderer::AddDeferredBlockCompression(uncompressed_src, resource->texture)`（`:981`）。→ **资产层只登记意图，GPU 工作推迟到渲染层每帧入口**（不在加载线程上录 GPU 命令）。

### 6.5 模型格式与导入【实测】
- **运行时只读序列化场景**：`LoadModel2(Scene&, fileName, ...)`（`wiScene.cpp:9230-9237`）开头即 `wi::Archive archive(fileName, true); if (!archive.IsOpen()) return; scene.Serialize(archive);`——运行时消费 `.wiscene`（版本历史见 `WickedEngine/ArchiveVersionHistory.txt`）。
- **格式导入器全在编辑器，不在引擎目录**：`Editor/ModelImporter.h:3-7` 声明 `ImportModel_OBJ/GLTF/FBX/PLY` 与 `ExportModel_GLTF`，实现在 `Editor/ModelImporter_{OBJ,GLTF,FBX,PLY}.cpp`；调用点 `Editor/Editor.cpp:5665,5669,5673,5677`（按扩展名分派）。支持类型清单：`Editor/Editor.cpp:1179` `params.description = ".wiscene, .obj, .gltf, .glb, .vrm, .fbx, .ply, .lua, .mp4, .png, ..."`。
- 资产类型表：`wiResourceManager.cpp:248-273`（IMAGE: JPG/JPEG/PNG/BMP/DDS/TGA/HDR/HEIC/HEIF；SOUND: WAV/OGG；SCRIPT: LUA；VIDEO: MP4/H264；FONT: TTF），`GetSupported*Extensions()`（`:274-300`）由该表派生。
→ **架构含义【判断】**：引擎运行时不链接 glTF/FBX 解析库，格式适配成本被隔离到工具链——这是原则③的正面范例（场景/资产层、导入器、渲染路径三者解耦）。TooiyumeRender 可直接照搬「离线/编辑器导入 + 运行时读自有格式」这条边界。

---

## 7. 场景/材质的「版本/脏」机制

【实测】- 序列化层版本：`Scene` 注册每个组件管理器时带 version（`wiScene.h:35-68`，如 materials version=11、meshes version=4、objects version=4），`ComponentLibrary::GetVersion(name)`（`wiECS.h:748`）供反序列化做兼容分支。
【实测】- 材质脏：`MaterialComponent::SetDirty()/IsDirty()`（`wiScene_Components.h:280-281`），所有 setter 自动 `SetDirty()`（`:309-383`）。
【实测】- 资源脏：`wi::Resource::SetOutdated()`（`wiResourceManager.h:45`）+ `MaterialComponent::CreateRenderData(force_recreate=true)` 时逐槽 `resource.SetOutdated()`（`wiScene_Components.cpp:506-516`）。
【实测】- 加速结构脏：`MeshComponent::BLAS_state`（`wiScene_Components.h:786-792`：`NEEDS_REBUILD/NEEDS_REFIT/COMPLETE`）与 `Scene::acceleration_structure_update_requested`（`wiScene.h:93-95`）。
【实测】- 光照贴图请求：`Scene::lightmap_request_allocator`（`wiScene.h:267`）与 `IsLightmapUpdateRequested()`（`:96`）。
消费方【实测】`Scene::Update`（`wiScene.cpp:34`）按序执行 `RunMeshUpdateSystem`（定义 `:4106`，调用 `:414`）、`RunMaterialUpdateSystem`（`:4298`，调用 `:418`）、`RunObjectUpdateSystem`（`:4558`，调用 `:434`），均以 `wi::jobsystem::Dispatch` 并行：
- 材质：`if (material.IsDirty()) material.SetDirty(false);`（`:4337-4340`）——脏标志只作「本帧是否需要刷新」的方向性提示，**不做逐字段 diff**；随后**无条件** `material.WriteShaderMaterial(materialArrayMapped + args.jobIndex)`（`:4352`）全量重写 GPU 材质数组；`sampler_descriptor < 0` 时取全局默认 sampler 描述符（`:4343-4350`）。
- 网格/BLAS：依材质状态改 BLAS 几何 flags（alpha test / 透明 / 不投影 → 去掉 `FLAG_OPAQUE`，`:4266-4273`），变化则 `mesh.BLAS_state = BLAS_STATE_NEEDS_REBUILD`（`:4276`）；有 streamout（动态顶点）时不重建而置 `BLAS_STATE_NEEDS_REFIT`（`:4278-4287`）并改绑 `streamoutBuffer` + `so_pos.offset`。
- 网格数据变化时重建 GPU 缓冲：`mesh.CreateRenderData()`（`:1815, 1874, 1954`）。
→ **【判断】** WE 的脏机制是“提示”而非精确失效：材质常量每帧全量重写（对该规模 CPU 开销可忽略），只有网格/BLAS 用显式状态机；PSO 完全不参与脏机制（全变体预热，见 §3.2）。

---

## 8. 3DGS（Neural Rendering）如何接入

【实测】- 独立类型 `wi::GaussianSplatModel`（`wiGaussianSplatModel.h:15-47`）：CPU 端 `positions/rotations/scales/opacities/f_dc/f_rest`（`:18-23`），非序列化端 `aabb_rest/aabb/transform/transform_inverse/maxScale`（`:26-30`）+ **自己的 GPU 缓冲** `wi::graphics::GPUBuffer buffer`（`:31`）+ `subresource_splatBuffer/subresource_shBuffer`（`:32-33`）；接口 `CreateRenderData()/Update(matrix)/Serialize(...)`（`:40-44`）。
【实测】- `wi::GaussianSplatScene`（`:50-67`）统一排序：`model_capacity/splat_capacity` 与 5 个 `GPUBuffer`：`modelBuffer/indirectBuffer/sortedIndexBuffer/distanceBuffer/splatLookupBuffer`（`:55-59`）；`MakeReservations()`（`:63`）、`UpdateGPU(models, count, cmd, viewmatrices, camera_count)  // culling and sorting. Multiple cameras can be provided...`（`:64`）、`Draw(cmd, camera_count)  // will be drawn with culling and sorting based on previous call to UpdateGPU`（`:65`）。
【实测】- 接入 Scene 的方式：`Scene` 注册 `ComponentManager<GaussianSplatComponent>& gaussian_splats`（`wiScene.h:68`，**无 version 号**）与 `wi::GaussianSplatScene gaussian_scene`；`wiScene.cpp:5405-5419` 遍历 `gaussian_splats`，逐实体取 `matrix` → `gaussian_splats[i].Update(matrix)` → `gaussian_scene.MakeReservations(gaussian_splats.GetData(), gaussian_splats.GetCount())`。
→ **【实测结论】3DGS 是独立模块**：自带 GPU 缓冲与排序管线，只通过“组件数组 + 一个 Scene 级 Gauss 场景对象”接入，**没有进入 `MeshComponent` 体系**（不共享 `generalBuffer`/`BufferView`/`Subset`），也不要求 RHI 新增概念（只用 `GPUBuffer` + `CommandList`）。
GPU 侧细节【实测】（`wiGaussianSplatModel.cpp`，515 行本体）：
- **自持 shader 与 PSO**：`static Shader computeShader/computeShader_indirect/vertexShader/pixelShader`（`:18-20`）、`static BlendState/RasterizerState/DepthStencilState`（`:21-23`）、`static PipelineState pipelineState`（`:24`）；`Initialize()`（`:204-247`）用 `wi::renderer::LoadShader(CS/VS/PS, ..., "gaussian_splatCS.cso")` 等 4 个（`:209-212`），再 `device->CreatePipelineState(&desc, &pipelineState)`（`:236-243`），状态为 `pt = TRIANGLESTRIP`、alpha 混合 `ONE/INV_SRC_ALPHA`、depth `GREATER` + `ZERO` 写（`:219-233`）。
- 每模型的 splat + 球谐数据打进**一个** `GPUBuffer buffer`，两个 SRV subresource（`subresource_splatBuffer`/`subresource_shBuffer`，`:158-162`），按 SH 阶数定每 splat 结构大小（`:42-45`）。
- 每帧 `GaussianSplatScene::UpdateGPU`（`:350-490`）用 `device->AllocateGPU(sizeof(ShaderGaussianSplatModel) * model_count, cmd)`（`:358`）写模型数组，逐模型 `GetDescriptorIndex(&model.buffer, SubresourceType::SRV, subresource_splatBuffer/shBuffer)`（`:364-365`），做 culling + 排序。
- 绘制 `GaussianSplatScene::Draw(cmd, camera_count)`（`:493-505`）：`BindPipelineState(&pipelineState)` → `BindResource(&sortedIndexBuffer, 0)/(&splatLookupBuffer, 1)/(&modelBuffer, 2)` → `PushConstants(&camera_count, ...)` → `DrawInstancedIndirect(&indirectBuffer, 0, cmd)`。
- shader 侧结构 `ShaderGaussianSplatModel` 在 `shaders/ShaderInterop_GaussianSplat.h`（`:2` include），**不属于 `ShaderInterop_Renderer.h`**。
→ 结论更强：3DGS 复用了 RHI（`GPUBuffer`/`CommandList`/`PushConstants`/`DrawInstancedIndirect`）与渲染器的 shader 加载函数，但 **wiGraphics 没有为它新增或修改任何类型**。

### 对照约束原则⑥【判断】
WE 的做法**印证**了原则⑥的可行性：Neural（3DGS）先以独立数据类型 + 独立缓冲 + 自主排序/绘制落地，**没有强耦合进 `wiGraphics` 的核心抽象**（`wiGraphics.h` 里找不到任何 gaussian 相关类型）。代价是 Scene 需要多持有一个 `GaussianSplatScene`，以及排序/绘制入口要与主渲染路径协调（谁调用 `UpdateGPU` —— 属渲染路径职责）。

---

## 9. 规模适用性：TooiyumeRender 的「最小可用抽象边界」

前提：单 D3D12 后端、约 1-3 万行、教学为主、已有 Deferred 管线。WE 的 `wiGraphics` 是**多后端**抽象（有 `wiGraphicsDevice_Vulkan.cpp` 344570 B / `wiGraphicsDevice_Metal.cpp` 217763 B / `wiGraphicsDevice_DX12.cpp` 291539 B）。

### 9.1 必须保留（否则高层无法表达 GPU 语义）
1. 资源句柄 + 描述结构：`GPUResource/GPUBuffer/Texture` + `desc`（`wiGraphics.h:862-919`）；
2. 资源状态与屏障：`ResourceState` + `Barrier()`（`:458-487`、`wiGraphicsDevice.h:256`）——原则⑤要求同步关系可见，**不可隐藏**；
3. 上传/回读语义：`Usage{UPLOAD,DEFAULT,READBACK}` + `mapped_data`（`:160`、`:868`）；
4. 帧与队列：`BeginCommandList/SubmitCommandLists/WaitForGPU/GetFrameCount/GetBufferCount/GetBufferIndex`（`wiGraphicsDevice.h:122-147`）；
5. 视图/子资源：`CreateSubresource` + `GetDescriptorIndex`（`:104-110`）——即“描述符”概念本身；
6. 管线与着色器：`Shader/PipelineStateDesc/PipelineState` + `CreateShader/CreatePipelineState`（`:95,99`）；
7. 绑定与绘制命令全集（`:215-270`）。

### 9.2 可砍（WE 面向多后端/大世界才需要）
| 可砍项 | WE 位置 | 理由 |
| --- | --- | --- |
| Vulkan/Metal 后端与 `ShaderFormat` 跨平台转译 | `wiGraphicsDevice_Vulkan.cpp` / `Metal.cpp` / `ShaderFormat`（`wiGraphics.h:57`） | 单后端不需要 |
| 视频解码全套（`VideoDecoder/VideoDecodeOperation/QUEUE_VIDEO_DECODE`） | `wiGraphics.h:921-931`、`wiGraphicsDevice.h:59-67` | 与渲染无关 |
| 稀疏纹理/虚拟纹理（`SparseTextureProperties/SparseUpdate/SetTextureVirtual`） | `wiGraphics.h:196,809`、`wiResourceManager.h:40` | 教学项目无流送需求 |
| 内存别名 `AliasingAllocator` | `wiGraphicsDevice.h:449-505` | 高级优化，可后续再加 |
| `RenderPass` 兼容层（已 deprecated） | `wiGraphics.h:2098-2300` | WE 自己都在淘汰 |
| 500000 绑定量级的 bindless 表 | `wiGraphicsDevice.h:44` | 教学项目可降到几千，或改用 per-material CB |
| Video/更多 `ResourceMiscFlag`/`TileRangeFlags` | `wiGraphics.h:400,1384` | 按需 |
| 光线追踪（若近期不做 DXR 路径） | `wiGraphics.h:1188-1368` | 可先留空接口 |

### 9.3 建议的抽象分层【判断】
- **RHI 层**：只暴露 9.1 的 7 项，类型名可保留 `wi::graphics` 风格但**不要保留“多后端”的间接层**（如 `ShaderFormat` 转译、`AdapterType`）；
- **场景层**：`MeshComponent` 可以持有 GPU 句柄（WE 证明这在大规模下可行），但**不应持有裸 `int` 描述符索引**——建议包成 `struct TextureHandle{...}`/`GeometryView`，把“索引”这个实现细节收进 RHI（WE 的泄漏点：`wiScene_Components.h:255-256,267,737-738`）；
- **材质层**：材质→PSO 的 key 值得学（`ObjectRenderingVariant`，`wiRenderer.cpp:348-362`），但 key 里不应出现 `renderpass`（那是渲染路径的知识，属原则②边界）；WE 把它放进去正是“材质与渲染路径耦合”的证据；
- **资产层**：`Resource` 句柄 + `weak_ptr` 缓存（`wiResourceManager.cpp:236`）可直接借鉴；`AddDeferredMIPGen` 这类**资产层反向调用渲染层**的设计应改为“资产层提供纹理，渲染层自行决定何时生成 MIP”。

### 9.4 WE 设计逐条适用性判断汇总（必需 / 可选 / 规模不匹配）【判断】

| WE 设计 | 位置 | 判断 | 理由与代价 |
| --- | --- | --- | --- |
| ECS：`ComponentManager<T>` 平行数组 + 桶哈希 lookup，组件用 `Entity` 互引 | `wiECS.h:147-705, 616-671` | **必需** | 数据局部性好、可序列化；替代方案（对象指针图）会破坏序列化与批量遍历。代价：`Remove` 打乱顺序、需 `Remove_KeepSorted` |
| Scene 持有全场景 GPU 数组（instance/geometry/material/skinning + bindless 描述符） | `wiScene.h:117-167` | **必需（但需瘦身）** | 这是「一次上传、shader 靠索引取」的核心；但把 SurfelGI/DDGI/VXGI/Impostor/Ocean 的 GPU 资源也塞进 Scene（`wiScene.h:170-294`）是规模不匹配，应归渲染模块 |
| 组件直接存裸 `int` 描述符索引 | `wiScene_Components.h:255-256, 267, 737-738` | **规模不匹配 / 应封装** | 把 DX12 描述符表语义泄漏进 ECS；教学项目应包成 `TextureHandle`/`GeometryView`，让 RHI 管索引 |
| 全局几何池 suballocation（一个 `generalBuffer` + offset 视图） | `wiScene_Components.cpp:1217-1223`、`wiRenderer.cpp:3385-3394` | **可选** | 收益是少绑 IB、纯 offset 绘制；代价是 offset 计算复杂（`indexOffset` 三重来源）。1-3 万行教学项目可先用「每 mesh 独立 buffer」，保留 `BufferView` 抽象以便后续优化 |
| 材质打包成 384 B 半精度 `ShaderMaterial` + bindless 数组 | `shaders/ShaderInterop_Renderer.h:393-513`、`wiScene.cpp:4352` | **可选（推荐学索引化）** | 省显存与带宽；代价是 HLSL/C++ 双份打包逻辑必须同步（`static_assert` 兜底）。教学项目可先用显式 `float4` 常量缓冲，但「材质数组 + 索引」比逐材质 CB 更易批处理 |
| PSO 变体 key：32 位位域 union + 二维缓存 | `wiRenderer.cpp:348-368, 3310-3323` | **必需（key 里的 renderpass 除外）** | 位域 key 使 PSO 查找 O(1) 且无需字符串哈希；但 `renderpass:4` 让材质/网格知识反向依赖渲染路径，违反原则②，TooiyumeRender 应把 pass 从 key 中移出（每个 RenderPath 自持 PSO 缓存） |
| 全变体后台预热 PSO | `wiRenderer.cpp:1945-2224, 3333-3334` | **规模不匹配** | WE 变体空间是 `pass × 12 shadertype × 2 mesh_shader × blend × cull × tess × alphatest × 4 msaa`（数千 PSO），启动编译量巨大，故放后台并允许跳帧；教学项目变体少，改为按需创建更简单 |
| root signature 内嵌 HLSL（`CreateShader` 反序列化） | `wiGraphicsDevice_DX12.cpp:3899-3916, 4102-4189` | **必需（D3D12 事实）** | 省去 RHI 暴露 root signature 概念；代价是 shader 必须自带 `[RootSignature]`，教学时不如显式 `D3D12_ROOT_SIGNATURE_DESC` 直观，可二选一但不要两套并存 |
| 帧计数延迟销毁（`destroyer_*` 队列 + `frame + BUFFERCOUNT < FRAMECOUNT`） | `wiGraphicsDevice_DX12.h:565-638` | **必需** | 直接落实原则⑤：释放由 RHI 统一延后，上层无需知道 fence；代价是资源占用多存活 1-2 帧 |
| 帧内线性分配器 `AllocateGPU` + 几何增长 | `wiGraphicsDevice.h:331-389` | **必需** | 每帧临时常量/结构（如 `ShaderGaussianSplatModel` 数组）必须有廉价落点；替代方案是每帧重建 buffer（更慢更碎） |
| `CopyBufferAsync`（独立 COPY 队列、跨帧） | `wiGraphicsDevice.h:198-200`、`wiScene.cpp:1127` | **可选** | 减少与图形队列争用，但把「上传何时可见」变成跨帧问题（故需双缓冲）；单后端教学项目可先在同队列 copy + fence，代码更直白 |
| 双缓冲 `BUFFERCOUNT = 2`（`GetBufferIndex()`） | `wiGraphicsDevice.h:72, 145-147`、`wiScene.h:90,126-160` | **必需** | CPU 写一帧、GPU 读一帧的基本安全边界，是所有动态上传的前提 |
| 资产缓存：路径键 + `weak_ptr` + 文件时间戳失效 | `wiResourceManager.cpp:236, 1036-1144` | **必需（低成本高收益）** | 引用计数由 `Resource` 的 shared_ptr 承担，缓存不阻止回收；替代方案（强引用缓存）会导致资源永不释放 |
| 延迟 MIP/BC：资产层登记意图、渲染层每帧执行 | `wiRenderer.cpp:191-193, 3531-3616`、`wiResourceManager.cpp:942, 981` | **必需（架构示范）** | 避免在加载线程录 GPU 命令；代价是资产层反向依赖 `wi::renderer`（本次调查发现的唯一反向依赖，可用回调/队列接口消除） |
| 模型格式（OBJ/GLTF/FBX/PLY/VRM）只由编辑器导入，运行时读自有序列化格式 | `Editor/ModelImporter.h:3-7`、`wiScene.cpp:9230-9237` | **必需（强烈建议照搬）** | 引擎不链接格式库，导入成本隔离到工具链；替代方案（运行时直接读 glTF）会把第三方解析器引入引擎核心 |
| 3DGS：独立类型 + 自持 PSO/共享 shader 加载，只经 Scene 组件数组接入 | `wiGaussianSplatModel.h:15-67`、`wiGaussianSplatModel.cpp:18-24, 236-243, 493-505` | **必需（印证原则⑥）** | 证明新渲染范式可以只复用 RHI 原语（`GPUBuffer/CommandList/PushConstants/DrawInstancedIndirect`）而不改 RHI；代价是需一个渲染侧入口来驱动 `UpdateGPU/Draw` |
| 多后端抽象（Vulkan/Metal + `ShaderFormat` 跨平台转译） | `wiGraphicsDevice_Vulkan.cpp`、`wiGraphicsDevice_Metal.cpp`、`wiGraphics.h:57` | **规模不匹配** | 单 D3D12 后端不需要；保留会迫使每个新特性都过「跨后端一致性」审查，直接阻碍教学与迭代 |
| 视频解码 / 稀疏虚拟纹理 / 内存别名 / deprecated RenderPass 兼容层 | `wiGraphics.h:921-931, 809, 1384, 2098-2300`；`wiGraphicsDevice.h:449-505` | **规模不匹配（应砍）** | 与当前目标（Deferred + 场景/材质/RHI 解耦）无关，属原则「迁移完成前禁止大规模新增无关特效」的对照面 |
| `Visibility` / `visibleObjects` 由渲染器持有，场景不参与可见性决策 | `wiRenderer.h:150, 174`、`wiRenderer.cpp:3727-3991, 5849-5853` | **必需** | 场景只提供数据、渲染路径决定可见性，这是原则①③的现成分界线，可直接作为 TooiyumeRender 的目标边界 |

---

## 附录 A 本次证据文件与行数（实测）
| 文件 | 行数 |
| --- | --- |
| `wiScene.h` | 711 |
| `wiScene_Components.h` | 2780 |
| `wiECS.h` | 919 |
| `wiGraphics.h` | 2340 |
| `wiGraphicsDevice.h` | 545 |
| `wiGraphicsDevice_DX12.h` | 646 |
| `wiGraphicsDevice_DX12.cpp` | 7858 |
| `wiResourceManager.h` | 153 |
| `wiResourceManager.cpp` | 1667 |
| `wiTextureHelper.h` | 90 |
| `wiGaussianSplatModel.h` | 68 |
| `wiGaussianSplatModel.cpp` | 517 |
| `wiGPUBVH.h` | 28 |
| `wiRenderer.h` | 1468 |
| `wiRenderer.cpp` | 19686 |

> 状态：**主体完成**（第 0-9 节均有实测证据；仅 §1.4、§5.4 各留 1 条【未确认】）。全程遵守「第一阶段只调查、不改运行时代码」：只读 WE 源码与 TooiyumeRender 文档，仅写入本报告与配套调查日志。
> 未读清单与实测/推断区分见配套日志 `docs/AIWorkLogs/01C_WE_场景材质与RHI解耦_调查日志.md`。