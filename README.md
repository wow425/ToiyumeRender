# ToiyumeRender

DX12 渲染器学习与改造

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
<p>啃完DynamicDescriptorHeap类，资源管理全流程大致梳理一遍。<p>
<p>CPU端处创建存放碎片化的资源堆，也就是DescriptorAllocator 负责这一职能，<p>
<p>GPU端处创建着色器可视堆，也就是DynamicDescriptorHeap类负责这一职能，<p>
<p>而CPU端处创建的DescriptorHandleCache (属于 DynamicDescriptorHeap 的内部结构)的描述符句柄缓存，负责存储根签名所需要的资源句柄，将根签名所需要的资源从碎片化的资源堆运往着色器可视堆，起到中转站作用。<p>
</p>
</details>

<br>

<details>
<summary><b>2026-04-11: </b></summary>
<p> 啃完CommandAllocatorPool类，CommandListManager类<p>
<p> 命令分配器池的锁是成员锁，确保不同类型的命令队列独立且并行。而描述符分配器池的锁是类静态锁，实现全局不竞争。<p>
<p> void CommandQueue::StallForFence(uint64_t FenceValue)方法学到了跨队列同步，生产者-消费者模型，自动化路由<p>
<p>了解到帧图架构，以后实现<p>
</p>
</details>

<details>
<summary><b>2026-04-12: </b></summary>
<p> 在啃CommandContext类<p>
</p>
</details>

<details>
<summary><b>2026-04-13: </b></summary>
<p> 在啃CommandContext类,编写完Pipelinestate类<p>
<p> 今日课多，没投入多少时间<p>
</p>
</details>

<details>
<summary><b>2026-04-14: </b></summary>
<p> 命令模块啃完<p>
<p> 通过SIMDMemCopy方法了解到通过读写混合缓冲区WCB来单独开辟一条适合单方向海量对齐数据传递给GPU的道路，<p>
<p> CPU对待内存写入有两套机制<p>
<p>WB回写WriteBack<p>
<p>数据先写到Cache中，再从Cache传输到主存中<p>
<p>WC写入合并WriteCombined<p>
<p>专为跨总线传输大量单项数据而设计的，上传堆默认是WC<p>
<p>绕开Cache，数据直接写在CPU的写入合并缓冲区WCB中，然后打包传输给PCIe总线上<p>
</p>
</details>

<details>
<summary><b>2026-04-16: </b></summary>
<p>运行流程梳理一遍，运行框架完成，今日花费时间不足<p>
<p> 学习了变量生命周期管理 <p>
<p>1.命令空间内的变量（在命名空间中直接定义的变量（如果在 .cpp 中且不在任何函数内），默认具有静态存储期）<p>
<p>2. 单例模式与静态成员<p>
<p>3. 常驻内存池与资源管理<p>
</p>
</details>

<details>
<summary><b>2026-04-17: </b></summary>
<p>Graphics::Initialize();图形模块初始化及其内部一些代码搞完<p>
</p>
</details>

<details>
<summary><b>2026-04-19: </b></summary>
<p>优化代码存储逻辑层次<p>
<p>增加Camera类，Model类（尚未完成），Renderer类（尚未完成），SystemTime类<p>
<p>再次删减多余渲染功能代码，之前删减不彻底导致现在继续学习依旧困难重重<p>
<p>迄今为止，前面学习没意识到问题，沿着流程实现一个方法，就顺带着把该方法的类全都实现了，导致陷入细节漩涡中，现在改用先完成最小化可运行，再逐渐增加功能的策略<p>
</p>
</details>

<details>
<summary><b>2026-04-20: </b></summary>
<p>ConstantBuffers完成，编写Model类未完成<p>
</p>
</details>

<details>
<summary><b>2026-04-21: </b></summary>
<p>model类完成，glTF类完成<p>
<p>model模块中的优化手段基本阉割掉，以后再添加<p>
<p>进一步熟悉模型的层次结构图和利用掩码图取代遍历查找<p>
</p>
</details>

<details>
<summary><b>2026-04-23: </b></summary>
<p>Model模块只差renderer方法编写就完成了，许多底层优化的都跳过了，未来再补<p>
</p>
</details>

<details>
<summary><b>2026-04-24: </b></summary>
<p>今日摸鱼，配置了DXC<p>
</p>
</details>

<details>
<summary><b>2026-04-25: </b></summary>
<p>今日娱乐，也就重新配置DXC以生成16进制的头文件格式，复刻miniengine的shader编译模式，并粗略编写了defaultVS，defaultPS<p>
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