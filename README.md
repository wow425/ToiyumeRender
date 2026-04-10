# ToiyumeRender

基于 DX12 龙书代码开始 DX12 渲染器学习与改造

---

<table width="100%">
<tr>
<td width="50%" valign="top">

<h3>📅 学习日志 (Learning Log)</h3>
<blockquote>记录每日的开发进度、踩坑点与图形学理论学习。</blockquote>

<details>
<summary><b>2026-04-08: </b></summary>
<p>完成 RootSignature 类的编写，学习了利用位图存储索引，使用位运算查找索引的高效遍历方法。</p>
<p>完成 DynamicDescriptorHeap 类的编写。</p>
</details>

<br>

<details>
<summary><b>2026-04-09: </b></summary>
<p>啃DynamicDescriptorHeap类，方法没啃完，逻辑关系有些复杂还没理清，明日继续</p>
</details>

<br>

<details>
<summary><b>2026-04-10: </b></summary>
<p>啃完DynamicDescriptorHeap类，资源管理全流程大致梳理一遍。
CPU端处创建存放碎片化的资源堆，也就是DescriptorAllocator 负责这一职能，
GPU端处创建着色器可视堆，也就是DynamicDescriptorHeap类负责这一职能，
而CPU端处创建的DescriptorHandleCache (属于 DynamicDescriptorHeap 的内部结构)的描述符句柄缓存，负责存储根签名所需要的资源句柄，将根签名所需要的资源从碎片化的资源堆运往着色器可视堆，起到中转站作用。
<img src="https://github.com/user-attachments/assets/210a8faa-0e70-4f60-8a83-e9cf296c819a" width="100%" alt="流程图"/>
</p>
</details>

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