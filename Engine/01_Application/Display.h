#pragma once

/*该类职责：“Present Queue + SwapChain 的抽象层”
*      1.管理SwapChain
*      2.管理BackBuffer资源封装
*     3. 执行Present操作
*
*
*/


#include <cstdint>

class ColorBuffer;

namespace Display
{
	extern std::shared_ptr<ColorBuffer> SceneColorBuffer;
	void Initialize(void);
	void Shutdown(void);
	void Resize(uint32_t width, uint32_t height);
	void Present();
	// 创建叠加层，水平模糊缓冲区
	void ResizeDisplayDependentBuffers(uint32_t NativeWidth, uint32_t NativeHeight);
}

namespace Graphics
{
	extern uint32_t g_DisplayWidth;
	extern uint32_t g_DisplayHeight;

	extern Microsoft::WRL::ComPtr<IDXGIFactory6> g_DXGIFactory; // 单例/全局共享，GraphicsCore创建，Display使用，用于交换链和窗口消息显示

	// 返回帧数
	uint64_t GetFrameCount(void);

	//上一个已完成帧所消耗的时间。在这一帧的部分时间内，CPU 和 GPU 可能会处于空闲状态。
	// 帧耗时（Frame Time）衡量的是两次“呈现（Present）”调用之间的时间间隔。
	float GetFrameTime(void);

	// 每秒总帧数
	float GetFrameRate(void);


}
