#include "00_Core/PCH.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "00_Core/Utility/Hash.h"
#include <map>
#include <thread>
#include <mutex>

using Math::IsAligned;
using namespace Graphics;
using Microsoft::WRL::ComPtr;
using namespace std;

static map< size_t, ComPtr<ID3D12PipelineState> > s_ComputePSOHashMap;
static map< size_t, ComPtr<ID3D12PipelineState> > s_GraphicsPSOHashMap;

void PSO::DestroyAll(void)
{
    s_GraphicsPSOHashMap.clear();
    s_ComputePSOHashMap.clear();
}

GraphicsPSO::GraphicsPSO(const wchar_t* Name) : PSO(Name)
{
    ZeroMemory(&m_PSODesc, sizeof(m_PSODesc));
    m_PSODesc.NodeMask = 1;
    m_PSODesc.SampleMask = 0xFFFFFFFFu;
    m_PSODesc.SampleDesc.Count = 1;
    m_PSODesc.InputLayout.NumElements = 0;
}

void GraphicsPSO::SetBlendState(const D3D12_BLEND_DESC& BlendDesc)
{
    m_PSODesc.BlendState = BlendDesc;
}

void GraphicsPSO::SetRasterizerState(const D3D12_RASTERIZER_DESC& RasterizerDesc)
{
    m_PSODesc.RasterizerState = RasterizerDesc;
}

void GraphicsPSO::SetDepthStencilState(const D3D12_DEPTH_STENCIL_DESC& DepthStencilDesc)
{
    m_PSODesc.DepthStencilState = DepthStencilDesc;
}

void GraphicsPSO::SetSampleMask(UINT SampleMask)
{
    m_PSODesc.SampleMask = SampleMask;
}

void GraphicsPSO::SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType)
{
    ASSERT(TopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED, "Can't draw with undefined topology");
    m_PSODesc.PrimitiveTopologyType = TopologyType;
}

void GraphicsPSO::SetPrimitiveRestart(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBProps)
{
    m_PSODesc.IBStripCutValue = IBProps;
}

void GraphicsPSO::SetDepthTargetFormat(DXGI_FORMAT DSVFormat, UINT MsaaCount, UINT MsaaQuality)
{
    SetRenderTargetFormats(0, nullptr, DSVFormat, MsaaCount, MsaaQuality);
}

void GraphicsPSO::SetRenderTargetFormat(DXGI_FORMAT RTVFormat, DXGI_FORMAT DSVFormat, UINT MsaaCount, UINT MsaaQuality)
{
    SetRenderTargetFormats(1, &RTVFormat, DSVFormat, MsaaCount, MsaaQuality);
}

void GraphicsPSO::SetRenderTargetFormats(UINT NumRTVs, const DXGI_FORMAT* RTVFormats, DXGI_FORMAT DSVFormat, UINT MsaaCount, UINT MsaaQuality)
{
    ASSERT(NumRTVs == 0 || RTVFormats != nullptr, "Null format array conflicts with non-zero length");
    for (UINT i = 0; i < NumRTVs; ++i) // 填充RTV
    {
        ASSERT(RTVFormats[i] != DXGI_FORMAT_UNKNOWN);
        m_PSODesc.RTVFormats[i] = RTVFormats[i];
    }
    for (UINT i = NumRTVs; i < m_PSODesc.NumRenderTargets; ++i) // 剩余的默认初始化
        m_PSODesc.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
    m_PSODesc.NumRenderTargets = NumRTVs;
    m_PSODesc.DSVFormat = DSVFormat;
    m_PSODesc.SampleDesc.Count = MsaaCount;
    m_PSODesc.SampleDesc.Quality = MsaaQuality;
}

void GraphicsPSO::SetInputLayout(UINT NumElements, const D3D12_INPUT_ELEMENT_DESC* pInputElementDescs)
{

    m_PSODesc.InputLayout.NumElements = NumElements;

    if (NumElements > 0)
    {
        // [TD-05d] 严禁使用 malloc() 交给默认删除器的 shared_ptr：malloc 与 scalar delete 跨分配器
        // 配对属于 C++ 未定义行为，在 Debug CRT 堆或 PageHeap 下释放 PSO 时会直接触发堆校验崩毁。
        // 改用 new[] 配合 shared_ptr<const D3D12_INPUT_ELEMENT_DESC[]> 自动调用 delete[]。
        D3D12_INPUT_ELEMENT_DESC* NewElements = new D3D12_INPUT_ELEMENT_DESC[NumElements];
        memcpy(NewElements, pInputElementDescs, NumElements * sizeof(D3D12_INPUT_ELEMENT_DESC));
        m_InputLayouts.reset(NewElements);
    }
    else
        m_InputLayouts = nullptr;
}

void GraphicsPSO::Finalize()
{
    // 需先确保根签名finalize
    m_PSODesc.pRootSignature = m_RootSignature->GetSignature();
    ASSERT(m_PSODesc.pRootSignature != nullptr);
    // [TD-09c] 哈希除 m_PSODesc 外同步纳入 InputLayout 语义字符串内容与各阶段着色器字节码长度/哈希，
    // 防止不同 SemanticName 字符串地址复用或同地址新字节码造成缓存误命中。
    m_PSODesc.InputLayout.pInputElementDescs = nullptr;
    size_t HashCode = Utility::HashState(&m_PSODesc);
    for (UINT i = 0; i < m_PSODesc.InputLayout.NumElements; ++i)
    {
        const D3D12_INPUT_ELEMENT_DESC& elem = m_InputLayouts[i];
        D3D12_INPUT_ELEMENT_DESC elemCopy = elem;
        elemCopy.SemanticName = nullptr;
        HashCode = Utility::HashState(&elemCopy, 1, HashCode);
        if (elem.SemanticName != nullptr)
        {
            const size_t nameLen = strlen(elem.SemanticName);
            HashCode = Utility::HashState(&nameLen, 1, HashCode);
            for (size_t c = 0; c < nameLen; ++c)
                HashCode = HashCode * 131 + static_cast<unsigned char>(elem.SemanticName[c]);
        }
    }
    m_PSODesc.InputLayout.pInputElementDescs = m_InputLayouts.get();


    // =============================================================================================
    // 基于哈希表的全局缓存机制
    // =============================================================================================
    ID3D12PipelineState** PSORef = nullptr;
    bool firstCompile = false;
    {
        static mutex s_HashMapMutex;
        lock_guard<mutex> CS(s_HashMapMutex);
        auto iter = s_GraphicsPSOHashMap.find(HashCode);

        if (iter == s_GraphicsPSOHashMap.end())
        {
            firstCompile = true;
            PSORef = s_GraphicsPSOHashMap[HashCode].GetAddressOf();
        }
        else
            PSORef = iter->second.GetAddressOf();
    }

    if (firstCompile)
    {
        // 确保DSV规格已设置
        auto test = m_PSODesc.RTVFormats[0];
        auto tesst1 = m_PSODesc.DSVFormat;
        // 要么用深度，要么就别配深度资源”
        // ASSERT(m_PSODesc.DepthStencilState.DepthEnable != (m_PSODesc.DSVFormat == DXGI_FORMAT_UNKNOWN));
        ASSERT_SUCCEEDED(g_Device->CreateGraphicsPipelineState(&m_PSODesc, TY_IID_PPV_ARGS(&m_PSO)));
        s_GraphicsPSOHashMap[HashCode].Attach(m_PSO); // 转交指针由哈希表管理
        m_PSO->SetName(m_Name);
    }
    else // 没抢到首次编译的线程就让步等着编译完成吃现的。
    {
        while (*PSORef == nullptr)
            this_thread::yield();
        m_PSO = *PSORef;
    }
}

void ComputePSO::Finalize()
{
    // 需先确保根签名finalize
    m_PSODesc.pRootSignature = m_RootSignature->GetSignature();
    ASSERT(m_PSODesc.pRootSignature != nullptr);
    // 计算PSO哈希值
    size_t HashCode = Utility::HashState(&m_PSODesc);

    // =============================================================================================
    // 基于哈希表的全局缓存机制
    // =============================================================================================
    ID3D12PipelineState** PSORef = nullptr;
    bool firstCompile = false;
    {
        static mutex s_HashMapMutex;
        lock_guard<mutex> CS(s_HashMapMutex);
        auto iter = s_ComputePSOHashMap.find(HashCode);

        if (iter == s_ComputePSOHashMap.end())
        {
            firstCompile = true;
            PSORef = s_ComputePSOHashMap[HashCode].GetAddressOf();
        }
        else
            PSORef = iter->second.GetAddressOf();
    }

    if (firstCompile)
    {

        ASSERT_SUCCEEDED(g_Device->CreateComputePipelineState(&m_PSODesc, TY_IID_PPV_ARGS(&m_PSO)));
        s_ComputePSOHashMap[HashCode].Attach(m_PSO); // 转交指针由哈希表管理
        m_PSO->SetName(m_Name);
    }
    else // 没抢到首次编译的线程就让步等着编译完成吃现的。
    {
        while (*PSORef == nullptr)
            this_thread::yield();
        m_PSO = *PSORef;
    }
}

ComputePSO::ComputePSO(const wchar_t* Name) : PSO(Name)
{
    ZeroMemory(&m_PSODesc, sizeof(m_PSODesc));
    m_PSODesc.NodeMask = 1;
}
