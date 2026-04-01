
#include "../Common/d3dApp.h"
#include "../Common/MathHelper.h"
#include "../Common/UploadBuffer.h"
#include "../Common/GeometryGenerator.h"
#include "../Common/Camera.h"
#include "FrameResource.h"
#include "../Model/Model.h"







struct TAA
{
	// 1. 资源
	ID3D12Resource* m_CurrentColor; // 本帧前序写
	ID3D12Resource* DepthBuffer;    // 前序写 
	ID3D12Resource* VelocityBuffer;  // 前序写

	// 用于 Ping-Pong 的两张纹理
	ID3D12Resource* m_TaaHistoryTextures[2]; // history和output
	UINT    CurrentHistoryIndex = 0; // 0/1
	D3D12_GPU_DESCRIPTOR_HANDLE m_TaaGpuHandles[2];
	// 用于绑定的两组 GPU 句柄起点.上帧与本帧color图
	// [0]: SRV(Read A) + UAV(Write B)
	// [1]: SRV(Read B) + UAV(Write A)



	// 4. 抖动数据
	float JitterX = 0.0f;
	float JitterY = 0.0f;
	int SampleIndex = 0;
	int MaxSample = 16;

	// 重投影用
	DirectX::XMMATRIX InvViewProj;
	DirectX::XMMATRIX PrevViewProj;

	// 5. 常量数据
	struct TAAConstants
	{
		XMFLOAT2 ScreenSize;
		XMFLOAT2 Jitter;
		float Alpha;
		XMFLOAT3 pad;

	} Constants;

	ID3D12Resource* ConstantBuffer;
};