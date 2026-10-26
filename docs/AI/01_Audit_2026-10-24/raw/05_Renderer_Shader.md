# 05 Renderer + Shader 深度审计

审计时间: 2026-10-24 12:00 - 13:25 (Asia/Shanghai)
范围: Engine/04_Renderer + Engine/Shader + Demos(ForwardApp/DeferredApp)
方式: 纯静态阅读 + grep 追踪调用者。未构建、未运行、未改源码。
标注: DONE / PARTIAL / ORPHAN(有代码无调用者) / STUB(空壳) / MISSING / BUG / UNKNOWN

---

## 0. 结论速览

| # | 结论 | 状态 |
| --- | --- | --- |
| 1 | DeferredRenderer 只有 GBufferPass / LightingPass / SkyPass 三段有真实命令; Shadow/SSAO/Transparent/PostProcess/TAA/Tonemap 六段只有 PIXBegin/EndEvent 空壳 | STUB |
| 2 | ForwardRenderer 的 Update()/BeginFrame() 是空函数, Render() 只画 kZPass+kOpaque | PARTIAL |
| 3 | Forward 用的是入库的旧 DXBC 头 DefaultVS.h/DefaultPS.h, 其内嵌根签名只有 2 个 CBV, 与 C++ 的 6 根参数不一致 | BUG |
| 4 | LightingPS.hlsl:85 分辨率硬编码 1920x1080 | BUG |
| 5 | EnvironmentLightingManager 的 m_HDRLoaded 全工程从未置位 | BUG |
| 6 | IBL 的 4 个 compute pass 都真实执行, 但参数/格式多处配错 | PARTIAL |
| 7 | 引擎内不存在 tonemap/bloom/SSAO/shadowmap 的任何实际资源与 PSO | MISSING |
| 8 | TAA 只有 hlsl + 一个 73 行死文件 TemporalEffects.cpp | ORPHAN |
| 9 | RendererRegistry 只做静态自注册, CreateRenderer 零调用 | ORPHAN |
| 10 | 交换链 R8G8B8A8_UNORM(非 sRGB), 而 LightingPS 里已做 ACES, gamma 链路混乱 | BUG |

---

## 1. ForwardRenderer

### 1.1 渲染阶段 (DONE, 有缺口)
- Render() 实际只做: Transition/Clear -> 绑全局 CBV -> 按 DefaultSorter 的 pass 循环 -> 每 mesh 绑 CBV/SRV/PSO -> 按 pass 选 vbOffset 或 vbDepthOffset -> DrawIndexed。证据 Engine/04_Renderer/Renderer/Forward/ForwardRenderer.cpp:122-245。
- 循环范围 `for (; CurrentPass <= pass; ...)` (ForwardRenderer.cpp:178)。Demo 传 DrawPass::kOpaque (Demos/ForwardApp/Tooiyume.cpp:123), 所以 kZPass 与 kOpaque 执行, kTransparent 不执行。
- 前置准备只有 RT/depth 清理与设置视口: ForwardRenderer.cpp:134-142 (Transition/Clear/SetViewportAndScissor), 此时尚未 SetRenderTargets。
- Update() 是空函数(只有注释): ForwardRenderer.cpp:111-120。BeginFrame() 空: 104-109。
- ShadowPass 段只有 `PIXBeginEvent(L"ShadowPass")` 且没有对应 PIXEndEvent: ForwardRenderer.cpp:160-162, 纯空壳且破坏 PIX 嵌套。

### 1.2 RT/Depth 格式 (DONE)
- Engine/04_Renderer/Renderer/RendererConfig.h:30-31: backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM, depthBufferFormat = DXGI_FORMAT_D32_FLOAT。
- SceneColorBuffer[0] 由 BufferManager::CreateColorBuffer 建立: ForwardRenderer.cpp:319-320。
- 不是 HDR 格式, 与 Deferred 的 SceneColor 也不同(见 2.2)。

### 1.3 光照 (MISSING / 未接线)
- PS 用 g_pDefaultPS (ForwardRenderer.cpp:303,370), 源文件 Engine/Shader/01_Default/DefaultPS.hlsl:51-56 只采样 baseColor 就 return, 无任何光照计算。
- Initialize 里取了灯光 StructuredBuffer 的 SRV (ForwardRenderer.cpp:68-71), 但从未写入 m_CommonTextures。m_CommonTextures 只在 BuildDescriptorHeaps 里 Alloc 1 个空槽 (ForwardRenderer.cpp:254-261); UpdateGlobalDescriptors 是空函数 (331-339)。等于没有光照。(注: 材质 5 槽 SRV 表的填充在 Engine/05_Scene/Model/ModelLoader.cpp:80-121, 与灯光无关。)

### 1.4 shader 与根签名不匹配 (BUG, 重点)
- Forward 用 g_pDefaultVS / g_pDefaultPS (ForwardRenderer.cpp:16-17, 302-303), 即 Engine/Shader/01_Default/DefaultVS.h (60167 B, 第 341 行 const unsigned char g_pDefaultVS[]) 与 DefaultPS.h (42742 B, 第 194 行 g_pDefaultPS)。
- 这两个 .h 第 1 行是 #if 0 (DefaultVS.h:1-339, DefaultPS.h:1-192), 后接旧工程 DXBC 反汇编注释 + 旧字节码; debug name 写作 D:/.../ToiyumeRender/Build/CompiledShaders/Model/PDBs/DefaultVS.pdb (DefaultVS.h:23), 不是本仓库 CMake 产物。
- 其内嵌根签名只声明 2 个 cbuffer (cb0 MeshConstants / cb1 GlobalConstants, 见 DefaultVS.h:64-99 的 Buffer Definitions 与 Resource Bindings), 没有 t0..t9 材质 SRV 表、s0..s9 采样器表、SRV t10 通用表层。
- 而 C++ 侧 m_RootSig 声明 6 个根参数 (ForwardRenderer.cpp:277-282)。PSO 用的根签名与 shader 内嵌的不一致 -> 运行期 root signature 不匹配 / 材质纹理取不到。这是 Forward 能跑但画面不对的最可能主因。
- 注意: DefaultPS.hlsl / DefaultVS.hlsl 这两个源文件不在 Engine/CMakeLists.txt 的 compile_shader 列表里(只编 StaticMeshVS/GBufferPS/FullScreenVS/LightingPS/SkyboxVS/SkyboxPS/4 个 CS), 属写了但不编的死源码。

### 1.5 kZPass 顶点流用错 (BUG)
- ForwardRenderer.cpp:230 在 kZPass 用 object.bufferPtr + mesh.vbDepthOffset, stride = mesh.vbDepthStride。
- 但 depth stream 只有 POSITION(12B, alphaTest 时 +TEXCOORD), 不含 NORMAL: Engine/03_AssetSystem/Importers/Mesh/MeshConvert.cpp:352-372。
- Forward 的输入布局断言要求 Position|Normal (ForwardRenderer.cpp:351-358), 所以 kZPass 会读错位数据。
- ForwardApp 两个模型都走 ModelSort (Demos/ForwardApp/Tooiyume.cpp:97,102), 只要有 alphaTest 材质就会产生 kZPass 条目。

### 1.6 生命周期缺陷 (BUG)
- DestroyForwardBufferTargets() 未判空 m_ForwardBuffer (ForwardRenderer.cpp:325-329), 而 Shutdown 在 m_Initialized==false 时直接 return (:77-80), OnResize 未初始化时也 return (:98)。
- BuildRootSignature() 里 m_RootSig.Reset(kNumRootBindings, 1) 连续调了两次 (ForwardRenderer.cpp:267, 273), 第一次浪费。
- BuildPSOs() 末尾 //defaultPSO.Finalize(); 被注释 (ForwardRenderer.cpp:305), m_DefaultPSO 是未 Finalize 的模板, 只能靠 GetPSOIndex 每次拷出再 Finalize (:388)。
- m_ForwardBuffer->SceneColorBuffer[1..7] 从未创建也从未销毁 (ForwardRenderer.h:48)。

---

## 2. DeferredRenderer

### 2.1 Pass 实现度
| 段 | 行号 | 实际内容 | 状态 |
| --- | --- | --- | --- |
| 1 GBufferPass | DeferredRenderer.cpp:204-272 | 清 4 张 GBuffer + depth + SceneColor, SetRenderTargets(4, ..., DSV), 绑全局 CB, 遍历 sorter DrawIndexed | DONE |
| 3 SSAOPass | 274-279 | 只有 PIXBegin/EndEvent | STUB |
| 4 ShadowPass | 281-285 | 只有 PIXBegin/EndEvent | STUB |
| 5 DeferredLighting | 287-335 | 真绑 4 GBuffer + depth SRV + 3 张 IBL + Light CBV + Camera CBV, Draw(3,0) | DONE |
| 6 SkyPass | 337-347 | 真调 s_EnvironmentLightingManager.SkyboxPass | DONE(带伤, 见 4.5) |
| 7 Transparent | 349-353 | 空壳 | STUB |
| 8 PostProcess | 355-359 | 空壳 | STUB |
| 9 TAA | 361-365 | 空壳 | STUB |
| 10 Tonemap | 367-371 | 空壳 | STUB |

Lead 给的 Shadow/SSAO/Transparent/PostProcess/TAA/Tonemap 六段空壳, 已在 DeferredRenderer.cpp:274-285 与 349-371 逐一核对, 无误。

### 2.2 GBuffer 规格 (DONE)
- 格式: Engine/04_Renderer/Renderer/RendererConfig.h:8-15 -> BaseColor R8G8B8A8_UNORM, Normal R16G16_FLOAT, Material R16G16B16A16_FLOAT, Emission R8G8B8A8_UNORM。
- SceneColor R8G8B8A8_UNORM (:17), Depth D32_FLOAT (:18), Velocity R16G16_FLOAT (:19)。
- Clear 用 ColorBuffer 默认 clear (Engine/02_RHI/Command/CommandContext.cpp:311-314 取 Target.GetClearColor()); DeferredRenderer.cpp:210-218 只调 context.ClearColor/ClearDepth, 没有逐 RT 设 ClearColor -> BaseColor 的 A(AO) 清成 0 默认值, 见 3.2。
- VelocityBuffer 建了 (DeferredRenderer.cpp:476) 但不写不读, ORPHAN。

### 2.3 世界坐标重建 (BUG, 重点)
- Engine/Shader/02_Deferred/LightingPS.hlsl:85-87:
  `float2 renderResolution = float2(1920, 1080);` `float2 uv = (position.xy + 0.5) / renderResolution;` `float3 positionWS = ReconstructWorldPosition(uv, depth, InViewProj);`
- 窗口非 1920x1080 时 uv 与 SV_POSITION 不匹配 -> 世界坐标错 -> 光照/视向量全错。必须从 CB 或 GetDimensions 取。
- 反投影公式本身在 DeferredCommon.hlsli:39-54: clipPos = float4(ndc, depth, 1.0); worldPos = mul(clipPos, invViewProj); worldPos.xyz /= worldPos.w; 前提 depth 0..1 非 reversed-Z 且行主序左乘, 与 Camera.h:129-133 (m_ReverseZ=false) 一致, 公式正确。

### 2.4 GBuffer 与 Lighting 的 slot 匹配 (DONE 但脆弱)
- C++ 按 LightingSlot 枚举值连续当 t0..t7 / b0,b1: DeferredRenderer.cpp:611-634, 枚举定义 DeferredRenderer.h:64-77 (BaseColor=0..BRDFLUT=7)。
- HLSL 也是 t0..t7: LightingPS.hlsl:7-15, 顺序一致。
- 但 SetDynamicDescriptor 的第一个参数 RootIndex 同时被当作 descriptor range 的 base register 使用 (DeferredRenderer.cpp:611-624 里 InitAsDescriptorRange(..., (uint32_t)LightingSlot::GBuffer_BaseColor, 1, ...)), 二者正好相等所以能跑, 属巧合耦合。

### 2.5 Lighting PSO 的 RT (BUG/UNKNOWN)
- BuildLightingPSO() 里 m_LightingPSO = m_DefaultPSO; (DeferredRenderer.cpp:572), 继承了 GBuffer PSO 的 4 个 MRT 格式; 之后只改一个 RT: SetRenderTargetFormat(SceneColorBufferFormat, SceneDepthBufferFormat) (:579)。
- 绘制时只绑 1 个 RTV 且不绑 DSV: context.SetRenderTarget(m_GBuffers->SceneColorBuffer->GetRTV()) (:330)。
- 若 SetRenderTargetFormat 只改 RTVFormats[0], PSO 的 NumRenderTargets 与实际绑定不符, D3D12 debug layer 会报错。具体 setter 未读, UNKNOWN。

### 2.6 EndFrame 与 sorter 生命周期 (BUG, 已修正早期判断)
- DeferredRenderer::EndFrame (DeferredRenderer.cpp:375-386) 遍历 frame.Models 调 model->ClearDirty(); ClearDirty 定义在 Engine/05_Scene/Model/Model.h:126 (m_IsDirty=false); ModelInstance::Update 内部不清脏标志 (Model.cpp:106-171, 只有 :109 的 early-out 和 :181 的置脏)。
- DeferredRenderer::Update (DeferredRenderer.cpp:140-163) 里是有脏检查的: for (auto* model : frame.Models) if (model->IsDirty()) { model->Update(...); this->ModelSort(*model); } (:144-152)。因此**不会每帧重复 ModelSort**(此前疑似的"sorter 每帧累积"不成立, 已修正)。
- **真正的缺陷是**: DeferredRenderer::EndFrame **没有调用 DefaultSorter.Reset()**(对比 ForwardRenderer::EndFrame: ForwardRenderer.cpp:251 有一行 DefaultSorter.Reset())。MeshSorter::Reset 才会清 m_SortObjects/m_SortKeys/m_PassCounts/m_CurrentDraw (Engine/04_Renderer/Renderer/Base/BaseRenderer.cpp:26-32), 而 Sort 只做 std::sort (:34-38)。
- 后果: Deferred 的 DefaultSorter 在进程生命周期内**只增不减** —— 任何一次模型重载/销毁/换模型(GatherRenderables 再次 AddMesh)都会把新条目追加到同一个 m_SortKeys, 旧条目永远画着。Forward 因为每帧 Reset 反而没这个问题。
- 另: Render() 里的 CurrentPass/CurrentDraw 都是 GetCurrentPass()/GetCurrentDraw() 的**局部拷贝** (DeferredRenderer.cpp:233-237), 循环内 ++CurrentDraw 不回写 sorter, 所以每帧都从 0 重画一遍全量; 这也是为什么现在"看起来还正常"。

---

## 3. PBR 数学正确性

### 3.1 两套重复实现 (ORPHAN)
- Engine/Shader/05_BRDF/BRDF.hlsl (218 行) + BRDFHelp.hlsl (36 行): 完整 VS/PS, 有 GGX/Smith/Schlick, 但 grep 全工程无任何 include, CMakeLists 也无 compile_shader -> ORPHAN 死代码 (BRDF.hlsl:18 include 的是 BRDFHelp.hlsl, 但没别人 include BRDF.hlsl)。
- Engine/Shader/02_Deferred/DeferredCommon.hlsli (145 行) 才是被 GBufferPS.hlsl:1 与 LightingPS.hlsl:4 include 的唯一活版本。

### 3.2 活版本 BRDF (DeferredCommon.hlsli)
| 项 | 实现 | 判定 |
| --- | --- | --- |
| GGX NDF | DistributionGGX :63-75, a=roughness^2, a2=a*a, denom=PI*(NdotH2*(a2-1)+1)^2, max(,1e-4) | DONE |
| Smith k | GeometrySchlickGGX :77-84 用 k=(r+1)^2/8 | 正确的 direct-light 近似 (Epic), DONE |
| Smith 乘积 | G = G1(NdotV)*G1(NdotL) :86-95 | DONE |
| Fresnel | F = F0 + (1-F0)*(1-HdotV)^5 :58-61 | DONE |
| 分母夹紧 | 4*max(dot(N,V),0)*max(dot(N,L),0) 再 max(,1e-4) :117 | DONE |
| 能量分配 | kS=F, kD=(1-kS)*(1-metallic) :122-125 | DONE |
| 漫反射 | kD*baseColor/PI :125 | DONE |
| F0 | lerp(0.04, baseColor, metallic) :109-110 | DONE |

### 3.3 GBuffer 写入侧的三个问题 (PARTIAL/BUG)
- specular 通道是假数据: GBufferPS.hlsl:120 恒写 0.5, LightingPS.hlsl:91 用 F0 = 0.16*specular^2 -> F0 恒 0.04, 未接 glTF specularFactor。PARTIAL。
- roughness 抬底: LightingPS.hlsl:72 roughness = max(material.g, 0.04); GBuffer 写的是 mrSample.g*roughnessFactor (GBufferPS.hlsl:102)。可接受但全粗糙表面偏亮。轻微。
- AO 未做 UV 选择: GBufferPS.hlsl:104 直接采 occlusionTexture, baseColorUV/occlusionUV 标志未使用; glTF 的 occlusion 常与 metallicRoughness 共用一张(R 通道), 这里当独立贴图。PARTIAL。
- AlphaTest/AlphaBlend 未实现: GBufferPS.hlsl:88-124 无任何 clip/discard 分支, 但 MeshSorter 已把 kZPass 单列 (Engine/04_Renderer/Renderer/Base/BaseRenderer.cpp:81-94)。MISSING。
- Normal 编码: GBufferPS.hlsl:119 写 float4(encodedNormal,0,0) 到 R16G16_FLOAT (RendererConfig.h:11) 后两位丢弃, LightingPS.hlsl:80 用 DecodeOctNormal 解码, 对得上, DONE。

### 3.4 IBL 能量补偿 (MISSING)
- LightingPS.hlsl:136-147: kD=(1-F)*(1-metallic), specularIBL=prefiltered*(F*envBRDF.x+envBRDF.y), 无 multiscatter GGX 的 E 项, 无 horizon occlusion -> 粗糙金属偏暗。MISSING。

### 3.5 与死代码 BRDF.hlsl 的分歧
- BRDFHelp.hlsl:24 的 Smith k 同时支持 (r+1)^2/8 (direct) 与 r^2/2 (IBL), 参数 isIBL; 活版本只有一种。
- MAX_REFLECTION_LOD 冲突: BRDF.hlsl:203 是 4.0, LightingPS.hlsl:143-144 是 8.0。

---

## 4. IBL 真实接通程度

### 4.1 4 个 compute pass 真的执行 (DONE)
- BakeEnvironmentTextures (Engine/04_Renderer/EnvironmentLightingManager.cpp:147-168) 依次调 EquirectangularToCubemapPass(:154) -> IrradianceConvolutionPass(:155) -> SpecularPrefilterPass(:156) -> BRDFLUTPass(:157), 再统一转 SHADER_RESOURCE 并 Finish(true)。
- 4 个函数体都有真实 SetRootSignature/SetPipelineState/Transition/SetDynamicDescriptor/SetDynamicConstantBufferView/Dispatch3D|2D/UAVBarrier (:183-223 / 225-253 / 255-291 / 293-320), 不是 TODO 空壳(函数开头 // TODO 只是旧注释)。

### 4.2 Context 类型靠 reinterpret_cast 双向骗过 (BUG, 设计层面)
- 这 4 个 pass 内部用 ComputeContext& (:203,232,265,303), 但传入的是 computeContext.GetGraphicsContext() (:154-157)。
- CommandContext.h:129-134 的 GetGraphicsContext 断言 m_Type != COMPUTE; ComputeContext::Begin(Async=false) 建的是 DIRECT 列表 (CommandContext.cpp:75-82), 所以断言不触发, 之后 GetComputeContext() 再 cast 回来。逻辑能跑但极脆弱。

### 4.3 Bake 触发条件 (BUG)
- :150 `if (m_Baked || m_HDRLoaded || !m_Textures.HDRTexture.IsValid()) return;`
- m_HDRLoaded 全工程只有这一处引用, 且从未被置 true; LoadHDR 成功后也不设它 (:170-179 只加载并打印失败信息)。
- 实际只靠 m_Baked 短路, m_Baked 只在 :166 设一次 -> 第一帧之后不再重跑。表面上不崩是巧合, 语义上这个条件是写错的。
- 另外 DeferredRenderer::Update 每帧都调 BakeEnvironmentTextures (DeferredRenderer.cpp:154), 而 Update 与 Render 是两个独立 CommandList (DeferredApp.cpp:91-104 与 110-126), Bake 内部 Finish(true) 同步阻塞 -> 架构错。

### 4.4 尺寸/mip/格式 (PARTIAL)
- 尺寸全在头文件硬编码: EnvironmentLightingManager.h:103-107 -> 512 / 32 / 128 / 5 / 256。
- 格式统一 R16G16B16A16_FLOAT (:374,377,380), BRDFLUT 是 R16G16_FLOAT (:383)。
- EnvironmentCube mip = floor(log2(512))+1 = 10 (:371-374)。
- Prefilter LOD 不匹配 (BUG): LightingPS.hlsl:143-144 写死 maxPrefilterMip = 8.0 并 SampleLevel(R, roughness*8.0), 但 PrefilterCube 只有 5 个 mip (.h:106)。roughness>0.5 之后全被 clamp 到同一 mip -> 高粗糙区域糊成一片。应改成 roughness*(m_PrefilterMipCount-1)。
- HDR 路径硬编码绝对路径 D:/CS-Self-Study/.../grasslands_sunset_2k.dds (DeferredRenderer.cpp:40)。

### 4.5 Skybox (BUG)
- SkyboxPass (EnvironmentLightingManager.cpp:323-350): SkyboxPS 用 DepthStateReadOnly (:67), 其 DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL (Engine/02_RHI/Pipeline/GraphicsCommon.cpp:153); SkyboxVS 输出 PositionH.z = 1 (SkyboxVS.hlsl:21) 即远平面, 配合 LESS_EQUAL 正确。
- UV 当 NDC 用 (BUG): SkyboxPS.hlsl:25-29 做 ndc = input.UV*2-1, 而 VS 里 o.uv = float2((vertexID<<1)&2, vertexID&2) 插值后覆盖 [0,2] (SkyboxVS.hlsl:19) -> ndc 范围 [-1,3], 地平线偏移。是从 FullScreenVS 抄 UV 造成的。
- CameraCB 只塞 InvView/InvProj (EnvironmentLightingManager.cpp:335-339), 取 3x3 天然去掉平移 (SkyboxPS.hlsl:35), 正确。

### 4.6 其他 (ORPHAN/STUB)
- SaveDDS() 空函数 (EnvironmentLightingManager.cpp:122-125), STUB。
- m_Textures 这套 TextureRef 结构 (.h:40-47) 与 m_EnvironmentCubeMap 等 ColorBuffer 两套并存, 实际只用 HDRTexture 一个, PARTIAL。
- C++ 建了 SamplerDesc PointSamplerDesc 但从未使用 (:404-408, 592-596), 死变量。
- DeferredRenderer.cpp:314-316 用 const_cast 强转 ColorBuffer 去 Transition, 绕过 const 语义, 轻微 BUG。

---

## 5. 后处理与色彩

### 5.1 色彩链路 (BUG, 重点)
- Deferred 的 SceneColor 是 R8G8B8A8_UNORM (RendererConfig.h:17), 但 LightingPS.hlsl:155 在 Lighting Pass 里已做 ACES: finalColor = ACESFilm(finalColor) -> 色调映射发生在写 LDR RT 之前。
- 随后 SkyPass 又叠 skybox 颜色 (DeferredRenderer.cpp:344 -> SkyboxPS.hlsl:37 直接 return float4(color,1)), skybox 没走 ACES -> 天空与地物色调不一致。
- Tonemap Pass 是空壳, 曝光完全不存在: 无 exposure CB、无 eye adaptation、无 bloom -> HDR 流是假的。
- sRGB 三处不一致: BaseColor 贴图用 TextureOptions(false) 按 Linear 加载 (Engine/03_AssetSystem/Importers/Model/ModelConvert.cpp:341-343), 但磁盘内容是 sRGB 编码, 活路径 GBufferPS 又不解码 -> 直接拿 sRGB 值当 linear 用, 偏暗; 中间 RT 全 UNORM 非 SRGB; 交换链 DXGI_FORMAT_R8G8B8A8_UNORM (Engine/01_Application/Display.cpp:29) 非 SRGB, Present 是纯 CopyBuffer (Display.cpp:249-261), 少一次 linear->sRGB。
- ColorBuffer 没有 GetSRVFormat/GetRTVFormat 做 sRGB 分离: ColorBuffer.cpp:107-128 与 152-195 直接透传 Format。

### 5.2 TAA (ORPHAN)
- Engine/Shader/04_PostProcess/TAACS.hlsl (93 行) 是 compute, numthreads(8,8,1) (:28), 输入 t1 当前色/t2 历史色/t4 velocity, 输出 u0 (:7-11), 3x3 方差钳制 (:61-87), lerp(history, curr, gAlpha) (:90)。输入用 space1, 与引擎 space0 体系不一致, 需根签名显式支持, UNKNOWN。
- Engine/04_Renderer/TemporalEffects.cpp 73 行是死文件: Initialize 里 CreatePSO 宏有效行被注释 (:47-51); Shutdown/Update/GetFrameIndexMod2/GetJitterOffset/ClearHistory/ResolveImage 只有重新声明没有函数体 (:54-72)。无调用者 (grep TemporalEffects:: 在 Demos/Engine 零命中), 一旦被调就链接失败。
- Renderer 里 TAA Pass 是空壳, Velocity Buffer 从未写入。结论: TAA 完全未接入。

---

## 6. Shader 编译链

### 6.1 真实编译链 (DONE)
- Engine/20_cmake/ShaderCompiler.cmake:30-92: compile_shader(TARGET SHADER_FILE ENTRY PROFILE) -> dxc -E -T -I Engine/Shader -Fo <BIN>/ShaderCache/X.cso -> BinToHeader.cmake 转成 unsigned char X_cso[] = {0x..} 的 .h (BinToHeader.cmake:13-27)。
- 输入 ${CMAKE_SOURCE_DIR}/Engine/ + SHADER_FILE, 输出 ${CMAKE_BINARY_DIR}/ShaderCache/, 通过 target_include_directories (Engine/CMakeLists.txt:107-115) 让 #include "X.h" 找到。
- 实际编译 10 个: StaticMeshVS/GBufferPS/FullScreenVS/LightingPS/SkyboxVS/SkyboxPS/EquirectangularToCubemapCS/IrradianceConvolutionCS/SpecularPrefilterCS/BRDFLUTCS (Engine/CMakeLists.txt:21-89)。
- 大小写陷阱 (BUG, 跨平台问题): CMake 写 Shader/06_Environment/EquirectangularToCubemapCS.hlsl (:65) 与 IrradianceConvolutionCS.hlsl (:72), 但磁盘上是 EquirectangularToCubemapCs.hlsl 与 irradianceConvolutionCS.hlsl。Windows 不区分大小写能过, get_filename_component(NAME_WE) 产出的符号名仍与代码使用的 EquirectangularToCubemapCS_cso / IrradianceConvolutionCS_cso 一致 (EnvironmentLightingManager.cpp:365, 94), 所以现在能过。
- 失败策略: dxc 失败会让 add_custom_command 失败, 构建报错, 不会静默; 但 .h 没加入 target 源文件, 只靠 add_custom_target + add_dependencies (CMakeLists.txt:94-105)。若直接编 Engine 而没编 *_Shader, 会吃到上次残留的旧 .h -> 这正是 Forward 吃 DefaultVS/PS.h 的场景。

### 6.2 ShaderLibrary 不存在 (MISSING)
- Engine/04_Renderer/Shader/ShaderLibrary.h 与 .cpp 都是 0 行 (wc -l 均为 0)。
- 因此不存在运行时加载 cso/嵌入头的 ShaderLibrary; 全部 shader 是编译期 #include "xxx.h" 硬链接。
- 结论: Shader 系统 = cmake 离线 dxc + BinToHeader + 静态数组, 无运行时编译、无热重载、无反射校验。

---

## 7. 阴影 (MISSING/ORPHAN)
- 无 ShadowMap 资源创建: grep ShadowBuffer 只命中 Engine/04_Renderer/BufferManager.h:18 的一个未使用 #include, 以及 Engine/02_RHI/Resource/ShadowBuffer.cpp/h 本身。
- ShadowCamera 无调用者: Engine/05_Scene/Camera/ShadowCamera.h:8, ShadowCamera.cpp:6。
- Forward/Deferred 的 GetFeatures() 都声明 RendererFeature::Shadow (ForwardRenderer.h:73, DeferredRenderer.h:109), 但没有任何 Pass 消费。
- MeshSorter 的 kShadows 分支有实现但从未实例化: Engine/04_Renderer/Renderer/Base/BaseRenderer.cpp:64-72 与 BaseRenderer.h:49; 全工程只有 MeshSorter DefaultSorter{BatchType::kDefault} 一个实例 (BaseRenderer.h:213), 也没有 Render(..., kShadows) 调用点。
- shadow sampler 只在 Engine/Shader/01_Default/Common.hlsli:20-26,32 声明, 无活 shader include。

---

## 8. 两个 Renderer 的重复 & RendererRegistry

### 8.1 重复度 (PARTIAL, 约 55%)
| 方法 | Forward | Deferred | 差异 |
| --- | --- | --- | --- |
| Initialize 骨架 | 36-75 | 60-104 | 只差 BuildLighting/BakeEnvironment |
| Shutdown | 77-91 | 106-120 | Deferred 多 s_EnvironmentLightingManager.Shutdown() |
| OnResize | 93-102 | 122-131 | 逐字重复 |
| BeginFrame | 104-109 | 133-138 | 完全相同(都空) |
| BuildDescriptorHeaps | 254-261 | 388-395 | 逐字重复 |
| BuildRootSignature | 263-284 | 397-421 | 仅改名字/注释, 根参数完全一样 |
| GetPSOIndex | 346-399 | 501-550 | 顶点布局 90% 重复; RenderTargetFormats 换成 GBuffer 4 张 |
| BindRenderState | 402-407 | 553-560 | Deferred 多 SetPrimitiveTopology |
| BindMaterial | 409-413 | 562-567 | Deferred 多一次 SetConstantBuffer(kMaterialConstants) |
| UpdateGlobalDescriptors | 331-339 | 486-494 | 同为空的 TODO |
| Destroy*Targets | 325-329 | 481-484 | 一个手动 Destroy, 一个 BufferManager::DestroyAll() |

### 8.2 RendererRegistry (ORPHAN)
- 提供 RegisterRenderer/CreateRenderer: Engine/04_Renderer/Renderer/Base/RendererRegistry.cpp:11-22。
- 两个 Renderer 通过 RendererAutoRegister 静态自注册: ForwardRenderer.cpp:27 (L"ForwardRenderer ", 注意尾随空格), DeferredRenderer.cpp:33 (L"DeferredRenderer")。
- CreateRenderer 全工程零调用; 三个 Demo 都是直接全局实例化 (Demos/ForwardApp/Tooiyume.cpp:51, Demos/DeferredApp/DeferredApp.cpp:53)。
- 尾随空格与类内 GetName() 一致 (ForwardRenderer.h:56), 但未来若用 GetName() 去 CreateRenderer, 少一个空格就找不到。

### 8.3 BufferManager 双持有与描述符泄漏 (BUG)
- BufferManager::DestroyAll() 只清两张 map (Engine/04_Renderer/BufferManager.cpp:62-66); Renderer 自己也 shared_ptr 持有同一批对象 (m_GBuffers)。
- OnResize 时 Deferred 先 DestroyAll 再 CreateDeferredBufferTargets (DeferredRenderer.cpp:129-130), 旧 GBuffer 的 SRV 描述符已分配未回收 (AllocateDescriptor 无回收) -> 每次 resize 泄漏 4+3 个描述符。

---

## 9. 死代码/空文件清单 (DONE)
| 文件 | 行数 | 状态 |
| --- | --- | --- |
| Engine/04_Renderer/Shader/ShaderLibrary.h/.cpp | 0 / 0 | 空文件 |
| Engine/04_Renderer/Pipeline/PSOBuilder.h/.cpp | 0 / 0 | 空文件 |
| Engine/04_Renderer/Material/MaterialFlags.h | 0 | 空文件(实际 flags 在 Material.h:14-20) |
| Engine/04_Renderer/RenderGraph/ | 空目录 | 空 |
| Engine/Shader/05_BRDF/BRDF.hlsl + BRDFHelp.hlsl | 218+36 | ORPHAN, 无编译单元引用 |
| Engine/Shader/01_Default/DefaultVS.h/.hlsl, DefaultPS.h/.hlsl | 60167/1709, 42742/1688 | .h 被 Forward 使用(旧字节码), .hlsl 不在编译列表 |
| Engine/04_Renderer/TemporalEffects.cpp/.h | 73 / 41 | ORPHAN |
| Engine/05_Scene/Camera/ShadowCamera.* | - | ORPHAN |
| Engine/02_RHI/Resource/ShadowBuffer.* | - | 仅被一个空 include 引用 |
| Engine/04_Renderer/environmentlightingmanager.txt | - | 纯笔记 |
| Engine/Shader/06_Environment.zip | 11551 B | 旧备份 |

---

## 10. 汇总表

| 功能 | 状态 | 文件:行 | 说明 |
| --- | --- | --- | --- |
| Forward 主流程 | PARTIAL | Renderer/Forward/ForwardRenderer.cpp:122-245 | 仅 kZPass+kOpaque, Update/BeginFrame 空 |
| Forward 光照 | MISSING | Shader/01_Default/DefaultPS.hlsl:51-56 | PS 只采样 baseColor 就 return; 灯光 SRV 未写入 m_CommonTextures |
| Forward 用旧 DXBC 头 | BUG | DefaultVS.h:1,341; DefaultPS.h:1,194 | #if 0 后是旧工程字节码; 内嵌根签名只 2 个 CBV, 与 C++:277-282 的 6 参数不一致 |
| Forward PIX 嵌套 | BUG | ForwardRenderer.cpp:160-162 | ShadowPass 只有 Begin 无 End |
| Forward kZPass 顶点流 | BUG | ForwardRenderer.cpp:230; MeshConvert.cpp:352-372 | depth stream 无 NORMAL, 与输入布局断言冲突 |
| Deferred GBufferPass | DONE | Deferred/DeferredRenderer.cpp:204-272 | 4 RT + depth 真清真画 |
| Deferred LightingPass | DONE | :287-335 | 绑 GBuffer/Depth/IBL/Light/Camera, Draw(3,0) |
| Deferred SSAO | STUB | :274-279 | 仅 PIXBegin/EndEvent |
| Deferred Shadow | STUB | :281-285 | 同上 |
| Deferred Transparent | STUB | :349-353 | 同上 |
| Deferred PostProcess | STUB | :355-359 | 同上 |
| Deferred TAA | STUB | :361-365 | 同上 |
| Deferred Tonemap | STUB | :367-371 | 同上 |
| Deferred SkyPass | DONE(带伤) | :337-347; SkyboxPS.hlsl:25 | 真绘制, 但 UV 当 NDC(范围 [-1,3]) |
| GBuffer 格式 | DONE | RendererConfig.h:8-19 | 4 RT + SceneColor + Depth + Velocity |
| GBuffer AO 清 0 | BUG | DeferredRenderer.cpp:210-217 | 未设 ClearColor, A=0 |
| VelocityBuffer | ORPHAN | DeferredRenderer.cpp:476 | 建了不写不读 |
| 世界坐标重建硬编码 | BUG | LightingPS.hlsl:85 | float2(1920,1080) |
| 反投影公式 | DONE | DeferredCommon.hlsli:39-54 | 与 Camera.h:129-133 的非 reversed-Z 一致 |
| GBuffer-Lighting slot | DONE(脆弱) | DeferredRenderer.cpp:611-631; LightingPS.hlsl:7-15 | t0..t7 一致; 枚举值同时当 RootIndex 与 base register |
| Specular 通道假数据 | PARTIAL | GBufferPS.hlsl:120; LightingPS.hlsl:91 | B 恒 0.5 -> F0 恒 0.04 |
| AlphaTest/Blend | MISSING | GBufferPS.hlsl:88-124 | 无 clip/discard |
| BRDF GGX/Smith/Fresnel | DONE | DeferredCommon.hlsli:58-129 | k=(r+1)^2/8, 能量分配正确 |
| IBL energy compensation | MISSING | LightingPS.hlsl:136-147 | 无 multiscatter/horizon occlusion |
| IBL 4 个 compute pass | DONE | EnvironmentLightingManager.cpp:147-320 | 都有真实 Dispatch |
| IBL Bake 条件错 | BUG | EnvironmentLightingManager.cpp:150 | m_HDRLoaded 无任何赋值 |
| IBL Prefilter LOD | BUG | LightingPS.hlsl:143-144 vs .h:106 | 用 8.0, 实际 5 mip |
| IBL 尺寸/路径硬编码 | PARTIAL | EnvironmentLightingManager.h:103-107; DeferredRenderer.cpp:40 | 512/32/128/5/256 与绝对路径 |
| Skybox UV 错 | BUG | SkyboxPS.hlsl:25-29 | UV in [0,2] 再 *2-1 -> NDC in [-1,3] |
| 双向 Context cast | BUG | EnvironmentLightingManager.cpp:154-157,203 | Finish(true) + reinterpret_cast |
| Tonemap/曝光/Bloom | MISSING | DeferredRenderer.cpp:367-371 | 全空; LightingPS.hlsl:155 ACES 位置错 |
| sRGB 链路 | BUG | Display.cpp:29; RendererConfig.h:17; ModelConvert.cpp:343 | 三处不一致 |
| TAA hlsl | DONE(未接) | Shader/04_PostProcess/TAACS.hlsl:28 | compute 8x8, 3x3 方差钳制 |
| TAA 接入 | ORPHAN | TemporalEffects.cpp:47-72 | CreatePSO 被注释, 6 函数无定义 |
| Shader 编译链 | DONE | ShaderCompiler.cmake:30-92; BinToHeader.cmake:13-27 | 离线 dxc -> cso -> 字节数组 .h |
| Shader 运行时加载 | MISSING | Shader/ShaderLibrary.h/.cpp | 均 0 行 |
| Shader 大小写陷阱 | BUG | Engine/CMakeLists.txt:65,72 | 与实际文件名大小写不一致 |
| Shadow 资源 | MISSING | BufferManager.h:18 | 只有 include, 无 Create |
| ShadowCamera | ORPHAN | ShadowCamera.h:8; .cpp:6 | 无调用者 |
| kShadows sorter 分支 | ORPHAN | BaseRenderer.cpp:64-72 | 只有 kDefault 实例 |
| RendererRegistry CreateRenderer | ORPHAN | RendererRegistry.cpp:16 | 零调用 |
| 两 Renderer 重复 | PARTIAL | Forward.cpp vs Deferred.cpp | 约 55% 骨架复制 |
| 描述符泄漏 | BUG | DeferredRenderer.cpp:129-130,481-484 | DestroyAll 只清 map |
| Deferred 缺 Sorter.Reset | BUG | DeferredRenderer.cpp:375-386 vs ForwardRenderer.cpp:251 | 只增不减, 重载模型后旧条目残留 |
| ForwardBuffer [1..7] | ORPHAN | ForwardRenderer.h:48 | 只创建 [0] |
| RootSig Reset 两次 | BUG | ForwardRenderer.cpp:267,273 | 第一次浪费 |
| Destroy 无判空 | BUG | ForwardRenderer.cpp:325-329 | 可空指针 |

---

## 11. 未验证项 (UNKNOWN)
1. DynamicDescriptorHeap 的 Commit 时机: SetDynamicDescriptor 只写 CPU 缓冲, 真正 Commit 依赖 SetPipelineState/Flush。未读 CommandContext::SetPipelineState(:364-372) 之后的 Commit 路径; 若缺失则 Deferred Lighting 的 5 个 SRV 全绑失败。UNKNOWN。
2. GraphicsPSO::SetRenderTargetFormat 是覆盖还是只改第 0 个: 若是后者, Lighting PSO 仍带 4 个 RTV 格式 -> Draw 时 debug layer 报错。UNKNOWN。
3. GetDepthFormat(D32_FLOAT) 的返回值: 定义在 Engine/02_RHI/Resource/PixelBuffer.cpp:136, 未读; 若不是 R32_FLOAT, DepthBuffer.cpp:82-93 的 DepthSRV 会创建失败。UNKNOWN。
4. GlobalConstants 大小不一致: CPU 侧 Engine/03_AssetSystem/Assets/Constants/ConstantBuffers.h:60-66 有 SunDirection/SunIntensity, 而 StaticMeshVS.hlsl:15-19 与 GBufferPS.hlsl:27-31 只有 ViewProjMatrix/ViewerPos; ViewProj 在偏移 0 所以暂不影响。UNKNOWN(影响面)。
5. ColorBuffer cube 路径 m_NumMipMaps = NumMips-1 (ColorBuffer.cpp:156) 与 SpecularPrefilterCS.hlsl:79 的 gMipLevel-1 是否差 1 导致采样越界: UNKNOWN。
6. 未构建/未运行: 本次全部结论来自静态阅读, Forward 是否黑屏、Deferred 是否真报 MRT 错误, 需 BuildVerify 实测。UNKNOWN。
