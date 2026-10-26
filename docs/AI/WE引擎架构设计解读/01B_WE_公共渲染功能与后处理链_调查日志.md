task-2 调查日志（we-renderfeat）：WE 渲染路径与公共渲染功能分离调查
【任务与身份】队友 we-renderfeat，负责共享任务 task-2。目标：精读 WickedEngine 源码，产出「渲染路径与公共渲染功能分离」的源码级证据报告，正面回答约束文档 §2.1（本版本 WE 里 Forward / Deferred / Tiled Rendering 与 Path Tracing 如何组织）。9 个必答问题见任务书。
【交付物（均已完成写入）】
- 报告：D:\CS-Self-Study\Computer_Graphics\DX12\TooiyumeRender\docs\AI\ToiyumeRender 架构演进专项约束\01B_WE_公共渲染功能与后处理链.md —— 505 行，12 章（结论速览 / 调查方法 / Q1 依赖矩阵 / Q2 DrawScene 语义 / Q3 核心问题 / Q4 后处理链顺序 / Q5 ping-pong / Q6 资源结构体 / Q7 生命周期与同步 / Q8 虚函数扩展面 / Q9 规模适用性 / 未确认清单 + 一句话总结）。文件名按 docs/AI/ToiyumeRender 架构演进专项约束/00_总览与阅读指南.md:70 的分配（该行把 01B 分配给 we-renderfeat，标题为 公共渲染功能与后处理链）。
- 本日志：D:\CS-Self-Study\Computer_Graphics\DX12\TooiyumeRender\docs\AIWorkLogs\01B_WE_公共渲染功能与后处理链_调查日志.md。
【关键实测结论（一句话）】本版本不存在 wiRenderPath3D_Deferred/_Forward/_Tiled（唯一子类是 wiRenderPath3D_PathTracing）；无 RENDERMODE 符号；唯一光照方案是 tiled(clustered) forward+（tile 32×32、entityTiles bucket buffer、Harada 2.5D 剔除），有两个执行模型由运行期 bool visibility_shading_in_compute 选择（PS 光栅 / visibility buffer compute 着色），不是 forward/deferred 切换；PathTracing 以整体重写 Render() 的方式替换 RT 管线并复制了整条后处理链；后处理链唯一 HDR→LDR 分界是 Postprocess_Tonemap，FXAA 在其后，FSR2 在链首、FSR1 在链尾。
【实际读过的文件与行范围（证据基础，全部为只读）】
- WickedEngine/wiRenderer.h（1468 行）：精读 :1-59（include/常量/helper）、:94-350（API 声明）、:385-1046（全部按效果 Resources 结构体与函数签名）。
- WickedEngine/wiRenderer.cpp（19687 行）：精读 :1-20（include）、:1947-1964、:2147、:3163-3166、:3257、:3363-3365、:3498（RENDERPASS 关键分支）、:7179-7283（DrawScene 完整实现体）、:10158-10173（CreateTiledLightResources / ComputeTiledLightCulling）、:11416-11438（CreateBloomResources）、:12841（CreateSSAOResources）、:17157（CreateTemporalAAResources）。未整读。
- WickedEngine/wiRenderPath3D.h（403 行）全文；WickedEngine/wiRenderPath3D.cpp（3327 行）：:1-16、:17-341（DeleteGPUResources/ResizeBuffers）、:375-830（PreRender）、:832-1786（Render 全文）、:1826-2332（9 个 Render* 虚函数实现）、:2334-2564（RenderPostprocessChain）、:2566-2790（RenderCameraComponents）、:2790-3320（setter）。
- WickedEngine/wiRenderPath3D_PathTracing.h（53 行）与 .cpp（733 行）全文。
- WickedEngine/wiRenderPath.h（51）、wiRenderPath2D.h（142）、wiRenderPath2D.cpp（550）全文要点。
- WickedEngine/wiApplication.cpp（938）：:64-90（ActivatePath）、:400-440（Update/FixedUpdate/Render/Compose 钩子调用）；WickedEngine/WickedEngine.h:137（using MainComponent = wi::Application）。
- WickedEngine/wiGraphics.h（GPUResource shared_ptr、ResourceState、GPUBarrier 与 Aliasing 构造）、wiGraphicsDevice.h（RenderPassBegin/End、Barrier、FlushDeallocations、AliasingAllocator:449-506）、wiGraphicsDevice_DX12.cpp:6079-6137 与 :6323-6377（LoadOp/StoreOp → D3D12 render pass access、自动 barrier 提交）。
- WickedEngine/shaders/：ShaderInterop_Renderer.h（:1148、:1223-1228、:1399）、lightCullingCS.hlsl（:4、:9-11、:16-34、:400-417）、shadingHF.hlsli（:43-49、:408）、globals.hlsli(:594-596)、visibility_shadeCS.hlsl（:1-30、:94）、objectHF.hlsli、hairparticlePS.hlsl:78、impostorPS.hlsl:59、oceanSurfacePS.hlsl:110、emittedparticlePS_soft.hlsl:128、tonemapCS.hlsl（:95-194）、bloomseparateCS.hlsl（:8-35）、ShaderInterop_Postprocess.h（:9-15、:30-38、:129-150）、postprocessVS.hlsl。
【用过的命令】grep -n / grep -rln（定位符号与调用点）、grep -c（验证零依赖：grep -c RenderPath wiRenderer.h/wiRenderer.cpp = 0、grep -c Application = 0、grep -c GetScene() wiRenderer.cpp = 0）、sed -n 'A,Bp'（区间精读大文件）、ls -la 与 wc -l（目录与行数核对）、grep -rln MainComponent / RENDERMODE（排除 third_party）。全程未运行构建、未执行 git checkout/restore/stash、未修改任何运行时代码。
【实测 vs 推断】报告内每条结论都标注了「实测」或「推断」；推断项集中在 RENDERPASS 的 PSO/着色器物化差异（只核实了 6 处关键分支站点，未读全 67 处）与 RENDERPASS_PREPASS_DEPTHONLY 是否绑定颜色 RT。报告 §11 列出 7 条未确认项及所需读取位置。
【踩到的坑与修正】
1. 最初以为 wiRenderPath3D.cpp 存在 RenderScene 函数（受旧版 WE 印象影响）——实测失败：RenderScene 只作为 RenderSceneMIPChain() 出现 3 次（wiRenderPath3D.cpp:2055、2060、2189），主场景几何/光照渲染全部内联在 void RenderPath3D::Render() const（wiRenderPath3D.cpp:832-1786）。修正后才定位到 Q3 的决定性分支（:1473、:1526-1529、:1544-1582）。
2. 一度按「Forward/Deferred/Tiled 三条子类」的思路组织问题——复核后确认不存在（无对应文件、无 RENDERMODE 符号、wiRenderPath3D.h:33-93 无 rendermode 成员），改为从「唯一光照方案 + 两个执行模型」作答。
3. compress 调用两次因 TypeScript 字符串内的转义/逗号问题失败（错误信息 Expected ',', got 'ident' / Expected ',', got 'string literal'）——修正：把摘要改为字符串数组 .join("\n")、正文内不再使用双引号，之后成功。
4. grep -rln MainComponent 只命中 using MainComponent = wi::Application;（WickedEngine/WickedEngine.h:137），说明约束文档 §2.1 提到的 MainComponent 在本版本是别名而非独立类——已写进报告结论速览。
5. 未整读 wiRenderer.cpp(19687 行) 与 wiRenderPath3D.cpp(3327 行)，全部用 grep 定位 + sed 区间读，避免上下文爆炸；代价是 RENDERPASS 分支只读了 6 处关键站点（已列入未确认清单）。
6. 报告体量大，采用「先落盘骨架 + 哨兵注释 <!--PART2-->…<!--PART5--> 分块追加、最后 grep -c 确认哨兵清零」的写法；写完实测 505 行、哨兵计数 0。
【剩余未读清单（同报告 §11）】
1. WickedEngine/wiRenderer.cpp:447-669 的 Get*TYPE shader 变体 switch、:2033-2203 的 renderpass_info 构造、:3161-3498 的 RenderMeshes/RenderImpostors PSO 与变体选择。
2. WickedEngine/wiRenderPath3D.cpp:1181-1247 的平面反射 depth prepass RenderPass 构造（确认 PREPASS_DEPTHONLY 是否绑颜色 RT）。
3. grep -rn temporalAAResources.frame WickedEngine/*.cpp（确认 TAA 帧序号自增位置）。
4. WickedEngine/wiRenderer.cpp:17761 起的 Postprocess_FSR2 实现。
5. WickedEngine/wiRenderer.cpp 内 UpdateRenderData / UpdateRenderDataAsync 的实现。
6. WickedEngine/wiGraphicsDevice_Vulkan.cpp（本报告的同步结论仅对 DX12 后端实测成立）。
7. TooiyumeRender 现有 RHI 是否支持显式 barrier / load-store 声明 / 内存别名（属 02_架构现状审计 范围，本次未读）。
【对 TooiyumeRender 的建议落点（详见报告 §10）】可直接借鉴：wi::renderer 式无状态公共功能层、DrawScene 的 flags/renderPass 两轴分离、按效果资源结构体（少量起步）、单一 tonemap 分界 + 后处理契约表、ping-pong 读写上下文、RenderPassBegin 式 load/store 声明。必须简化：23 个资源结构体按需增长、不用 bindless 负索引哨兵。规模不匹配/延后：tiled 光列表、visibility compute shading、内存别名(AliasingAllocator)、FSR2、异步多队列。
【时间与状态】调查时间 2026-10-24（Asia/Shanghai）。报告与日志均已落盘；下一步：team_task_get 取最新 revision → team_task_update action=complete → send_message 给 lead 发 ≤40 行摘要。