#include "00_Core/PCH.h"
#include "RootSignature.h"
#include "02_RHI/GraphicsCore.h"
#include "00_Core/Utility/Hash.h"
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using namespace Graphics;
using namespace std;
using Microsoft::WRL::ComPtr;

namespace
{
	// [TD-09c] 根签名缓存条目同时保存规范化描述字节串，命中哈希桶后做逐字节等价校验，
	// 彻底消除仅靠 64-bit 哈希值导致的静默哈希碰撞与错绑根签名风险。
	struct CachedRootSignature
	{
		std::vector<uint8_t> CanonicalKey;
		ComPtr<ID3D12RootSignature> Signature;
	};

	template <typename T>
	void AppendPodBytes(std::vector<uint8_t>& out, const T& value)
	{
		const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
		out.insert(out.end(), ptr, ptr + sizeof(T));
	}

	std::vector<uint8_t> BuildCanonicalRootSigKey(const D3D12_ROOT_SIGNATURE_DESC& desc)
	{
		std::vector<uint8_t> key;
		key.reserve(256);
		AppendPodBytes(key, desc.Flags);
		AppendPodBytes(key, desc.NumParameters);
		AppendPodBytes(key, desc.NumStaticSamplers);

		for (UINT s = 0; s < desc.NumStaticSamplers; ++s)
		{
			AppendPodBytes(key, desc.pStaticSamplers[s]);
		}

		for (UINT p = 0; p < desc.NumParameters; ++p)
		{
			const D3D12_ROOT_PARAMETER& param = desc.pParameters[p];
			// [TD-09c] 无论参数类型如何，必须纳入 ParameterType 与 ShaderVisibility，
			// 否则两个仅可见性不同（如 VERTEX vs PIXEL）或参数类型不同的描述符表会错误复用同一根签名。
			AppendPodBytes(key, param.ParameterType);
			AppendPodBytes(key, param.ShaderVisibility);
			switch (param.ParameterType)
			{
			case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
				AppendPodBytes(key, param.DescriptorTable.NumDescriptorRanges);
				for (UINT r = 0; r < param.DescriptorTable.NumDescriptorRanges; ++r)
					AppendPodBytes(key, param.DescriptorTable.pDescriptorRanges[r]);
				break;
			case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
				AppendPodBytes(key, param.Constants);
				break;
			case D3D12_ROOT_PARAMETER_TYPE_CBV:
			case D3D12_ROOT_PARAMETER_TYPE_SRV:
			case D3D12_ROOT_PARAMETER_TYPE_UAV:
				AppendPodBytes(key, param.Descriptor);
				break;
			default:
				break;
			}
		}
		return key;
	}
}

static std::map<size_t, std::vector<CachedRootSignature>> s_RootSignatureHashMap; // 根签名哈希表（带碰撞桶）

void RootSignature::DestroyAll(void)
{
	s_RootSignatureHashMap.clear();
}

void RootSignature::InitStaticSampler(UINT Register, const D3D12_SAMPLER_DESC& NonStaticSamplerDesc, D3D12_SHADER_VISIBILITY Visibility)
{
	ASSERT(m_NumInitializedStaticSamplers < m_NumSamplers);
	D3D12_STATIC_SAMPLER_DESC& StaticSamplerDesc = m_SamplerArray[m_NumInitializedStaticSamplers++];

	StaticSamplerDesc.Filter = NonStaticSamplerDesc.Filter;
	StaticSamplerDesc.AddressU = NonStaticSamplerDesc.AddressU;
	StaticSamplerDesc.AddressV = NonStaticSamplerDesc.AddressV;
	StaticSamplerDesc.AddressW = NonStaticSamplerDesc.AddressW;
	StaticSamplerDesc.MipLODBias = NonStaticSamplerDesc.MipLODBias;
	StaticSamplerDesc.MaxAnisotropy = NonStaticSamplerDesc.MaxAnisotropy;
	StaticSamplerDesc.ComparisonFunc = NonStaticSamplerDesc.ComparisonFunc;
	StaticSamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	StaticSamplerDesc.MinLOD = NonStaticSamplerDesc.MinLOD;
	StaticSamplerDesc.MaxLOD = NonStaticSamplerDesc.MaxLOD;
	StaticSamplerDesc.ShaderRegister = Register;
	StaticSamplerDesc.RegisterSpace = 0;
	StaticSamplerDesc.ShaderVisibility = Visibility;

	if (StaticSamplerDesc.AddressU == D3D12_TEXTURE_ADDRESS_MODE_BORDER ||
		StaticSamplerDesc.AddressV == D3D12_TEXTURE_ADDRESS_MODE_BORDER ||
		StaticSamplerDesc.AddressW == D3D12_TEXTURE_ADDRESS_MODE_BORDER)
	{
		WARN_ONCE_IF_NOT(
			// Transparent Black
			NonStaticSamplerDesc.BorderColor[0] == 0.0f &&
			NonStaticSamplerDesc.BorderColor[1] == 0.0f &&
			NonStaticSamplerDesc.BorderColor[2] == 0.0f &&
			NonStaticSamplerDesc.BorderColor[3] == 0.0f ||
			// Opaque Black
			NonStaticSamplerDesc.BorderColor[0] == 0.0f &&
			NonStaticSamplerDesc.BorderColor[1] == 0.0f &&
			NonStaticSamplerDesc.BorderColor[2] == 0.0f &&
			NonStaticSamplerDesc.BorderColor[3] == 1.0f ||
			// Opaque White
			NonStaticSamplerDesc.BorderColor[0] == 1.0f &&
			NonStaticSamplerDesc.BorderColor[1] == 1.0f &&
			NonStaticSamplerDesc.BorderColor[2] == 1.0f &&
			NonStaticSamplerDesc.BorderColor[3] == 1.0f,
			"Sampler border color does not match static sampler limitations");

		if (NonStaticSamplerDesc.BorderColor[3] == 1.0f)
		{
			if (NonStaticSamplerDesc.BorderColor[0] == 1.0f)
				StaticSamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
			else
				StaticSamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
		}
		else
			StaticSamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	}
}

// 最终化, 根签名类的难点
void RootSignature::Finalize(const std::wstring& name, D3D12_ROOT_SIGNATURE_FLAGS Flags)
{
	// 确保一个根签名对象只被编译一次
	if (m_Finalized)
		return;
	// 确保初始化的静态采样器数量与设定的数量一致
	ASSERT(m_NumInitializedStaticSamplers == m_NumSamplers);

	D3D12_ROOT_SIGNATURE_DESC RootDesc;
	RootDesc.NumParameters = m_NumParameters;
	RootDesc.pParameters = (const D3D12_ROOT_PARAMETER*)m_ParamArray.get();
	RootDesc.NumStaticSamplers = m_NumSamplers;
	RootDesc.pStaticSamplers = (const D3D12_STATIC_SAMPLER_DESC*)m_SamplerArray.get();
	RootDesc.Flags = Flags;

	// 双位图初始化
	m_DescriptorTableBitMap = 0;
	m_SamplerTableBitMap = 0;

	for (UINT Param = 0; Param < m_NumParameters; ++Param)
	{
		const D3D12_ROOT_PARAMETER& RootParam = RootDesc.pParameters[Param];
		m_DescriptorTableSize[Param] = 0;
		if (RootParam.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
		{
			ASSERT(RootParam.DescriptorTable.pDescriptorRanges != nullptr);
			if (RootParam.DescriptorTable.pDescriptorRanges->RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
				m_SamplerTableBitMap |= (1 << Param);
			else
				m_DescriptorTableBitMap |= (1 << Param);

			for (UINT TableRange = 0; TableRange < RootParam.DescriptorTable.NumDescriptorRanges; ++TableRange)
				m_DescriptorTableSize[Param] += RootParam.DescriptorTable.pDescriptorRanges[TableRange].NumDescriptors;
		}
	}

	// [TD-09c] 构造完整规范化字节序列（覆盖 Flags/NumParameters/NumSamplers/StaticSamplers/
	// ParameterType/ShaderVisibility/DescriptorRanges）并做整块哈希 + 桶内逐字节等价校验。
	std::vector<uint8_t> canonicalKey = BuildCanonicalRootSigKey(RootDesc);
	const size_t HashCode = Utility::HashRange(
		reinterpret_cast<const uint32_t*>(canonicalKey.data()),
		reinterpret_cast<const uint32_t*>(canonicalKey.data() + (canonicalKey.size() & ~size_t(3))),
		2166136261U);

	static mutex s_HashMapMutex;
	{
		lock_guard<mutex> CS(s_HashMapMutex);
		auto& bucket = s_RootSignatureHashMap[HashCode];
		for (const auto& entry : bucket)
		{
			if (entry.CanonicalKey == canonicalKey)
			{
				m_Signature = entry.Signature.Get();
				m_Finalized = TRUE;
				return;
			}
		}
	}

	ComPtr<ID3DBlob> pOutBlob, pErrorBlob;
	const HRESULT serHr = D3D12SerializeRootSignature(&RootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		pOutBlob.GetAddressOf(), pErrorBlob.GetAddressOf());
	if (FAILED(serHr))
	{
		if (pErrorBlob)
			LOG_ERROR(L"[RootSignature] 序列化失败 (%ws, hr=0x%08X): %hs\n",
				name.c_str(), (unsigned)serHr, (const char*)pErrorBlob->GetBufferPointer());
		ASSERT_SUCCEEDED(serHr);
		return;
	}

	ComPtr<ID3D12RootSignature> createdSig;
	ASSERT_SUCCEEDED(g_Device->CreateRootSignature(1, pOutBlob->GetBufferPointer(), pOutBlob->GetBufferSize(),
		TY_IID_PPV_ARGS(&createdSig)));
	createdSig->SetName(name.c_str());

	{
		lock_guard<mutex> CS(s_HashMapMutex);
		auto& bucket = s_RootSignatureHashMap[HashCode];
		for (const auto& entry : bucket)
		{
			if (entry.CanonicalKey == canonicalKey)
			{
				m_Signature = entry.Signature.Get();
				m_Finalized = TRUE;
				return;
			}
		}
		bucket.push_back({ std::move(canonicalKey), createdSig });
		m_Signature = createdSig.Get();
	}

	m_Finalized = TRUE;
}
