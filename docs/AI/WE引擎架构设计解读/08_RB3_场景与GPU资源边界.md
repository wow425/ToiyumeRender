# 08 RB3 场景系统与 GPU 资源边界

> 研究块 RB3（`07_WE研究实施方案.md` §7.3），服务 WP1、WP3。研究者：rb3-scene。
> 标注：`[实测]` = 命令输出中直接看到该行；`[推断]` = 由已读代码推理、未逐行验证。
> WE 路径前缀省略：`...\WickedEngine-master\WickedEngine\`；本项目前缀：`...\TooiyumeRender\Engine\`。

**核心问题**：WE 把「场景数据 / GPU 资源 / 描述符」的边界划在哪？为什么？本项目（1-3 万行、单 D3D12、教学为主）该划在哪？

## 0. 结论速览

1. **WE 的边界划在「组件 struct 内部」，而不是「模块之间」**：同一个 `MeshComponent` 里既有 CPU 顶点数组（`:673-685`）又有 `GPUBuffer generalBuffer`（`:728`）和裸 `int descriptor_srv`（`:737`）`[实测]`。所以「场景层不碰 GPU」在 WE 里**根本不成立**——它的分界线是「不出现 `ID3D12*`」，而非「不出现 GPU 概念」`[实测：wiScene.h/wiScene_Components.h 全文无 D3D12 类型；01C 已论证]`。
2. WE 真正干净的那条线在 **CPU 侧逐帧结构（instance/geometry/material 三数组）与 shader 之间**：Scene 只负责把 `Shader*{}` 结构 memcpy 进 mapped 指针，shader 用 bindless 索引取`[实测 wiScene.cpp:4237/4352/4784 + globals.hlsli:586-589]`。
3. **场景层完全不录命令、不设 barrier**：`grep -c Barrier wiScene.cpp` = **0**，`wiScene_Components.cpp` = **0**`[实测]`。这是 WE 最值得照搬的一条边界。
4. 可见性不属于 Scene：`visibleObjects` 由 `wi::renderer::UpdateVisibility` 写（`wiRenderer.cpp:3619,3727,3805,3991`）、渲染器读（`:5853,7223`），Scene 只提供 `aabb_objects`/`matrix_objects``[实测]`。
5. **本项目建议的划法**（详见 §7）：Scene 持 `Mesh`（CPU 侧几何 + 偏移量 + 材质槽索引）、`Material` 纯参数、`AABB`/`matrix` 流、`revision`；**不持** `GPUBuffer`、**不持** `D3D12_GPU_VIRTUAL_ADDRESS`、**不持** 描述符堆偏移、**不持** PSO。GPU 侧三数组（instance/geometry/material）放 `RenderFeatures`，理由是它天然按 pass 变（本项目 3 个 pass，WE 是 12 个 shadertype × 16 个 renderpass）。

## §1 职责边界（负责什么 / 不负责什么）

### 1.1 WE 的 Scene 负责什么`[实测]`

| 职责 | 证据 |
|---|---|
| 组件存储与生命周期（增删查、序列化、合并、实例化） | `wiScene.h:29` `ComponentLibrary componentLibrary`；`:33-68` 约 40 个 `ComponentManager<T>&`；`wiECS.h:300-344` Create/Remove |
| 每帧推进各子系统（物理/动画/变换/层级/网格/材质/对象/相机/光照…） | `wiScene.cpp:34` `Scene::Update(float dt)`；调用点 `:48,293,297,414,418,434,436,444` |
| 产出「shader 可见的场景常量」 | `wiScene.h:118` `ShaderScene shaderscene`；填充 `wiScene.cpp:927-938` |
| 持有并维护每帧上传三件套 | `wiScene.h:126-147`；写入 `wiScene.cpp:4237,4352,4784`；提交 `:1127` |
| 提供剔除用的 CPU 侧数据（AABB 流、世界矩阵流） | `wiScene.h:107-111`；填充 `wiScene.cpp:4574-4576`(`aabb_objects`)、`:4746-4749`(`matrix_objects`) |
| 持有少量 Scene 级 GPU 资源（TLAS、cloudmap、query heap、GI 缓冲） | `wiScene.h:87,90,164`；`:173-294` 的 SurfelGI/DDGI/VXGI/Ocean 子结构 |

### 1.2 WE 的 Scene 不负责什么`[实测]`

| 不负责 | 证据 |
|---|---|
| 不决定可见性 | `visibleObjects` 定义在 `wiRenderer.h:150`，由 `wiRenderer.cpp:3619` `UpdateVisibility` 填充；Scene 无此成员 |
| 不录制命令、不设 barrier | `grep -c Barrier` 两个场景 cpp 均为 0`[实测]` |
| 不创建 PSO / 不做 shader 变体解析 | `PSO_object` 在 `wiRenderer.cpp:366`；变体 key `ObjectRenderingVariant` 在 `:348-362` |
| 不做纹理格式导入 | `MaterialComponent::CreateRenderData` 只调 `resourcemanager::Load`（`wiScene_Components.cpp:504-522`） |
| 不持有渲染目标 | CameraComponent 的 RT 只出现在 `RenderToTexture`（`:1531-1544`）这一「可选的相机自渲染」场景，主路径 RT 在 RenderPath |

### 1.3 边界的真实画法（一句话）`[实测+推断]`

> **WE 的边界 = 「场景层可以持有 GPU 资源的*句柄与索引*，但不得提交命令、不得创建管线、不得决定可见性」。**
> 证据：组件持有 `GPUBuffer`（`:728-729`）、`Texture`（`:1203-1204`）、`int` 描述符索引（`:737,1206,1392,1511-1528`）`[实测]`；但 `Device->CreatePipelineState`/`Barrier`/`Draw*` 在场景层零出现`[实测 grep]`。

## §2 关键类型与数据结构

### 2.0 Scene 容器的组织`[实测]`

```
Scene (wiScene.h:25)
├─ ComponentLibrary componentLibrary            :29
│   └─ unordered_map<string, LibraryEntry>      wiECS.h:717
│       └─ { unique_ptr<ComponentManager_Interface>, version }  wiECS.h:711-715
├─ ComponentManager<MaterialComponent>& materials :35 (version=11)
├─ ComponentManager<MeshComponent>&     meshes    :36 (version=4)
├─ ComponentManager<ObjectComponent>&   objects   :38 (version=4)
├─ ComponentManager<LightComponent>&    lights    :42 (version=5)
├─ ComponentManager<CameraComponent>&   cameras   :43 (version=3)
├─ ComponentManager<TransformComponent>& transforms :33
├─ ComponentManager<HierarchyComponent>& hierarchy :34
└─ 每帧数据：aabb_objects :107 / matrix_objects :110 / shaderscene :118
            instanceUpload+Buffer :126-129 / geometry :137-141 / material :144-147
```

- **按类型成组存储**：`ComponentManager<T>` 内部是两条平行数组 `wi::vector<Component> components`（`wiECS.h:461`，注释 "linear array of alive components"）与 `wi::vector<Entity> entities`（`:463`），外加 `LookupTable lookup`（`:470`，默认桶哈希 `LOOKUP_BUCKET_HASH`，按 `entity>>6`/桶内 `entity&63` 定位）`[实测]`。
- **遍历方式**：`GetCount()`（`:435`）+ `operator[](index)`（`:444/446`）做紧密线性遍历；`GetEntity(index)`（`:439`）取回句柄；`GetData()`（`:456-457`）拿裸数组给 GPU 上传/批量拷贝用`[实测]`。
- **Entity 句柄语义**：`using Entity = uint64_t`（`wiECS.h:25`），`INVALID_ENTITY = 0`（`:26`），`CreateEntity()` 由全局 `std::atomic<Entity>` 自增发号（`:28-32`）——**全局唯一、非索引、永不复用**；组件间用 Entity 互引（`ObjectComponent::meshID` = `wiScene_Components.h:1184`），因此可序列化`[实测]`。
- **删除语义**：`Remove()`（`:324-344`）用 `components[index] = std::move(components.back())` 做 swap-and-pop，**会打乱顺序**；要保序须用 `Remove_KeepSorted()`（`:347-372`，O(n)）`[实测]`。这对「每帧按组件索引直接写 GPU 数组」是前提：索引必须当帧重新算（WE 正是这么做的，见 §4）。

### 2.1 三层归属表（本文核心产出）

归属判据（四个桶，互斥）：

| 桶 | 含义 | 判据 |
|---|---|---|
| **C** 纯 CPU 数据 | 可序列化、与设备无关 | 是否能写进 `.wiscene`（`Serialize` 里出现的字段） |
| **G** GPU 资源句柄 | 持有设备对象（`GPUBuffer`/`Texture`/`BLAS`） | 类型是否 `wi::graphics::*` 的资源类 |
| **D** 描述符索引 | 裸 `int`，指向 bindless 堆槽位 | 是否经 `GetDescriptorIndex` 产出 |
| **R** 渲染缓存 | 每帧由更新系统重算、不入序列化、只服务当帧绘制 | 注释里写 "Non-serialized" 且由 `Run*UpdateSystem` 写 |

#### 2.1.1 MaterialComponent（`wiScene_Components.h:117-402`）

| 字段 | 行 | 归属 | 依据 |
|---|---|---|---|
| `_flags`(DIRTY/CAST_SHADOW/DOUBLE_SIDED/…) | 140 | **C** | 序列化；`IsCastingShadow()` 等纯查询 |
| `shaderType` (SHADERTYPE enum) | 159 | **C**（但消费方在 L4） | 值本身是场景数据；`ObjectRenderingVariant.bits.shadertype` 由渲染器读（`wiRenderer.cpp:3310`）`[实测]` |
| `shaderTypeDefines` | 163-175 | **C**（静态表） | `inline static`，供 shader 编译期 define |
| `engineStencilRef` | 178 | **R** | 每帧由 `RunMaterialUpdateSystem` 依 `IsCustomShader()/IsOutlineEnabled()` 重算（`wiScene.cpp:4306-4326`）`[实测]` |
| `userBlendMode` | 179 | **C** | 序列化；`GetBlendMode()` 有 FILTER_TRANSPARENT 覆盖逻辑（`:289`） |
| `baseColor`…`mesh_blend`（22 个 float/float4） | 181-202 | **C** | 纯参数；全部经 `WriteShaderMaterial` 打包 |
| `sheenColor/sheenRoughness/clearcoat/clearcoatRoughness` | 204-207 | **C** | 同上 |
| `userStencilRef` | 209 | **C** | `SetUserStencilRef` 掩 0x0F（`:271`） |
| `shadingRate` | 210 | **C** | 值可序列化；消费方是 `BindShadingRate`（`wiRenderer.cpp:3365`）`[实测]` |
| `texAnimDirection/texAnimFrameRate` | 212-213 | **C** | UV 动画参数 |
| `texAnimElapsedTime` | 214 | **R** | 每帧累加并回写 `texMulAdd.z/.w`（`wiScene.cpp:4305-4313`）`[实测]` |
| `interiorMappingRotation/Scale/Offset` | 216-218 | **C** | 参数 |
| `TEXTURESLOT` 枚举（15 槽） | 220-239 | **C** | 槽位契约，非数据 |
| `TextureMap::name` | 242 | **C** | 资产路径字符串，序列化 |
| `TextureMap::resource` | 243 | **C→G 的桥** | `wi::Resource` 是引用计数资产句柄；`GetGPUResource()`（`:245-250`）才拿到 `GPUResource*``[实测]` |
| `TextureMap::uvset` | 244 | **C** | 序列化 |
| `TextureMap::lod_clamp` / `virtual_anisotropy` | 253-254 | **C** | 注释 Non-serialized，但**被打包进 shader**（`WriteShaderMaterial` `:404`）`[实测]`，故不是 R |
| `TextureMap::sparse_residencymap_descriptor` | 255 | **D** | 裸 `int`，稀疏纹理驻留图槽位 |
| `TextureMap::sparse_feedbackmap_descriptor` | 256 | **D** | 同上 |
| `textures[TEXTURESLOT_COUNT]`（15 个 TextureMap） | 258 | **C+D 混合** | 见上两行 |
| `customShaderID` | 260 | **C** | 用户句柄；`IsCustomShader()` = `>=0`（`:296`） |
| `userdata` (uint4) | 261 | **C** | 透传 shader |
| `cameraSource` (Entity) | 263 | **C** | 组件间 Entity 引用（取相机 RT 当纹理） |
| `layerMask` | 266 | **R** | 每帧从 `LayerComponent` 拷入（`wiScene.cpp:4307-4310`）`[实测]` |
| `sampler_descriptor` | 267 | **D** | 用户可覆盖的采样器槽位（-1 = 用全局） |
| `cached_wrapSampler` | 268 | **R** | 每帧重算（`wiScene.cpp:4343-4351`），注释 "do not modify, system will write this" |
| `WriteShaderMaterial(ShaderMaterial*)` | 399 | 打包器 | 输出 384B GPU 结构（`wiScene_Components.cpp:282-445`）；**在打包时现场取描述符**：`:418` 取纹理、`:431` 取采样器`[实测]` |
| `CreateRenderData(bool)` | 398 | **G 创建点** | 语义是「按 name 重新 Load 纹理并 `SetOutdated`」（`:504-522`）——**材质自己没有 GPUBuffer**`[实测]` |

**关键发现**：`MaterialComponent` 里**没有任何 `GPUBuffer` 成员**`[实测]`。材质参数的去处是 Scene 级的 `materialBuffer`（`wiScene.h:147`），材质只提供「打包函数」。→ 材质的 GPU 侧完全由 Scene 统一拥有，这是 WE 里最干净的一处分层。

#### 2.1.2 MeshComponent（`wiScene_Components.h:655-1124`）

| 字段 | 行 | 归属 | 依据 |
|---|---|---|---|
| `_flags`(RENDERABLE/DOUBLE_SIDED/DYNAMIC/BVH_ENABLED/QUANTIZED_POSITIONS_DISABLED) | 784 | **C** | `set_flag` 系列（`:787-792`），序列化 |
| `vertex_positions/normals/tangents/uvset_0/uvset_1` | 673-677 | **C** | CPU 几何源数据 |
| `vertex_boneindices/weights`(+2 组) | 678-681 | **C** | 蒙皮权重（CPU） |
| `vertex_atlas/colors/windweights` | 682-684 | **C** | 顶点属性 |
| `indices` | 685 | **C** | CPU 索引 |
| `MeshSubset::surfaceName/materialID/indexOffset/indexCount` | 694-697 | **C** | 子集表；`materialID` 是 Entity 引用 |
| `MeshSubset::materialIndex` | 700 | **R** | 每帧由 `materials.GetIndex(subset.materialID)` 重算（`wiScene.cpp:4213-4221`）`[实测]` |
| `MeshSubset::flags` | 701 | **R** | 每帧依 doubleSided 重算（`wiScene.cpp:4207-4219`） |
| `subsets` | 705 | **C** | 容器 |
| `armatureID` | 707 | **C** | Entity 引用；`IsSkinned()` = `!= INVALID`（`:812`） |
| `tessellationFactor` | 709 | **C** | 参数 |
| `subsets_per_lod` | 710 | **C** | LOD 划分 |
| `MorphTarget::vertex_positions/normals/sparse_indices/weight` | 714-719 | **C** | 形态目标 CPU 数据 |
| `MorphTarget::offset_pos/offset_nor` | 721-722 | **R** | 每帧布局进 `skinningBuffer`（`wiScene.cpp:4228-4231`） |
| `morph_targets` | 724 | **C** | 容器 |
| `aabb` | 727 | **R** | `CreateRenderData` 时由顶点算出（`wiScene_Components.cpp:614-621`），非序列化 |
| **`generalBuffer`** (`GPUBuffer`) | **728** | **G** | 注释 "index buffer + all static vertex buffers"——全场景共享大缓冲的子分配别名 |
| **`streamoutBuffer`** (`GPUBuffer`) | **729** | **G** | "all dynamic vertex buffers"（蒙皮/morph 输出） |
| `generalBufferOffsetAllocation` (PageAllocator::Allocation) | 730 | **G 的元数据** | 记录在全局块中的 `byte_offset`；绘制时算 `indexOffset`（`wiRenderer.cpp:3388`）`[实测]` |
| `generalBufferOffsetAllocationAlias` (`GPUBuffer`) | 731 | **G** | 别名的块 buffer，实际绑定用它（`wiRenderer.cpp:3358`）`[实测]` |
| `BufferView::offset/size` | 734-735 | **G 的视图** | 在 `generalBuffer` 内的字节区间 |
| `BufferView::subresource_srv` | 736 | **D 的前体** | `CreateSubresource` 返回的子资源号（RHI 内部概念） |
| **`BufferView::descriptor_srv`** | **737** | **D** | 裸 `int` bindless 索引，`GetDescriptorIndex` 产出 |
| `BufferView::subresource_uav` / `descriptor_uav` | 738-739 | **D** | 仅 streamout 用（`wiScene_Components.cpp:1335-1337`） |
| `ib_provoke/ib_reorder/ib/vb_pos_wind/vb_nor/vb_tan/vb_uvs/vb_atl/vb_col/vb_bon/vb_mor/vb_clu/vb_bou/so_pos/so_nor/so_tan/so_pre` (17 个 BufferView) | 746-761 | **G+D** | 每个 = 区间 + 描述符；`so_pre` 与 `so_pos` 每帧 swap（`wiScene.cpp:4112-4115`）`[实测]` |
| `geometryOffset` | 763 | **R** | 每帧由 `geometryAllocator.fetch_add` 分配（`wiScene.cpp:270`）`[实测]` |
| `meshletCount` | 764 | **R** | 每帧重算（`wiScene.cpp:4202,4236`） |
| `active_morph_count` / `morphGPUOffset` | 765-766 | **R** | 每帧重算（`wiScene.cpp:4126,4229`） |
| `uv_range_min/max` | 767-768 | **C→GPU 归一化参数** | `CreateRenderData` 算出（`wiScene_Components.cpp:673-694`），每帧进 geometry 数组 |
| **`BLASes`** (`vector<RaytracingAccelerationStructure>`) | **770** | **G** | 每 LOD 一个 BLAS |
| `bvh_leaf_aabbs` / `bvh` | 772-773 | **C**（CPU 侧 BVH，非 GPU） | `BVH_ENABLED` 时才建（`:789`） |
| `cluster_ranges` | 779 | **R** | meshlet 用，`CreateRenderData` 重建 |
| `precomputed_rigidbody_physics_shape` | 781 | **C** | 物理预计算 |
| `BLAS_state` (NEEDS_REBUILD/REFIT/COMPLETE) | 792 | **R 状态机** | `mutable`，见 §6.6 |
| `GetIndexFormat()/GetIndexStride()` | 814-816 | 派生 | 依 `vertex_positions.size()` 决定 16/32 位索引 |
| `DeleteRenderData()/CreateRenderData()` | 835-838 | **G 创建/销毁点** | 实现 `wiScene_Components.cpp:532-557 / 559-1303` |
| `CreateStreamoutRenderData()` | 839 | **G** | `wiScene_Components.cpp:1304-1368` |
| `CreateRaytracingRenderData()` | 840 | **G** | `wiScene_Components.cpp:1369-1427`，入口即置 `BLAS_state = NEEDS_REBUILD`（`:1376`）`[实测]` |

**关键发现**：`MeshComponent` 是三层**全部混在一起**的典型——CPU 顶点数组、`GPUBuffer`、裸描述符索引、每帧缓存四者同居一 struct。这是 WE 最需要在移植时拆开的地方。

#### 2.1.3 ObjectComponent（`wiScene_Components.h:1164-1262`）

| 字段 | 行 | 归属 | 依据 |
|---|---|---|---|
| `_flags`(RENDERABLE/CAST_SHADOW/DYNAMIC/FOREGROUND/NOT_VISIBLE_IN_MAIN_CAMERA/WETMAP_ENABLED…) | 1181 | **C** | 序列化；默认 `RENDERABLE｜CAST_SHADOW` |
| `userStencilRef` | 1183 | **C** | 掩 0x0F（`:1253`） |
| `meshID` | 1184 | **C** | Entity → MeshComponent；**组件间唯一连线** |
| `cascadeMask` | 1185 | **C** | 阴影级联跳过掩码 |
| `filterMask` | 1186 | **C** | 用户通道过滤位 |
| `color` / `emissiveColor` | 1187-1188 | **C** | 打包成 half4 进 instance（`wiScene.cpp:4769-4770`） |
| `lod_bias` | 1189 | **C** | LOD 公式输入（`wiScene.cpp:9096`） |
| `draw_distance` | 1190 | **C** | 距离淡出上限 |
| `lightmapWidth/Height` / `lightmapTextureData` | 1191-1193 | **C** | 光照图源数据 |
| `vertex_ao` | 1194 | **C** | 顶点 AO 源数据（→ `vb_ao`） |
| `alphaRef` / `rimHighlightColor` / `rimHighlightFalloff` / `sort_priority` | 1195-1199 | **C** | 参数；`sort_priority` 进 sort_bits（`wiScene.cpp:4694`） |
| `filterMaskDynamic` | 1201 | **R** | 每帧由材质 `GetFilterMask()` 或起来（`wiScene.cpp:4575,4706`） |
| **`lightmap_render`** (`Texture`) | **1203** | **G** | 渲染目标用途的光照图 |
| **`lightmap`** (`Texture`) | **1204** | **G** | 采样用光照图；`GetDescriptorIndex` 于 `wiScene.cpp:4765``[实测]` |
| `lightmapIterationCount` | 1205 | **R** | `mutable`，渐进更新计数 |
| **`vb_ao_srv`** (`int`) | **1206** | **D** | 由 `GetDescriptorIndex(&vb_ao, SRV)` 产出（`wiScene_Components.cpp:2152`）`[实测]` |
| **`vb_ao`** (`GPUBuffer`) | **1207** | **G** | 顶点 AO 缓冲，`ObjectComponent::CreateRenderData` 创建（`:2131-2154`） |
| **`wetmap`** (`GPUBuffer`) | **1208** | **G** | 湿地图；惰性创建于对象更新里（`wiScene.cpp:4602-4613`）`[实测]` |
| `center` / `radius` | 1210-1211 | **R** | 每帧由 `aabb.getCenter()/getRadius()` 重算（`wiScene.cpp:4663-4664`） |
| `fadeDistance` | 1212 | **R** | 每帧重置为 `draw_distance` 再与 impostor 取 min（`wiScene.cpp:4594,4645-4648`） |
| `lod` | 1214 | **R** | 每帧 `ComputeObjectLODForView`（`wiScene.cpp:4672`） |
| `wetmap_cleared` / `mesh_blend_required` | 1215-1216 | **R** | `mutable` 每帧状态 |
| `occlusion` (OcclusionResult) | 1218 | **R** | 32 帧历史位域 `occlusionHistory`（`:1152`），非序列化 |
| **`mesh_index`** | **1221** | **R（仅当帧有效）** | 注释 "these will only be valid for a single frame"；= `meshes.GetIndex(meshID)`（`wiScene.cpp:4609-4610`） |
| **`sort_bits`** | **1222** | **R（仅当帧有效）** | 32 位位域，见 §4.3 |
| `CreateRenderData()/DeleteRenderData()` | 1259-1260 | **G 创建/销毁点** | `wiScene_Components.cpp:2126-2154` |
| `Vertex_AO` 嵌套类型（R8_UNORM 步长） | 1264-1269 | **C 布局契约** | `FORMAT = R8_UNORM`，与 shader 对齐 |

#### 2.1.4 LightComponent（`wiScene_Components.h:1347-1472`）

| 字段 | 行 | 归属 | 依据 |
|---|---|---|---|
| `_flags`(CAST_SHADOW/VOLUMETRICS/VISUALIZER/LIGHTMAPONLY_STATIC/VOLUMETRICCLOUDS) | 1358 | **C** | 序列化 |
| `type` (DIRECTIONAL/POINT/SPOT/RECTANGLE) | 1369 | **C** | 枚举；`sun_index` 依赖它（`wiScene.cpp:5077-5084`） |
| `color` / `intensity` / `range` / `outerConeAngle` / `innerConeAngle` / `radius` / `length` / `height` / `volumetric_boost` | 1370-1378 | **C** | 纯光参数（glTF KHR_lights_punctual 语义，注释 `:1371`） |
| `cascade_distances` / `lensFlareNames` | 1380-1381 | **C** | 序列化容器 |
| `forced_shadow_resolution` | 1383 | **C** | -1 = 自动 |
| `cameraSource` (Entity) | 1385 | **C** | 组件 Entity 引用 |
| `position/direction/rotation/scale` | 1388-1391 | **R** | 每帧从 TransformComponent 分解（`wiScene.cpp:5098-5104`）`[实测]` |
| **`maskTexDescriptor`** (`int`) | **1392** | **D** | 裸 bindless 索引 |
| `occlusion` | 1394 | **R** | 同 Object |
| `lensFlareRimTextures` (`vector<Resource>`) | 1396 | **C** | 资产句柄 |
| `GetSphere()/GetAABB()` | 1441/1465 | 派生 | 计算剔除用包围体 |

**关键发现**：`LightComponent` 有 **0 个 `GPUBuffer`/`Texture`**`[实测：1394-1396 之外无 graphics 类型]`，只有 1 个裸描述符 `maskTexDescriptor`。光源的 GPU 侧完全在 Scene 级的 `shaderscene`/各 pass 常量里。→ 光线数据是**纯 C + 1 个 D**，可作为「场景层最小 GPU 污染」的样板。

#### 2.1.5 CameraComponent（`wiScene_Components.h:1474-1579`）

| 字段 | 行 | 归属 | 依据 |
|---|---|---|---|
| `_flags`(DIRTY/CUSTOM_PROJECTION/ORTHO/CRT_FILTER) | 1482 | **C** | 序列化 |
| `width/height/zNearP/zFarP/fov/focal_length/aperture_size/aperture_shape/ortho_vertical_size` | 1487-1495 | **C** | 投影参数 |
| `Eye/At/Up` | 1497-1499 | **R** | 每帧由 `TransformCamera` 从世界矩阵重算（`wiScene.cpp:4932-4934`） |
| `View/Projection/VP` | 1500 | **R** | `UpdateCamera()` 重算 |
| `frustum` | 1501 | **R** | 剔除用，`UpdateCamera()` 重算 |
| `InvView/InvProjection/InvVP` | 1502 | **R** | 同上 |
| `rotationMatrix/jitter/clipPlane/clipPlaneOriginal` | 1503-1507 | **R** | 每帧（jitter 由 TAA 设） |
| `canvas/scissor/sample_count` | 1508-1510 | **R** | 视口状态，非序列化 |
| **`texture_primitiveID_index` 等 19 个 `int`** | **1511-1528** | **D** | bindless 索引：primitiveID/depth/velocity/normalRoughness/reflection/…/vxgi/reprojectedDepth。**相机持有整条管线各 pass 的输出槽位** |
| `shadercamera_options` | 1529 | **C** | 位掩码，进 shader |
| `RenderToTexture::resolution/sample_count/update_interval` | 1533-1535 | **C** | 相机自渲染的配置 |
| `RenderToTexture::rendertarget_MSAA/rendertarget_render/rendertarget_display/depthstencil/depthstencil_resolved` | 1536-1540 | **G** | 5 张 `Texture` |
| `RenderToTexture::tileCount/entityTiles` | 1541-1542 | **G** | 分块剔除缓冲 |
| `RenderToTexture::visibility` (`shared_ptr<void>`) | 1543 | **R（类型擦除的 Visibility）** | 把渲染器的可见性结果挂在相机上，**类型擦除以避开场景层依赖渲染层**`[推断：shared_ptr<void> 无法证明是 Visibility，但 01C 记录 RenderPath 会写它]` |
| `RenderToTexture::time_accumulator` | 1544 | **R** | 累积计时 |

**关键发现（边界泄漏最严重处）**：`CameraComponent` 的 19 个 `int` 索引（`:1511-1528`）本质是**整条渲染路径的 RT 语义表**，却被放在场景层的相机上。这直接违反本项目 B3 的「L3 Scene 不得依赖 L4 Features/Paths」。→ 本项目应把这些槽位放进 `RenderPath`，而不是 `Camera`。

### 2.2 三层归属汇总（按桶统计）

| 组件 | C 纯 CPU | G GPU 句柄 | D 描述符索引 | R 渲染缓存 | 结论 |
|---|---|---|---|---|---|
| MaterialComponent | ~35 字段 | **0** | 3（`sampler_descriptor` + 2 sparse） | 3（`engineStencilRef`/`layerMask`/`cached_wrapSampler`） | **最干净**：无 GPU 缓冲，靠打包函数 |
| MeshComponent | ~20 数组 | 3（`generalBuffer`/`streamoutBuffer`/`alias`）+ 17 BufferView + `BLASes` | 17×2（srv/uav 索引） | ~8（`geometryOffset`/`meshletCount`/`aabb`/…） | **最脏**：四桶同居一 struct |
| ObjectComponent | ~18 字段 | 4（2 Texture + 2 GPUBuffer） | 2（`vb_ao_srv`、lightmap 索引按需） | ~10 | 中：GPU 资源都是「可选装饰」 |
| LightComponent | ~18 字段 | **0** | 1（`maskTexDescriptor`） | 5（position/direction/…） | **接近纯数据** |
| CameraComponent | ~12 字段 | 8（2 组 RT 纹理 + entityTiles） | **19** | ~12 | **边界最差**：19 个 pass 输出槽位 |

**为什么 WE 会这样**（`[推断]`，基于以下实测事实推理）：
1. **ECS 的 `GetData()` 批量访问要求「一组件一连续数组」**（`wiECS.h:456-457`），把 GPU 句柄拆到旁路 map 会破坏缓存局部性——WE 选择了局部性优先。
2. **`CreateRenderData()` 作为组件成员函数**（`wiScene_Components.h:836-840`）意味着组件必须能直接摸到 `device`；WE 用全局 `wi::graphics::GetDevice()`（`wiScene_Components.cpp:563`）解决，而不是注入上下文。
3. **延迟销毁在 RHI 内部**（`wiGraphicsDevice_DX12.h:565-581` 的 10 个 `destroyer_*` 队列），组件 `= {}` 置空即「释放」，所以组件持有 GPU 句柄**不需要**生命周期管理负担——这是它能混放的关键前提`[推断]`。

## §3 调用链（入口到 GPU 命令，逐步 file:line）

### 3.1 CreateRenderData 完整数据流：CPU 顶点数组 → GPU 可绘制`[实测]`

入口：`MeshComponent::CreateRenderData()`（声明 `wiScene_Components.h:836`，实现 `wiScene_Components.cpp:559-1303`）。

| # | 步骤 | 位置 | 事实 |
|---|---|---|---|
| 0 | 先销毁旧资源 | `wiScene_Components.cpp:561` → `:532-557` | `DeleteRenderData()` 把所有 BufferView 置 `{}`、`BLASes.clear()` |
| 1 | 补生成切线 | `:565-590` | 无 `vertex_tangents` 且**有** uv0+normals 时用 `meshopt_generateTangents` 现算 |
| 2 | 生成 provoking/reorder 索引 | `:592-598` | `meshopt_generateProvokingIndexBuffer`（绕开 SV_PrimitiveID 的 GS 模拟） |
| 3 | 算 AABB | `:600-612` | 遍历 `vertex_positions` 求 min/max → `aabb`（`:612`） |
| 4 | 定顶点位置格式（量化） | `:614-663` | 先试 `Vertex_POS16`，逐点验证误差 ≤ 1mm，失败升 32 位；UNORM 时 AABB 每轴至少 0.01 宽（`:645-661`） |
| 5 | 定 UV 格式 | `:670-694` | UV 包围盒跨度 > 65536 则退化到 `Vertex_UVS32` |
| 6 | **算总字节数** | `:702-750` | `bd.size = align(pos) + align(provoke) + align(reorder) + align(indices) + align(normals) + align(tangents) + align(uvs) + align(atlas) + align(colors) + align(bone1) + align(bone2)`，再叠加 morph（`:735-747`）与 meshlet（`:839-841`）。**每段按 `GetMinOffsetAlignment` 对齐**（`:718`） |
| 7 | 填 `GPUBufferDesc` | `:702-716` | `usage = DEFAULT`（UMA 则 `UPLOAD`，`:704-712`）；`bind_flags = VB｜IB｜SRV`；`misc = BUFFER_RAW｜TYPED_FORMAT_CASTING｜NO_DEFAULT_DESCRIPTORS`（+ `RAY_TRACING`，`:714-717`） |
| 8 | **全局池子分配** | `:1220` | `wi::renderer::SuballocateGPUBuffer(bd.size)` |
| 9 | **创建（别名）缓冲** | `:1223` | `device->CreateBuffer2(&bd, init_callback, &generalBuffer, &suballoc.alias, suballoc.allocation.byte_offset)` |
| 10 | 失败退化 | `:1230-1234` | 独立 `CreateBuffer2(&bd, init_callback, &generalBuffer)`（此时 offset 相对自身） |
| 11 | **`init_callback` 逐段写入** | `:846-1214` | 闭包按 `:849` 的 `buffer_offset` 游标依次填 position（量化 `:857-870`）、provoke、reorder、index、normal、tangent、uv、atlas、color、bone、morph、cluster；**每段写前设置 `vb_xxx.offset/size`** |
| 12 | 建索引 SRV | `:1240-1245` | `CreateSubresource(&generalBuffer, SRV, ib.offset, ib.size, &ib_format)` → `GetDescriptorIndex(..., subresource_srv)` |
| 13 | 建顶点 SRV ×8 | `:1248-1296` | 同模式：`vb_pos_wind`(1248)、`vb_nor`(1253)、`vb_tan`(1258)、`vb_uvs`(1263)、`vb_atl`(1268)、`vb_col`(1273)、`vb_bon`(1278)、`vb_mor`(1283)、`vb_clu`(1289)、`vb_bou`(1295) |
| 14 | 需要蒙皮则建 streamout | `:1300-1302` | `if (!vertex_boneindices.empty() ｜｜ !morph_targets.empty()) CreateStreamoutRenderData()` |
| 15 | streamout 建 SRV+UAV | `:1334-1367` | `so_pos` 同时建 SRV 与 UAV（注释 `:1335` "UAV can't have RGB32_F format!"） |
| 16 | BLAS 惰性创建（不在本函数） | `wiScene.cpp:4245-4248` | 只有 `TLAS_instancesMapped != nullptr` 时才 `CreateRaytracingRenderData()` |

**关键结构（第 11 步的产物）**：`gd.size` 是**一次分配、多段切分**；每段的 `BufferView::offset` 是相对 `generalBuffer`（可能是别名块内的偏移）的**字节**偏移，而绘制时被换算成**索引元素**偏移：

```
// wiRenderer.cpp:3385-3394   [实测]
if (mesh.generalBufferOffsetAllocation.IsValid())
    indexOffset = ((allocation.byte_offset + ibv.offset) / ib_stride) + subset.indexOffset;
else
    indexOffset = (ibv.offset / ib_stride) + subset.indexOffset;
```

→ 这就是 suballocation 的**全部代价**：绘制侧多了三重来源的偏移运算（块偏 + 视图偏 + 子集偏）。

### 3.2 全局几何池的实现`[实测]`

| 事实 | 位置 |
|---|---|
| 池子块大小 = 256 MB | `wiRenderer.cpp:2782` `GPUSubAllocator::blocksize` |
| 单次请求 > 半块（128MB）则不子分配、返回无效 | `:2796-2797` |
| 顺序遍历已有块尝试 `block.allocator.allocate(size)` | `:2803-2812` |
| 都不行则新建块：`ALIASING_BUFFER｜NO_DEFAULT_DESCRIPTORS` | `:2819-2838` |
| 延迟回收 = `update_deferred_release(FRAMECOUNT, BUFFERCOUNT)` | `:2854` |
| 块空则销毁 | `:2856-2860` |
| 结构定义 `BufferSuballocation{alias, allocation}` | `wiRenderer.h:75-79` |
| 接口注释说明目的："bind the large GPUBuffer once as index buffer… while the small buffers can be allocated/deallocated from it with memory aliasing" | `wiRenderer.h:80-84` |

### 3.3 一帧的完整链路（从场景到 DrawIndexedInstanced）`[实测]`

```
RenderPath3D::Update
  └─ scene->Update(dt)                              wiRenderPath3D.cpp:371
       ├─ cpu_gpu_mapped_resource_index = device->GetBufferIndex()   wiScene.cpp:37
       ├─ geometryAllocator.store(0); mesh.geometryOffset = fetch_add(subsets.size())  :266-271
       ├─ RunMeshUpdateSystem      :414  → memcpy(geometryArrayMapped + geometryOffset + subset, ...)  :4237
       │                                   → geometry.* = mesh.vb_*.descriptor_srv                            :4143-4164
       ├─ RunMaterialUpdateSystem  :418  → WriteShaderMaterial(materialArrayMapped + i)                    :4352
       ├─ RunObjectUpdateSystem    :434  → memcpy(instanceArrayMapped + i, &inst)                          :4784
       │                                   inst.baseGeometryOffset = mesh.geometryOffset                     :4773
       ├─ shaderscene.instancebuffer = GetDescriptorIndex(&instanceBuffer[idx], SRV)                       :930-938
       └─ CopyBufferAsync(upload → DEFAULT, 独立 COPY 队列)                                                :1127
RenderPath3D::PreRender
  └─ UpdateVisibility(visibility_main)              wiRenderPath3D.cpp:393 / wiRenderer.cpp:3619
       └─ vis.visibleObjects[...] = groupOffset + stream_compaction.list[i]              wiRenderer.cpp:3805
RenderPath3D::Render → DrawScene
  └─ for (instanceIndex : vis.visibleObjects)       wiRenderer.cpp:7223
       └─ renderQueue.add(object.mesh_index, instanceIndex, distance, object.sort_bits)  :7243
            └─ RenderMeshes(vis, renderQueue, renderPass, filterMask, cmd)               :3137
                 ├─ AllocateGPU(sizeof(ShaderMeshInstancePointer)*n)  :3170  ← 帧内线性区
                 ├─ batch_flush() 按 meshIndex/stencil/lod 切批              :3425-3444
                 ├─ indexOffset 三重偏移计算                                :3385-3394
                 └─ DrawIndexedInstanced(subset.indexCount, batch.instanceCount, indexOffset, 0, 0, cmd)  :3418
shader 侧：
  objectHF.hlsli:53   PUSHCONSTANT(push, ObjectPushConstants)
  objectHF.hlsli:58   #define GetMesh() (load_geometry(push.geometryIndex))
  objectHF.hlsli:59   #define GetMaterial() (load_material(push.materialIndex))
  objectHF.hlsli:178  GetInstancePointer() = bindless_buffers[push.instances].Load<ShaderMeshInstancePointer>(push.instance_offset + instanceID*sizeof)
  objectHF.hlsli:217  GetInstance() = load_instance(GetInstancePointer().GetInstanceIndex())
  globals.hlsli:586-589 load_instance/load_geometry/load_material/load_meshlet 宏
```

**这条链的精髓**：CPU 只发一个 16 字节 push constant（`ObjectPushConstants`：`geometryIndex/materialIndex/samplerIndex/instances/instance_offset`，`ShaderInterop_Renderer.h:835-844`，`static_assert(sizeof==16)`）`[实测]`，其余全部由 shader 两次间接索引拿到（instance → geometry → material）。**没有 per-draw 常量缓冲刷新**。

## §4 算法与公式

### 4.1 每帧 CPU→GPU 三件套与双缓冲`[实测]`

三组「upload buffer + mapped 指针 + device buffer」由 `GraphicsDevice::GetBufferCount()`（=2，`wiGraphicsDevice.h:72`）定长：

| 数据 | upload | mapped 指针 | device buffer | 元素结构/大小 |
|---|---|---|---|---|
| 实例 | `instanceUploadBuffer[2]` `wiScene.h:126` | `instanceArrayMapped` `:127` | `instanceBuffer[2]` `:129` | `ShaderMeshInstance` 256B（`ShaderInterop_Renderer.h:799`） |
| 几何 | `geometryUploadBuffer[2]` `:137` | `geometryArrayMapped` `:138` | `geometryBuffer[2]` `:140` | `ShaderGeometry` 128B（`:616`） |
| 材质 | `materialUploadBuffer[2]` `:144` | `materialArrayMapped` `:145` | `materialBuffer[2]` `:147` | `ShaderMaterial` 384B（`:513`） |

**怎么写**：
1. 帧首取索引：`cpu_gpu_mapped_resource_index = device->GetBufferIndex()`（`wiScene.cpp:37`），而 `GetBufferIndex() = GetFrameCount() % GetBufferCount()`（`wiGraphicsDevice.h:147`）`[实测]`。
2. 映射指针重绑：`instanceArrayMapped = (ShaderMeshInstance*)instanceUploadBuffer[idx].mapped_data`（`wiScene.cpp:153`）、`materialArrayMapped`（`:195`）、`geometryArrayMapped`（`:378`）`[实测]`。
3. **容器增长策略**：容量不足时重建 `desc.size = stride * count * 2`（`wiScene.cpp:131` 实例、`:167` 材质、`:349` 几何）——注释 "// *2 to grow fast"；同时重建**全部 2 个**（`for i in arraysize(...)`）`[实测]`。
4. **先清零再写**：只有 `dt > 0` 时用 `shader_mesh_instance_null` 填满整个 instance 数组（`wiScene.cpp:277-283`），保证失效实例不残留`[实测]`。
5. 三处写入点（`std::memcpy` 直写，注释统一为 "memcpy whole structure into mapped pointer to avoid read from uncached memory"）：
   - 几何：`wiScene.cpp:4237`（逐 mesh 逐 subset）、`:5243` / `:5358` / `:5552`（probe/rain 特例）
   - 材质：`wiScene.cpp:4352`（`WriteShaderMaterial` 内部再 memcpy，`wiScene_Components.cpp:444`）
   - 实例：`wiScene.cpp:4784`、`:5268` / `:5371` / `:5565`

**怎么翻（双缓冲）**：
- `CopyBufferAsync`（`wiScene.cpp:1127`）把当帧 upload → `[idx]` device buffer。因为该复制在**独立 COPY 队列**上异步执行（`wiGraphicsDevice_DX12.h:113` 注释 "create separate copy queue to reduce interference"；`wiGraphicsDevice_DX12.cpp:5997-6020` 实现为一次 `CopyBufferRegion` + `copyAllocator.submit`）`[实测]`，与图形队列有帧重叠，所以 **device buffer 也必须双缓冲**——代码原注释（`wiScene.cpp:1084`）：
  > "Note: with the CopyBufferAsync we also have to double buffer the dedicated GPU memory buffers as the async copy will be kicked with frame overlap"
- 于是「`[idx]` 写、`[idx]` 读」在同帧自洽；下一帧 `idx` 翻转，旧帧 buffer 仍可被在飞的 GPU 工作读取。
- **UMA 例外**：UMA 下 upload buffer 直接带 `SHADER_RESOURCE` 绑定并被 shader 读（`wiScene.cpp:929-933`），非 UMA 才用独立 DEFAULT buffer（`:936-939`）`[实测]`。

### 4.2 描述符索引的产出与消费

| 环节 | 位置 |
|---|---|
| 产出（场景级三 buffer） | `wiScene.cpp:930-932`（UMA）/ `:936-938`（非 UMA） |
| 产出（mesh 每个视图） | `wiScene_Components.cpp:1240-1296`（17 处 `CreateSubresource` + `GetDescriptorIndex`） |
| 产出（材质纹理） | `wiScene_Components.cpp:418`（`device->GetDescriptorIndex(texturemap.GetGPUResource(), SRV, subresource)`），空槽写 `-1`（`:422`） |
| 产出（采样器） | `wiScene_Components.cpp:431` |
| 汇总进 shader 常量 | `wiRenderer.cpp:4421` `frameCB.scene = vis.scene->shaderscene;` |
| 消费 | `globals.hlsli:586-589` 的 `load_instance/load_geometry/load_material/load_meshlet` 宏 |
| 绑定量级 | `BINDLESS_SAMPLER_CAPACITY = 256`（`wiGraphicsDevice.h:45`，注释 "chosen to be addressable by 8 bits"）；`ObjectPushConstants::samplerIndex` 只给 8 位（`ShaderInterop_Renderer.h:838`）`[实测]` |

### 4.3 对象排序位域（sort_bits）`[实测]`

`ObjectComponent` 的排序位域在 `RunObjectUpdateSystem` 内构造（`wiScene.cpp:4675-4729`）：

| 位域 | 位数 | 含义 | 赋值处 |
|---|---|---|---|
| `shadertype` | 6 | 最多 64 种 shader | `:4716`（**按位或** `1 << material->shaderType`） |
| `blendmode` | 3 | 最多 8 种混合 | `:4717` |
| `doublesided` | 1 | 双面 | `:4718` |
| `tessellation` | 1 | 细分 | `:4692` |
| `alphatest` | 1 | alpha 测试 | `:4719` |
| `customshader` | 8 | 自定义 shader | `:4723-4727`（`1u << (id & 7u)` 折叠） |
| `sort_priority` | 4 | 用户优先级 | `:4694` |
| `unused` | 8 | — | — |

- `static_assert(sizeof(SortBits) == sizeof(uint32_t))`（`:4690`）`[实测]`。
- `shadertype` 用**位或**意味着：一个对象多个 subset 材质不同时，该字段变成「用到了哪些 shadertype」的位掩码，而非单值`[实测]`。
- 用途：进 `RenderBatch::sort_bits`（`wiRenderer.cpp:214`，24 位），排序时**优先减少 PSO 切换**、距离次之（`:242-300`）`[实测]`。

### 4.4 LOD 选择公式`[实测]`

`Scene::ComputeObjectLODForView`（`wiScene.cpp:9052-9101`）：

1. 8 个 AABB 角点 → clip：`C = C * VP`，`w = max(C.w, 1e-4)`，`C /= w` → NDC（`:9072-9084`）。
2. NDC → UV：`uv = C * (0.5,-0.5,1,1) + (0.5,0.5,0,0)`（`:9071-9072, 9088`）。
3. 夹到 `[-1, 2]`（`:9075-9076`）——注释说明这是修「近平面 x/w 爆炸导致恒为 LOD0」的旧 bug（`:9058-9069`）。
4. `maxdim = max(width, height)`，`lod = clamp(log2(1/maxdim) + lod_bias, 0, lod_max)`（`:9095-9096`）。

### 4.5 实例结构里的 UNORM 反量化修正`[实测]`

顶点位置量化到 16 位后，GPU 变换需带 AABB 反量化矩阵（`wiScene.cpp:4738-4749`）：
- `inst.transformRaw.Create(worldMatrix)` 存**原始 W**（`:4740`）。
- 若 `IsFormatUnorm(mesh.position_format) && !mesh.so_pos.IsValid()`，则 `R = mesh.aabb.getUnormRemapMatrix()`，`inst.transform.Create(R * W)`、`inst.transformPrev.Create(R * worldMatrixPrev)`（`:4741-4749`）。
- 注释："The UNORM correction is only done for the GPU data!"（`:4745`）→ CPU 侧 `matrix_objects` 存未修正的 W`[实测]`。

### 4.6 遮挡剔除的 32 帧保守判据`[实测]`

- `OcclusionResult`（`wiScene_Components.h:1148-1163`）含 `occlusionQueries[BUFFERCOUNT]`（`:1150`）与 `occlusionHistory`（`:1152`）。
- 每帧 `occlusionHistory <<= 1`（`wiScene.cpp:9969`）；查询可见则 `|= 1`（`:9974-9978`）；否则也置 1（`:9980-9984`，无结果时保守当可见）。
- `IsOccluded() = (occlusionHistory == 0)`（`wiScene_Components.h:1158-1162`）——**32 帧全被挡才判定遮挡**。
- 代价：遮挡恢复最多 32 帧延迟，换来「不会因单帧查询抖动而闪烁」`[推断]`。

## §5 资源与同步

### 5.1 谁创建 / 谁使用 / 谁销毁`[实测]`

| 资源 | 创建 | 使用 | 销毁 |
|---|---|---|---|
| `MeshComponent::generalBuffer` | `wiScene_Components.cpp:1223/1232` | 绑 IB（`wiRenderer.cpp:3358`）、shader 作 SRV | `DeleteRenderData()` `:532-557` 置 `{}`，真正释放交 RHI 延迟队列 |
| `generalBuffer` 所在的 256MB 块 | `wiRenderer.cpp:2834` | 所有子分配 | 块空即销毁（`:2856-2860`） |
| `streamoutBuffer`（蒙皮/morph） | `wiScene_Components.cpp:1304-1332` | 变换反馈写 + 每帧 swap（`wiScene.cpp:4112-4115`） | `DeleteRenderData()` |
| `BLASes` | `wiScene_Components.cpp:1369-1427`（惰性，`wiScene.cpp:4245`） | TLAS 实例引用（`wiScene.cpp:4806`） | `DeleteRenderData()` 的 `BLASes.clear()` |
| Scene 级 instance/geometry/material buffer | 容量不足时重建（`wiScene.cpp:127-151` / `:163-184` / `:338-361`） | shader bindless | `Scene::Clear()`（`:1130-1157`）**不清三件套**，只清 CPU 数组与子系统`[实测]` |
| `TLAS` / `TLAS_instancesUpload` | `wiScene.cpp:305-320` | `UpdateRaytracingAccelerationStructures`（`wiRenderer.cpp:5700-5720`） | `Scene::Clear()` `:1137` |
| `queryHeap` / `queryResultBuffer` | `wiScene.cpp:226-247` | `OcclusionCulling_Render`（`wiRenderer.cpp:5838-5900`） | 未见显式销毁`[推断]` |

### 5.2 barrier 与同步在哪里`[实测]`

- **场景层零 barrier**：`grep -c Barrier wiScene.cpp` = **0**，`wiScene_Components.cpp` = **0**`[实测]`。
- 场景层唯一的跨队列动作是 `CopyBufferAsync`（`wiScene.cpp:1127`）；barrier 由 RHI 内部自理（`wiGraphicsDevice_DX12.cpp:5997-6020` 可见只有 `CopyBufferRegion` + `submit`）`[推断]`。
- 上传→读取的时序安全靠**三条叠加**：① device buffer 双缓冲（`wiScene.h:126-147`）② COPY 队列带 fence（`wiGraphicsDevice_DX12.h:116-121` 的 `CopyCMD{fence, fenceValue}`）③ RHI 延迟销毁判据 `front.frame + BUFFERCOUNT < FRAMECOUNT`（`wiGraphicsDevice_DX12.h:565-638`）。
- 本项目可照搬 ①③，②建议简化（见 §6）。

### 5.3 脏机制与状态机`[实测]`

| 机制 | 位置 | 管什么 | 实际效力 |
|---|---|---|---|
| `MaterialComponent::SetDirty/IsDirty` | `wiScene_Components.h:280-281` | 材质参数变过 | **仅提示**：`RunMaterialUpdateSystem` 里 `if (IsDirty()) SetDirty(false);`（`wiScene.cpp:4337-4340`）之后**无条件**全量 `WriteShaderMaterial`（`:4352`）`[实测]` |
| `TransformComponent::SetDirty/IsDirty` | `:72-73` | 局部变换变过 | `RunTransformUpdateSystem` **无条件** `UpdateTransform()`（`wiScene.cpp:2961-2968`），脏标志未用于跳过`[实测]` |
| `CameraComponent::SetDirty/IsDirty` | `:1556-1557` | 投影参数变过 | `RunCameraUpdateSystem` **无条件** `UpdateCamera()`（`wiScene.cpp:4924-4937`）`[实测]` |
| `MeshComponent::BLAS_state` | `:786-792` | BLAS 重建/refit/完成 | **真状态机**：写 `wiScene.cpp:4276`（材质变换→REBUILD）、`:4280-4283`（有 streamout→REFIT）、`wiScene_Components.cpp:1376`（新建→REBUILD）；读 `wiRenderer.cpp:5729-5737`（switch 决定 `BuildRaytracingAccelerationStructure` 传不传 source），用后立刻置 COMPLETE（`:5743`）`[实测]` |
| `Scene::acceleration_structure_update_requested` | `wiScene.h:93-95` | 本帧是否需要 TLAS | RenderPath 置（`wiRenderPath3D.cpp:367`）、`Scene::Update` 消费（`wiScene.cpp:303`）、渲染器清（`wiRenderer.cpp:5815`）`[实测]` |
| 序列化 version | `wiScene.h:35-68` 第二参数 | 反序列化兼容 | materials=11、meshes=4、objects=4；`ComponentLibrary::GetVersion`（`wiECS.h:748`） |

**结论**：WE 用同一个「脏」字覆盖了三种完全不同的东西——① 纯提示（Material/Transform/Camera，实际不省任何工作）② 真状态机（BLAS_state）③ 一次性请求位（AS update）。移植时必须分开，否则会把 ① 当 ②，得到「以为什么都没变、其实每帧全量重算」的错觉`[推断]`。

### 5.4 可见性：谁写、谁读、如何剔除`[实测]`

- **类型**：`wi::renderer::Visibility`（`wiRenderer.h:124-165`）——**不属于 Scene**。含 `visibleObjects/visibleDecals/visibleEnvProbes/visibleEmitters/visibleHairs/visibleLights/visibleColliders`（`:150-156`）、`shadow_packer`（`:157`）、两个 `std::atomic<uint32_t>` 计数器（`:161-162`）。
- **写方**：`UpdateVisibility`（定义 `wiRenderer.cpp:3619`，声明 `wiRenderer.h:211`）：
  1. `wi::jobsystem::Dispatch(ctx, object_loop, groupSize, ...)`（`:3728`），每线程组一个 `StreamCompaction` 本地计数（`:3731-3735`）。
  2. 判据 `(aabb.layerMask & vis.layerMask) && vis.frustum.CheckBoxFast(aabb)`（`:3741`）。
  3. 组内压缩 `stream_compaction.list[count++] = args.groupIndex`（`:3744`）。
  4. 组末合并 `prev_count = vis.object_counter.fetch_add(count)`，`visibleObjects[prev_count+i] = groupOffset + list[i]`（`:3801-3806`）。
  5. `wi::jobsystem::Wait(ctx)` 后收缩 `visibleObjects.resize(object_counter.load())`（`:3991`）。
  6. 顺带分配遮挡查询槽 `object.occlusion.occlusionQueries[queryheap_idx] = queryAllocator.fetch_add(1)`（`:3795`）。
- **读方**：① `OcclusionCulling_Render`（`wiRenderer.cpp:5849-5853`）② `DrawScene` 的 `for (uint32_t instanceIndex : vis.visibleObjects)`（`:7223`）→ `renderQueue.add(...)`（`:7243`）③ `DrawWireframeOverlay`（`:8717` 判空）。
- **Scene 的角色**：只提供 `aabb_objects`（`wiScene.h:107`，填充 `wiScene.cpp:4589`）与 `matrix_objects`（`:110`，填充 `:4746-4749`）。**Scene 不知道谁被剔除**`[实测]`。

## §6 可迁移性判定（逐项：必需 / 可选 / 规模不匹配 + 理由 + 本项目替代方案）

判定口径：**必需** = 不这么做就无法表达 GPU 语义或会撞上正确性 bug；**可选** = 有明确收益但也有明确代价，可按规模取；**规模不匹配** = WE 因为「多后端 / 大世界 / 数千 PSO」才需要，本项目照搬会净亏。

| # | WE 设计 | 位置 | 判定 | 理由 | 本项目替代方案 |
|---|---|---|---|---|---|
| 1 | **ECS 组件式存储**：`ComponentManager<T>` 平行数组 + 桶哈希 lookup，组件用 `Entity` 互引 | `wiECS.h:147-705, 461-470, 616-671` | **规模不匹配** | 收益（数据局部性、可序列化、批量遍历）对本项目 1-3 万行确实存在，但代价是：① swap-and-pop 打乱索引（`:324-344`），所有「组件索引 ↔ GPU 数组槽位」都得每帧重算；② 40 个组件管理器的注册样板（`wiScene.h:33-68`）对一个只有 5 类可渲染对象的项目是纯开销；③ 04 §4.6 已明确「不做完整 ECS」 | **保留「Entity = 稳定句柄 + 组件用句柄互引」这一条语义，丢掉泛型 ComponentManager**。本项目用：`Scene::Scene` 持有 `std::vector<Instance>`、`std::vector<Mesh>`、`std::vector<Material>`、`std::vector<Light>`，`Instance::meshIndex` 用 `uint32_t` 索引 + 一个 `std::vector<uint32_t>` 空闲槽表（复用槽位，避免 swap-and-pop 打乱期）。**理由**：本项目对象数 ≤ 数千，线性遍历足够；槽位稳定性直接让「GPU 数组槽位 = CPU 索引」，省掉 WE 每帧 `geometryAllocator.fetch_add` 的整段逻辑（`wiScene.cpp:266-272`）。 |
| 2 | **全局几何池 suballocation**：所有 mesh 顶点/索引塞进 256MB 大块，靠 offset 绘制 | `wiRenderer.cpp:2782-2848`、`wiScene_Components.cpp:1217-1234`、`wiRenderer.cpp:3385-3394` | **可选（本期不取）** | 收益：少绑 IB（`wiRenderer.cpp:3355-3361` 有 `prev_ib_internal` 比较才重绑），纯 offset 绘制。代价：① 绘制侧三重偏移（块偏+视图偏+子集偏，`:3388`）；② 需要 `ALIASING_BUFFER` 与 RHI 的 `CreateBuffer2(alias, offset)` 全套（`wiGraphicsDevice.h:93`）；③ 教学上「一个 mesh 一个 buffer」直观得多 | **每 mesh 独立 `GpuBuffer`，但保留 `Mesh::vbOffset/ibOffset` 抽象**（本项目已有，`Model.h:31-41` 的 `vbOffset/vbSize/ibOffset/ibSize`）。这样将来要上池子只需换 `Model` 内部实现，不动绘制侧。**理由**：本项目 mesh 数 ≤ 数百，绑 IB 的开销可忽略；优先级远低于 WP3/WP4。 |
| 3 | **bindless 描述符索引**：组件存裸 `int`，shader 用 `descriptor_index()` 两级间接 | `wiScene_Components.h:737`、`globals.hlsli:586-589`、`ShaderInterop_Renderer.h:540-556` | **必需（但要封装）** | 必需性来自「材质数组 + 几何数组 + 实例数组」这套索引化设计本身：shader 需要「按索引取结构」。但 WE 把**裸 int 暴露进 ECS** 是泄漏（`:737, 1206, 1392, 1511-1528` 共 22 处） | **保留索引化，禁止裸 int 进场景层**：定义 `struct GeometryView { uint32_t offset, size; TextureHandle srv; }` 与 `struct DescriptorIndex { uint32_t value; }`，索引由 RHI 分配、由 `RenderFeatures` 在打包 `ShaderGeometry` 时读取。场景层只见 `GeometryView`，不见 int。**理由**：一旦裸 int 进场景层，RHI 换堆策略就要改场景代码——这正是本项目 B3 要防的。 |
| 4 | **每帧 mapped 数组（instance/geometry/material 三件套）** | `wiScene.h:126-147`、写入 `wiScene.cpp:4237/4352/4784` | **必需** | 这是「一次上传、shader 靠索引取」的落点，没有它就得 per-object 常量缓冲刷新（本项目现状 `Model.cpp:117` 就是每实例 map 一次）。收益实测可见：WE 一次 draw 只推 16 字节 push constant（`ShaderInterop_Renderer.h:835-844`） | **保留，但归属改到 `RenderFeatures` 而非 `Scene`**（见 §7）。实现上本项目已有 `UploadBuffer`（`02_RHI/Resource/Heap/UploadBuffer.h`）与 `LinearAllocator`（`02_RHI/Memory/LinearAllocator.h`，每上下文线性区），可直接承载 |
| 5 | **双缓冲（`BUFFERCOUNT = 2`）**：CPU 写一帧、GPU 读一帧 | `wiGraphicsDevice.h:72, 145-147`、`wiScene.h:126-160` | **必需** | CPU 与 GPU 并行一帧的最小安全边界；没有它就必须每帧等 GPU 完成 | **照搬语义，不照搬 `GraphicsDevice::BUFFERCOUNT` 的位置**：本项目在 `02_RHI` 暴露 `GetFrameIndex()`（0/1 交替）+ 帧资源环（WP4 已规划）。**理由**：这是物理事实，不是 WE 特化。 |
| 6 | **`CopyBufferAsync`（独立 COPY 队列 + 跨帧）** | `wiGraphicsDevice_DX12.h:110-132`、`wiGraphicsDevice_DX12.cpp:5997-6020`、`wiScene.cpp:1127` | **规模不匹配** | 收益是减少与图形队列争用；代价是「上传何时可见」变成跨帧问题，**迫使 device buffer 也双缓冲**（`wiScene.cpp:1084` 原注释），正确性推理复杂度翻倍。本项目每帧上传量 = 数千个 128-384B 结构，量级 <1MB | **同队列 copy + 一次 fence**：在帧资源环里用 `GraphicsContext` 直接 `CopyBufferRegion`（upload→default），末尾统一 fence。**理由**：单队列下「本帧写、本帧读」时序是显然的，符合教学项目的「同步关系必须可见」原则⑤。 |
| 7 | **Scene 持有全场景 GPU 数组 + bindless 描述符**（`shaderscene`） | `wiScene.h:118, 126-167`、填充 `wiScene.cpp:927-938` | **可选（推荐拆分后取）** | WE 把三数组放 Scene 是因为「Scene 是唯一知道所有对象的地方」；但本项目有 `RenderFeatures` 层，三数组天然是按 pass 变的（本项目 3 pass vs WE 16 renderpass） | **三数组放 `RenderFeatures::SceneDrawData`**，由 `Scene` 只读遍历生成；`Scene` 持 `revision`，`SceneDrawData` 按 revision 失效。**理由**：满足 B2（RenderFeatures 不得依赖路径）同时让 Scene 彻底无 GPU 资源。 |
| 8 | **`Visibility` 由渲染侧持有**（visibleObjects 索引数组） | `wiRenderer.h:124-165`、`wiRenderer.cpp:3619-3991` | **必需（这条正是目标边界）** | 「场景只提供数据、渲染路径决定可见性」是原则①③的现成分界线；WE 把它做在 `wi::renderer` namespace 而非 Scene 里，是它最正确的一处分层 | **照搬这个归属**：`Scene` 提供 `GetInstances()` + `AABB`；`RenderPath` 持有 `Visibility{`visibleObjects, frustum, counters`}。**理由**：无需改造，本项目现状（`MeshSorter` 在 `BaseRenderer.h:114-167`）已经接近，只需把「剔除」补进 `MeshSorter` 之前 |

## §7 对本项目 WPx 的具体建议

### 7.1 直接回答：本项目 `Scene::Scene` 该持有什么、不该持有什么

结论先行（本节判定同时服务 04 §4.3 的 B3 契约与 05 的 WP3）：

**该持有（纯 CPU 数据 + 元数据）**

| 持有项 | 类型建议 | 依据 |
|---|---|---|
| 实例/网格/材质/灯光 四组 CPU 数组 | `std::vector<Instance/Mesh/Material/Light>` | §6#1：保留「稳定句柄 + 组件互引」语义，丢掉泛型 ComponentManager |
| 槽位空闲表 | `std::vector<uint32_t>` 复用栈 | §6#1：避开 swap-and-pop 打乱索引，使 CPU 索引 == GPU 数组槽位 |
| 每对象 AABB | `Math::BoundingBox`（本项目已有） | §6#8：Scene 只提供数据、可见性判定归渲染侧 |
| 版本号 | `uint64_t revision` | §6#7：RenderFeatures::SceneDrawData 按 revision 失效重建 |
| 材质**纯 CPU** 描述（底色/粗糙度/金属度/贴图句柄/标志位） | `MaterialData` POD | 04 §4.3-B3：材质数据下沉，禁炎症 D3D12 类型 |

**不该持有（必须清出去）**

| 现状 | 位置 | 归属改为 | 理由 |
|---|---|---|---|
| `ID3D12_GPU_VIRTUAL_ADDRESS` | Engine/04_Renderer/Renderer/Base/BaseRenderer.h:130-136（SortObject::meshCBV / bufferPtr） | RHI 内部 | 原则③：D3D12 类型不得出现在渲染器公开头文件 |
| `D3D12_VIEWPORT` / `D3D12_RECT` | BaseRenderer.h:88-89（GetMainViewport/GetMainScissor） | RHI / Path 实现 | 同上；视口是物理事实，但不该由抽象基类以 D3D12 类型暴露 |
| 裸描述符索引（`int`） | 本项目暂无（WE 在 wiScene_Components.h:737） | 包成 `GeometryView` | §6#3：裸 int 进场景层会让换堆策略改场景代码 |
| `GPUBuffer` / `Texture` 成员 | 本项目暂无（WE 在 wiScene_Components.h:728-729） | RenderFeatures::SceneDrawData | §6#7：Scene 必须彻底无 GPU 资源 |
| `DescriptorHeap s_TextureHeap/s_SamplerHeap` 全局堆 | BaseRenderer.h:22-23 | RHI 内部 + 通过 `GpuDescriptorHandle` 传参 | 原则③：两个全局堆外露等于广播 RHI 实现 |

### 7.2 对 WP3（Scene 容器）的施工建议

1. **Scene 不是重构而是新建**。本项目现状**不存在 Scene 类**（全仓仅 Engine/03_AssetSystem/Importers/glTF.h:217 的 `struct Scene`，那是 glTF 解析用的）。因此 WP3 是「新增一个容器」而非「改造现有类」——风险低于 05 中其他包。
2. **第一步只做搬移，不改语义**。WP3 第一刀只把现在散在 DeferredRenderer/ForwardRenderer/GameCore 里的 Models 向量、LightingSystem 持有、相机持有，集中到一个 Scene 对象，**不引入 revision、不引入 SceneDrawData**。先让编译过、画面不变，再在 WP4 里加 revision。依据：05 的硬依赖要求每个包可独立回滚。
3. **Scene 不得 include 任何 04_Renderer 头**。这是 WP1（解环）的验收条件之一，因为现在的 Engine/05_Scene/Model/Model.h:5-12 反向 include 了 04_Renderer/Pipeline/PipelineDesc.h 与 04_Renderer/Material/Material.h。搬材质数据的同时必须把 Material 拆成「05_Scene 的 MaterialData（纯数据）」+「04_Renderer 的 PSO 绑定侧字段」两半，否则 WP1 无解。
4. **不要做 ComponentManager<T> 泛型**。参考 §6#1 的代价分析：本项目可渲染对象 ≤ 数千，线性遍历足够；泛型组件管理器引入的 40 个管理器样板（wiScene.h:33-68）对 5 类对象是纯开销。
5. **Scene 的只读遍历接口只给三个**：`GetInstances()`（返回 `const std::vector<Instance>&`）、`GetMeshes()`、`GetMaterials()`。**不要给 Render()、不要给 Update()**——一旦 Scene 有了 Update，它就会开始持有时序逻辑，反向依赖会从另一头长回来。

### 7.3 与 WP1/WP4 的接口约定

- 给 **WP1**：WP3 完成后 Scene 及其依赖（05_Scene 内）不得出现在 02_RHI / 04_Renderer 的 include 列表中；反向亦然。这条可用 04 §4.7 的 R1-R6 grep 规则机检。
- 给 **WP4**：SceneDrawData 的生命周期绑帧资源环，**不绑 Scene revision 以外的任何东西**。revision 变化 → 当帧重建三数组；revision 不变 → 直接复用上一帧的 RenderFeatures 侧副本（这正是 §6#4 每帧 mapped 数组与本项目现状「每实例 map 一次」Model.cpp:117 的区别所在）。
- 给 **WP5**：Scene 只被 RenderFeatures 的 DrawScene 读，不被 RenderPath 直接读。RenderPath 只持 `Scene*` 传给 DrawScene。

## §8 未确认项

| # | 未确认 | 为什么没确认 | 确认需读哪一段 |
|---|---|---|---|
| 1 | `CameraComponent::RenderToTexture::visibility`（`wiScene_Components.h:1543`，类型 `wi::allocator::shared_ptr<void>`）到底存的什么 | 类型被擦除，场景层无静态线索 | 搜 `render_to_texture.visibility` 的读写点（预计在 `wiRenderPath3D.cpp` 的 RTT 相机绘制段）。命令：`grep -rn "render_to_texture.visibility" .` |
| 2 | 一个 object 挂多 subset、各 subset 材质通道不同时，`filterMaskDynamic` 位或（`wiScene.cpp:4706`）是否会让「半透明 subset」被整对象当不透明画 | 已读到位或的写入，未读 DrawScene 对多通道的二次判断 | 细读 `wiRenderer.cpp:7223-7243` 与 `renderQueue.add` 的 filterMask 判据（`:7237`）如何与 subset 级过滤共存 |
| 3 | `PageAllocator` 的页对齐与释放粒度（删掉一个 mesh 后其 `generalBuffer` 页何时真正可复用） | 只看到调用点（`wiRenderer.cpp:2854`）与块大小（`:2782`），未读分配器实现 | `wiAllocator.h/.cpp` 中 `PageAllocator::init/allocate/update_deferred_release` |
| 4 | `Scene::Clear()` 是否释放 instance/geometry/material 三件套 GPU buffer | 已读 `wiScene.cpp:1130-1157`，只见清 CPU 数组与子系统，未见 `instanceBuffer = {}` | 读 `Scene::`Scene()` 或反查 `grep -n "instanceBuffer" wiScene.cpp` 的非 Update 出现位置 |
| 5 | 本项目 `SortObject::bufferPtr`（`BaseRenderer.h:136`）在绘制时具体怎么用 | 只读了写入（`BaseRenderer.cpp:41-110`），未读消费 | `grep -n "GetSortObjects" Engine/04_Renderer/` 后读对应 Render 函数 |
| 6 | WE 的 `meshletAllocator` 与 `geometryAllocator` 是否有次序依赖 | 已读 geometry 分配（`wiScene.cpp:266-272`），meshlet 段在 `:470` 之后但未逐一确认 | `grep -n "meshletAllocator" wiScene.cpp` 看全部 fetch_add 点相对次序 |

## §9 证据索引

> W = `WickedEngine-master/WickedEngine/`，P = `TooiyumeRender/Engine/`。全部为 `[实测]`（由 `sed -n` / `grep -n` / `wc -l` 输出直接确认）。

| # | 文件 | 行号 | 事实 |
|---|---|---|---|
| 1 | W wiScene.h | 25 | `struct Scene` 定义起点 |
| 2 | W wiScene.h | 29 | `wi::ecs::ComponentLibrary componentLibrary` |
| 3 | W wiScene.h | 33-68 | 约 40 个 `ComponentManager<T>&`；materials=:35(v11)、meshes=:36(v4)、objects=:38(v4)、lights=:42(v5)、cameras=:43(v3)、gaussian_splats=:68 |
| 4 | W wiScene.h | 71 | `float dt = 0`（非序列化） |
| 5 | W wiScene.h | 79 | `CameraComponent camera;` 仅用于 LOD 与 3D 声 |
| 6 | W wiScene.h | 87 | `wi::graphics::Texture cloudmap`（Scene 持 GPU 纹理） |
| 7 | W wiScene.h | 90 | `GPUBuffer TLAS_instancesUpload[GetBufferCount()]` |
| 8 | W wiScene.h | 93-95 | `acceleration_structure_update_requested` + Set/Is |
| 9 | W wiScene.h | 102 | `uint32_t cpu_gpu_mapped_resource_index` |
| 10 | W wiScene.h | 107-111 | 五个 aabb_* 流 + `matrix_objects`/`matrix_objects_prev` |
| 11 | W wiScene.h | 118 | `ShaderScene shaderscene` |
| 12 | W wiScene.h | 126-129 | `instanceUploadBuffer[2]` / `instanceArrayMapped` / `instanceBuffer[2]` |
| 13 | W wiScene.h | 137-141 | `geometryUploadBuffer[2]` / `geometryArrayMapped` / `geometryBuffer[2]` / `geometryAllocator` |
| 14 | W wiScene.h | 144-147 | `materialUploadBuffer[2]` / `materialArrayMapped` / `materialBuffer[2]` |
| 15 | W wiScene.h | 154/161/167 | meshletAllocator / skinningAllocator / queryAllocator |
| 16 | W wiScene.h | 164-165 | `queryResultBuffer[arraysize(...)]` + `UpdateOcclusionResult` 声明 |
| 17 | W wiScene.h | 173-294 | SurfelGI/DDGI/VXGI/Ocean/Impostor GPU 子结构（Scene 持有） |
| 18 | W wiScene_Components.h | 44-107 | TransformComponent：`scale_local/rotation_local/translation_local` + `world`（非序列化）+ SetDirty/IsDirty（:72-73） |
| 19 | W wiScene_Components.h | 109-114 | HierarchyComponent{ parentID, layerMask_bind } |
| 20 | W wiScene_Components.h | 117 | MaterialComponent 起点，alignas(32) |
| 21 | W wiScene_Components.h | 140 | `uint32_t _flags = CAST_SHADOW` |
| 22 | W wiScene_Components.h | 159 | `SHADERTYPE shaderType = SHADERTYPE_PBR`（12 种） |
| 23 | W wiScene_Components.h | 181-202 | 22 个材质标量/向量参数（baseColor…mesh_blend） |
| 24 | W wiScene_Components.h | 220-239 | TEXTURESLOT 15 槽 |
| 25 | W wiScene_Components.h | 242-250 | TextureMap{ name, resource, uvset, GetGPUResource() } |
| 26 | W wiScene_Components.h | 255-256 | sparse_residencymap_descriptor / sparse_feedbackmap_descriptor（裸 int = D 层） |
| 27 | W wiScene_Components.h | 258 | `TextureMap textures[TEXTURESLOT_COUNT]` |
| 28 | W wiScene_Components.h | 266-268 | layerMask / sampler_descriptor / cached_wrapSampler |
| 29 | W wiScene_Components.h | 280-281 | 材质 SetDirty/IsDirty |
| 30 | W wiScene_Components.h | 398-399 | CreateRenderData(bool) / WriteShaderMaterial 声明 |
| 31 | W wiScene_Components.h | 655 | MeshComponent 起点，alignas(32) |
| 32 | W wiScene_Components.h | 673-685 | 12 个 CPU 几何数组（positions…indices） |
| 33 | W wiScene_Components.h | 692-703 | MeshSubset{ surfaceName, materialID, indexOffset, indexCount, materialIndex, flags } |
| 34 | W wiScene_Components.h | 728-731 | generalBuffer / streamoutBuffer / generalBufferOffsetAllocation / Alias（G 层） |
| 35 | W wiScene_Components.h | 732-739 | BufferView{ offset,size,subresource_srv,descriptor_srv,subresource_uav,descriptor_uav }（G+D） |
| 36 | W wiScene_Components.h | 746-761 | 17 个 BufferView 实例（ib_provoke…so_pre） |
| 37 | W wiScene_Components.h | 763-768 | geometryOffset / meshletCount / active_morph_count / morphGPUOffset / uv_range_min,max |
| 38 | W wiScene_Components.h | 770 | `wi::vector<RaytracingAccelerationStructure> BLASes` |
| 39 | W wiScene_Components.h | 784 | `uint32_t _flags = RENDERABLE`（mesh） |
| 40 | W wiScene_Components.h | 786-792 | BLAS_STATE 三态 + `mutable BLAS_state` |
| 41 | W wiScene_Components.h | 836-840 | Delete/CreateRenderData + CreateStreamout/CreateRaytracingRenderData 声明 |
| 42 | W wiScene_Components.h | 1148-1163 | OcclusionResult：occlusionQueries[BUFFERCOUNT] / occlusionHistory / IsOccluded() |
| 43 | W wiScene_Components.h | 1164 | ObjectComponent 起点，alignas(32) |
| 44 | W wiScene_Components.h | 1181-1199 | object 全部序列化字段（_flags…sort_priority） |
| 45 | W wiScene_Components.h | 1184 | meshID（Entity 引用） |
| 46 | W wiScene_Components.h | 1201 | filterMaskDynamic（R） |
| 47 | W wiScene_Components.h | 1203-1208 | lightmap_render/lightmap(Texture) + vb_ao_srv(int) + vb_ao/wetmap(GPUBuffer) |
| 48 | W wiScene_Components.h | 1210-1216 | center/radius/fadeDistance/lod/wetmap_cleared/mesh_blend_required（R） |
| 49 | W wiScene_Components.h | 1218 | `mutable OcclusionResult occlusion` |
| 50 | W wiScene_Components.h | 1221-1222 | mesh_index / sort_bits（注释 "only valid for a single frame"） |
| 51 | W wiScene_Components.h | 1250 | GetFilterMask() = filterMask 位或 filterMaskDynamic |
| 52 | W wiScene_Components.h | 1347 | LightComponent 起点 |
| 53 | W wiScene_Components.h | 1370-1378 | 光照参数（color/intensity/range/cone/radius/length/height） |
| 54 | W wiScene_Components.h | 1380-1383 | cascade_distances / lensFlareNames / forced_shadow_resolution |
| 55 | W wiScene_Components.h | 1388-1392 | position/direction/rotation/scale（R）+ maskTexDescriptor（D） |
| 56 | W wiScene_Components.h | 1474 | CameraComponent 起点 |
| 57 | W wiScene_Components.h | 1500-1510 | View/Projection/VP / frustum / Inv* / jitter / clipPlane / sample_count（R） |
| 58 | W wiScene_Components.h | 1511-1528 | 19 个 int texture_*_index（D 层，整条管线 RT 槽位） |
| 59 | W wiScene_Components.h | 1531-1544 | RenderToTexture：5 张 Texture + entityTiles(GPUBuffer) + visibility(shared_ptr<void>) |
| 60 | W wiScene_Components.cpp | 282-444 | WriteShaderMaterial：打包 + 现场取描述符（:418 纹理、:431 采样器）+ :444 memcpy |
| 61 | W wiScene_Components.cpp | 504-522 | MaterialComponent::CreateRenderData 只做 resourcemanager::Load + SetOutdated（无 GPUBuffer） |
| 62 | W wiScene_Components.cpp | 532-557 | MeshComponent::DeleteRenderData 置空所有 BufferView + BLASes.clear() |
| 63 | W wiScene_Components.cpp | 559-1303 | MeshComponent::CreateRenderData 全函数（745 行） |
| 64 | W wiScene_Components.cpp | 614-663 | 位置格式量化选择（POS16→POS32，1mm 判据；UNORM 时 AABB 至少 0.01） |
| 65 | W wiScene_Components.cpp | 702-750 | bd.size 逐段 align 累加 + GPUBufferDesc 填 |
| 66 | W wiScene_Components.cpp | 846-1214 | init_callback 按 buffer_offset 游标逐段写入并设各 vb_*.offset/size |
| 67 | W wiScene_Components.cpp | 1217-1219 | suballocation 策略原注释（avoid rebinding the index buffer for every mesh…purely offsets） |
| 68 | W wiScene_Components.cpp | 1220-1234 | SuballocateGPUBuffer → CreateBuffer2(..., alias, byte_offset)；失败退化独立 buffer |
| 69 | W wiScene_Components.cpp | 1240-1296 | 17 组 CreateSubresource + GetDescriptorIndex（ib_reorder/ib/vb_pos_wind/…/vb_bou） |
| 70 | W wiScene_Components.cpp | 1300-1368 | CreateStreamoutRenderData（SRV+UAV 双视图，:1335 注释 UAV 不支持 RGB32_F） |
| 71 | W wiScene_Components.cpp | 1369-1427 | CreateRaytracingRenderData，:1376 置 BLAS_state = NEEDS_REBUILD |
| 72 | W wiScene_Components.cpp | 2126-2154 | ObjectComponent::Delete/CreateRenderData（vb_ao R8_UNORM + vb_ao_srv） |
| 73 | W wiScene.cpp | 34 | `void Scene::Update(float dt)` 定义 |
| 74 | W wiScene.cpp | 37 | cpu_gpu_mapped_resource_index = device->GetBufferIndex() |
| 75 | W wiScene.cpp | 48/293/297/414/418/434/436/444 | 各 Run*UpdateSystem 调用点（脚本/变换/层级/网格/材质/对象/相机/光照） |
| 76 | W wiScene.cpp | 127-151 | instance buffer 容量不足重建（size*2，双缓冲一起建） |
| 77 | W wiScene.cpp | 153 | instanceArrayMapped = instanceUploadBuffer[idx].mapped_data |
| 78 | W wiScene.cpp | 163-184 | material buffer 重建 |
| 79 | W wiScene.cpp | 195 | materialArrayMapped = ... |
| 80 | W wiScene.cpp | 226-247 | queryHeap / queryResultBuffer 创建 + queryheap_idx 赋值 |
| 81 | W wiScene.cpp | 251-283 | dt>0 分支：lightmap 扫描 + geometry/skinning 分配 + instance 数组清零（:277-283） |
| 82 | W wiScene.cpp | 266-272 | geometryAllocator.store(0) + mesh.geometryOffset = fetch_add(subsets.size()) |
| 83 | W wiScene.cpp | 338-361 | geometry buffer 重建 |
| 84 | W wiScene.cpp | 378 | geometryArrayMapped = ... |
| 85 | W wiScene.cpp | 927-939 | shaderscene.instancebuffer/geometrybuffer/materialbuffer = GetDescriptorIndex(...)（UMA vs 非 UMA 两支） |
| 86 | W wiScene.cpp | 1084-1127 | async_buffer_uploads（:1085）+ 4 条 CopyCommand + CopyBufferAsync（:1127）；:1084 注释说明双缓冲必要性 |
| 87 | W wiScene.cpp | 1130-1157 | Scene::Clear() 清组件库与 CPU 数组，未清三件套 |
| 88 | W wiScene.cpp | 1568/1815/1874/1954 | 四处 mesh/material.CreateRenderData() 调用 |
| 89 | W wiScene.cpp | 2961-2968 | RunTransformUpdateSystem 无条件 UpdateTransform() |
| 90 | W wiScene.cpp | 2969-3030 | RunHierarchyUpdateSystem 沿 parentID 链累乘局部矩阵（:3000-3022） |
| 91 | W wiScene.cpp | 4106-4296 | RunMeshUpdateSystem 全函数 |
| 92 | W wiScene.cpp | 4112-4115 | so_pos / so_pre 每帧 swap |
| 93 | W wiScene.cpp | 4140-4238 | 逐 subset 填 ShaderGeometry 并 memcpy（:4237） |
| 94 | W wiScene.cpp | 4213-4221 | subset.materialIndex = materials.GetIndex(subset.materialID)（每帧重算） |
| 95 | W wiScene.cpp | 4245-4287 | BLAS 惰性创建 + 依材质改 geometry.flags + 置 BLAS_state（:4276/4280-4283） |
| 96 | W wiScene.cpp | 4298-4391 | RunMaterialUpdateSystem 全函数 |
| 97 | W wiScene.cpp | 4305-4313 | texAnimElapsedTime 累加 → 改 texMulAdd.z/.w → SetDirty() |
| 98 | W wiScene.cpp | 4337-4352 | if (IsDirty()) SetDirty(false) 后无条件 WriteShaderMaterial(:4352) |
| 99 | W wiScene.cpp | 4343-4351 | cached_wrapSampler 每帧重算 |
| 100 | W wiScene.cpp | 4558-4830 | RunObjectUpdateSystem 全函数 |
| 101 | W wiScene.cpp | 4574-4576 | aabb_objects resize + UpdateOcclusionResult |
| 102 | W wiScene.cpp | 4589-4594 | aabb=AABB() / filterMaskDynamic=0 / sort_bits={} / fadeDistance=draw_distance |
| 103 | W wiScene.cpp | 4609-4610 | object.mesh_index = meshes.GetIndex(object.meshID) |
| 104 | W wiScene.cpp | 4672 | object.lod = ComputeObjectLODForView(...) |
| 105 | W wiScene.cpp | 4675-4729 | SortBits union 定义 + 各位域赋值 + object.sort_bits 写下 |
| 106 | W wiScene.cpp | 4738-4749 | inst.transformRaw + UNORM 反量化 R = aabb.getUnormRemapMatrix() |
| 107 | W wiScene.cpp | 4765 | inst.lightmap = GetDescriptorIndex(&object.lightmap, SRV) |
| 108 | W wiScene.cpp | 4769-4782 | inst.uid/layerMask/color/emissive/baseGeometryOffset/geometryOffset/fadeDistance/center/radius/vb_ao/vb_wetmap |
| 109 | W wiScene.cpp | 4784 | memcpy(instanceArrayMapped + jobIndex, &inst, sizeof(inst)) |
| 110 | W wiScene.cpp | 4924-4937 | RunCameraUpdateSystem：TransformCamera + UpdateCamera()（无条件） |
| 111 | W wiScene.cpp | 5071-5120 | RunLightUpdateSystem：sun_index 扫描 + 从世界矩阵分解 position/direction/rotation/scale |
| 112 | W wiScene.cpp | 9052-9101 | ComputeObjectLODForView：w 保护 + UV 夹取 + lod = clamp(log2(1/maxdim)+bias) |
| 113 | W wiScene.cpp | 9967-9989 | UpdateOcclusionResult：历史左移、查询读取、失效查询槽 |
| 114 | W wiECS.h | 25-32 | Entity = uint64_t / INVALID_ENTITY = 0 / 全局原子发号 |
| 115 | W wiECS.h | 300-321 | Create：断言唯一、push_back 到尾 |
| 116 | W wiECS.h | 324-344 | Remove：swap-and-pop 打乱顺序 |
| 117 | W wiECS.h | 347-372 | Remove_KeepSorted：保序 O(n) |
| 118 | W wiECS.h | 405-457 | GetComponent / GetIndex / GetCount / GetEntity / operator[] / GetData |
| 119 | W wiECS.h | 461-470 | components / entities 平行数组 + LookupTable lookup |
| 120 | W wiECS.h | 709-760 | ComponentLibrary：unordered_map<string, LibraryEntry{manager,version}> + Register/Get/GetVersion |
| 121 | W wiRenderer.h | 75-85 | BufferSuballocation{alias, allocation} + SuballocateGPUBuffer/UpdateGPUSuballocator 声明与注释 |
| 122 | W wiRenderer.h | 124-165 | Visibility：layerMask/scene/camera/flags + visible* 数组 + 原子计数器 |
| 123 | W wiRenderer.h | 211 | void UpdateVisibility(Visibility&) 声明 |
| 124 | W wiRenderer.cpp | 207-302 | RenderBatch（16B）：meshIndex/instanceIndex/distance/camera_mask/lod_override/sort_bits + 比较算子 |
| 125 | W wiRenderer.cpp | 305-338 | RenderQueue{batches} + add/sort_opaque/sort_transparent；:338 thread_local 实例 |
| 126 | W wiRenderer.cpp | 2782 | GPUSubAllocator::blocksize = 256MB |
| 127 | W wiRenderer.cpp | 2795-2848 | SuballocateGPUBuffer 实现（半块上限、遍历块、新建块 ALIASING_BUFFER/NO_DEFAULT_DESCRIPTORS） |
| 128 | W wiRenderer.cpp | 2851-2862 | UpdateGPUSuballocator：update_deferred_release(FRAMECOUNT, BUFFERCOUNT) + 空块销毁 |
| 129 | W wiRenderer.cpp | 3137-3200 | RenderMeshes 头：flags、AllocateGPU(sizeof(ShaderMeshInstancePointer)*n)（:3170）、InstancedBatch |
| 130 | W wiRenderer.cpp | 3288-3320 | ObjectRenderingVariant 构造（:3310 shadertype） |
| 131 | W wiRenderer.cpp | 3333-3334 | PSO 未就绪则 continue（跳帧） |
| 132 | W wiRenderer.cpp | 3355-3361 | prev_ib_internal/prev_ibformat 比较后才 BindIndexBuffer |
| 133 | W wiRenderer.cpp | 3372-3380 | ObjectPushConstants push 填充 |
| 134 | W wiRenderer.cpp | 3385-3394 | indexOffset 三重来源计算（suballoc vs 独立） |
| 135 | W wiRenderer.cpp | 3414-3418 | DispatchMesh 或 DrawIndexedInstanced(subset.indexCount, batch.instanceCount, indexOffset, 0, 0, cmd) |
| 136 | W wiRenderer.cpp | 3425-3444 | 批刷新条件（新 mesh / stencil / lod） |
| 137 | W wiRenderer.cpp | 3619-3991 | UpdateVisibility 全函数（:3727 resize、:3741 判据、:3744 组内压缩、:3801-3806 合并、:3991 收缩） |
| 138 | W wiRenderer.cpp | 4179 | UpdatePerFrameData 定义起点 |
| 139 | W wiRenderer.cpp | 4421 | frameCB.scene = vis.scene->shaderscene |
| 140 | W wiRenderer.cpp | 5700-5745 | UpdateRaytracingAccelerationStructures：TLAS 复制 + BLAS switch（:5729-5737）+ 置 COMPLETE（:5743） |
| 141 | W wiRenderer.cpp | 5815 | scene.acceleration_structure_update_requested = false |
| 142 | W wiRenderer.cpp | 5822-5834 | OcclusionCulling_Reset：QueryReset 全堆 |
| 143 | W wiRenderer.cpp | 5838-5900 | OcclusionCulling_Render：遍历 vis.visibleObjects（:5853）画包围盒查询 |
| 144 | W wiRenderer.cpp | 7223-7243 | DrawScene 遍历 visibleObjects → renderQueue.add(mesh_index, instanceIndex, distance, sort_bits) |
| 145 | W wiRenderer.cpp | 8717 | DrawWireframeOverlay 判 visibleObjects.empty() |
| 146 | W wiGraphicsDevice.h | 72 | static constexpr uint32_t BUFFERCOUNT = 2 |
| 147 | W wiGraphicsDevice.h | 145-147 | GetBufferCount()=2 / GetBufferIndex() = FRAMECOUNT % BUFFERCOUNT |
| 148 | W wiGraphicsDevice.h | 45 | BINDLESS_SAMPLER_CAPACITY = 256（"addressable by 8 bits"） |
| 149 | W wiGraphicsDevice.h | 93 | CreateBuffer2(desc, init_callback, buffer, alias, alias_offset) |
| 150 | W wiGraphicsDevice.h | 104-110 | CreateSubresource（Texture/Buffer 两版）+ GetDescriptorIndex（资源/采样器两版） |
| 151 | W wiGraphicsDevice.h | 178 | GetMinOffsetAlignment(desc) |
| 152 | W wiGraphics.h | 862-891 | GPUResource{internal_state, mapped_data, mapped_size, type} + IsValid() |
| 153 | W wiGraphics.h | 893 | struct GPUBuffer final : GPUResource |
| 154 | W wiGraphics.h | 647-655 | GPUBufferDesc{size,stride,alignment,usage,format,bind_flags,…} |
| 155 | W wiGraphicsDevice_DX12.h | 110-132 | CopyAllocator：独立 queue（"separate copy queue to reduce interference"）+ CopyCMD{fence,fenceValue}；:132 mutable copyAllocator |
| 156 | W wiGraphicsDevice_DX12.h | 560-640 | 10 个 destroyer_* 队列 + Update(FRAMECOUNT,BUFFERCOUNT) 判据 front.frame + BUFFERCOUNT < FRAMECOUNT |
| 157 | W wiGraphicsDevice_DX12.cpp | 5997-6020 | CopyBufferAsync：copyAllocator.allocate → 逐条 CopyBufferRegion → copyAllocator.submit |
| 158 | W shaders/ShaderInterop_Renderer.h | 9-13 | ShaderScene 头 4 字段即 4 个 bindless 索引（instancebuffer/geometrybuffer/materialbuffer/meshletbuffer） |
| 159 | W shaders/ShaderInterop_Renderer.h | 540-556 | ShaderGeometry 的 8 个 int bindless 索引 + materialIndex/meshletOffset/indexOffset/indexCount/flags |
| 160 | W shaders/ShaderInterop_Renderer.h | 616 | static_assert(sizeof(ShaderGeometry) == 128) |
| 161 | W shaders/ShaderInterop_Renderer.h | 714-735 | ShaderMeshInstance 字段（uid/flags/layerMask/geometryOffset/transform{,Prev,Raw}/vb_ao/vb_wetmap/lightmap） |
| 162 | W shaders/ShaderInterop_Renderer.h | 799 | static_assert(sizeof(ShaderMeshInstance) == 256) |
| 163 | W shaders/ShaderInterop_Renderer.h | 803-830 | ShaderMeshInstancePointer：24 位 instanceIndex + 4 位 camera + 4 位 dither |
| 164 | W shaders/ShaderInterop_Renderer.h | 835-844 | ObjectPushConstants{geometryIndex, materialIndex:24, samplerIndex:8, instances, instance_offset} + 16B 断言 |
| 165 | W shaders/ShaderInterop_Renderer.h | 393/513 | ShaderMaterial 定义 + 384B 断言 |
| 166 | W shaders/globals.hlsli | 586-589 | load_instance/load_geometry/load_material/load_meshlet 宏（两级间接） |
| 167 | W shaders/objectHF.hlsli | 53-59 | PUSHCONSTANT(push, ObjectPushConstants) + GetMesh()/GetMaterial() 宏 |
| 168 | W shaders/objectHF.hlsli | 176-223 | GetInstancePointer()（:178）+ GetInstance()（:217）两级间接 |
| 169 | W wiRenderPath3D.cpp | 371 | scene->Update(dt * GetGameSpeed()) |
| 170 | W wiRenderPath3D.cpp | 367 | scene->SetAccelerationStructureUpdateRequested(true) |
| 171 | W wiRenderPath3D.cpp | 385-412 | visibility_main 填参（:385-391）→ UpdateVisibility（:393）→ 反射相机（:396-410） |
| 172 | P Engine/05_Scene/Scene.md | 1-21 | 现状职责：Entity/Transform/MeshRenderer/Light/Scene管理/BoundingBox/Culling数据；禁止「GPU命令录制」 |
| 173 | P Engine/05_Scene/Model/Model.h | 31-41 | Mesh{vbOffset,vbSize,vbDepthOffset/Size,ibOffset,ibSize,ibFormat,vbStride,vbDepthStride}（注释即 BufferLocation - Buffer.GpuVirtualAddress） |
| 174 | P Engine/05_Scene/Model/Model.h | 43-46 | meshCBV / materialSlotIdx / srvTable（裸描述符堆偏移） |
| 175 | P Engine/05_Scene/Model/Model.h | 50-57 | Draw{primCount,startIndex,baseVertex} |
| 176 | P Engine/05_Scene/Model/Model.h | 61-72 | GraphNode{xform,rotation,scale,matrixIdx:28,hasSibling/Children/staleMatrix/skeletonRoot} |
| 177 | P Engine/05_Scene/Model/Model.h | 84-92 | m_DataBuffer / m_MaterialConstants / m_MeshData / m_SceneGraph / textures / m_Materials |
| 178 | P Engine/05_Scene/Model/Model.h | 98-137 | ModelInstance：m_MeshConstantsCPU(UploadBuffer) / m_MeshConstantsGPU(ByteAddressBuffer) / m_IsDirty(:136) |
| 179 | P Engine/05_Scene/Model/Model.h | 3-12 | include 了 CommandContext.h / GpuBuffer.h / UploadBuffer.h / PipelineDesc.h / Material.h（WP1 的三条反向边 + 越级直连） |
| 180 | P Engine/04_Renderer/Material/Material.h | 23-34 | Material{Flags, SRVTable, SamplerTable, D3D12_GPU_VIRTUAL_ADDRESS MaterialCBV}（数据与 GPU 绑定混放） |
| 181 | P Engine/04_Renderer/Material/Material.h | 39-47 | MaterialConstantData（未对齐镜像） |
| 182 | P Engine/04_Renderer/Renderer/Base/BaseRenderer.h | 72-78 | RenderFrameDesc{Camera, std::vector<ModelInstance*> Models, delatT} |
| 183 | P Engine/04_Renderer/Renderer/Base/BaseRenderer.h | 116-127 | MeshSorter::SortKey（objectIdx:16/psoIdx:12/key:32/passID:4） |
| 184 | P Engine/04_Renderer/Renderer/Base/BaseRenderer.h | 131-137 | SortObject{mesh*, material*, D3D12_GPU_VIRTUAL_ADDRESS meshCBV, bufferPtr} |
| 185 | P Engine/04_Renderer/Renderer/Base/BaseRenderer.h | 46-47 | extern DescriptorHeap s_TextureHeap / s_SamplerHeap |
| 186 | P Engine/04_Renderer/Renderer/Base/BaseRenderer.h | 54-63 | enum RootBindings + kNumRootBindings |
| 187 | P Engine/04_Renderer/Renderer/Base/BaseRenderer.cpp | 41-110 | MeshSorter::AddMesh：构 PipelineDesc → PipelineStateCache::GetPipelineIndex（PSO 解析在排序期） |
| 188 | P Engine/05_Scene/Lighting/LightingSystem.h | 31-48 | LightData：DirectionIntensity/DirectionalColor/PointLightPositionRange/PointLightColorIntensity |
| 189 | P Engine/05_Scene/Lighting/LightingSystem.h | 50/66-67 | MaxLights = 1；m_LightCPUBuffer + m_LightGPUBuffer（场景层直接持 GPU 结构缓冲） |
| 190 | P Engine/02_RHI/Descriptor/DescriptorHeap.h | 44-60 | DescriptorHandle（CPU+GPU 双句柄封装，已存在可复用） |
| 191 | P Engine/02_RHI/Memory/LinearAllocator.h | 31-45 | LinearAllocatorType{kGpuExclusive,kCpuWritable} + 页大小 64K/2MB + DynAlloc{Buffer,Offset,Size} |
| 192 | P Engine/03_AssetSystem/Importers/Model/ModelConvert.cpp | 276-286 | m_MaterialConstants.resize(numMaterials) + 逐材质填 MaterialConstantData |
| 193 | P Engine/03_AssetSystem/Importers/Model/ModelConvert.cpp | 444-470 | 写盘：header.numMaterials + m_MaterialConstants.data() 连续写 |
| 194 | W wiScene_Components.cpp | 563 | GraphicsDevice* device = wi::graphics::GetDevice()（组件自取全局设备） |
| 195 | W wiScene_Components.cpp | 2131-2154 | ObjectComponent::CreateRenderData：CreateBuffer2(&desc, fill_ao, &vb_ao) + GetDescriptorIndex |
| 196 | W wiScene.cpp | 4602-4613 | object.wetmap 惰性创建/销毁 + SetName("wetmap") |

**索引合计：196 条**（远超 40 条下限）。

---

## 附录 A：与第一轮（01C）的关系

本文**不重复** 01C 已论证的内容，只补实现细节与归属判定：
- 01C §1.1/§1.2（ECS 容器结构）→ 本文 §2.0 补 `GetData/operator[]/GetIndex` 精确行号与「swap-and-pop 对 GPU 数组槽位的影响」。
- 01C §2.1（组件既存 CPU 又存 GPU）→ 本文 §2.1 给出**逐字段四桶归属**（C/G/D/R），新增产出。
- 01C §2.2（suballocation 注释）→ 本文 §3.2 补完整分配器实现（块大小、半块上限、延迟释放）。
- 01C §2.3（mapped 三件套）→ 本文 §4.1 补「怎么翻」与 UMA 分支、增长策略、清零逻辑。
- 01C §7（脏机制）→ 本文 §5.3 给出「三种脏语义分级」结论。
- 01C §9.4（逐条适用性）→ 本文 §6 给出 13 项判定与替代方案（新增 #6/#10/#11）。

## 附录 B：G3 抽查记录

见 `docs/AIWorkLogs/08_RB3_调查日志.md` 的「G3 抽查」一节（3 条，均回源命中）。
