# R4：光栅 PBR 闭环（显示编码链 / IBL 状态机 / AlphaTest）

> 阶段：R4（路线图 P1）｜状态：**DONE**（构建 + 运行 + GPU 回读 + Debug Layer 全部实测）
> 日期：2026-10-26｜基线：master @ 51c09d0（工作区含 R0–R3 未提交改动，本次只做增量、未覆盖任何已有改动）
> 证据目录：docs/AI/R4/evidence/ ｜基线对照：docs/AI/R4/baseline/

---

## 0. 施工前状态：先纠正三处旧文档与代码不符的结论

施工前必须校正既有认知，否则会去修不存在的问题、或漏掉真问题。

| # | 旧文档结论 | 代码实测 | 判定 |
|---|---|---|---|
| 1 | R_执行表.md §7「IBL 烘焙 4 个 Pass 是空壳，只有 TODO 没有 GPU 命令」 | 四个 Pass 都有真实 Dispatch3D/Dispatch2D（EnvironmentLightingManager.cpp:254 / :284 / :322 / :352） | **旧结论过期**。空壳说法来自更早版本；git show HEAD 版本同样已含 Dispatch。真正缺的是状态机与参数衔接，不是 GPU 命令 |
| 2 | R_执行表.md §7「m_Ready 被置 true 但内容未定义」 | m_Ready 在烘焙完成后才置 true（:198-199），配合 R3 的复位逻辑已正确 | **已被 R3 顺带修掉**，但 m_HDRLoaded 仍是有问题的死标志（见 §1 R4.4） |
| 3 | R_执行表.md §7「模型整体偏白，主因 IBL 直接采样 HDR」 | 偏白的真实根因是显示编码链缺失：SceneColor/swapchain 都是 *_UNORM，全链路无 sRGB 编码，且 Skybox 完全绕过曝光 | **归因需修正**，详见 §1 R4.1 |

### 0.1 基线量化（可复现）

对 docs/AI/R4/baseline/r3_00_initial_deferred.png（R4 前的 GPU 回读）取两个固定采样区：

- 模型区（x∈[0.38w,0.62w], y∈[0.08h,0.55h]）：平均亮度 136.8，**23.0% 像素硬截断**（max channel ≥ 250）
- 天空区（x∈[0,0.25w], y∈[0,0.18h]）：平均亮度 207.9，**52.0% 像素硬截断**

天空有 42–52% 的纯白像素不是「天空很亮」，而是**曝光链漏掉了天空**这一结构性缺陷。

---

## 1. 五条缺陷与修复

### R4.1 显示编码链：Skybox 绕过曝光/ACES，且全链路无 sRGB

**问题**：SkyboxPS.hlsl 旧实现只有一行 return float4(color, 1.0)，把动辄远大于 1.0 的 HDR radiance 原样写进 R8G8B8A8_UNORM 的 SceneColor。同时 SwapChainFormat 与 SceneColorBufferFormat 都是 *_UNORM（非 *_SRGB），硬件不会替我们做 gamma 编码。

**后果**：天空成为整幅画面里唯一「曝光与色调映射管不到」的区域；线性值被当作 gamma 值显示，模型整体偏灰偏暗。这正是旧文档里「模型整体偏白」的另一半真相：天空被截断成白，模型相对显得暗，两者都源于编码链缺失。

**修复**：新增共享实现 Engine/Shader/02_Deferred/ColorPipeline.hlsli：

    float3 EncodeDisplayColor(float3 hdrColor, float exposure, bool tonemap, bool srgbEncode)
    {
        float3 exposed = hdrColor * max(exposure, 0.0);
        float3 mapped  = tonemap ? ACESFilm(exposed) : saturate(exposed);
        return srgbEncode ? LinearToSRGB(mapped) : mapped;
    }

Lighting 与 Skybox **都必须**调它。拆成单独 .hlsli 的理由是：任何一处漏用，画面就重新出现「两个 Pass 处在不同亮度空间」的 bug——而这正是修复前的状态。

LinearToSRGB 用 IEC 61966-2-1 精确传递函数，而非 pow(c, 1/2.2) 近似：两者在暗部差数个 8bit 级，会让改动前后的对比失去可比性。

### R4.2 常量缓冲契约 96 → 112 字节

RenderParams 新增 EnableSRGBEncode；GPU 侧追加 float4 DisplayParams。三处**必须同步**：

| 层 | 文件 | 字段 |
|---|---|---|
| CPU 真源 | Engine/04_Renderer/RenderParams.h | EnableSRGBEncode |
| CPU 镜像 | Engine/05_Scene/Lighting/LightingSystem.h | DisplayParams + static_assert(sizeof==112) |
| GPU | Engine/Shader/02_Deferred/LightingPS.hlsl | cbuffer Light 末尾 float4 DisplayParams |

新增字段只允许**追加在末尾**。kLightShaderParamsBytes 同步改为 112。

### R4.3 Prefilter mip 上界：写死 8.0 → 真实 mip 数

**问题**：LightingPS.hlsl:166 写死 const float maxPrefilterMip = 8.0f，而 PrefilterCube 实际只有 m_PrefilterMipCount = 5 个 mip（EnvironmentLightingManager.h:106）。roughness = 1 时采样到 mip 8，被硬件 clamp 到最后一个 mip。

**后果**：高光粗糙度响应在 roughness ≈ 0.6 之后被完全压平——粗糙度滑杆的后半程失去意义。

**修复**：由渲染器在烘焙后回填真实值（DeferredRenderer.cpp 的 Update）：

    s_EnvironmentLightingManager.BakeEnvironmentTextures(gfxContext);
    if (s_EnvironmentLightingManager.IsReady())
    {
        const uint32_t mipCount = s_EnvironmentLightingManager.GetPrefilterMipCount();
        Scene::LightingSystem::SetPrefilterMaxMip(mipCount > 1 ? (float)(mipCount - 1) : 1.0f);
    }

**顺序理由**：SetPrefilterMaxMip 会清掉 LightingSystem 的「已应用」标志，紧随其后的 ApplyParams 会把新值一起打包上传。若反过来先 ApplyParams 再回填，新 mip 上界要拖到下一帧才生效。

**归属**：该值**刻意不进 RenderParams**。它不是用户可调参数，而是烘焙产物的资产事实；放进用户真源会形成两份可能分叉的真相。因此由 Scene::LightingSystem 持有，UI 只做只读展示。

### R4.4 IBL 状态机：m_HDRLoaded 语义反了的死标志

**问题**（EnvironmentLightingManager.cpp:182 旧写法）：

    if (m_Baked || m_HDRLoaded || !m_Textures.HDRTexture.IsValid()) return;

把「HDR 已加载」当成了**禁止烘焙**的理由——语义正好反了：HDR 已加载恰恰是**可以烘焙**的前提。

**为什么一直没炸**：m_HDRLoaded 全仓只有置 false（Initialize / Shutdown），从无置 true。该分支恒为假，缺陷被一个死标志掩盖。**一旦有人按字面意思补上赋值，烘焙就会被永久跳过，Skybox 与 IBL 全部退回空 cubemap。**

**修复**：LoadHDR 里赋予它真实语义（m_HDRLoaded = HDRTexture.IsValid()），烘焙的短路判断不再包含它。

### R4.5 AlphaTest：GBufferPS 从未 clip，MASK 材质画成实心

**问题**：GBufferPS.hlsl 里完全没有 alpha 测试。glTF alphaMode = "MASK" 的材质（如 toneriko1）镂空边缘被画成实心方块。

**数据链核实**（不是猜的）：解析 toneriko1.ty 的 MaterialConstantData，确认 mat2 带 alphaTest=1、alphaRef=0.500，其余 21 个材质为 0。说明标志位与阈值**都正确到达了 CPU 镜像**，缺的只是 Shader 分支。

**修复**：位域解码 + clip：

    #define MATFLAG_ALPHA_TEST (1u << 6)
    float DecodeAlphaRef(uint flags) { return f16tof32((flags >> 16) & 0xFFFFu); }

    if ((flags & MATFLAG_ALPHA_TEST) != 0)
        clip(basecolor.a - DecodeAlphaRef(flags));

两个关键细节：

1. alphaRef 以 **half 精度**存在高 16 位。必须用内建 f16tof32，手写移位会得到完全错误的阈值（0.5 → 6.1e-5 量级）。
2. 放在像素着色器**最前面**：被裁掉的片元不产生任何后续采样与 GBuffer 写入；clip 对 MRT 的全部目标一致生效，不会出现「部分 RT 被写」的不一致。该 PSO 同时用于 Depth Pass，因此镂空深度也一致，不会「深度实心、颜色镂空」。

---

## 2. 验证证据

命令（全部实际执行）：

    set VCPKG_ROOT=F:/VS/rep/vcpkg
    scripts\build.bat Debug        :: exit 0
    scripts\build.bat Release      :: exit 0
    RendererDemoApp.exe -R4Test <abs-path-to-docs\AI\R4\evidence>   :: exit 0

### 2.1 构建

| 配置 | 结果 | 说明 |
|---|---|---|
| Debug | exit 0 | 3 个 Shader 重新编译（GBufferPS / LightingPS / SkyboxPS），见构建日志 [1/14]–[3/14] |
| Release | exit 0 | 同源同 Shader 链 |

Shader 重编译是必要的取证：生成头、PCH、缓存都可能让改动静默失效。

### 2.2 逐条验收（A/B 单变量）

| 验收项 | 操作 | 结果 | 证据 |
|---|---|---|---|
| **sRGB 编码真实生效** | 只翻 EnableSRGBEncode | 模型区 176.9 → 132.3，md5 16c8ae… → 394922… | r4_00 vs r4_01 |
| **ACES 作用在 HDR 上** | 只翻 EnableTonemap | 关闭后模型 32.2%、天空 67.2% 硬截断 | r4_02 |
| **曝光是乘性缩放** | 1.0 → 0.5 | 模型 176.9 → 142.6，天空 224.9 → 194.7，均单调下降 | r4_03 |
| **Skybox 跟随曝光** | 1.0 → 4.0 | 天空 224.9 → 250.2（100% 截断） | r4_04 |
| **AlphaTest(MASK)** | 关/开 clip 的 A/B 构建 | 差异**只**落在镂空区 bbox x[670..696] y[114..120]（67 px）；关掉时该区被写成 RGB(0,0,0) 实心，开启后露出天空 | r4_10_alphatest_ab_zoom.png |
| **IBL 只留环境光** | 关太阳+点光 | 模型 167.1 | r4_07 |
| **IBL 关断对照** | 再关 IBL | 模型 167.1 → 146.5，证明 IBL 通路真实贡献 | r4_08 |
| **IBL 状态机可观测** | 启动日志 | [R4] IBL 烘焙完成：HDRLoaded=1 PrefilterMips=5 (roughness 映射 0..4) | tooiyume_log.txt:19 |
| **Prefilter mip 上界** | 同上日志 | 映射上界 4（真实 mip 数 5 − 1），不再是写死的 8.0 | 同上 |
| **无描述符泄漏** | 10 步连续切换 | RTV 13 / SRV 65 / pool 8 全程恒定（切模型瞬间 SRV 58 属正常资源差异） | [R4-Test] 每行 |
| **可重复性** | 首帧 vs 末帧 | r4_00 与 r4_09 **md5 完全相同** 16c8aeec294844b9e3fedde4df78a349 | 状态复原正确 |
| **Debug Layer** | 全流程 | grep -c "D3D12 ERROR" = **0** | 日志 |

### 2.4 架构自检（ai-check.ps1）

`docs/AI/渲染器AI编程流程skill/scripts/ai-check.ps1` 报 **6 项 RED**：

| 闸门 | 现状 | 归属 |
|---|---|---|
| R1 Scene 不得含 D3D12 类型 | 16 处 | WP0–WP8 已排期的架构债 |
| R2 RHI 不得 include 上层 | 6 处 | 同上 |
| R3 Scene 不得 include 上层 | 9 处 | 同上 |
| R4 AssetSystem 不得依赖 Renderer | 2 处 | 同上 |
| R13 全局堆不得 extern 泄漏 | 2 处 | 同上 |
| R9 Demos 不得引用 DrawPass | 5 处 | 同上 |

**这 6 项在 R4 之前即已是红灯**，不是本次引入。核对方式：`git diff` 中新增的 `#include` 只有 `04_Renderer/RenderParams.h`、`02_RHI/GraphicsCore.h`、`<cstring>`，三者均来自 R3 的未提交改动——**R4 自身没有新增任何跨层依赖**。

按项目规则「发现新问题登记、不顺手做」，这些闸门属 WP0–WP8 排期债，不在 R4 范围内修复。

**AlphaTest 的证据是本轮最硬的一条**：r4_10_alphatest_ab_zoom.png 左半为关闭 clip 的构建，右半为开启。左图角色眼睛是**实心黑块**（透明区被当作不透明几何绘制），右图显示真实虹膜。这是「缺陷存在」与「修复生效」的同时证明。

### 2.3 画面改善量化

| 指标 | R4 前 | R4 后 | 变化 |
|---|---|---|---|
| 模型区平均亮度 | 136.8 | 176.9 | +29% |
| 模型区硬截断 | 23.0% | 0.1% | 截断基本消除 |
| 天空区硬截断 | 52.0% | 0.0% | 截断完全消除 |

注意：这不是「把画面调亮」，而是「把被截断的 HDR 信息按正确曲线映射到 8bit」。R4 前的 23%/52% 截断意味着这些像素的信息已经丢失，单纯调亮度无法恢复。

---

## 3. 未验证 / 已知限制

| 项 | 状态 | 说明 |
|---|---|---|
| 视觉回归基准的人工确认 | **未验证** | 量化 + A/B 已足够证明链路正确，但「画面是否美观」属人肉项，需用户自行判断 |
| PIX/RenderDoc GPU 抓帧 | **未执行** | 本机未做捕获；Dispatch 有日志与资源状态佐证，但缺 GPU 事件级证据 |
| Forward 的光照 | 不在 R4 范围 | DefaultPS 仍只输出 baseColor，面板已如实禁用（R3 遗留） |
| AlphaBlend（BLEND） | 不在 R4 范围 | 路线图 §7.1 明确「BLEND 与 AlphaTest 分开排期」 |
| 其他 MASK 材质的覆盖 | **部分验证** | 只用 toneriko1（唯一带 MASK 且已在模型目录的模型）验证；其余模型无 MASK 材质 |
| IBL 烘焙开销 | **未测量** | 烘焙只在首帧执行一次，未做 GPU 时间采样 |
| m_HDRLoaded 修复的独立 A/B | **无法独立构造** | 修复前该标志恒假，缺陷不可达；因此只能做静态论证 + 修复后日志（HDRLoaded=1 且烘焙仍发生），无法构造「修复前会崩」的运行证据 |

关于最后一项的诚实说明：这条修复是**防御性**的——它修的是一个当前不可达、但语义明显错误的条件。状态定为 DONE 的理由是语义已改正且烘焙行为经日志验证仍正确；但它的「缺陷严重性」不应被夸大。

---

## 4. 变更文件

| 文件 | 改动 |
|---|---|
| Engine/Shader/02_Deferred/ColorPipeline.hlsli | **新增**。LinearToSRGB / ACESFilm / EncodeDisplayColor 唯一实现 |
| Engine/Shader/02_Deferred/LightingPS.hlsl | 引入共享链；删本地 ACES 副本；cbuffer Light 追加 DisplayParams；mip 上界改读 CB |
| Engine/Shader/06_Environment/SkyboxPS.hlsl | 接入共享链；cbuffer CameraCB 追加 gDisplayParams |
| Engine/Shader/02_Deferred/GBufferPS.hlsl | 标志位解码宏 + DecodeAlphaRef + MASK clip |
| Engine/04_Renderer/RenderParams.h | 新增 EnableSRGBEncode；块大小 96 → 112；记录 mip 上界归属决策 |
| Engine/05_Scene/Lighting/LightingSystem.h/.cpp | LightData 追加 DisplayParams；新增 Set/GetPrefilterMaxMip（清缓存触发重传） |
| Engine/04_Renderer/EnvironmentLightingManager.h/.cpp | SkyboxPass 增参数与 CB；修正 m_HDRLoaded 语义；新增 IsHDRLoaded |
| Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp | 烘焙→回填→ApplyParams 顺序；Skybox 传参；烘焙诊断日志 |
| Demos/RendererDemoApp/RendererDemoApp.cpp | -R4Test 验收（10 步）；DevTools 加 sRGB 开关与只读 mip 展示 |
---

## 5. 关键设计决策

| 决策 | 选择 | 替代方案与代价 |
|---|---|---|
| 色彩链落点 | 独立 ColorPipeline.hlsli，两个 Pass 共用 | 各写一份：改一处漏一处，正是修复前的 bug 来源 |
| sRGB 实现 | 精确 IEC 曲线 | pow(1/2.2) 近似：暗部偏差数个 8bit 级，破坏 A/B 可比性 |
| mip 上界归属 | 渲染器派生状态，UI 只读 | 放进 RenderParams：形成「用户真源 vs 资产事实」两份可能分叉的真相 |
| AlphaTest 位置 | 像素着色器最前 | 放在采样之后：无谓开销，且易漏写某个 RT |
| AlphaRef 解码 | f16tof32 内建 | 手写位运算：half 指数/尾数布局不同，结果量级全错 |
| Skybox 参数传递 | 显式形参 const RenderParams& | 内部读全局：类型上无法保证天空与模型用同一份曝光 |

---

## 6. 学习路线

### 6.1 建议阅读顺序

1. Engine/Shader/02_Deferred/ColorPipeline.hlsli（最短，先建立「一条链」的概念）
2. Engine/04_Renderer/RenderParams.h（CPU 真源，看 112 字节契约注释）
3. Engine/05_Scene/Lighting/LightingSystem.cpp 的 PackParams（CPU → GPU 镜像的唯一折叠点）
4. Engine/Shader/02_Deferred/LightingPS.hlsl 末尾 40 行（消费端）
5. Engine/Shader/06_Environment/SkyboxPS.hlsl（对照：另一个消费者）
6. Engine/04_Renderer/EnvironmentLightingManager.cpp 的 BakeEnvironmentTextures 与四个 Pass
7. Engine/Shader/02_Deferred/GBufferPS.hlsl 开头 40 行（AlphaTest）

### 6.2 必须掌握的概念

- **HDR 线性空间 vs 显示空间**：*_UNORM 目标不会自动做 gamma 编码
- **为什么 tonemap 必须在曝光之后**：ACES 的输入定义域是已曝光的 HDR
- **mip 与 roughness 的对应**：预滤波立方体的 mip 数决定粗糙度分辨率上限
- **多 RT 的 clip 语义**：像素着色器终止对全部 RT 一致生效
- **half 精度在常量缓冲里的存储**：glTF 的 alphaCutoff 为何用 half

### 6.3 关键机制如何连接

```
RenderParams (CPU 真源)
   | PackParams()
   v
LightData (GPU 镜像, 112B) --WriteBuffer--> StructuredBuffer --CBV--> cbuffer Light
   ^                                                                     |
   | SetPrefilterMaxMip (渲染器烘焙后回填)                                 v
EnvironmentLightingManager --烘焙--> PrefilterCube(5 mip)      EncodeDisplayColor()
                                                                          |
SkyboxPS -- 同一函数 ------------------------------------------------------+
```

---

## 7. 主动回忆问题

1. 为什么把 ACES 与 sRGB 编码抽到 ColorPipeline.hlsli，而不是各自留在 Lighting 与 Skybox 里？修复前的具体症状是什么？
2. SceneColorBuffer 与 swapchain 都是 R8G8B8A8_UNORM。如果完全不写 sRGB 编码这一步，画面会偏亮还是偏暗？r4_01 的量化数据支持哪个方向？
3. maxPrefilterMip 写死 8.0 而 PrefilterCube 只有 5 个 mip。超出范围的 mip 请求会发生什么？为什么这个错误「看起来能用」？
4. SetPrefilterMaxMip 为什么会清掉 s_bHasApplied？如果不清，新的 mip 上界要多久才能生效？
5. m_HDRLoaded 的旧短路条件为什么一直没暴露问题？如果按字面语义补上赋值，会发生什么？
6. alphaRef 用 f16tof32 解码，若改写成 (flags >> 16) / 65535.0，alphaCutoff = 0.5 会变成多少？
7. clip 放在 GBufferPS 的最前面而不是采样之后，除了省开销还有什么正确性理由？

---

## 8. 下一项候选（不自动开始）

**R5.0 前置：DXR 设备能力与请求链**（路线图 §8 D0）——RequiresRaytracingSupport() 的 const override 签名问题、Raytracing Tier 日志、无 DXR 时 UI 禁用。

另有两个 R4 顺带发现、**未处理**的候选：

- Forward 的 DefaultPS 仍不消费光照（R3 遗留，路线图 §7.3 要求接通或明确标注）
- PipelineStateCache::s_PipelineDescs 的 Reset() 仍无调用者，跨 Renderer 单调增长（R_执行表 §7，归 R6）