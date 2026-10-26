#pragma once

#include <string>

class ColorBuffer;

// ============================================================
// GPU 截图工具（R1 诊断基础设施）
// ------------------------------------------------------------
// 为什么需要它：
//   用 PrintWindow / CopyFromScreen 抓 DX12 窗口时，拿到的可能是陈旧内容
//   （实测两次完全不同的 Shader 输出得到了字节完全相同的 PNG）。
//   这会直接导致「改了代码以为没生效」的误判。
//
// 做法：把 GPU 上的 ColorBuffer 通过 ReadbackBuffer 回读到 CPU，
// 再用 DirectXTex 的 WIC 编码器写成真正的 PNG。
// 这是 GPU 真实输出，可作为可复现的视觉证据。
//
// 限制：要求源缓冲当前处于可 CopySource 的状态，函数内部会做状态转换；
//       必须在 Render 之后、且不在其它已开启的 GraphicsContext 中间调用。
// ============================================================
namespace Renderer::DebugCapture
{
	// 把 ColorBuffer 内容导出为 PNG。返回是否成功。
	// 支持 R8G8B8A8_UNORM / R16G16B16A16_FLOAT（后者会先做简单 tonemap 压缩到 8bit）。
	bool SaveColorBufferToPNG(ColorBuffer& source, const std::wstring& filePath);
}
