#include "PCH.h"
#include "RootSignature.h"
#include "../GraphicsCore.h"
#include "../Utility/Hash.h"
#include <map>
#include <thread>
#include <mutex>

using namespace Graphics;
using namespace std;
using Microsoft::WRL::ComPtr;

static std::map< size_t, ComPtr<ID3D12RootSignature> > s_RootSignatureHashMap; // 根签名哈希表

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
    // 1.检验是否被编译过，验证采样器数量一致与否
    // 2. 双位图存储索引
    // 3. 多线程并发控制模型编译根签名




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
    // =============================================================================================
    // 基于哈希表的全局缓存机制
    // =============================================================================================

        // 双位图初始化
    m_DescriptorTableBitMap = 0;
    m_SamplerTableBitMap = 0;
    // 为根签名配置+静态采样器生成哈希值
    size_t HashCode = Utility::HashState(&RootDesc.Flags);
    HashCode = Utility::HashState(RootDesc.pStaticSamplers, m_NumSamplers, HashCode);

    for (UINT Param = 0; Param < m_NumParameters; ++Param)
    {

        const D3D12_ROOT_PARAMETER& RootParam = RootDesc.pParameters[Param]; // 获取根签名指定索引的根参数
        m_DescriptorTableSize[Param] = 0;
        // 当前根参数为描述符表的话
        if (RootParam.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
        {
            // 断言存在范围
            ASSERT(RootParam.DescriptorTable.pDescriptorRanges != nullptr);
            // 累加该根参数的哈希值
            HashCode = Utility::HashState(RootParam.DescriptorTable.pDescriptorRanges,
                RootParam.DescriptorTable.NumDescriptorRanges, HashCode);

            // 记录该根参数位于哪张位图。采样器描述符表跟资源描述符表分开管理
            if (RootParam.DescriptorTable.pDescriptorRanges->RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) // 采样器描述符表
                m_SamplerTableBitMap |= (1 << Param); // 左移1位进行按位或
            else // 资源描述符表
                m_DescriptorTableBitMap |= (1 << Param);
            // 统计该根参数所包含的描述符数量
            for (UINT TableRange = 0; TableRange < RootParam.DescriptorTable.NumDescriptorRanges; ++TableRange)
                m_DescriptorTableSize[Param] += RootParam.DescriptorTable.pDescriptorRanges[TableRange].NumDescriptors;
        }
        else
            HashCode = Utility::HashState(&RootParam, 1, HashCode);
    }
    // 验证所有权移交是否成功
    // 无指针是硬件地址，单指针是指向硬件，m_Signature是单指针，*RSRef是单指针,s_RootSignatureHashMap存储双指针
    ID3D12RootSignature** RSRef = nullptr;
    bool firstCompile = false;
    // 局部作用域配合lock_guard锁
    {
        static mutex s_HashMapMutex;
        lock_guard<mutex> CS(s_HashMapMutex);
        auto iter = s_RootSignatureHashMap.find(HashCode);

        // Reserve space so the next inquiry will find that someone got here first.
        if (iter == s_RootSignatureHashMap.end()) // 该哈希值(对应的根签名）不存在时
        {
            RSRef = s_RootSignatureHashMap[HashCode].GetAddressOf(); // 访问不存在的键时，强行插入该键，并生成个初始值
            firstCompile = true; // 首次编译
        }
        else
            RSRef = iter->second.GetAddressOf();
    }

    if (firstCompile)
    {
        ComPtr<ID3DBlob> pOutBlob, pErrorBlob;

        ASSERT_SUCCEEDED(D3D12SerializeRootSignature(&RootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            pOutBlob.GetAddressOf(), pErrorBlob.GetAddressOf()));

        ASSERT_SUCCEEDED(g_Device->CreateRootSignature(1, pOutBlob->GetBufferPointer(), pOutBlob->GetBufferSize(),
            TY_IID_PPV_ARGS(&m_Signature)));

        m_Signature->SetName(name.c_str());

        s_RootSignatureHashMap[HashCode].Attach(m_Signature); // m_Signature所有权移交给s_RootSignatureHashMap，以进行管理
        ASSERT(*RSRef == m_Signature);
    }
    else
    {
        // 没抢到锁的就让出时间片等待其他线程完成编译后，拿现成的即可
        while (*RSRef == nullptr)
            this_thread::yield();
        m_Signature = *RSRef;
    }

    m_Finalized = TRUE;
}
