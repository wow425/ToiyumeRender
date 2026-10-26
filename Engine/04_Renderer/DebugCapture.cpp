#include "00_Core/PCH.h"
#include "04_Renderer/DebugCapture.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/Heap/ReadbackBuffer.h"
#include "02_RHI/Command/CommandContext.h"
#include "00_Core/Utility/Utility.h"

#include <DirectXTex.h>
#include <DirectXPackedVector.h>
#include <vector>

namespace Renderer::DebugCapture
{
	// IEEE 754 binary16 -> float。
	// 不直接用 DirectXMath 的 XMConvertHalfToFloat：该声明藏在
	// DirectXPackedVector.h 的条件编译块里，在本工程的包含顺序下不可见。
	// 手写一份（含 denormal / inf / nan 处理），逻辑简单且可移植。
	namespace
	{
		inline float HalfToFloat(uint16_t h)
		{
			const uint32_t sign = (uint32_t)(h >> 15) & 0x1u;
			const uint32_t exp  = (uint32_t)(h >> 10) & 0x1Fu;
			const uint32_t mant = (uint32_t)h & 0x3FFu;

			uint32_t f;
			if (exp == 0)
			{
				if (mant == 0)
				{
					f = sign << 31;                       // ±0
				}
				else
				{
					// denormal：规格化到 2^-14 基准
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
				f = (sign << 31) | 0x7F800000u | (mant << 13);  // inf / nan
			}
			else
			{
				f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
			}

			float out;
			memcpy(&out, &f, sizeof(out));
			return out;
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

		// 1) 回读：开一个独立的上下文，复制到 READBACK 堆后立即同步等待
		ReadbackBuffer temp;
		uint32_t rowPitch = 0;
		{
			GraphicsContext& ctx = GraphicsContext::Begin(L"DebugCapture Readback");
			rowPitch = ctx.ReadbackTexture(temp, source);
			ctx.Finish(true);   // 必须同步，否则下面 Map 会读到未完成的数据
		}

		// 2) 映射并按目标格式转换
		const uint8_t* src = (const uint8_t*)temp.Map();
		if (src == nullptr)
		{
			Utility::Printf(L"[Capture] 错误：Map 失败\n");
			return false;
		}

		std::vector<uint8_t> rgba((size_t)width * height * 4);

		if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM || fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
		{
			for (uint32_t y = 0; y < height; ++y)
				memcpy(rgba.data() + (size_t)y * width * 4, src + (size_t)y * rowPitch, (size_t)width * 4);
		}
		else if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT)
		{
			// 简单线性 → 8bit，不做 tonemap，仅用于诊断观测
			for (uint32_t y = 0; y < height; ++y)
			{
				const uint16_t* row = (const uint16_t*)(src + (size_t)y * rowPitch);
				uint8_t* dst = rgba.data() + (size_t)y * width * 4;
				for (uint32_t x = 0; x < width; ++x)
				{
					for (int c = 0; c < 3; ++c)
					{
						const float f = HalfToFloat(row[x * 4 + c]);
						const float clamped = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
						dst[x * 4 + c] = (uint8_t)(clamped * 255.0f + 0.5f);
					}
					dst[x * 4 + 3] = 255;
				}
			}
		}
		else
		{
			temp.Unmap();
			Utility::Printf(L"[Capture] 不支持的格式 %d，暂不导出\n", (int)fmt);
			return false;
		}
		temp.Unmap();

		// 3) 用 DirectXTex 写 PNG
		DirectX::Image img = {};
		img.width  = width;
		img.height = height;
		img.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		img.rowPitch = (size_t)width * 4;
		img.slicePitch = img.rowPitch * height;
		img.pixels = rgba.data();

		HRESULT hr = DirectX::SaveToWICFile(img, DirectX::WIC_FLAGS_NONE,
			DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), filePath.c_str());
		if (FAILED(hr))
		{
			Utility::Printf(L"[Capture] 错误：SaveToWICFile 失败 hr=0x%08X\n", (unsigned)hr);
			return false;
		}

		Utility::Printf(L"[Capture] 已导出 %ws (%ux%u)\n", filePath.c_str(), width, height);
		return true;
	}
}
