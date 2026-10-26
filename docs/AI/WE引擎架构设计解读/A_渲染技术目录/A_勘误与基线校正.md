# A-勘误与基线校正（2026-10-24 23:0x）

> 本文件是 A 卷（A0–A6）的**唯一勘误权威**。A 卷任何一行与本文件冲突，以本文件为准。
> 起因：A1–A6 写完后做了一次**全量机器复核**（把卷内每一处 `文件:行号` 重新解析、重新读数），发现三类系统性缺陷。本文记录缺陷、根因、已做的修正、以及**仍未闭合**的项。

---

## §1 复核方法与可复现命令

不采信卷内自述，全部重算：

```bash
WE=<WE 路径>/WickedEngine

# 1) 目录规模（三者口径不同，必须分开报）
cd "$WE/shaders"
find . -maxdepth 1 -type f | wc -l      # 365 = 305 hlsl + 40 hlsli + 18 h + 2 vcxitems
cat *.hlsl  | wc -l                     # 22,660
cat *.hlsli | wc -l                     # 18,984
cat *.h      | wc -l                     # 4,462

# 2) 单文件逻辑行（推荐口径，与编辑器行号一致）
awk 'END{print NR}' <file>

# 3) 全量行号引用校验：对 A 卷每处 文件:行 重新解析，检查文件存在 + 行号 <= 实际行数
node audit.js        # 见 §5；首轮结果 600 处引用 / 0 不可解析 / 0 越界；A3 重写后总量升至 764 处（见 §6 U-A2）
```

**结论**：A 卷共 **600 处** `文件:行` 引用，复核后 **600 处全部可解析且行号在范围内**（复核前为 204 处引用、17 处不可解析、64 处区间端点越界）。

> **2026-10-24 23:50 终检（全部并行写入停止后，Lead 独立复跑）**：A 卷最终 **179 条 / 782 处 `文件:行` 引用**（A1:53 / A2:41 / A3:209 / A4:164 / A5:133 / A6:182），**0 处不可解析、0 处行号越界**。终检只再发现 1 处命名缺陷并已修正：`A3_后处理链.md:61` 的 MSAO 第二张预处理 CS 原写作简写 `2CS.hlsl:1-35`，全名为 `msao_preparedepthbuffers2CS.hlsl`。终检口径＝宽正则（允许 `Engine/` 前缀与含中文的路径），命令见 §5 与 `A0_索引与使用说明.md:99-101`。

---

## §2 缺陷一：行数统计「每文件 +1」（已修）

### 现象

`_data/WE_shader_inventory.txt` 与 `_data/WE_shader_inventory.md` 里**每一个文件**的行数都比真实值多 1，汇总值也随之偏大。

### 根因（可证伪）

旧汇总值恰好等于「真实行数 + 文件数」：

| 类别 | 真实（`cat *.x \| wc -l`） | 文件数 | 相加 | `_data` 旧值 |
|---|---|---|---|---|
| `.hlsl` | 22,660 | 305 | **22,965** | 22,965 ✅吻合 |
| `.hlsli` | 18,984 | 40 | **19,024** | 19,024 ✅吻合 |
| `.h` | 4,462 | 18 | **4,480** | 4,480 ✅吻合 |

即统计脚本对每个文件都多计了 1 行（典型成因：把「换行符个数」当行数后再 +1，或对 `split('\n')` 数组直接取 length，末尾换行会多出一个空元素）。

### 影响（已全部消除）

1. **A0 §5 规模基线**四个数偏大 → 已改为 22,660 / 18,984 / 4,462 / 3,326。
2. **A1–A5 的 64 处行区间端点越界**：写作时把「区间末行 = 文件行数 + 1」当成了合法端点（例：`objectHF.hlsli` 写成 1–**1153**，而实际只有 1,152 行）。已逐条改为真实末行。

   > 注：本文档自身也受同一检查脚本约束。此处**故意保留错值 1153 作为证据**，故写成「1–1153」而非 `文件:1-1153` 形式，以免被校验脚本误判为活引用。
3. **`_data/WE_shader_inventory.md` 的 354 行表格 + `.txt` 的 354 行清单**：已按 `awk 'END{print NR}'` 口径就地重算覆盖。

### 三种行数口径（务必区分）

| 命令 | 语义 | 陷阱 |
|---|---|---|
| `wc -l` | 换行符个数 | shader 目录有 **9 个文件无尾换行**（`sphereVS.hlsl`、`upsample_bilateral_float1CS.hlsl`、`volumetriclight_pointVS.hlsl`、`circle/cone/cube/cylinder/icosphere/quad.hlsli`），这些文件会**少算 1** |
| `awk 'END{print NR}'` | 逻辑行数 | **推荐**，与文本编辑器显示一致，无尾换行文件也对 |
| 「wc -l 再 +1」 | — | **一律错**，即本次缺陷 |

> 唯一例外：`wiRenderPath3D.cpp` 的 3,327 不是同一缺陷（真实 3,326，+1 来自别处），但结论相同：以实测为准。

---

## §3 缺陷二：事实性错误（已逐条修正）

| # | 位置 | 旧内容（错） | 实测事实 | 后果 |
|---|---|---|---|---|
| 1 | A2 §3 π 归一化 | 「漫反射的 `/PI` 在 `BRDF_GetDiffuse:189`，π 归一化落在 **BRDF 函数内**，不在光照累加处」 | `BRDF_GetDiffuse` **不含** `/PI`（`brdf.hlsli:203` 直接 `return diffuse * NdotL`）；`/PI` 真正在 `lightingHF.hlsli:49 ApplyLighting` | **最严重**。照旧结论抄 BRDF 会再除一次 π → 漫反射**暗 3.14×**；反向则亮 3.14×。此条直接回答 09 §8.1 的 U-1 阻塞项 |
| 2 | A1 §4 renderQueue | 「`wiRenderPath3D.cpp:929`」 | 队列声明在 `wiRenderer.cpp:338-339`（`static thread_local`）；`:929` 是 `drawscene_flags` 常量 | 按旧行号读会读到无关常量，误判「队列不可变」 |
| 3 | A1-23 毛发 | 「`DrawHairParticles`」 | **全仓无此函数**；入口是 `wiRenderer.cpp:7274` 的 `hair.Draw(material, renderPass, cmd)` | 按函数名 grep 会空手而归 |
| 4 | A1-23 毛发 | 「`hairparticlePS*.hlsl`（6 个）」 | 实测 **5 个** | 数量错 |
| 5 | A1-22 水面 | 「入口 `DrawWaterRipples:5972`」 | 水面渲染是 `wiOcean.cpp:633 Ocean::Render`（由 `wiRenderer.cpp:7205` 进）；`DrawWaterRipples` 是**涟漪**，另一件事 | 认错入口，估工对象错 |
| 6 | A1-19 笔刷半径 | 「入口 `wiRenderer.cpp:7179 DrawScene`」 | 实际在 `wiRenderer.cpp:8550`（`DrawDebugWorld` 的 `DEBUGRENDERING_PAINTRADIUS`） | 认错入口 |
| 7 | A1-07 meshlet | 「粒子上传阶段」 | `wiRenderer.cpp:5413`，在 `UpdateRenderData` 内 | 归错阶段 |
| 8 | A1-20 Occludee | 功能描述残缺（「包围盒 vs」） | `occludeeVS.hlsl:14-18` 用 14 顶点三角形条带立方体做深度查询；C++ 入口 `OcclusionCulling_Render:5834` | 描述不完整 |
| 9 | A2-13 环境探针 | 「`envMap_skyVS/PS`」 | 无 `envMap_skyPS.hlsl`；实为 `envMap_skyPS_static.hlsl` + `envMap_skyPS_dynamic.hlsl` | 文件名不存在 |
| 10 | A2-12 IBL | 「入口 `RefreshEnvProbes:9077`」 | `:9077` 是函数**起始行**；实际 filter pass 在 `:9514` | 行号指向函数头而非 pass |
| 11 | A2-20 灯光可视化 | 「`vPointLightVS`」等无扩展名 | 实为 `.hlsl`，且 PS 只有 `vRectLightPS.hlsl`（点/聚光无 PS） | 文件名残缺 |
| 12 | A3-10 景深 | 「等 11 个」 | 实测 **10 个** `depthoffield_*.hlsl` | 数量错 |
| 13 | A3-16 高斯 | 「`blur_gaussian_float{1,4}CS`（+wide）」 | 实测 4 个：`float1/float4` + `wide_float1/wide_float4` | 表述含糊 |
| 14 | A5 多处 | 简写文件名（`_sphbinningCS.hlsl`、`kickoffUpdateCS.hlsl`、`renderCS.hlsl`、`_detailnoiseCS.hlsl` 等 17 处） | 均为 `emittedparticle_*` / `volumetricCloud_*` 前缀的完整名 | **17 处引用不可解析**，按名找不到文件 |

---

## §4 缺陷三：与并行会话的时间赛跑（已处理）

复核期间（22:40–23:05）A1–A5 被并行会话**重写**，并新增 **A6 卷**（`A6_渲染基础设施与执行模型.md`，294 行 / 28 条）；23:05 后 A3 又被重写（24 → 35 条、189 → 430 行）。本文件所有修正都在**每次编辑前重读该文件**之后施加，未覆盖他人改动。

> **教训（给后续写卷者）**：A 卷是多人/多会话并行产物，任何「整文件替换」都会吞掉对方改动。**只做定点 edit，并在每次 edit 前重读**。

---

## §5 复核脚本（可直接重跑）

```js
// node audit.js  —— 校验 A 卷全部 文件:行 引用
const fs=require('fs'), path=require('path');
const DIR='<A 卷目录>', WE='<WE 路径>', TR='<项目路径>';
const idx=new Map();
(function walk(root){ const st=[root]; while(st.length){ const d=st.pop();
  for(const e of fs.readdirSync(d,{withFileTypes:true})){
    if(e.isDirectory()){ if(['out','.git','vcpkg_installed','node_modules'].includes(e.name))continue; st.push(path.join(d,e.name)); }
    else { (idx.get(e.name)||idx.set(e.name,[]).get(e.name)).push(path.join(d,e.name)); } } } })(WE);
(function(){ const st=[TR]; /* 同上，索引 TooiyumeRender */ })();
const cache={};
const realLines=f=>{ if(cache[f])return cache[f]; const b=fs.readFileSync(f); let n=0;
  for(const c of b) if(c===10)n++; if(b.length&&b[b.length-1]!==10)n++; return cache[f]=n; };
function resolve(p){ const c=[];
  if(p.startsWith('shaders/')) c.push(path.join(WE,p));
  if(p.startsWith('Engine/')) c.push(path.join(TR,p));
  if(idx.has(p)) c.push(...idx.get(p));
  for(const x of c){ try{ if(fs.statSync(x).isFile()) return x; }catch(e){} } return null; }
let total=0, bad=[];
for(const name of fs.readdirSync(DIR).filter(f=>/^A[0-9]_.*\.md$/.test(f))){
  fs.readFileSync(path.join(DIR,name),'utf8').split('\n').forEach((ln,i)=>{
    const re=/([\w\-\.\/]+\.(?:hlsl|hlsli|h|cpp|hpp)):(\d+)(?:-(\d+))?/g; let m;
    while((m=re.exec(ln))){ total++;
      const f=resolve(m[1]); if(!f){ bad.push(name+':'+(i+1)+' UNRESOLVED '+m[1]); continue; }
      const L=realLines(f), hi=m[3]?+m[3]:+m[2];
      if(+m[2]>L||hi>L) bad.push(name+':'+(i+1)+' OOR '+m[1]+' real='+L);
    } });
}
console.log('refs '+total+' problems '+bad.length); bad.forEach(b=>console.log('  '+b));
```

**首轮结果**：`refs 600 problems 0`（A0–A6）。
**2026-10-24 23:3x 复跑**：A3 被并行会话重写（24 条 → 35 条）、A6 新增并二次校订，全卷总量 **764 处**引用，`problems 0`（A6 单独复跑 170 处、27 个 WE 文件 + 13 个项目文件，0 越界）。

---

## §6 仍未闭合（下一轮要做）

| # | 项 | 为什么没做 | 怎么闭合 |
|---|---|---|---|
| U-A1 | A0 §6「`_data/WE_shader_inventory.md` 365 个 shader 按 7 域分类（A1:81/A2:48/A3:73/A4:61/A5:56/压缩:8/共享头:38）」 | 分域是**人工归类**，不是文件名可推导；重分类需要逐条判断 | 若入库 A6 的 28 条，总数会超过 365（A6 多为 C++ API，无着色器），需先定义「A6 归入哪一域」再重算 |
| U-A2 | ~~A6 卷未纳入全量复核的**语义**核对~~ **已部分闭合（2026-10-24 23:3x）** | 本轮对 A6 做了第二轮：① 全部 170 处引用**逐个文件存在性 + 行号越界**校验（27 个 WE 文件 + 13 个项目文件，0 越界）；② 对 `wiGraphics.h`（GPUBarrier / RenderPassImage / PipelineHash）、`wiGraphicsDevice.h`（BUFFERCOUNT / STATIC_SAMPLER / QueryBegin…）、`wiRenderer.cpp`（PushBarrier / FlushBarriers / ComputeShadingRateClassification）、`wiGraphicsDevice_DX12.cpp`（CreateShader 根签名反序列化 / ExecuteIndirect 六签名 / SubmitCommandLists）、`wiRenderPath3D.cpp`（异步通道 / aliasing）**逐段打印原文比对**，修正 9 处行号区间与 4 处结论措辞（PushConstant 上限、PipelineStateCache 线性比对、遮挡 PSO 行号、海洋回读归属）| 仍**未**逐条回源：A6-20（wiGPUSortLib 内部趟数）、A6-25（流送 staging 回读细节）、A6-26（登记队列实现），见 A6 §6 |
| U-A3 | 各卷「迁移判定」（必需/可选/规模不匹配）的一致性 | 判定是**工程取舍**，不是事实，无法用脚本判错 | 与 09 §6「不许抄清单」做一次人工对齐 |
| U-A4 | `_data/WE_renderer_api.md` 的 207 条 API 行号未重算 | 与本轮缺陷不同源（不来自行数脚本） | 用同一 `audit.js` 思路扩展校验 |
