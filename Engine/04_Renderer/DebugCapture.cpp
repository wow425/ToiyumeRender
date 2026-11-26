// ============================================================================
// 模块：DebugCapture（GPU 回读截帧工具 · 实现）
// 架构层：Engine/04_Renderer（WP-2C / TD-10c）
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/DebugCapture.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/Heap/ReadbackBuffer.h"
#include "02_RHI/Command/CommandContext.h"
#include "00_Core/Utility/Utility.h"

#include <DirectXTex.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace Renderer::DebugCapture
{
	namespace
	{
		// IEEE 754 binary16 -> binary32（含 denormal / inf / nan）
		inline float HalfToFloat(uint16_t h)
		{
			const uint32_t sign = static_cast<uint32_t>(h >> 15) & 0x1u;
			const uint32_t exp  = static_cast<uint32_t>(h >> 10) & 0x1Fu;
			const uint32_t mant = static_cast<uint32_t>(h) & 0x3FFu;

			uint32_t f;
			if (exp == 0)
			{
				if (mant == 0)
				{
					f = sign << 31;
				}
				else
				{
					uint32_t e = 0;
					uint32_t m = mant;
					while ((m & 0x400u) == 0) { m <<= 1; ++e; }
					m &= 0x3FFu;
					const uint32_t exp32 = 127 - 15 - e + 1;
					f = (sign << 31) | (exp32 << 23) | (m << 13);
				}
			}
			else if (exp == 0x1F)
			{
				f = (sign << 31) | 0x7F800000u | (mant << 13);
			}
			else
			{
				f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
			}

			float out = 0.0f;
			memcpy(&out, &f, sizeof(out));
			return out;
		}

		inline float Clamp01(float v)
		{
			if (!std::isfinite(v) || v <= 0.0f) return 0.0f;
			return v >= 1.0f ? 1.0f : v;
		}

		inline float ACESFilmScalar(float x)
		{
			constexpr float a = 2.51f;
			constexpr float b = 0.03f;
			constexpr float c = 2.43f;
			constexpr float d = 0.59f;
			constexpr float e = 0.14f;
			if (!std::isfinite(x) || x <= 0.0f) return 0.0f;
			return Clamp01((x * (a * x + b)) / (x * (c * x + d) + e));
		}

		inline float LinearToSRGBScalar(float linearColor)
		{
			const float c = Clamp01(linearColor);
			if (c <= 0.0031308f)
				return c * 12.92f;
			return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
		}

		inline uint8_t FloatToByte(float v)
		{
			return static_cast<uint8_t>(Clamp01(v) * 255.0f + 0.5f);
		}
	}

	bool SaveColorBufferToPNG(ColorBuffer& source, const std::wstring& filePath)
	{
		if (!source.GetResource())
		{
			Utility::Printf(L"[Capture] 错误：ColorBuffer 未创建，无法导出 %ws\n", filePath.c_str());
			return false;
		}

		const uint32_t width  = source.GetWidth();
		const uint32_t height = source.GetHeight();
		const DXGI_FORMAT fmt = source.GetFormat();
		if (width == 0 || height == 0)
		{
			Utility::Printf(L"[Capture] 错误：尺寸为 0，无法导出\n");
			return false;
		}

		ReadbackBuffer temp;
		uint32_t rowPitch = 0;
		{
			GraphicsContext& ctx = GraphicsContext::Begin(L"DebugCapture Readback");
			rowPitch = ctx.ReadbackTexture(temp, source);
			ctx.Finish(true);
		}

		const uint8_t* src = static_cast<const uint8_t*>(temp.Map());
		if (src == nullptr)
		{
			Utility::Printf(L"[Capture] 错误：Map 失败\n");
			return false;
		}

		std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);

		if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM || fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
		{
			for (uint32_t y = 0; y < height; ++y)
				memcpy(rgba.data() + static_cast<size_t>(y) * width * 4,
					src + static_cast<size_t>(y) * rowPitch,
					static_cast<size_t>(width) * 4);
		}
		else if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT)
		{
			for (uint32_t y = 0; y < height; ++y)
			{
				const uint16_t* row = reinterpret_cast<const uint16_t*>(src + static_cast<size_t>(y) * rowPitch);
				uint8_t* dst = rgba.data() + static_cast<size_t>(y) * width * 4;
				for (uint32_t x = 0; x < width; ++x)
				{
					for (int c = 0; c < 3; ++c)
						dst[x * 4 + c] = FloatToByte(HalfToFloat(row[x * 4 + c]));
					dst[x * 4 + 3] = 255;
				}
			}
		}
		else if (fmt == DXGI_FORMAT_R16G16_FLOAT)
		{
			// [WP-2C / TD-10c] 支持 GBuffer_Normal (R16G16_FLOAT) 导出，RG 通道映射为 [0,255]，B=128
			for (uint32_t y = 0; y < height; ++y)
			{
				const uint16_t* row = reinterpret_cast<const uint16_t*>(src + static_cast<size_t>(y) * rowPitch);
				uint8_t* dst = rgba.data() + static_cast<size_t>(y) * width * 4;
				for (uint32_t x = 0; x < width; ++x)
				{
					dst[x * 4 + 0] = FloatToByte(HalfToFloat(row[x * 2 + 0]));
					dst[x * 4 + 1] = FloatToByte(HalfToFloat(row[x * 2 + 1]));
					dst[x * 4 + 2] = 128;
					dst[x * 4 + 3] = 255;
				}
			}
		}
		else if (fmt == DXGI_FORMAT_R32_FLOAT)
		{
			// [契约 4] 支持 DXR PrimarySurface Depth (R32_FLOAT) 导出为灰度 PNG 供自动化轮廓与深度范围校验
			for (uint32_t y = 0; y < height; ++y)
			{
				const float* row = reinterpret_cast<const float*>(src + static_cast<size_t>(y) * rowPitch);
				uint8_t* dst = rgba.data() + static_cast<size_t>(y) * width * 4;
				for (uint32_t x = 0; x < width; ++x)
				{
					const uint8_t v = FloatToByte(row[x]);
					dst[x * 4 + 0] = v;
					dst[x * 4 + 1] = v;
					dst[x * 4 + 2] = v;
					dst[x * 4 + 3] = 255;
				}
			}
		}
		else
		{
			temp.Unmap();
			Utility::Printf(L"[Capture] 不支持的格式 %d，暂不导出\n", static_cast<int>(fmt));
			return false;
		}
		temp.Unmap();

		DirectX::Image img = {};
		img.width      = width;
		img.height     = height;
		img.format     = DXGI_FORMAT_R8G8B8A8_UNORM;
		img.rowPitch   = static_cast<size_t>(width) * 4;
		img.slicePitch = img.rowPitch * height;
		img.pixels     = rgba.data();

		HRESULT hr = DirectX::SaveToWICFile(img, DirectX::WIC_FLAGS_NONE,
			DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), filePath.c_str());
		if (FAILED(hr))
		{
			Utility::Printf(L"[Capture] 错误：SaveToWICFile 失败 hr=0x%08X\n", static_cast<unsigned>(hr));
			return false;
		}

		Utility::Printf(L"[Capture] 已导出 %ws (%ux%u)\n", filePath.c_str(), width, height);
		return true;
	}

	bool SaveHDRBufferToPNG(ColorBuffer& source, const std::wstring& filePath,
		float exposure, bool applyACES, bool applySRGB)
	{
		if (!source.GetResource() || source.GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT)
		{
			return SaveColorBufferToPNG(source, filePath);
		}

		const uint32_t width  = source.GetWidth();
		const uint32_t height = source.GetHeight();
		if (width == 0 || height == 0)
			return false;

		ReadbackBuffer temp;
		uint32_t rowPitch = 0;
		{
			GraphicsContext& ctx = GraphicsContext::Begin(L"DebugCapture HDR->PNG Readback");
			rowPitch = ctx.ReadbackTexture(temp, source);
			ctx.Finish(true);
		}

		const uint8_t* src = static_cast<const uint8_t*>(temp.Map());
		if (src == nullptr)
			return false;

		std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
		const float safeExposure = (std::max)(exposure, 0.0f);

		for (uint32_t y = 0; y < height; ++y)
		{
			const uint16_t* row = reinterpret_cast<const uint16_t*>(src + static_cast<size_t>(y) * rowPitch);
			uint8_t* dst = rgba.data() + static_cast<size_t>(y) * width * 4;
			for (uint32_t x = 0; x < width; ++x)
			{
				for (int c = 0; c < 3; ++c)
				{
					float v = HalfToFloat(row[x * 4 + c]) * safeExposure;
					v = applyACES ? ACESFilmScalar(v) : Clamp01(v);
					if (applySRGB)
						v = LinearToSRGBScalar(v);
					dst[x * 4 + c] = FloatToByte(v);
				}
				dst[x * 4 + 3] = 255;
			}
		}
		temp.Unmap();

		DirectX::Image img = {};
		img.width      = width;
		img.height     = height;
		img.format     = DXGI_FORMAT_R8G8B8A8_UNORM;
		img.rowPitch   = static_cast<size_t>(width) * 4;
		img.slicePitch = img.rowPitch * height;
		img.pixels     = rgba.data();

		HRESULT hr = DirectX::SaveToWICFile(img, DirectX::WIC_FLAGS_NONE,
			DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), filePath.c_str());
		if (FAILED(hr))
		{
			Utility::Printf(L"[Capture] 错误：SaveHDRBufferToPNG 失败 hr=0x%08X\n", static_cast<unsigned>(hr));
			return false;
		}

		Utility::Printf(L"[Capture] 已导出 HDR 预览 PNG %ws (%ux%u)\n", filePath.c_str(), width, height);
		return true;
	}

	bool SaveHDRBufferToHDR(ColorBuffer& source, const std::wstring& filePath)
	{
		if (!source.GetResource() || source.GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT)
		{
			Utility::Printf(L"[Capture] 错误：SaveHDRBufferToHDR 仅支持 R16G16B16A16_FLOAT\n");
			return false;
		}

		const uint32_t width  = source.GetWidth();
		const uint32_t height = source.GetHeight();
		if (width == 0 || height == 0)
			return false;

		ReadbackBuffer temp;
		uint32_t rowPitch = 0;
		{
			GraphicsContext& ctx = GraphicsContext::Begin(L"DebugCapture HDR Readback");
			rowPitch = ctx.ReadbackTexture(temp, source);
			ctx.Finish(true);
		}

		const uint8_t* src = static_cast<const uint8_t*>(temp.Map());
		if (src == nullptr)
			return false;

		std::vector<float> rgba32(static_cast<size_t>(width) * height * 4);
		for (uint32_t y = 0; y < height; ++y)
		{
			const uint16_t* row = reinterpret_cast<const uint16_t*>(src + static_cast<size_t>(y) * rowPitch);
			float* dst = rgba32.data() + static_cast<size_t>(y) * width * 4;
			for (uint32_t x = 0; x < width; ++x)
			{
				for (int c = 0; c < 3; ++c)
				{
					const float v = HalfToFloat(row[x * 4 + c]);
					dst[x * 4 + c] = (std::isfinite(v) && v > 0.0f) ? v : 0.0f;
				}
				dst[x * 4 + 3] = 1.0f;
			}
		}
		temp.Unmap();

		DirectX::Image img = {};
		img.width      = width;
		img.height     = height;
		img.format     = DXGI_FORMAT_R32G32B32A32_FLOAT;
		img.rowPitch   = static_cast<size_t>(width) * 4 * sizeof(float);
		img.slicePitch = img.rowPitch * height;
		img.pixels     = reinterpret_cast<uint8_t*>(rgba32.data());

		HRESULT hr = DirectX::SaveToHDRFile(img, filePath.c_str());
		if (FAILED(hr))
		{
			Utility::Printf(L"[Capture] 错误：SaveToHDRFile 失败 hr=0x%08X\n", static_cast<unsigned>(hr));
			return false;
		}

		Utility::Printf(L"[Capture] 已导出线性 HDR 参考图 %ws (%ux%u)\n", filePath.c_str(), width, height);
		return true;
	}
}
