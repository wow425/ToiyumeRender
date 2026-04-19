#pragma once

#include "PCH.h"

class DescriptorCache;

class RootParameter
{
	friend class RootSignature;
public:
    // 默认设置为-1，要求开发时必须设置。便于检验(0是合法的)
    RootParameter()
    {
        m_RootParameter.ParameterType = (D3D12_ROOT_PARAMETER_TYPE)0xFFFFFFFF;
    }

    ~RootParameter()
    {
        Clear();
    }

    void Clear()
    {
        if (m_RootParameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            delete[] m_RootParameter.DescriptorTable.pDescriptorRanges;

        m_RootParameter.ParameterType = (D3D12_ROOT_PARAMETER_TYPE)0xFFFFFFFF;
    }

    void InitAsConstants(UINT Register, UINT NumDwords, D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL, UINT Space = 0)
    {
        m_RootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        m_RootParameter.ShaderVisibility = Visibility;
        m_RootParameter.Constants.Num32BitValues = NumDwords;
        m_RootParameter.Constants.ShaderRegister = Register;
        m_RootParameter.Constants.RegisterSpace = Space;
    }

    void InitAsConstantBuffer(UINT Register, D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL, UINT Space = 0)
    {
        m_RootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        m_RootParameter.ShaderVisibility = Visibility;
        m_RootParameter.Descriptor.ShaderRegister = Register;
        m_RootParameter.Descriptor.RegisterSpace = Space;
    }

    void InitAsBufferSRV(UINT Register, D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL, UINT Space = 0)
    {
        m_RootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        m_RootParameter.ShaderVisibility = Visibility;
        m_RootParameter.Descriptor.ShaderRegister = Register;
        m_RootParameter.Descriptor.RegisterSpace = Space;
    }

    void InitAsBufferUAV(UINT Register, D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL, UINT Space = 0)
    {
        m_RootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        m_RootParameter.ShaderVisibility = Visibility;
        m_RootParameter.Descriptor.ShaderRegister = Register;
        m_RootParameter.Descriptor.RegisterSpace = Space;
    }

    void InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE Type, UINT Register, UINT Count, D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL, UINT Space = 0)
    {
        InitAsDescriptorTable(1, Visibility);
        SetTableRange(0, Type, Register, Count, Space);
    }

    void InitAsDescriptorTable(UINT RangeCount, D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL)
    {
        m_RootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        m_RootParameter.ShaderVisibility = Visibility;
        m_RootParameter.DescriptorTable.NumDescriptorRanges = RangeCount;
        m_RootParameter.DescriptorTable.pDescriptorRanges = new D3D12_DESCRIPTOR_RANGE[RangeCount]; // 
    }

    void SetTableRange(UINT RangeIndex, D3D12_DESCRIPTOR_RANGE_TYPE Type, UINT Register, UINT Count, UINT Space = 0)
    {
        D3D12_DESCRIPTOR_RANGE* range = const_cast<D3D12_DESCRIPTOR_RANGE*>(m_RootParameter.DescriptorTable.pDescriptorRanges + RangeIndex);
        range->RangeType = Type;
        range->NumDescriptors = Count;
        range->BaseShaderRegister = Register;
        range->RegisterSpace = Space;
        range->OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }

    const D3D12_ROOT_PARAMETER& operator() (void) const { return m_RootParameter; }

protected:

	D3D12_ROOT_PARAMETER m_RootParameter;
};



// Maximum 64 DWORDS divied up amongst all root parameters.
// Root constants = 1 DWORD * NumConstants
// Root descriptor (CBV, SRV, or UAV) = 2 DWORDs each
// Descriptor table pointer = 1 DWORD
// Static samplers = 0 DWORDS (compiled into shader)
class RootSignature
{
    friend class DynamicDescriptorHeap;

public:

    RootSignature(UINT NumRootParams = 0, UINT NumStaticSamplers = 0) : m_Finalized(FALSE), m_NumParameters(NumRootParams)
    {
        Reset(NumRootParams, NumStaticSamplers);
    }

    ~RootSignature()
    {
    }

    static void DestroyAll(void);

    void Reset(UINT NumRootParams, UINT NumStaticSamplers = 0)
    {
        if (NumRootParams > 0)
            m_ParamArray.reset(new RootParameter[NumRootParams]);
        else
            m_ParamArray = nullptr;
        m_NumParameters = NumRootParams;

        if (NumStaticSamplers > 0)
            m_SamplerArray.reset(new D3D12_STATIC_SAMPLER_DESC[NumStaticSamplers]);
        else
            m_SamplerArray = nullptr;

        m_NumSamplers = NumStaticSamplers;
        m_NumInitializedStaticSamplers = 0;
    }

    RootParameter& operator[] (size_t EntryIndex)
    {
        ASSERT(EntryIndex < m_NumParameters);
        return m_ParamArray.get()[EntryIndex];
    }

    const RootParameter& operator[] (size_t EntryIndex) const
    {
        ASSERT(EntryIndex < m_NumParameters);
        return m_ParamArray.get()[EntryIndex];
    }

    void InitStaticSampler(UINT Register, const D3D12_SAMPLER_DESC& NonStaticSamplerDesc,
        D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL);

    void Finalize(const std::wstring& name, D3D12_ROOT_SIGNATURE_FLAGS Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ID3D12RootSignature* GetSignature() const { return m_Signature; }

protected:
    ID3D12RootSignature* m_Signature;

    BOOL m_Finalized; // 记录根签名是否被编译过

    UINT m_NumParameters; // 根参数数量
    UINT m_NumSamplers;   // 采样器数量
    UINT m_NumInitializedStaticSamplers; // 
    // D3D12硬件底层设计中，采样器堆与资源视图堆(CBV/SRV/UAV)是完全独立的，故必须用两个独立的位图分开
    // 位图：利用位来存储索引。 |=按位或 存储索引， &按位与查找
    uint32_t m_DescriptorTableBitMap;		// 资源描述符表位图，记录哪些索引位置是资源描述符表。
    uint32_t m_SamplerTableBitMap;			// 采样器表位图
    uint32_t m_DescriptorTableSize[16];		// 记录非采样器描述符表包含多少个描述符，16为根参数上限
    std::unique_ptr<RootParameter[]> m_ParamArray;
    std::unique_ptr<D3D12_STATIC_SAMPLER_DESC[]> m_SamplerArray;
};

