#include "PCH.h"
#include "../GraphicsCore.h"
#include "PipelineState.h"
#include "RootSignature.h"
#include "Hash.h"
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
        D3D12_INPUT_ELEMENT_DESC* NewElements = (D3D12_INPUT_ELEMENT_DESC*)malloc(sizeof(D3D12_INPUT_ELEMENT_DESC) * NumElements);
        memcpy(NewElements, pInputElementDescs, NumElements * sizeof(D3D12_INPUT_ELEMENT_DESC));
        m_InputLayouts.reset((const D3D12_INPUT_ELEMENT_DESC*)NewElements);
    }
    else
        m_InputLayouts = nullptr;
}

void GraphicsPSO::Finalize()
{

}

void ComputePSO::Finalize()
{

}








ComputePSO::ComputePSO(const wchar_t* Name) : PSO(Name)
{
    ZeroMemory(&m_PSODesc, sizeof(m_PSODesc));
    m_PSODesc.NodeMask = 1;
}