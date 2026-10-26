# Engine/03_AssetSystem + Engine/05_Scene 深度审计

> 范围: `Engine/03_AssetSystem` + `Engine/05_Scene` + 关联的 `Engine/04_Renderer` 材质/PSO 侧。
> 方式: 逐文件通读 + grep 追调用链。结论均给 文件:行号。未读到的标注 UNKNOWN，不臆测。
> 状态: DONE / PARTIAL / ORPHAN / STUB / MISSING / BUG / UNKNOWN

---

## 0. 结论速览(按影响排序)

1. **纹理槽位两套枚举顺序不同(确定性 bug)**：`glTF.h:138` 是 (BaseColor, MetallicRoughness, Occlusion, Emissive, Normal)，`ConstantBuffers.h:18-26` 是 (BaseColor, Normal, Emissive, Occlusion, MetallicRoughness)。`ModelConvert.cpp:343-351` 用的是全局那套去索引 glTF 顺序的数组 → 5 张图的 sRGB/alpha 编译选项整体错配。见 §5.2。
2. **.ty 模型缓存被无条件重建**：demo 硬编码 `forceRebuild = true`(`Demos/DeferredApp/DeferredApp.cpp:77`) → 每次启动全量重解析 glTF + 重算索引 + 重转纹理。见 §2.2。
3. **资产路径全是硬编码盘符绝对路径**，共 11 处；其中 1 处在引擎层而非 demo。见 §3.1。
4. **1920x1080 硬编码在 PBR 位置重建里**：`LightingPS.hlsl:85`，窗口一改就错位。
5. **sRGB 语义链靠隐式约定**：BaseColor 以 Linear(非 _SRGB) 格式编译；Emissive 用 _SRGB 却写进 UNORM GBuffer。见 §6。
6. **当前 Assets 目录为空**(只有 Skybox)，demo 引用的模型文件不存在 → 无法做运行时验证，一切实际画面结论为 UNKNOWN。见 §3.3。

---

## 1. glTF 管线 (问题 a)

### 1.1 运行时直接解析，无独立中间格式
- `glTF.cpp:622-755` `Asset::Parse`：.glb 手工解析 GLBHeader + Chunk0(JSON) + Chunk1(BIN)(`631-688`)；.gltf 走 `ReadFileSync`(`695`)。状态 DONE。
- 顶点属性枚举 `glTF.h:147`：`kPosition,kNormal,kTangent,kTexcoord0,kTexcoord1,kColor0,kJoints0,kWeights0,kNumAttribs`。

### 1.2 顶点属性：定义 8 种，实际绑定 6 种，进 VB 的只有 4 种
- 实际 `FindAttribute` 调用 `glTF.cpp:207-212`：POSITION / NORMAL / TANGENT / TEXCOORD_0 / TEXCOORD_1 / COLOR_0。
- JOINTS_0 / WEIGHTS_0 从未被查找 → 蒙皮 **MISSING**(`glTF.h:147` 有枚举，`glTF.cpp:207-212` 无调用)。
- COLOR_0 解析但不进输出：`MeshConvert.cpp:157-178` 的 InputElements 与 `MeshConvert.cpp:308-327` 的 OutputElements 均无 COLOR → **ORPHAN**。
- `glTF::Mesh::skin`(`glTF.h:163`)全仓库无赋值 → **STUB**。

### 1.3 PBR 材质字段读取清单 `glTF.cpp:276-369`

| glTF 字段 | 读取 | 行号 | 下游 |
|---|---|---|---|
| `baseColorFactor` | 是 | 323-324 | DONE 进 CB |
| `metallicFactor` | 是 | 326-327 | DONE |
| `roughnessFactor` | 是 | 329-330 | DONE |
| `baseColorTexture` | 是 | 332-334 | DONE |
| `metallicRoughnessTexture` | 是 | 336-338 | DONE |
| `doubleSided` | 是 | 342-343 | DONE(→ CullMode) |
| `alphaMode` BLEND/MASK | 是 | 292-299 | DONE |
| `alphaCutoff` | 是 | 301-305 | **ORPHAN**：写进 alphaCutoff:16 位域，但 `ModelConvert.cpp:276-294` 未把它落进 MaterialConstantData.flags；HLSL 里也没有 alphaRef 声明 |
| `emissiveFactor` | 是 | 350-351 | DONE |
| `emissiveTexture` | 是 | 359-361 | DONE |
| `occlusionTexture` | 是 | 354-356 | DONE(但 strength 未读 → 恒 1.0) |
| `normalTexture` | 是 | 363-365 | DONE |
| normalTexture.scale | **键名读错** | 346-347 | **BUG**：查的是 `thisMaterial.find("normalTextureScale")`，规范里 scale 在 normalTexture.scale 子对象内(对比 363 行写法)，故恒为初值 1.0(`290`) |
| KHR_materials_* 扩展 | 否 | — | **MISSING**(全文无 extensions 解析) |
| `samplers[].magFilter/minFilter` | 否(被注释) | 572-583 | **PARTIAL**：强制 ANISOTROPIC(`566`) |
| `texture.texCoord` 选 UV0/UV1 | 读到但不用 | 262-274 | **ORPHAN**：见 §5.8 |

**支持子集** = 金属粗糙度 PBR 五贴图 + alphaMode + doubleSided + emissiveFactor。**不支持** = 蒙皮、顶点色、KHR 扩展、多 UV 集生效、alphaCutoff 生效、occlusion strength、normal scale(键名错)、sampler 过滤。

---

## 2. 离线转换与强制重建 (问题 b)

### 2.1 产物格式
- 模型：自定义二进制 `.ty`。版本 `ModelLoader.h:12` `CURRENT_MINI_FILE_VERSION 2`；魔数 `"TY"`(`ModelConvert.cpp:415`)；布局注释 `ModelLoader.cpp:359-376`。写 `ModelConvert.cpp:408-454`，读 `ModelLoader.cpp:189-357`。
- 纹理：一律转 `.dds`(`TextureConvert.cpp:246`)，加载时只认 DDS(`ModelLoader.cpp:73-75` 把扩展名换成 .dds 再 LoadDDSFromFile)。

### 2.2 存在无条件重建，且是 demo 里写死的常量
证据链：
1. `Demos/DeferredApp/DeferredApp.cpp:77` `bool forceRebuild = true;` —— 写死，不是配置/命令行/宏。
2. `DeferredApp.cpp:78-79` 两路 `LoadModel(..., forceRebuild)` 直接吃它。
3. `ModelLoader.cpp:217` `bool needBuild = forceRebuild;` → 恒 true。
4. `ModelLoader.cpp:242` `if (needBuild)` → `glTF::Asset asset(filePath)`(258) 全量解析 + `BuildModel`(260) + `SaveModel`(270) 覆写 .ty。
- 状态 **BUG**。注意 `ModelLoader.cpp:220` 的 mtime 比较逻辑本身是对的(`miniFileMissing || sourceFileStat.st_mtime > miniFileStat.st_mtime`)，只是被上层 true 短路。改成 false 即可生效。
- 另两个 demo：`Demos/ForwardApp/Tooiyume.cpp`、`Demos/RayTracingApp/RayTracingApp.cpp` 也有同名变量，其取值未逐行验证 → **UNKNOWN**。

### 2.3 纹理侧：没有无条件重建，但有 sRGB 缓存键缺陷
- `TextureConvert.cpp:9-30` `CompileTextureOnDemand`：仅当 `ddsFileMissing || ddsFileStat.st_mtime < srcFileStat.st_mtime` 才重转(`25`)。其文件 mtime 判定逻辑 DONE。
- **BUG**：DDS 的缓存键只有路径 + mtime，不含 flags。同一张图先以 `kSRGB=0` 转了 DDS，之后材质改成需要 sRGB，源图 mtime 未变 → 不重转。
- **BUG**：运行期 `ModelLoader.cpp:75` `LoadDDSFromFile(ddsFile)` 不传 forceSRGB(默认 false，`TextureManager.h:36`)，`TextureManager.cpp:132-133` 里 `CreateDDSTextureFromMemory(..., 0, forceSRGB, ...)` 的第 5 参恒 false。→ 运行期 SRV 永不带 sRGB，只剩 DDS 文件格式这一条路。

### 2.4 纹理 flags 传递链是断的 → 贴图完全没压缩
- 收集：`ModelConvert.cpp:258` `std::map<string,uint8_t> textureOptions`；写入 `343-351`。
- `TextureOptions(sRGB, alpha, flip)`(`TextureConvert.h:23-26`)只能产出 `kSRGB|kPreserveAlpha|kFlipVertical`。
- **BUG**：全仓库 grep，`kDefaultBC` / `kQualityBC` / `kNormalMap` 仅在 `EnvironmentLightingManager.cpp:173`(HDR)被传入；材质路径从未传 → `TextureConvert.cpp:223` `bBlockCompress=false` → 所有材质贴图不压缩，恒为 RGBA8(`TextureConvert.cpp:167`)，显存是 BC7/BC3 的 4~8 倍。

---

## 3. 资产路径 (问题 c)

### 3.1 硬编码盘符绝对路径(共 11 处)
| 文件:行 | 内容 |
|---|---|
| `Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp:40` | `constexpr wchar_t HDRtexture[] = L"D:/.../grasslands_sunset_2k.dds"` ← 引擎层硬编码，最严重 |
| `Demos/DeferredApp/DeferredApp.cpp:55-64` | 7 个模型绝对路径 |
| `Demos/ForwardApp/Tooiyume.cpp:53-58` | 5 个模型绝对路径 |
| `Demos/RayTracingApp/RayTracingApp.cpp:57-62` | 5 个模型绝对路径 |
- 状态 **BUG**。换机/换盘直接返回 nullptr(`ModelLoader.cpp:211-215`)。

### 3.2 资源根：没有，完全依赖传入路径本身
- `glTF.cpp:715` `m_basePath = Utility::GetBasePath(filepath)`；`Utility.cpp` 的 `GetBasePath` 只做 `rfind('/')` 截断，不拼 cwd。
- `ModelLoader.cpp:285` 同上。
- `TextureManager` 的 `s_RootPath` 在 `DeferredRenderer.cpp:85` 初始化成空串，`TextureManager.cpp:97` 拼 `s_RootPath + fileName`；因为传入的已是绝对路径才恰好可用。状态 **MISSING**(无资源根概念)。

### 3.3 Assets 目录当前是空的 → 无法运行验证
- `ls Assets` 只有 Skybox；`find Assets/Model -name "*.ty"` = 0、`*.dds` = 0、`*.png|*.jpg` = 0。
- demo 引用的 `Assets/Model/Merlin/merlin.gltf` 等不存在 → `ModelLoader.cpp:211-215` 打印 Error 并 return nullptr。
- 推论：本次审计无法做任何运行时验证，凡涉及实际渲染结果的结论一律 **UNKNOWN**。

---

## 4. 顶点布局三方对照 (问题 d)

| Semantic | 导入期(`MeshConvert.cpp:157-178`) | 烘焙产物(`MeshConvert.cpp:308-327`) | 运行期 PSO(`DeferredRenderer.cpp:509-519`) | 一致 |
|---|---|---|---|---|
| POSITION | R32G32B32_FLOAT(158) | R32G32B32_FLOAT(311) | R32G32B32_FLOAT(511) | 是 |
| NORMAL | R32G32B32_FLOAT(159-162) | R10G10B10A2_UNORM(312) | R10G10B10A2_UNORM(513) | 是 |
| TANGENT | R32G32B32A32_FLOAT(163-166) | R10G10B10A2_UNORM(315) | R10G10B10A2_UNORM(515) | 是 |
| TEXCOORD0 | AccessorFormat(167-172) | R16G16_FLOAT(320) | R16G16_FLOAT(517) | 是 |
| TEXCOORD1 | AccessorFormat(173-178) | R16G16_FLOAT(325) | R16G16_FLOAT(519) | 是 |

- HLSL `StaticMeshVS.hlsl:23-29` 的 `MeshVertex` 与运行期 PSO 匹配(UV1 未声明，多余输入合法)。状态 **DONE**。

### 4.1 **BUG：UNORM 法线/切线的 double-scale 还原不一致**
- 烘焙用 x2bias=true 把 [-1,1] 映射到 [0,1] 存 UNORM(`MeshConvert.cpp:343,345`)。
- Deferred VS 第 49-50 行还原了 Normal 和 Tangent，但 56 行又用未还原的 `vsInput.Tangent.xyz`：
  `vsOutput.TangentWS = float4(normalize(mul(vsInput.Tangent.xyz, (float3x3)WorldIT)), vsInput.Tangent.w);`(`StaticMeshVS.hlsl:56`)
- → 切线的 xyz 仍是 [0,1] 值，TBN 错误，法线贴图方向错。同一个 bug 也在 `DefaultVS.hlsl:65`；且 `DefaultVS.hlsl:55` 的 normal 根本没做 *2-1(Deferred 做了)，两套 shader 行为不一致。状态 **BUG**。

### 4.2 **BUG 风险：DepthVB 的 UV 选择只看 baseColorUV**
- `MeshConvert.cpp:356-373`：alphaTest 时给 DepthVB 加 TEXCOORD0，取值 `material.baseColorUV ? texcoord1.get() : texcoord0.get()`(`372`)。若选到 UV1 而网格没有 TEXCOORD_1，`texcoord1.get()` 为 nullptr → 行为 **UNKNOWN**。

---

## 5. 材质 → GPU 三方对照 (问题 e)

### 5.1 绑定槽位对照(全部一致)
| 通道 | RootBindings(`BaseRenderer.h:54-64`) | RootSignature(`DeferredRenderer.cpp:411-420`) | HLSL |
|---|---|---|---|
| Mesh CB | `kMeshConstants` | `InitAsConstantBuffer(0, VERTEX)`(413) | `b0`(`StaticMeshVS.hlsl:9`) |
| Material CB | `kMaterialConstants` | `InitAsConstantBuffer(0, PIXEL)`(414) | `b0`(`GBufferPS.hlsl:15`) |
| Material SRV | `kMaterialSRVs` | `Range(SRV, 0, 10)`(415) | `t0..t4` |
| Material Sampler | `kMaterialSamplers` | `Range(SAMPLER, 0, 10)`(416) | `s0..s4` |
| Common SRV | `kCommonSRVs` | `Range(SRV, 10, 10)`(417) | 无使用者 |
| Common CBV | `kCommonCBV` | `InitAsConstantBuffer(1)`(418) | `b1`(`GBufferPS.hlsl:27`) |
- 状态 **DONE**。

### 5.2 **BUG(最严重)：两套纹理槽枚举顺序不同**
| 来源 | 顺序 |
|---|---|
| `glTF.h:138` `glTF::Material` | kBaseColor=0, kMetallicRoughness=1, kOcclusion=2, kEmissive=3, kNormal=4 |
| `ConstantBuffers.h:18-26` 全局匿名 enum | kBaseColor=0, kNormal=1, kEmissive=2, kOcclusion=3, kMetallicRoughness=4 |
| HLSL 实际 x 绑定(`GBufferPS.hlsl:3-7`) | t0=baseColor, t1=metallicRoughness, t2=occlusion, t3=emissive, t4=normal ← 与 glTF 顺序一致 |

分析：
- `ModelConvert.cpp:271` 的 srcMat 是 `glTF::Material`，`srcMat.textures[ti]` 按 glTF 顺序。
- `ModelConvert.cpp:309-337` 的循环把 stringIdx[ti] 按 glTF 顺序原样落盘；运行期 `ModelLoader.cpp:114-133` 按 j=0..4 直接拷进 SRV 表；HLSL 按 glTF 顺序采样 → 这条链自洽，SRV 槽没串。
- 但 `ModelConvert.cpp:343-351` 的 `SetTextureOptions(textureOptions, srcMat.textures[kMetallicRoughness], ...)` 用的是全局 k 值去索引 glTF 顺序数组：
  - `kMetallicRoughness`(全局=4) 实际取到 glTF 的 Normal 贴图；
  - `kEmissive`(全局=2) 实际取到 glTF 的 Occlusion；
  - `kNormal`(全局=1) 实际取到 glTF 的 MetallicRoughness。
- 后果：5 张贴图的 kSRGB / kPreserveAlpha 编译选项整体错配 → 该 sRGB 的没 sRGB、该保 alpha 的没保。状态 **BUG**。
- 修法：改用限定名 `glTF::Material::kBaseColor` 等，或删掉 `glTF.h:138` 统一到 `ConstantBuffers.h:18-26`。

### 5.3 CB 结构体 C++ ↔ HLSL 逐字段对照
| C++ `MaterialConstantData`(`Material.h:39-47`) | offset | HLSL(`GBufferPS.hlsl:15-25`) | 一致 |
|---|---|---|---|
| baseColorFactor[4] | 0 | float4 baseColorFactor | 是 |
| emissiveFactor[3] | 16 | float3 emissiveFactor | 是 |
| normalTextureScale | 28 | float normalTextureScale | 是 |
| metallicFactor | 32 | float metallicFactor | 是 |
| roughnessFactor | 36 | float roughnessFactor | 是 |
| flags | 40 | uint flags | 是 |

- 32 字节，正好是 `alignas(256) MaterialConstants`(`ConstantBuffers.h:29-58`) 的前缀；`ModelLoader.cpp:320-325` 逐条 read + memcpy 低 32 字节 → **DONE**。
- **BUG**：`ConstantBuffers.h:55` 有 alphaRef:16 位域，但 HLSL `GBufferPS.hlsl:24` 只有 `uint flags`、没有 alphaRef 也没有 alpha test 分支。alpha test 仅靠 DepthVB 预写深度模拟(见 §4.2)。状态 **PARTIAL/BUG**。

### 5.4 材质状态位映射(一致)
- `ModelLoader.cpp:35-45` 用 1<<7 / 1<<6 / 1<<5 取 AlphaBlend / AlphaTest / DoubleSided。
- `ConstantBuffers.h:42-51` 位域：baseColorUV:0 … normalUV:4, twoSided:5, alphaTest:6, alphaBlend:7 → 完全对上，状态 **DONE**。

### 5.5 采样器 addressModes 打包/解包
- 打包 `ModelConvert.cpp:301-337`，每槽 4 bit；解包 `ModelLoader.cpp:157-159`(`addressModes & 0xF` 后右移 4) → 语义一致。
- **PARTIAL**：`ModelLoader.cpp:27-30` 注释写 0:Wrap,1:Mirror,2:Clamp，但存的是 `D3D12_TEXTURE_ADDRESS_MODE` 原值(WRAP=1,MIRROR=2,CLAMP=3,BORDER=4) → 注释误导；且 BORDER=4 会溢出 2 bit。默认 0x5 恰好解成 (WRAP,WRAP) 能工作。

### 5.6 **BUG：默认纹理槽顺序错位，AO 恒为 0**
- `ModelLoader.cpp:97-104` `DefaultTextures[]` 按 (baseColor, emissive, normal, metallic, roughness) 语义填，但数组下标 0..4 对应的是 ConstantBuffers.h 顺序 (BaseColor, Normal, Emissive, Occlusion, MR)。
- 于是槽 3 = Occlusion 拿到 `kBlackTransparent2D` → 没有 AO 贴图的模型 ao=0 → `LightingPS.hlsl:147` 的 `ambient = (...)*ao` 直接归零。
- 槽 4 = MR 拿到 `kDefaultNormalMap`。状态 **BUG**。

### 5.7 ORPHAN：Mesh::srvTable
- `Model.h:46` 声明，但 `ModelConvert.cpp:102-120` 从不赋值；`DeferredRenderer.cpp:565` 用的是 `material.SRVTable` → 死字段。

### 5.8 ORPHAN：多 UV 集选择
- `glTF.cpp:262-274` 读到 texCoord 并写进 baseColorUV 等位域，但 `GBufferPS.hlsl:73,92,100,104,106` 全部只用 `input.UV0` → 位域无消费者，多 UV 集材质会取错 UV。

---

## 6. 纹理色彩空间 (问题 f)

| 贴图 | 编译 flag | 实际格式 | 证据 |
|---|---|---|---|
| BaseColor | `TextureOptions(false, alpha)` → kSRGB=0 | `R8G8B8A8_UNORM`(Linear) | `ModelConvert.cpp:343` + `TextureConvert.cpp:167` |
| MetallicRoughness | `TextureOptions(false)` | `R8G8B8A8_UNORM` | `ModelConvert.cpp:345` |
| Occlusion | `TextureOptions(false)` | `R8G8B8A8_UNORM` | `ModelConvert.cpp:347` |
| Emissive | `TextureOptions(true)` → kSRGB=1 | `R8G8B8A8_UNORM_SRGB` | `ModelConvert.cpp:349` |
| Normal | `TextureOptions(false)` | `R8G8B8A8_UNORM` | `ModelConvert.cpp:351` |

- **BUG(设计取舍)**：BaseColor 被显式编译为 Linear(`ModelConvert.cpp:341` 注释解释为最终输出才 sRGB)。而 `RendererConfig.h:17` `SceneColorBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM`(非 _SRGB) → 两头都不做 sRGB 转换，视觉自洽，但数值上不符合 PBR 的线性光照。
- **BUG**：Emissive 是 _SRGB，采样自动解码成线性，写进 `GBuffer_Emission = R8G8B8A8_UNORM`(`RendererConfig.h:13`)不做逆编码 → 线性值截断进 UNORM，HDR 自发光被饱和。
- **BUG/脆弱**：sRGB 完全依赖 DDS 文件自身格式这一个隐含约定；一旦 `CompileTextureOnDemand` 因 mtime 未变而不重转(§2.3)，格式就永久错。
- **MISSING**：法线贴图不会被转 BC5(`kNormalMap` 从未传，§2.4)。

---

## 7. Model / Scene 组织 (问题 g)

### 7.1 Model ↔ Mesh ↔ Primitive (设计 DONE)
- `Model`(`Model.h:74-96`)持一整块 `m_DataBuffer`(VB+DepthVB+IB 混排)与变长的 `m_MeshData`。
- `CompileMesh`(`ModelConvert.cpp:28-158`)按 `prim.hash`(`MeshConvert.h:27-35`，union 为 vertexFlags:16 | index32:1 | materialIdx:15)归并同 PSO 的 primitive 成一条 Mesh(`ModelConvert.cpp:62-70`)，共享 VB/IB 并用 numDraws 拆分(`102-120`)。**DONE**。
- `GatherRenderables`(`Model.cpp:19-44`)用 `pMesh += sizeof(Mesh)+(numDraws-1)*sizeof(Draw)`(`42`)正确跳过变长尾部。**DONE**。

### 7.2 **BUG：CompileMesh 的深度 VB 写入偏移不随 draw 递增**
- `ModelConvert.cpp:133`：`std::memcpy(uploadMem + curDepthVBOffset, draw->DepthVB->data(), ...)`。
- 对比 131 行 VB 用了 + curVertOffset、134 行 IB 用了 + curIndexOffset；深度 VB 没有对应的 per-draw 偏移，也不累加 → 多 draw 时后写的覆盖前写的，深度预通道读错顶点。状态 **BUG**。

### 7.3 **BUG(潜在)：numDraws 的隐式契约**
- `ModelConvert.cpp:86` `malloc(sizeof(Mesh)+sizeof(Draw)*(numDraws-1))`，若 numDraws==0 则 size_t 下溢。实际由 `ModelConvert.cpp:66` 的 push_back 保证 ≥1，但 `ModelConvert.cpp:422,444` 与 `Model.cpp:42` 都复用了同一表达式，无防护。

### 7.4 ModelInstance 与多实例
- `ModelInstance`(`Model.h:98-138`)持 `shared_ptr<const Model>` + 每节点一份 MeshConstants(`Model.cpp:72-73`) → 多实例共享几何、各自变换，设计 **DONE**。
- **BUG**：`Model.cpp:141` `matrixStack[stackIdx++]` 无上限检查，`kMaxStackDepth=32`(`Model.cpp:111`)被破坏会越界写栈。
- **BUG**：`Model.cpp:192-211` 的 GetRadius / GetBoundingBox 里 locator 变换被注释，直接返回模型空间值 → `Resize`(`Model.cpp:174-182`)设的 scale 不反映在包围盒上。
- **ORPHAN**：`Model.h:133` `m_BoundingSphereTransforms` 的赋值被注释(`Model.cpp:130-134`)。
- **MISSING**：视锥剔除被注释(`Model.cpp:51`)，distance 恒传 0(`Model.cpp:36`)。
- **BUG**：透明排序 `key.key = ~dist.u`(`BaseRenderer.cpp:77`)在 distance 恒 0 时退化，语义上等价于无排序。

### 7.5 Scene 层几乎不存在
- `Engine/05_Scene/Scene.md` 描述了 Entity/Transform/MeshRenderer/Light/Scene 管理/剔除，但目录下只有 Camera/ Lighting/ Model/ —— 没有 Entity/ECS/Scene 类。状态 **MISSING**。
- `BaseRenderer.h:72-78` `RenderFrameDesc` 里 `// const Scene* scene` 被注释，改传 `std::vector<ModelInstance*>`。

---

## 8. 相机 (问题 h)

| 项 | 结论 | 证据 |
|---|---|---|
| 投影类型 | 仅透视 | `Camera.cpp:55-98` |
| 正交投影 | MISSING(枚举有实现无) | `glTF.h:168` 有 kOrthographic |
| FOV | 垂直，默认 45° | `Camera.h:132` |
| 长宽比 | 语义为 height/width，默认 9/16 | `Camera.h:132`、`Camera.cpp:59` X=Y*aspect |
| 近远平面 | 默认 1/1000，demo 改 0.1/100 | `Camera.h:132`、`DeferredApp.cpp:84` SetZRange |
| Reverse-Z | 支持但默认关 | `Camera.h:129` |
| Y 轴 | 右手，Forward=-Z，C++ 行主序左乘 | `Camera.h:3-5,59-60` |
| View 矩阵 | `Invert(CameraToWorld)` | `Camera.cpp:45` |
| 行/列主序 | HLSL `#pragma pack_matrix(row_major)` | `DeferredCommon.hlsli:6` |

### 8.1 **BUG：长宽比从不更新**
- 全仓库 grep `SetAspectRatio`：只有声明(`Camera.h:82`)，零调用。
- `OnResize`(`DeferredRenderer.cpp:122-131` / `ForwardRenderer.cpp:93`)只重建 RT，不通知相机 → 窗口一改投影仍按 9:16 → 画面拉伸。状态 **BUG**。

### 8.2 PARTIAL/ORPHAN：Reverse-Z 与 Infinite-Z
- `Camera.h:93` 注释说 reverse-Z 要用 GREATER_EQUAL；而 `GraphicsCommon.cpp:148` `DepthStateReadWrite.DepthFunc = COMPARISON_FUNC_LESS`；`GraphicsCommon.cpp:48-52` 没有 `DepthStateReadWriteReversed` → ReverseZ(true) 时会状态不匹配(当前默认 false 所以一致)。
- `Camera.h:106` `m_InfiniteZ` 从未被设为 true → **ORPHAN**。

### 8.3 相机 → HLSL 衔接
- `DeferredRenderer.cpp:227` `globals.ViewProjMatrix = frame.Camera->GetViewProjMatrix()`，HLSL `mul(float4(pos,1.0), ViewProjMatrix)`(`StaticMeshVS.hlsl:52-53`) → 行向量左乘一致 **DONE**。
- `LightingPS.hlsl:87` 的 `ReconstructWorldPosition(uv, depth, InViewProj)` 用 `mul(clipPos, invViewProj)`(`DeferredCommon.hlsli:49`) → 一致；但依赖 reverse-Z=false 与投影矩阵第 3 列为 (0,0,Q1,-1)(`Camera.cpp:92-97`)。

---

## 9. 光照系统 (问题 i)

### 9.1 支持的类型与数量
- `LightingSystem.h:31-48` `LightData` 同时含 1 个方向光(DirectionIntensity, DirectionalColor)与 1 个点光(PointLightPositionRange, PointLightColorIntensity)。
- `LightingSystem.h:50` `enum { MaxLights = 1 };` → 数组只有 1 元素。状态 **PARTIAL**(上限 1)。
- `LightingSystem.h:52-58` `enum { point, direction, Lights }` 从未使用 → **ORPHAN**。

### 9.2 上传路径(Deferred 一致，Forward 断裂)
- CPU→GPU：`LightingSystem.cpp:72` `InitializeBuffer(m_LightGPUBuffer, m_LightCPUBuffer, MaxLights*sizeof(LightData))` → `CommandContext.cpp:592-606` 走 upload heap + CopyBufferRegion + `Finish(true)`。
- Deferred(三方一致): `DeferredRenderer.cpp:322` `SetConstantBuffer((uint32_t)LightingSlot::Light, GetGpuVirtualAddress())` ↔ `DeferredRenderer.cpp:633` `InitAsConstantBuffer(0, PIXEL)` ↔ `LightingPS.hlsl:18` `cbuffer Light : register(b0)` → **DONE**。
- Forward(ORPHAN): `ForwardRenderer.cpp:70` 绑 `GetSRV()`，但 `DefaultPS.hlsl:51-57` 只返回 baseColor、完全没有光照代码 → 绑了没人用。
- ORPHAN: `LightingSystem::UpdataState`(`LightingSystem.cpp:76-80`)零调用者，且把 buffer 转到 PIXEL_SHADER_RESOURCE，与 Deferred 需要的 CBV 状态冲突。

### 9.3 **BUG：灯光是硬编码常量，无场景驱动**
- `LightingSystem.cpp:33-74` `CreateLights` 写死 方向(0.3,-1,-0.5)、强度 10、颜色(1,0.95,0.9)、点光(0,2,-2) range 30 强度 80。没有任何 setter 或从场景读取的路径 → 光源配置 **MISSING**。
- `DeferredRenderer.cpp:156-160` 的 `// 2. 灯光常量` 是空的 TODO。
- `LightingSystem.h:63` 函数名 `UpdataState` 拼写错误(应为 Update)。

### 9.4 ORPHAN：GlobalConstants.SunDirection / SunIntensity
- `ConstantBuffers.h:64-65` 声明了，`DeferredRenderer.cpp:226-229` 只填 ViewProjMatrix/CameraPos，HLSL `StaticMeshVS.hlsl:15-19` / `GBufferPS.hlsl:27-31` 也只声明 ViewProj + ViewerPos → 永不读取。

---

## 10. 汇总表

| 功能 | 状态 | 文件:行 |
|---|---|---|
| 运行时解析 .gltf/.glb | DONE | `glTF.cpp:622-755` |
| POSITION/NORMAL/TANGENT/UV0/UV1 | DONE | `glTF.cpp:207-212` |
| JOINTS_0/WEIGHTS_0 | MISSING | `glTF.h:147 / glTF.cpp:207-212` |
| COLOR_0 顶点色 | ORPHAN | `glTF.cpp:212 / MeshConvert.cpp:157-178,308-327` |
| glTF Mesh.skin | STUB | `glTF.h:163` |
| PBR 五贴图读取 | DONE | `glTF.cpp:332-365` |
| normalTexture.scale | BUG | `glTF.cpp:346-347` |
| occlusionTexture.strength | MISSING | `glTF.cpp:354-356` |
| alphaCutoff | ORPHAN | `glTF.cpp:301-305 / ModelConvert.cpp:276-294` |
| KHR_materials_* | MISSING | `glTF.cpp 全文` |
| sampler mag/minFilter | PARTIAL | `glTF.cpp:566,572-583` |
| texCoord 多 UV 集 | ORPHAN | `glTF.cpp:262-274 / GBufferPS.hlsl:73,92` |
| 离线烘焙 .ty | DONE | `ModelConvert.cpp:408-454 / ModelLoader.cpp:189-357` |
| .ty 时间戳缓存 | DONE | `ModelLoader.cpp:220` |
| forceRebuild=true 强制重建 | BUG | `DeferredApp.cpp:77 -> ModelLoader.cpp:217,242` |
| DDS mtime 缓存 | DONE | `TextureConvert.cpp:25` |
| DDS 缓存键缺 flags | BUG | `TextureConvert.cpp:9-30` |
| 运行期 forceSRGB | BUG | `ModelLoader.cpp:75` |
| 材质贴图实际压缩 | MISSING | `ModelConvert.cpp:343-351` |
| 法线 BC5 压缩 | MISSING | `TextureConvert.h:16-19` |
| 硬编码绝对路径 | BUG | `DeferredApp.cpp:55-64 / Tooiyume.cpp:53-58 / RayTracingApp.cpp:57-62 / DeferredRenderer.cpp:40` |
| 资源根/cwd 解析 | MISSING | `TextureManager.cpp:53,97` |
| Assets 可用性 | BUG | `Assets/ 只有 Skybox，0 个 .ty/.dds/图` |
| 顶点 Layout 三方一致 | DONE | `MeshConvert.cpp:308-327 / DeferredRenderer.cpp:509-519 / StaticMeshVS.hlsl:23-29` |
| UNORM 法线还原(Deferred) | BUG | `StaticMeshVS.hlsl:50,56` |
| UNORM 法线还原(Forward) | BUG | `DefaultVS.hlsl:55,57,63,65` |
| DepthVB alphaTest UV | BUG风险 | `MeshConvert.cpp:356-373` |
| 材质槽枚举冲突 | BUG | `glTF.h:138 vs ConstantBuffers.h:18-26 / ModelConvert.cpp:343-351` |
| Material CB vs HLSL | DONE | `Material.h:39-47 / GBufferPS.hlsl:15-25` |
| alphaRef | PARTIAL | `ConstantBuffers.h:55 / GBufferPS.hlsl:24` |
| 材质 flags 位映射 | DONE | `ModelLoader.cpp:35-45 / ConstantBuffers.h:42-51` |
| 默认纹理槽顺序 | BUG | `ModelLoader.cpp:97-104` |
| Mesh::srvTable | ORPHAN | `Model.h:46 / ModelConvert.cpp:102-120` |
| addressModes 打包 | PARTIAL | `ModelConvert.cpp:301-337 / ModelLoader.cpp:24-33` |
| sRGB BaseColor | BUG(设计) | `ModelConvert.cpp:343 / TextureConvert.cpp:167` |
| sRGB Emissive | PARTIAL | `ModelConvert.cpp:349 / RendererConfig.h:13` |
| Model/Mesh 关系 | DONE | `ModelConvert.cpp:62-144` |
| CompileMesh DepthVB 偏移 | BUG | `ModelConvert.cpp:133` |
| ModelInstance 多实例 | DONE | `Model.cpp:58-75,106-171` |
| 场景图矩阵栈越界 | BUG | `Model.cpp:141` |
| BoundingSphereTransforms | ORPHAN | `Model.h:133 / Model.cpp:130-134` |
| Locator 影响包围盒 | BUG | `Model.cpp:192-211` |
| 视锥剔除 | MISSING | `Model.cpp:51,28` |
| 透明排序 | BUG | `BaseRenderer.cpp:77` |
| Scene/Entity/ECS | MISSING | `Engine/05_Scene/ 目录 / Scene.md` |
| 相机透视投影 | DONE | `Camera.cpp:55-98` |
| 相机长宽比更新 | BUG | `Camera.h:82` |
| 相机 Reverse-Z | PARTIAL | `Camera.h:129 / GraphicsCommon.cpp:148` |
| 相机 InfiniteZ | ORPHAN | `Camera.h:106` |
| 相机正交投影 | MISSING | `Camera.cpp 全文` |
| 行/列主序 | DONE | `DeferredCommon.hlsli:6` |
| LightingPS uv to NDC | DONE | `DeferredCommon.hlsli:43-53` |
| 光源类型/数量 | PARTIAL | `LightingSystem.h:31-50` |
| 光源可配置 | MISSING | `LightingSystem.cpp:33-74` |
| 灯光上传 Deferred | DONE | `DeferredRenderer.cpp:322,633 / LightingPS.hlsl:18` |
| 灯光上传 Forward | ORPHAN | `ForwardRenderer.cpp:70 / DefaultPS.hlsl:51-57` |
| LightingSystem::UpdataState | ORPHAN | `LightingSystem.cpp:76-80` |
| GlobalConstants.Sun* | ORPHAN | `ConstantBuffers.h:64-65` |
| ShadowCamera | STUB | `ShadowCamera.cpp:6-11` |
| Shadow Pass | MISSING | `DeferredRenderer.cpp:282-285` |

---

## 11. 修复优先级

1. **P0** 统一纹理槽枚举：改 `ModelConvert.cpp:343-351` 用 `glTF::Material::kBaseColor` 等限定名，或删 `glTF.h:138` 统一到 `ConstantBuffers.h:18-26`。
2. **P0** `ModelLoader.cpp:97-104` `DefaultTextures[]` 按 ConstantBuffers.h 顺序重排(槽1=DefaultNormalMap，槽3=WhiteOpaque2D)。
3. **P0** `ModelConvert.cpp:133` 加 per-draw 深度偏移并累加。
4. **P0** `StaticMeshVS.hlsl:56` / `DefaultVS.hlsl:65` 改用还原后的 Tangent.xyz；`DefaultVS.hlsl:55` 给 normal 补 *2-1。
5. **P1** `DeferredApp.cpp:77` 改 false 并给三个 demo 统一开关；`DeferredRenderer.cpp:40` 的 HDR 改相对路径。
6. **P1** `Camera.h:82` SetAspectRatio 接到 `DeferredRenderer.cpp:122-131` 的 OnResize。
7. **P1** `ModelConvert.cpp:343-351` 补 kDefaultBC|kQualityBC，法线补 kNormalMap。
8. **P2** `LightingPS.hlsl:85` 的 float2(1920,1080) 改成 CB 传入的分辨率；`LightingSystem.h:63` 改名 UpdateState。
9. **P2** `Model.cpp:141` 加 stackIdx < kMaxStackDepth 保护；`Model.cpp:192-211` 恢复 locator 变换。

> 未验证项(本轮无证据，不作结论)：Forward/RayTracing demo 的 forceRebuild 实际取值；EnvironmentLightingManager 的 IBL 烘焙流程；DDSTextureLoader 第 4/5 参语义细节；所有运行时渲染结果。
