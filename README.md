# ToiyumeRender

DX12 渲染器学习与改造

---

<table width="100%">
<tr>
<td width="50%" valign="top">

<h3>📅 学习日志 (Learning Log)</h3>
<blockquote>记录每日的开发进度、踩坑点与图形学理论学习。</blockquote>

<!-- ================= 年 ================= -->
<details open>
<summary><b>📂 2026年</b></summary>

<br>

<!-- ================= 月 ================= -->
<details open>
<summary><b>📅 2026-04</b></summary>

<br>

<!-- ===== 日 ===== -->
<details>
<summary><b>2026-04-08</b></summary>
<p>完成 RootSignature 类的编写，学习了利用位图存储索引，使用位运算查找索引的高效遍历方法。</p>
<p>完成 DynamicDescriptorHeap 类的编写。</p>
</details>

<details>
<summary><b>2026-04-09</b></summary>
<p>啃DynamicDescriptorHeap类，方法没啃完，逻辑关系有些复杂还没理清，明日继续</p>
</details>

<details>
<summary><b>2026-04-10</b></summary>
<p>啃完DynamicDescriptorHeap类，资源管理全流程大致梳理一遍。</p>
<p>CPU端 DescriptorAllocator：管理碎片化资源堆</p>
<p>GPU端 DynamicDescriptorHeap：管理可见堆</p>
<p>DescriptorHandleCache：作为中转缓存</p>
</details>

<details>
<summary><b>2026-04-11</b></summary>
<p>啃完CommandAllocatorPool类，CommandListManager类</p>
<p>学习跨队列同步（Fence + 生产者消费者模型）</p>
</details>

<details>
<summary><b>2026-04-12</b></summary>
<p>在啃CommandContext类</p>
</details>

<details>
<summary><b>2026-04-13</b></summary>
<p>继续CommandContext类，完成PipelineState类</p>
</details>

<details>
<summary><b>2026-04-14</b></summary>
<p>命令模块完成，学习WC/WB内存模型</p>
</details>

<details>
<summary><b>2026-04-16</b></summary>
<p>运行流程梳理，框架初步完成</p>
</details>

<details>
<summary><b>2026-04-17</b></summary>
<p>Graphics::Initialize 完成</p>
</details>

<details>
<summary><b>2026-04-19</b></summary>
<p>重构架构：Camera / Model / Renderer</p>
</details>

<details>
<summary><b>2026-04-20</b></summary>
<p>ConstantBuffer完成，Model未完成</p>
</details>

<details>
<summary><b>2026-04-21</b></summary>
<p>Model + glTF 完成</p>
</details>

<details>
<summary><b>2026-04-23</b></summary>
<p>Model模块接近完成</p>
</details>

<details>
<summary><b>2026-04-24</b></summary>
<p>配置DXC</p>
</details>

<details>
<summary><b>2026-04-25</b></summary>
<p>Shader编译流程搭建</p>
</details>

<details>
<summary><b>2026-04-26</b></summary>
<p>Renderer完成，Model模块完成</p>
</details>

<details>
<summary><b>2026-04-27</b></summary>
<p>流程跑通，材质系统待扩展</p>
</details>

<details>
<summary><b>2026-04-28</b></summary>
<p>排BUG（纹理 / 顶点 / VS输出异常）</p>
</details>

<details>
<summary><b>2026-04-29</b></summary>
<p>Mesh CB绑定问题，Debug Layer问题</p>
</details>

<details>
<summary><b>2026-04-30</b></summary>
<p>修复DFS写入问题，统一Factory管理</p>
</details>

</details> <!-- 月结束 -->

<br>

<!-- ================= 月 ================= -->
<details open>
<summary><b>📅 2026-05</b></summary>

<br>

<details>
<summary><b>2026-05-02</b></summary>
<p>深度测试问题（NDC z错误）</p>
</details>

<details>
<summary><b>2026-05-03</b></summary>
<p>矩阵问题未修复，准备重写</p>
</details>

<details>
<summary><b>2026-05-04</b></summary>
<p>渲染器框架完成</p>
<p>问题定位：DepthBuffer默认值错误</p>
<p>TODO：反转Z / 无限Z</p>
</details>

</details> <!-- 月结束 -->

</details> <!-- 年结束 -->

</td>
<td width="50%" valign="top">

<h3>🖼️ 渲染成果展示 (Gallery)</h3>

<div align="center">
<img src="https://github.com/user-attachments/assets/e1615c8f-0794-4747-83cb-c486a59db285" width="100%" alt="TAA前"/>
<br>
<img src="https://github.com/user-attachments/assets/e1b77377-6160-4819-b1d9-a07036f38526" width="100%" alt="TAA后"/>
<br><br>
<b>时间性抗锯齿</b><br>
Temporal Anti-Aliasing (TAA)<br>
テンポラルアンチエイリアシング
</div>

</td>
</tr>
</table>