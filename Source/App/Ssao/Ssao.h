//***************************************************************************************
// Ssao.h by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

#ifndef SSAO_H
#define SSAO_H

#pragma once

#include "../Common/d3dUtil.h"
#include "FrameResource.h"
 
 
class Ssao
{
public:

	Ssao(ID3D12Device* device, 
        ID3D12GraphicsCommandList* cmdList, 
        UINT width, UINT height);
    Ssao(const Ssao& rhs) = delete;
    Ssao& operator=(const Ssao& rhs) = delete;
    ~Ssao() = default; 

	void OnResize(UINT newWidth, UINT newHeight);
  
 

private:
 



private:
	ID3D12Device* md3dDevice;


	 


	UINT mRenderTargetWidth;
	UINT mRenderTargetHeight;



	D3D12_VIEWPORT mViewport;
	D3D12_RECT mScissorRect;
};

#endif 