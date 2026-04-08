# ToiyumeRender

A modern DirectX 12 rendering engine built from scratch.
(ゼロから構築されたモダンな DirectX 12 レンダリングエンジン)

---

## 📅 学习日志 (Learning Log / 学習ログ)

> 记录每日的开发进度、踩坑点与图形学理论学习。
> Daily development progress, bug fixes, and CG theory studies.
> 日々の開発進捗、バグ修正、およびCG理論の学習記録。

<details>
<summary><b>2026-04-08: 封装 DX12 根签名绑定 (Root Signature Binding)</b></summary>

* **中文**: 
  * **进度**: 使用 C++11 Lambda 表达式重构了描述符表 (Descriptor Table) 的绑定逻辑。
  * **踩坑**: 之前因为指针生命周期问题导致了 GPU Device Removed 崩溃，现已通过智能指针管理解决。
* **English**:
  * **Progress**: Refactored the binding logic of Descriptor Tables using C++11 Lambda expressions.
  * **Issue**: Resolved a "GPU Device Removed" crash caused by pointer lifecycle issues, now managed via smart pointers.
* **日本語**:
  * **進捗**: C++11のラムダ式を用いて、デスクリプタテーブル (Descriptor Table) のバインドロジックをリファクタリングしました。
  * **課題**: 以前、ポインタのライフサイクル問題により「GPU Device Removed」クラッシュが発生していましたが、スマートポインタによる管理で解決しました。

</details>

<details>
<summary><b>2026-04-07: 初始化项目基建 (Project Initialization)</b></summary>

* 搭建了基础的 Win32 窗口，并成功初始化了 DX12 设备 (ID3D12Device)。
* Set up the basic Win32 window and successfully initialized the DX12 device.
* 基本的なWin32ウィンドウを構築し、DX12デバイスの初期化に成功しました。

</details>
