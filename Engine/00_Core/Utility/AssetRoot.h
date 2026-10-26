#pragma once

#include <string>

/*
 * AssetRoot —— 统一的资源路径解析（R0.2）
 *
 * 解决的问题（对应审计报告 §2.7「硬编码绝对路径 18 处」）：
 *   原来 3 个 Demo 和 DeferredRenderer 里写死了
 *     L"D:/CS-Self-Study/Computer_Graphics/DX12/TooiyumeRender/Assets/Model/..."
 *   换机器、改目录名、放到 CI 上都会直接失败，而且失败表现是「一行日志 + 黑屏」，
 *   很难定位。这里把「资源根目录」收敛成一个可配置的单一来源。
 *
 * 解析优先级（从高到低）：
 *   1. 命令行 -AssetsRoot <path>          —— 显式覆盖，CI/脚本用
 *   2. 环境变量 TOOIYUME_ASSETS_ROOT     —— 部署/开发机用
 *   3. 从 exe 所在目录逐级向上找含 "Assets" 的祖先目录（最多 8 层）
 *      —— 覆盖 <root>/out/build/x64-debug/Demos/DeferredApp/ 这类 CMake 产物布局
 *   4. 从当前工作目录逐级向上找（同样的规则）—— 覆盖从 IDE 启动的情况
 *   5. 编译期注入的 TOOIYUME_SOURCE_ASSETS_DIR（CMake 提供，兜底）
 *
 * 这样设计的原因：
 *   资源目录既可能在源码树里（开发期），也可能被复制到 exe 旁边（发布期）。
 *   「向上找包含 Assets 的祖先」这一条规则同时覆盖两种情况，且不需要硬编码层级。
 */
namespace Asset
{
	// 初始化并解析出资源根目录。必须在任何资源加载之前调用。
	// 返回 false 表示没有找到可用的资源根（此时 g_AssetsRoot 为空）。
	bool Initialize();

	// 已解析出的资源根，形如 "D:\\...\\TooiyumeRender\\Assets\\"（带尾部分隔符）。
	// Initialize 之前为空字符串。
	extern std::wstring g_AssetsRoot;

	// 把「相对 Assets 的路径」拼成绝对路径。
	// 例：Resolve(L"Model/Merlin/merlin.gltf") -> "<root>/Assets/Model/Merlin/merlin.gltf"
	std::wstring Resolve(const std::wstring& relativePath);

	// 检查某个相对路径对应的文件是否存在（用于启动期的资产自检）。
	bool Exists(const std::wstring& relativePath);

	// 打印解析结果与失败原因，供启动日志使用。
	void LogResolution();
}
