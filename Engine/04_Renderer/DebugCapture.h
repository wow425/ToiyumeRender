// ============================================================================
// 模块：DebugCapture（GPU 回读截帧工具：LDR PNG / HDR Tonemapped PNG / 线性 .hdr）
// 架构层：Engine/04_Renderer（WP-2C / TD-10c）
// 核心职责：通过 ReadbackBuffer 将 GPU ColorBuffer 真实内容同步回读到 CPU 并落盘，
//           支持 R8G8B8A8_UNORM、R16G16B16A16_FLOAT（Linear HDR）与 R16G16_FLOAT（Normal GBuffer），
//           并新增无损线性 Radiance .hdr 导出供 Phase 4 科研同色域误差评估使用。
// ============================================================================
#pragma once

#include <string>

class ColorBuffer;

namespace Renderer::DebugCapture
{
	// 把 ColorBuffer 内容导出为 8-bit PNG（支持 R8G8B8A8_UNORM / R16G16B16A16_FLOAT / R16G16_FLOAT）。
	bool SaveColorBufferToPNG(ColorBuffer& source, const std::wstring& filePath);

	// [WP-2C / TD-10c] 将 Linear HDR (R16G16B16A16_FLOAT) 按与 TonemapPS 相同的 Exposure+ACES+sRGB 链
	// 导出为便于人眼检查的 8-bit PNG。
	bool SaveHDRBufferToPNG(ColorBuffer& source, const std::wstring& filePath,
		float exposure = 1.0f, bool applyACES = true, bool applySRGB = true);

	// [WP-2C / TD-10c] 将 Linear HDR (R16G16B16A16_FLOAT) 无损转为 R32G32B32A32_FLOAT 并导出为
	// Radiance .hdr 线性参考图（不做 tonemap/gamma 截断）。
	bool SaveHDRBufferToHDR(ColorBuffer& source, const std::wstring& filePath);
}
