#include "00_Core/PCH.h"
#include "03_RHI/Resource/ShadowBuffer.h"
#include "03_RHI/CommandSystem/CommandContext.h"


void ShadowBuffer::Create(const std::wstring& Name, uint32_t Width, uint32_t Height, D3D12_GPU_VIRTUAL_ADDRESS VidMemPtr)
{
	DepthBuffer::Create(Name, Width, Height, DXGI_FORMAT_D16_UNORM, VidMemPtr); // 默认m_ClearDepth为1.0

	m_Viewport.TopLeftX = 0.0f;
	m_Viewport.TopLeftY = 0.0f;
	m_Viewport.Width = (float)Width;
	m_Viewport.Height = (float)Height;
	m_Viewport.MinDepth = 0.0f;
	m_Viewport.MaxDepth = 1.0f;

	// 不渲染边缘像素,避免Shadow Edge Stretching
	m_Scissor.left = 1;
	m_Scissor.top = 1;
	m_Scissor.right = (LONG)Width - 2;
	m_Scissor.bottom = (LONG)Height - 2;
}

// 资源转为depth write并绑定深度模板，视口和裁剪
void ShadowBuffer::BeginRendering(GraphicsContext& Context)
{
	Context.TransitionResource(*this, D3D12_RESOURCE_STATE_DEPTH_WRITE, true); // 立即转为depthwrite
	Context.ClearDepth(*this);
	Context.SetDepthStencilTarget(GetDSV()); // 绑定深度模板
	Context.SetViewportAndScissor(m_Viewport, m_Scissor); // 绑定视口和裁剪
}

// 资源转为shader resource
void ShadowBuffer::EndRendering(GraphicsContext& Context)
{
	Context.TransitionResource(*this, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
