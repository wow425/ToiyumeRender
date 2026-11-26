// ============================================================================
// 模块：CharacterPassCommon.h（少前2角色各 Pass 共享顶点布局与 PSO 构造辅助）
// 架构层：Engine/04_Renderer/Passes/Character
// ============================================================================
#pragma once

#include <d3d12.h>
#include <cstdint>

namespace Renderer::GFL2
{
	inline const D3D12_INPUT_ELEMENT_DESC kCharacterInputLayout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	inline constexpr uint32_t kCharacterInputLayoutCount = static_cast<uint32_t>(_countof(kCharacterInputLayout));

	inline D3D12_DEPTH_STENCIL_DESC MakeStencilDepthDesc(
		bool depthWrite,
		D3D12_COMPARISON_FUNC depthFunc,
		bool stencilEnable,
		D3D12_COMPARISON_FUNC frontFunc,
		D3D12_STENCIL_OP frontPassOp,
		D3D12_COMPARISON_FUNC backFunc = D3D12_COMPARISON_FUNC_ALWAYS,
		D3D12_STENCIL_OP backPassOp = D3D12_STENCIL_OP_KEEP)
	{
		D3D12_DEPTH_STENCIL_DESC ds = {};
		ds.DepthEnable                  = TRUE;
		ds.DepthWriteMask               = depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
		ds.DepthFunc                    = depthFunc;
		ds.StencilEnable                = stencilEnable ? TRUE : FALSE;
		ds.StencilReadMask              = 0xFF;
		ds.StencilWriteMask             = 0xFF;
		ds.FrontFace.StencilFunc        = frontFunc;
		ds.FrontFace.StencilPassOp      = frontPassOp;
		ds.FrontFace.StencilFailOp      = D3D12_STENCIL_OP_KEEP;
		ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		ds.BackFace.StencilFunc         = backFunc;
		ds.BackFace.StencilPassOp       = backPassOp;
		ds.BackFace.StencilFailOp        = D3D12_STENCIL_OP_KEEP;
		ds.BackFace.StencilDepthFailOp  = D3D12_STENCIL_OP_KEEP;
		return ds;
	}
}
