#include "../Common/d3dApp.h"
#include "../Common/MathHelper.h"
#include "../Common/UploadBuffer.h"
#include "../Common/GeometryGenerator.h"
#include "../Common/Camera.h"
#include "FrameResource.h"
#include "../Model/Model.h"
#include "HaltonJitter.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

const int gNumFrameResources = 3;

struct TAA
{
	// 1. 资源
	ID3D12Resource* m_CurrentColor; // 本帧前序写
	ID3D12Resource* DepthBuffer;    // 前序写 
	ID3D12Resource* VelocityBuffer;  // 前序写

	D3D12_CPU_DESCRIPTOR_HANDLE m_CurrentColorRtvHandle;
	D3D12_CPU_DESCRIPTOR_HANDLE m_VelocityBUfferRtvHandle;
	D3D12_CPU_DESCRIPTOR_HANDLE m_DepthDsvHandle;

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
// TAA

TAA taa;

std::unique_ptr<UploadBuffer<TAA::TAAConstants>> mTaaCB = nullptr;

int FrameCount = -1;


DirectX::XMFLOAT4X4 mPrevCleanViewProj = MathHelper::Identity4x4(); 




struct RenderItem
{
	RenderItem() = default;
	RenderItem(const RenderItem& rhs) = delete;

	// World matrix of the shape that describes the object's local space
	// relative to the world space, which defines the position, orientation,
	// and scale of the object in the world.
	XMFLOAT4X4 World = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 PrevWorld = MathHelper::Identity4x4(); // 【新增】逻辑层的上一帧矩阵

	XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();

	// Dirty flag indicating the object data has changed and we need to update the constant buffer.
	// Because we have an object cbuffer for each FrameResource, we have to apply the
	// update to each FrameResource.  Thus, when we modify obect data we should set 
	// NumFramesDirty = gNumFrameResources so that each frame resource gets the update.
	int NumFramesDirty = gNumFrameResources;

	// Index into GPU constant buffer corresponding to the ObjectCB for this render item.
	UINT ObjCBIndex = -1;

	Material* Mat = nullptr;
	MeshGeometry* Geo = nullptr;

	// Primitive topology.
	D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

	// DrawIndexedInstanced parameters.
	UINT IndexCount = 0;
	UINT StartIndexLocation = 0;
	int BaseVertexLocation = 0;
};

enum class RenderLayer : int
{
	Opaque = 0,
	Debug,
	Sky,
	Count
};

class SsaoApp : public D3DApp
{
public:
	SsaoApp(HINSTANCE hInstance);
	SsaoApp(const SsaoApp& rhs) = delete;
	SsaoApp& operator=(const SsaoApp& rhs) = delete;
	~SsaoApp();

	virtual bool Initialize()override;

	std::unique_ptr<Model> Saber = std::make_unique<Model>();

private:
	virtual void CreateRtvAndDsvDescriptorHeaps()override;
	virtual void OnResize()override;
	virtual void Update(const GameTimer& gt)override;
	virtual void Draw(const GameTimer& gt)override;

	virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
	virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
	virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

	void OnKeyboardInput(const GameTimer& gt);
	void UpdateObjectCBs(const GameTimer& gt);
	void UpdateMaterialBuffer(const GameTimer& gt);
	void UpdateMainPassCB(const GameTimer& gt);

	void UpdateObjectTransforms(); // 新增 物体变换矩阵用于TAA

	void LoadTextures();
	void BuildRootSignature();
	void BuildDescriptorHeaps();
	void BuildShadersAndInputLayout();
	void BuildShapeGeometry();
	void BuildPSOs();
	void BuildFrameResources();
	void BuildMaterials();
	void BuildRenderItems();
	void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);



	std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> GetStaticSamplers();

private:

	std::vector<std::unique_ptr<FrameResource>> mFrameResources;
	FrameResource* mCurrFrameResource = nullptr;
	int mCurrFrameResourceIndex = 0;

	ComPtr<ID3D12RootSignature> mRootSignature = nullptr;

	ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr;

	std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
	std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
	std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures;
	std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;

	std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

	// List of all the render items.
	std::vector<std::unique_ptr<RenderItem>> mAllRitems;

	// Render items divided by PSO.
	std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];





	PassConstants mMainPassCB;  // index 0 of pass cbuffer.


	Camera mCamera;



	float mLightNearZ = 0.0f;
	float mLightFarZ = 0.0f;
	XMFLOAT3 mLightPosW;
	XMFLOAT4X4 mLightView = MathHelper::Identity4x4();
	XMFLOAT4X4 mLightProj = MathHelper::Identity4x4();


	float mLightRotationAngle = 0.0f;
	XMFLOAT3 mBaseLightDirections[3] = {
		XMFLOAT3(0.57735f, -0.57735f, 0.57735f),
		XMFLOAT3(-0.57735f, -0.57735f, 0.57735f),
		XMFLOAT3(0.0f, -0.707f, -0.707f)
	};
	XMFLOAT3 mRotatedLightDirections[3];

	POINT mLastMousePos;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
	PSTR cmdLine, int showCmd)
{
	// Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	try
	{
		SsaoApp theApp(hInstance);
		if (!theApp.Initialize())
			return 0;

		return theApp.Run();
	}
	catch (DxException& e)
	{
		MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
		return 0;
	}
}

SsaoApp::SsaoApp(HINSTANCE hInstance)
	: D3DApp(hInstance)
{

}

SsaoApp::~SsaoApp()
{
	if (md3dDevice != nullptr)
		FlushCommandQueue();
}

bool SsaoApp::Initialize()
{
	if (!D3DApp::Initialize())
		return false;



	// Reset the command list to prep for initialization commands.
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	mCamera.SetPosition(0.0f, 2.0f, -15.0f);

	// 模型加载
	Saber->LoadFromFile("../../Assets/Models/Saber_Emiya/Saber_Emiya.glb"); 
	if (Saber->LoadFromFile("../../Assets/Models/Saber_Emiya/Saber_Emiya.glb"))
	{
		// 2. 打印检验报告
		Saber->PrintTextureInfo();
	}
	// 模型加载

	// TAA
	mTaaCB = std::make_unique<UploadBuffer<TAA::TAAConstants>>(md3dDevice.Get(), 1, true);



	LoadTextures();
	BuildRootSignature();
	BuildDescriptorHeaps();
	BuildShadersAndInputLayout();
	BuildShapeGeometry();
	BuildMaterials();
	BuildRenderItems();
	BuildFrameResources();
	BuildPSOs();

	// TAA

// 1. 定义资源描述 (Texture2D)
	D3D12_RESOURCE_DESC texDesc = {};
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Alignment = 0;
	texDesc.Width = mClientWidth;   // 替换为你的屏幕宽度变量
	texDesc.Height = mClientHeight; // 替换为你的屏幕高度变量
	texDesc.DepthOrArraySize = 1;
	texDesc.MipLevels = 1;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	// 关键标志：必须允许作为渲染目标
	texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	// 2. 必须使用默认堆 (纯显存)
	CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);

	// 3. 定义优化的清屏颜色 (极大提升 ClearRenderTargetView 的性能)
	CD3DX12_CLEAR_VALUE optClear(DXGI_FORMAT_R8G8B8A8_UNORM, Colors::Black);

	// 4. 为 CurrentColor 分配物理内存
	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&defaultHeapProps,
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_COMMON, // 初始状态，之后在 Draw 里用 Barrier 转为 RENDER_TARGET
		&optClear,
		IID_PPV_ARGS(&taa.m_CurrentColor)));

	// 5. 为 VelocityBuffer 分配物理内存
	texDesc.Format = DXGI_FORMAT_R16G16_FLOAT; // 速度通常只需要 RG 两个通道
	optClear.Format = DXGI_FORMAT_R16G16_FLOAT;
	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&defaultHeapProps,
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_COMMON,
		&optClear,
		IID_PPV_ARGS(&taa.VelocityBuffer)));

	// 6. 为 TAA History 纹理 (Ping-Pong 0 和 1) 分配物理内存
// History 图在 Compute Shader 中被用作 UAV (Unordered Access View) 写入
	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	// 极其重要：仅作为 UAV 使用的资源，创建时 ClearValue 必须为 nullptr
	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&defaultHeapProps,
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_COMMON, // 初始状态，之后通过 Barrier 转换
		nullptr,
		IID_PPV_ARGS(&taa.m_TaaHistoryTextures[0])));

	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&defaultHeapProps,
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(&taa.m_TaaHistoryTextures[1])));

	// 7. 为离屏 DepthBuffer 分配物理内存
	// 必须允许作为深度模板缓冲
	texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	// 【核心硬核点】：资源本身必须是 TYPELESS
	// 因为它既要生成 DSV (D32_FLOAT)，又要生成 SRV (R32_FLOAT) 供 TAA 采样
	texDesc.Format = DXGI_FORMAT_R32_TYPELESS;

	// 深度图的清屏优化值，必须匹配实际用于 Clear 的格式 (D32_FLOAT)
	CD3DX12_CLEAR_VALUE depthOptClear(DXGI_FORMAT_D32_FLOAT, 1.0f, 0);

	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&defaultHeapProps,
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_COMMON,
		&depthOptClear,
		IID_PPV_ARGS(&taa.DepthBuffer)));



	// ==========================================
	// 3. 获取 TAA RTV 的 CPU 句柄偏移
	// ==========================================
	// 游标先跳过 SwapChain 占用的槽位
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(
		mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
		SwapChainBufferCount,
		mRtvDescriptorSize);

	// 4. 为 CurrentColor 创建 RTV
	taa.m_CurrentColorRtvHandle = rtvHeapHandle; // 记录下来给 Draw 用
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	md3dDevice->CreateRenderTargetView(taa.m_CurrentColor, &rtvDesc, taa.m_CurrentColorRtvHandle);

	// 5. 偏移游标，为 VelocityBuffer 创建 RTV
	rtvHeapHandle.Offset(1, mRtvDescriptorSize);
	taa.m_VelocityBUfferRtvHandle = rtvHeapHandle; // 记录下来给 Draw 用
	rtvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
	md3dDevice->CreateRenderTargetView(taa.VelocityBuffer, &rtvDesc, taa.m_VelocityBUfferRtvHandle);

	// ==========================================
	// 【新增点 2：为 TAA 的独立深度图创建 DSV】
	// ==========================================
	// 假设第 0 个槽位是给交换链默认深度图用的。我们偏移 1 个位置给 TAA 用。
	// （如果你的 Shadow Map 占了第 1 个位置，这里就把偏移改成 2，以此类推）
	CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHeapHandle(
		mDsvHeap->GetCPUDescriptorHandleForHeapStart(),
		1, // 偏移量，跳过前面的默认深度图
		mDsvDescriptorSize);

	taa.m_DepthDsvHandle = dsvHeapHandle; // 保存这个句柄，Draw 函数里清屏和绑定都会用到它

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	// 极其关键：物理内存是 TYPELESS，这里必须明确解释为 D32_FLOAT 才能作为深度图写入
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsvDesc.Texture2D.MipSlice = 0;

	md3dDevice->CreateDepthStencilView(taa.DepthBuffer, &dsvDesc, taa.m_DepthDsvHandle);
	// Execute the initialization commands.
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Wait until initialization is complete.
	FlushCommandQueue();

	return true;
}

void SsaoApp::CreateRtvAndDsvDescriptorHeaps()
{
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	// 原本可能是 SwapChainBufferCount (通常是 2)，现在加上 TAA 的 2 个
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount + 2;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&rtvHeapDesc, IID_PPV_ARGS(mRtvHeap.GetAddressOf())));

	// ==========================================
	// 【修改点 1：扩大 DSV 堆的容量】
	// 原本是 2 (默认深度图 1 + Shadow Map 1)。现在加入 TAA 深度图，改为 3
	// ==========================================
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 3; // <--- 这里改成 3
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc, IID_PPV_ARGS(mDsvHeap.GetAddressOf())));

	
}

void SsaoApp::OnResize()
{
	D3DApp::OnResize();

	mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);


}

void SsaoApp::Update(const GameTimer& gt)
{
	OnKeyboardInput(gt);

	// Cycle through the circular frame resource array.
	mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
	mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();
	FrameCount++;

	// Has the GPU finished processing the commands of the current frame resource?
	// If not, wait until the GPU has completed commands up to this fence point.
	if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}

	//
	// Animate the lights (and hence shadows).
	//

	mLightRotationAngle += 0.1f * gt.DeltaTime();

	XMMATRIX R = XMMatrixRotationY(mLightRotationAngle);
	for (int i = 0; i < 3; ++i)
	{
		XMVECTOR lightDir = XMLoadFloat3(&mBaseLightDirections[i]);
		lightDir = XMVector3TransformNormal(lightDir, R);
		XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
	}

	// AnimateMaterials(gt);
	UpdateObjectCBs(gt);
	UpdateMaterialBuffer(gt);
	UpdateMainPassCB(gt);

// TAA
	TAA::TAAConstants taaData;
	taaData.ScreenSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);

	taaData.Jitter = GenerateHaltonJitter(FrameCount, taaData.ScreenSize.x, taaData.ScreenSize.y);
	taaData.Alpha = 0.1f; // 例如历史帧占 90%，当前帧占 10%
	taaData.pad = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);

	// 将数据拷贝到 Upload Heap 中，索引为 0
	mTaaCB->CopyData(0, taaData);

// 
}

void SsaoApp::Draw(const GameTimer& gt)
{
	auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;
	ThrowIfFailed(cmdListAlloc->Reset());
	ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque"].Get()));

	// =========================================================
	// 阶段 1：离屏前向渲染 (Off-screen Forward Rendering)
	// =========================================================
	// 注意：不要再用 CurrentBackBuffer()！
	// 必须清空并绑定 TAA 结构体中的 CurrentColor 和 VelocityBuffer

// 1. 设置资源屏障：将上一帧用于读取的 SRV 状态，转换回写入状态
	D3D12_RESOURCE_BARRIER beginBarriers[3];

	// CurrentColor: SRV -> RENDER_TARGET
	beginBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(taa.m_CurrentColor,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

	// VelocityBuffer: SRV -> RENDER_TARGET
	beginBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(taa.VelocityBuffer,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

	// DepthBuffer: SRV -> DEPTH_WRITE (注意！深度缓冲是 DEPTH_WRITE，绝对不是 RENDER_TARGET)
	beginBarriers[2] = CD3DX12_RESOURCE_BARRIER::Transition(taa.DepthBuffer,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

	mCommandList->ResourceBarrier(3, beginBarriers);

	// 2. 设置 MRT (多渲染目标)
	D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] =
	{
		taa.m_CurrentColorRtvHandle,
		taa.m_VelocityBUfferRtvHandle
	};
	// 使用 TAA 专用的深度缓冲句柄！(需要你在初始化堆时自己创建并保存这个句柄)
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = taa.m_DepthDsvHandle;

	mCommandList->ClearRenderTargetView(rtvs[0], Colors::Black, 0, nullptr);
	mCommandList->ClearRenderTargetView(rtvs[1], Colors::Black, 0, nullptr);
	mCommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	// 绑定 MRT 和 专属深度图
	mCommandList->OMSetRenderTargets(2, rtvs, false, &dsvHandle);

	// 3. 执行常规的 Opaque 绘制 (你的 Graphics Root Signature 等逻辑保持不变)
	// 前提：你的 Default.hlsl 的 PS 输出必须改为 struct 包含 SV_Target0 (Color) 和 SV_Target1 (Velocity)
	ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
	mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

	mCommandList->SetGraphicsRootSignature(mRootSignature.Get()); // 根签名绑定

	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	// 材质绑定
	auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
	mCommandList->SetGraphicsRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());
	// 纹理绑定
	mCommandList->SetGraphicsRootDescriptorTable(3, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	// 过程CB绑定
	auto passCB = mCurrFrameResource->PassCB->Resource();
	mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

	// opaque队列绘制
	mCommandList->SetPipelineState(mPSOs["opaque"].Get());
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

	// =========================================================
		// 阶段 2：TAA Compute Shader 阶段
		// =========================================================

		// 1. 设置资源屏障：转换资源状态供 Compute Shader 读取/写入
		// CurrentColor -> NON_PIXEL_SHADER_RESOURCE (SRV)
		// DepthBuffer -> NON_PIXEL_SHADER_RESOURCE (SRV)
		// VelocityBuffer -> NON_PIXEL_SHADER_RESOURCE (SRV)
		// m_TaaHistoryTextures[HistoryWriteIndex] -> UNORDERED_ACCESS (UAV)
		// ... (补充 ResourceBarrier 代码) ...

	// 计算 Ping-Pong 索引
	UINT readIndex = taa.CurrentHistoryIndex;
	UINT writeIndex = (taa.CurrentHistoryIndex + 1) % 2;

	// 1. 设置资源屏障：打包提交以提升性能
	D3D12_RESOURCE_BARRIER computeBarriers[5];

	// 从前向渲染的输出状态，转为 Compute Shader 的读取状态 (SRV)
	computeBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(taa.m_CurrentColor,
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	computeBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(taa.DepthBuffer,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	computeBarriers[2] = CD3DX12_RESOURCE_BARRIER::Transition(taa.VelocityBuffer,
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	// TAA 历史帧资源状态流转
	// 上一帧的 WriteTexture 在 Phase 3 被转成了 COPY_SOURCE，现在它变成了本帧的 ReadTexture
	computeBarriers[3] = CD3DX12_RESOURCE_BARRIER::Transition(taa.m_TaaHistoryTextures[readIndex],
		D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	// 上一帧的 ReadTexture 是 NON_PIXEL_SHADER_RESOURCE，现在变成了本帧的 WriteTexture
	computeBarriers[4] = CD3DX12_RESOURCE_BARRIER::Transition(taa.m_TaaHistoryTextures[writeIndex],
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// 一次性提交 5 个屏障
	mCommandList->ResourceBarrier(5, computeBarriers);

		// 2. 切换到 Compute 模式
	mCommandList->SetPipelineState(mPSOs["taaCS"].Get());
	mCommandList->SetComputeRootSignature(mRootSignature.Get()); // 需要一个专门给 Compute 用的根签名

	// 3. 绑定 TAA 常量缓冲区 (b2)
	D3D12_GPU_VIRTUAL_ADDRESS taaCBAddress = mTaaCB->Resource()->GetGPUVirtualAddress();
	mCommandList->SetComputeRootConstantBufferView(5, taaCBAddress); // 对应 register(b2)

	// 4. 绑定 SRV 和 UAV (t1~t4, u0)
	mCommandList->SetComputeRootDescriptorTable(4, taa.m_TaaGpuHandles[taa.CurrentHistoryIndex]);


	// 5. 派发线程组 (Dispatch)
	// 你的 shader 是 [numthreads(8, 8, 1)]
	UINT dispatchX = (mClientWidth + 7) / 8;
	UINT dispatchY = (mClientHeight + 7) / 8;
	mCommandList->Dispatch(dispatchX, dispatchY, 1);


	// =========================================================
	// 阶段 3：上屏 (Copy / Blit) 与 Ping-Pong
	// =========================================================
	ID3D12Resource* taaOutput = taa.m_TaaHistoryTextures[writeIndex]; // 本帧计算得出的全新图像
	// 1. 将 BackBuffer 从 PRESENT 转为 COPY_DEST
	// 将 TAA 的输出纹理从 UNORDERED_ACCESS 转为 COPY_SOURCE
	// ... (补充 ResourceBarrier 代码) ...

	// 1. 将 BackBuffer 从 PRESENT 转为 COPY_DEST，将 TAA 输出转为 COPY_SOURCE
	D3D12_RESOURCE_BARRIER copyBarriers[2];
	copyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);

	copyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(taaOutput,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

	mCommandList->ResourceBarrier(2, copyBarriers);

	// 2. 拷贝资源上屏
	mCommandList->CopyResource(CurrentBackBuffer(), taaOutput);

	// 3. 将 BackBuffer 转回 PRESENT 状态准备呈现
	auto presentBarrier = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
	mCommandList->ResourceBarrier(1, &presentBarrier);

	// 4. 更新 Ping-Pong 索引，为下一帧做准备
	taa.CurrentHistoryIndex = writeIndex;

	// 【极其重要的提示】：
	// 在你下一帧进入 Draw() 的 Phase 1 (Opaque 绘制) 之前，
	// 必须把 taa.m_CurrentColor 和 taa.VelocityBuffer 从 NON_PIXEL_SHADER_RESOURCE 转回 RENDER_TARGET！
	// 把 DepthBuffer 从 NON_PIXEL_SHADER_RESOURCE 转回 DEPTH_WRITE！
	// 





	// 关闭命令列表并执行
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
	// 交换链呈现
	ThrowIfFailed(mSwapChain->Present(0, 0));

	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;
	mCurrFrameResource->Fence = ++mCurrentFence;
	mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}










void SsaoApp::OnMouseDown(WPARAM btnState, int x, int y)
{
	mLastMousePos.x = x;
	mLastMousePos.y = y;

	SetCapture(mhMainWnd);
}

void SsaoApp::OnMouseUp(WPARAM btnState, int x, int y)
{
	ReleaseCapture();
}

void SsaoApp::OnMouseMove(WPARAM btnState, int x, int y)
{
	if ((btnState & MK_LBUTTON) != 0)
	{
		// Make each pixel correspond to a quarter of a degree.
		float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
		float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

		mCamera.Pitch(dy);
		mCamera.RotateY(dx);
	}

	mLastMousePos.x = x;
	mLastMousePos.y = y;
}

void SsaoApp::OnKeyboardInput(const GameTimer& gt)
{
	const float dt = gt.DeltaTime();

	if (GetAsyncKeyState('W') & 0x8000)
		mCamera.Walk(5.0f * dt);

	if (GetAsyncKeyState('S') & 0x8000)
		mCamera.Walk(-5.0f * dt);

	if (GetAsyncKeyState('A') & 0x8000)
		mCamera.Strafe(-10.0f * dt);

	if (GetAsyncKeyState('D') & 0x8000)
		mCamera.Strafe(10.0f * dt);

	if (GetAsyncKeyState('E') & 0x8000) // 上
	{
		mCamera.Up(5.0f * dt);
	}

	if (GetAsyncKeyState('Q') & 0x8000) // 下
	{
		mCamera.Down(5.0f * dt);
	}

	mCamera.UpdateViewMatrix();
}



void SsaoApp::UpdateObjectCBs(const GameTimer& gt)
{
	auto currObjectCB = mCurrFrameResource->ObjectCB.get();
	for (auto& e : mAllRitems)
	{
		// Only update the cbuffer data if the constants have changed.  
		// This needs to be tracked per frame resource.
		if (e->NumFramesDirty > 0)
		{
			XMMATRIX world = XMLoadFloat4x4(&e->World);
			XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);

			ObjectConstants objConstants;
			XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
			XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
			objConstants.MaterialIndex = e->Mat->MatCBIndex;

			currObjectCB->CopyData(e->ObjCBIndex, objConstants);

			// Next FrameResource need to be updated too.
			e->NumFramesDirty--;
		}
	}
}

void SsaoApp::UpdateMaterialBuffer(const GameTimer& gt)
{
	auto currMaterialBuffer = mCurrFrameResource->MaterialBuffer.get();
	for (auto& e : mMaterials)
	{
		// Only update the cbuffer data if the constants have changed.  If the cbuffer
		// data changes, it needs to be updated for each FrameResource.
		Material* mat = e.second.get();
		if (mat->NumFramesDirty > 0)
		{
			XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

			MaterialData matData;
			matData.DiffuseAlbedo = mat->DiffuseAlbedo;
			matData.FresnelR0 = mat->FresnelR0;
			matData.Roughness = mat->Roughness;
			XMStoreFloat4x4(&matData.MatTransform, XMMatrixTranspose(matTransform));
			matData.DiffuseMapIndex = mat->DiffuseSrvHeapIndex;
			matData.NormalMapIndex = mat->NormalSrvHeapIndex;

			currMaterialBuffer->CopyData(mat->MatCBIndex, matData);

			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}
}



void SsaoApp::UpdateMainPassCB(const GameTimer& gt)
{
	XMMATRIX view = mCamera.GetView();
	XMMATRIX proj = mCamera.GetProj(); // 这是纯净的投影矩阵 (Clean Proj)

	// 1. 获取纯净的 ViewProj
	XMMATRIX cleanViewProj = XMMatrixMultiply(view, proj);

	// 2. 生成带 TAA 抖动的投影矩阵 (Jittered Proj)
	XMMATRIX jitteredProj = proj;
	// ... 在这里对 jitteredProj 的 m[2][0] 和 m[2][1] 添加 Jitter 偏移 ...

	// 1. 获取本帧的 Jitter 像素偏移
// 假设这个函数返回的值已经在 [-0.5, 0.5] 范围内
	DirectX::XMFLOAT2 jitterPixel = GenerateHaltonJitter(FrameCount, (float)mClientWidth, (float)mClientHeight);

	// 2. 将像素级别的偏移转换为 NDC (Normalized Device Coordinates) 空间的偏移
	// NDC 空间的宽度和高度跨度都是 2.0 (从 -1 到 1)
	float jitterX_NDC = (jitterPixel.x * 2.0f) / (float)mClientWidth;
	float jitterY_NDC = (jitterPixel.y * 2.0f) / (float)mClientHeight;



	// 4. 应用 Jitter 偏移
	// --- 方法 A：通过 XMFLOAT4X4 结构体修改 (可读性好，推荐) ---
	DirectX::XMFLOAT4X4 projFloat;
	XMStoreFloat4x4(&projFloat, jitteredProj);

	projFloat.m[2][0] += jitterX_NDC;
	// 注意 DX 中 NDC 的 Y 轴是向上的，具体加减符号需要根据你的 TAA 采样的方向来匹配
	projFloat.m[2][1] += jitterY_NDC;

	jitteredProj = XMLoadFloat4x4(&projFloat);

	// 3. 生成带抖动的 ViewProj，用于光栅化渲染 (SV_Position)
	XMMATRIX jitteredViewProj = XMMatrixMultiply(view, jitteredProj);


	// 存入 CB
	XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
	// 注意：传给着色器做常规顶点变换的必须是带 Jitter 的
	XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(jitteredViewProj));

	// 【新增】传入上一帧的纯净 ViewProj
	XMStoreFloat4x4(&mMainPassCB.PrevViewProj, XMMatrixTranspose(XMLoadFloat4x4(&mPrevCleanViewProj)));


	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(cleanViewProj), cleanViewProj);

	// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
	XMMATRIX T(
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f);

	XMMATRIX viewProjTex = XMMatrixMultiply(cleanViewProj, T);


	XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(cleanViewProj));
	XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProjTex, XMMatrixTranspose(viewProjTex));

	mMainPassCB.EyePosW = mCamera.GetPosition3f();
	mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.NearZ = 1.0f;
	mMainPassCB.FarZ = 1000.0f;
	mMainPassCB.TotalTime = gt.TotalTime();
	mMainPassCB.DeltaTime = gt.DeltaTime();
	mMainPassCB.AmbientLight = { 0.4f, 0.4f, 0.6f, 1.0f };

	//mMainPassCB.Lights[0].Direction = mRotatedLightDirections[0];
	//mMainPassCB.Lights[0].Strength = { 0.4f, 0.4f, 0.5f };
	//mMainPassCB.Lights[1].Direction = mRotatedLightDirections[1];
	//mMainPassCB.Lights[1].Strength = { 0.1f, 0.1f, 0.1f };
	//mMainPassCB.Lights[2].Direction = mRotatedLightDirections[2];
	//mMainPassCB.Lights[2].Strength = { 0.0f, 0.0f, 0.0f };

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);

	// 【极其重要】在函数末尾，把本帧的纯净 ViewProj 保存下来，留给下一帧使用！
	XMStoreFloat4x4(&mPrevCleanViewProj, cleanViewProj);
}



void SsaoApp::LoadTextures()
{
	std::vector<std::string> texNames =
	{
		"tileDiffuseMap",
	};

	std::vector<std::wstring> texFilenames =
	{
		L"D:/CS-Self-Study/Computer_Graphics/DX12/ToiyumeRender/Assets/Textures/uesugi.dds",
	};

	for (int i = 0; i < (int)texNames.size(); ++i)
	{
		auto texMap = std::make_unique<Texture>();
		texMap->Name = texNames[i];
		texMap->Filename = texFilenames[i];
		ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
			mCommandList.Get(), texMap->Filename.c_str(),
			texMap->Resource, texMap->UploadHeap));
		// 创texture指针加载数据到缓冲区，最后转交指针权	
		mTextures[texMap->Name] = std::move(texMap);
	}

	// Saber
	const auto& SaberTextures = Saber->GetLoadedTextures();

	for (size_t i = 0; i < SaberTextures.size(); ++i)
	{
		const RawTextureData& texData = SaberTextures[i];

		if (texData.Pixels != nullptr)
		{
			auto texMap = std::make_unique<Texture>();
			texMap->Name = "Texture" + std::to_string(i);

			Microsoft::WRL::ComPtr<ID3D12Resource> textureDefaultBuffer;
			Microsoft::WRL::ComPtr<ID3D12Resource> textureUploadBuffer;
			// ==========================================
			// 1. 创建 Default Buffer (存在于 GPU 高速显存中)
			// ==========================================
			D3D12_RESOURCE_DESC texDesc = {};
			texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			texDesc.Alignment = 0;
			texDesc.Width = texData.Width;
			texDesc.Height = texData.Height;
			texDesc.DepthOrArraySize = 1;
			texDesc.MipLevels = 1; // 暂时只用 1 级 Mipmap
			texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // 与 stb_image 的 4 通道对应
			texDesc.SampleDesc.Count = 1;
			texDesc.SampleDesc.Quality = 0;
			texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			ThrowIfFailed(md3dDevice->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
				D3D12_HEAP_FLAG_NONE,
				&texDesc,
				D3D12_RESOURCE_STATE_COPY_DEST, // 初始状态必须是 COPY_DEST，准备接收数据
				nullptr,
				IID_PPV_ARGS(&textureDefaultBuffer)));
			// ==========================================
			// 2. 创建 Upload Buffer (存在于 CPU/GPU 共享内存中)
			// ==========================================
			// 计算上传这张纹理需要多少字节的中间缓冲区
			const UINT64 uploadBufferSize = GetRequiredIntermediateSize(textureDefaultBuffer.Get(), 0, 1);

			ThrowIfFailed(md3dDevice->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
				D3D12_HEAP_FLAG_NONE,
				&CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&textureUploadBuffer)));
			// ==========================================
			// 3. 填充 D3D12_SUBRESOURCE_DATA 并执行拷贝指令
			// ==========================================
			D3D12_SUBRESOURCE_DATA subResourceData = {};
			subResourceData.pData = texData.Pixels;
			subResourceData.RowPitch = texData.Width * 4; // 1行像素占用的字节数 (RGBA = 4 bytes)
			subResourceData.SlicePitch = subResourceData.RowPitch * texData.Height; // 整张图的大小

			// 使用 d3dx12.h 提供的辅助函数，将数据从 CPU -> UploadHeap -> DefaultHeap
			UpdateSubresources(mCommandList.Get(), textureDefaultBuffer.Get(), textureUploadBuffer.Get(), 0, 0, 1, &subResourceData);

			// 拷贝完成后，插入一个资源屏障，将纹理状态从 COPY_DEST 转为 PIXEL_SHADER_RESOURCE
			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
				textureDefaultBuffer.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

			// 保存 COM 智能指针，防止出了大括号被销毁
			texMap->Resource = textureDefaultBuffer;
			texMap->UploadHeap = textureUploadBuffer;


			mTextures[texMap->Name] = std::move(texMap);
		}


	}

}

void SsaoApp::BuildRootSignature()
{

	CD3DX12_DESCRIPTOR_RANGE texRangesTex;
	texRangesTex.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 0, 0); 	// 纹理
// TAA
	// TAA用SRV,UAV
	CD3DX12_DESCRIPTOR_RANGE taaRanges[2];

	// Current, history, Depth, Velocity
	taaRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 1, 1); // 占用t1-t4的s1 SRV
	taaRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 1); // 占用u0 s1的 gOutputColor

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[6];

	// Perfomance TIP: Order from most frequent to least frequent.
	// 按照更新频率依次绑定
	slotRootParameter[0].InitAsConstantBufferView(0); // 物体常量 (CBV, b0, space0)
	slotRootParameter[1].InitAsConstantBufferView(1); // 渲染过程常量 (CBV, b1, space0)
	slotRootParameter[2].InitAsShaderResourceView(0, 1, D3D12_SHADER_VISIBILITY_ALL); // 材质结构化缓冲区 (Root SRV)
	slotRootParameter[3].InitAsDescriptorTable(1, &texRangesTex, D3D12_SHADER_VISIBILITY_ALL); // 纹理数组表 (Descriptor Table)
// TAA
	slotRootParameter[4].InitAsDescriptorTable(2, taaRanges, D3D12_SHADER_VISIBILITY_ALL); // TAA资源表 (Descriptor Table)
	slotRootParameter[5].InitAsConstantBufferView(2); // TAA常量数据


	auto staticSamplers = GetStaticSamplers();

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(6, slotRootParameter,
		(UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}


void SsaoApp::BuildDescriptorHeaps()
{
	// SRV堆
	// Create the SRV heap.
	//
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = 30;
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

	//
	// Fill out the heap with actual descriptors.
	//
	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
	//=============================

	std::vector<ComPtr<ID3D12Resource>> tex2DList =
	{
		mTextures["tileDiffuseMap"]->Resource,
	};

	// Saber
	int saberTexCount = (int)mTextures.size() - 1;
	for (int i = 0; i < saberTexCount; i++)
	{
		std::string texName = "Texture" + std::to_string(i);
		if (mTextures.count(texName) > 0)
		{
			tex2DList.push_back(mTextures[texName]->Resource);
		}
		else
		{
			std::string msg = "[Warning] " + texName + " not found in mTextures!\n";
			OutputDebugStringA(msg.c_str());
		}
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

	for (UINT i = 0; i < (UINT)tex2DList.size(); ++i)
	{
		srvDesc.Format = tex2DList[i]->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = tex2DList[i]->GetDesc().MipLevels;
		md3dDevice->CreateShaderResourceView(tex2DList[i].Get(), &srvDesc, hDescriptor);

		// next descriptor
		hDescriptor.Offset(1, mCbvSrvUavDescriptorSize);
	}


// TAA 顺序：4个SRV:current color, history color, depth, velocity
// 1个UAV: output


	D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart(); // 加上对应偏移
	gpuHandle.ptr += tex2DList.size() * mCbvSrvUavDescriptorSize; // 跳过之前的纹理描述符

	D3D12_SHADER_RESOURCE_VIEW_DESC taaSrvDesc = {};
	taaSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	taaSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	taaSrvDesc.Texture2D.MostDetailedMip = 0;
	taaSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	taaSrvDesc.Texture2D.MipLevels = 1;

	for (int i = 0; i < 2; i++) // 两组SRV/UAV
	{
		taa.m_TaaGpuHandles[i] = gpuHandle; // 记录每组的GPU句柄，供渲染时绑定

		int readIndex = i;
		int writeIndex = 1 - i;
	
		// current color SRV(两组都读同一张当前帧)
		taaSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		md3dDevice->CreateShaderResourceView(taa.m_CurrentColor, &taaSrvDesc, hDescriptor);
		hDescriptor.Offset(1, mCbvSrvUavDescriptorSize);

		// history color SRV(ping-pong 绑定readIndex)
		md3dDevice->CreateShaderResourceView(taa.m_TaaHistoryTextures[readIndex], &taaSrvDesc, hDescriptor);
		hDescriptor.Offset(1, mCbvSrvUavDescriptorSize);

		// depth SRV (两组固定)
		taaSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		md3dDevice->CreateShaderResourceView(taa.DepthBuffer, &taaSrvDesc, hDescriptor);
		hDescriptor.Offset(1, mCbvSrvUavDescriptorSize);

		// velocity SRV (两组固定
		taaSrvDesc.Format = DXGI_FORMAT_R16G16_FLOAT; // 速度图常用格式
		md3dDevice->CreateShaderResourceView(taa.VelocityBuffer, &taaSrvDesc, hDescriptor);
		hDescriptor.Offset(1, mCbvSrvUavDescriptorSize);

		// output UAV (ping-pong 绑定writeIndex)
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		md3dDevice->CreateUnorderedAccessView(taa.m_TaaHistoryTextures[writeIndex], nullptr, &uavDesc, hDescriptor);
		hDescriptor.Offset(1, mCbvSrvUavDescriptorSize);

		gpuHandle.ptr += 5 * mCbvSrvUavDescriptorSize; 
	}

}

void SsaoApp::BuildShadersAndInputLayout()
{
	const D3D_SHADER_MACRO alphaTestDefines[] =
	{
		"ALPHA_TEST", "1",
		NULL, NULL
	};

	mShaders["standardVS"] = d3dUtil::CompileShader(L"../Shaders/Default.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["opaquePS"] = d3dUtil::CompileShader(L"../Shaders/Default.hlsl", nullptr, "PS", "ps_5_1");

	// TAA CS
	mShaders["taaCS"] = d3dUtil::CompileShader(L"../Shaders/TAA.hlsl", nullptr, "CS", "cs_5_1");

	mInputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
}

void SsaoApp::BuildShapeGeometry()
{
	GeometryGenerator geoGen;
	// 
	GeometryGenerator::MeshData grid = geoGen.CreateGrid(20.0f, 30.0f, 60, 40);

	UINT gridVertexOffset = 0;
	UINT gridIndexOffset = 0;

	SubmeshGeometry gridSubmesh;
	gridSubmesh.IndexCount = (UINT)grid.Indices32.size();
	gridSubmesh.StartIndexLocation = gridIndexOffset;
	gridSubmesh.BaseVertexLocation = gridVertexOffset;

	auto totalVertexCount = grid.Vertices.size();

	std::vector<Vertex> vertices(totalVertexCount);

	UINT k = 0;
	for (size_t i = 0; i < grid.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = grid.Vertices[i].Position;
		vertices[k].Normal = grid.Vertices[i].Normal;
		vertices[k].TexC = grid.Vertices[i].TexC;
		vertices[k].TangentU = grid.Vertices[i].TangentU;
	}


	std::vector<std::uint16_t> indices;
	indices.insert(indices.end(), std::begin(grid.GetIndices16()), std::end(grid.GetIndices16()));


	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);
	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "shapeGeo";
	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);
	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);
	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);
	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);
	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = ibByteSize;
	geo->DrawArgs["grid"] = gridSubmesh;
	mGeometries[geo->Name] = std::move(geo);

	// ==========================================
	// Saber 模型处理 (合并 Buffer 架构)
	// ==========================================
	std::vector<Submesh> SaberMeshes = Saber->GetMeshes();

	// 1. 预先统计 Saber 整个模型共有多少个顶点和索引
	UINT totalSaberVertices = 0;
	UINT totalSaberIndices = 0;
	for (const auto& mesh : SaberMeshes)
	{
		totalSaberVertices += (UINT)mesh.Vertices.size();
		totalSaberIndices += (UINT)mesh.Indices.size();
	}

	// 2. 创建用于合并的大数组 (注意类型必须与你 Assimp 读取时一致)
	std::vector<ModelVertex> saberVertices;
	saberVertices.reserve(totalSaberVertices);
	std::vector<unsigned int> saberIndices;
	saberIndices.reserve(totalSaberIndices);

	// 创建唯一的一个大 MeshGeometry 来装整个 Saber
	auto saberGeo = std::make_unique<MeshGeometry>();
	saberGeo->Name = "SaberGeo";

	// 3. 遍历拼接数据，并记录各个子网格的偏移量 (Offset)
	UINT saberVertexOffset = 0;
	UINT saberIndexOffset = 0;

	for (int i = 0; i < SaberMeshes.size(); ++i)
	{
		SubmeshGeometry submesh;
		submesh.BaseVertexLocation = saberVertexOffset;
		submesh.StartIndexLocation = saberIndexOffset;
		submesh.IndexCount = (UINT)SaberMeshes[i].Indices.size();

		// 将该 submesh 存入 DrawArgs，名字按索引区分，例如 "Submesh0", "Submesh1"
		std::string submeshName = "Submesh" + std::to_string(i);
		saberGeo->DrawArgs[submeshName] = submesh;

		// 把当前子网格的数据塞进大数组里 (Vector 拼接)
		saberVertices.insert(saberVertices.end(), SaberMeshes[i].Vertices.begin(), SaberMeshes[i].Vertices.end());
		saberIndices.insert(saberIndices.end(), SaberMeshes[i].Indices.begin(), SaberMeshes[i].Indices.end());

		// 累加偏移量，给下一个子网格用
		saberVertexOffset += (UINT)SaberMeshes[i].Vertices.size();
		saberIndexOffset += (UINT)SaberMeshes[i].Indices.size();
	}

	// 4. 计算大数组的总字节数
	const UINT saberVbByteSize = (UINT)saberVertices.size() * sizeof(ModelVertex);
	const UINT saberIbByteSize = (UINT)saberIndices.size() * sizeof(unsigned int);

	// 5. 将大数组上传到 GPU (整个 Saber 只需要上传一次)
	ThrowIfFailed(D3DCreateBlob(saberVbByteSize, &saberGeo->VertexBufferCPU));
	CopyMemory(saberGeo->VertexBufferCPU->GetBufferPointer(), saberVertices.data(), saberVbByteSize);

	ThrowIfFailed(D3DCreateBlob(saberIbByteSize, &saberGeo->IndexBufferCPU));
	CopyMemory(saberGeo->IndexBufferCPU->GetBufferPointer(), saberIndices.data(), saberIbByteSize);

	saberGeo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), saberVertices.data(), saberVbByteSize, saberGeo->VertexBufferUploader);

	saberGeo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), saberIndices.data(), saberIbByteSize, saberGeo->IndexBufferUploader);

	// 6. 数据格式配置
	saberGeo->VertexByteStride = sizeof(ModelVertex);
	saberGeo->VertexBufferByteSize = saberVbByteSize;
	saberGeo->IndexFormat = DXGI_FORMAT_R32_UINT;      // 必须是 R32 (因为是 unsigned int)
	saberGeo->IndexBufferByteSize = saberIbByteSize;

	// 7. 移交所有权
	mGeometries[saberGeo->Name] = std::move(saberGeo);

}



void SsaoApp::BuildPSOs()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC basePsoDesc;


	ZeroMemory(&basePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	basePsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
	basePsoDesc.pRootSignature = mRootSignature.Get();
	basePsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["standardVS"]->GetBufferPointer()),
		mShaders["standardVS"]->GetBufferSize()
	};
	basePsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
		mShaders["opaquePS"]->GetBufferSize()
	};
	basePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	basePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	basePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	basePsoDesc.SampleMask = UINT_MAX;
	basePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	basePsoDesc.NumRenderTargets = 1;
	basePsoDesc.RTVFormats[0] = mBackBufferFormat;
	basePsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	basePsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	basePsoDesc.DSVFormat = mDepthStencilFormat;

	//
	// PSO for opaque objects.
	//

	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc = basePsoDesc;

	// 【修改点：必须在创建 PSO 之前设置 MRT 参数】
	opaquePsoDesc.NumRenderTargets = 2;
	// 对应 SV_Target0 (Color)
	opaquePsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	// 对应 SV_Target1 (Velocity)
	opaquePsoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;

	// 【新增这一行】强制指定深度格式为 D32_FLOAT，与 TAA 深度图匹配
	opaquePsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mPSOs["opaque"])));

	// TAA CS PSO
	D3D12_COMPUTE_PIPELINE_STATE_DESC taaPsoDesc = {};
	taaPsoDesc.pRootSignature = mRootSignature.Get();
	taaPsoDesc.CS =
	{
		reinterpret_cast<BYTE*>(mShaders["taaCS"]->GetBufferPointer()),
		mShaders["taaCS"]->GetBufferSize()
	};
	taaPsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

	ThrowIfFailed(md3dDevice->CreateComputePipelineState(&taaPsoDesc, IID_PPV_ARGS(&mPSOs["taaCS"])));



}

void SsaoApp::BuildFrameResources()
{
	for (int i = 0; i < gNumFrameResources; ++i)
	{
		mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
			2, (UINT)mAllRitems.size(), (UINT)mMaterials.size()));
	}
}

void SsaoApp::BuildMaterials()
{


	auto tile0 = std::make_unique<Material>();
	tile0->Name = "tile0";
	tile0->MatCBIndex = 0;
	tile0->DiffuseSrvHeapIndex = 0;
	tile0->NormalSrvHeapIndex = 0;
	tile0->DiffuseAlbedo = XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f);
	tile0->FresnelR0 = XMFLOAT3(0.2f, 0.2f, 0.2f);
	tile0->Roughness = 0.1f;

	mMaterials["tile0"] = std::move(tile0);

	const std::vector<Submesh>& SaberMeshes = Saber->GetMeshes();
	for (int i = 0; i < Saber->GetMeshes().size(); ++i) // saber纹理是从1开始的
	{
		auto SaberSubMesh = std::make_unique<Material>();
		std::string submeshName = "SaberSubMesh" + std::to_string(i);

		SaberSubMesh->Name = submeshName;
		SaberSubMesh->MatCBIndex = i + 1;
		// 根据子网格体对应的纹理索引来设置.
		// 如果按照 tile0->DiffuseSrvHeapIndex = i;的话
		// 会因为子网格共用纹理贴图而出现纹理读取越界
		int realTexIndex = SaberMeshes[i].MaterialIndex;
		SaberSubMesh->DiffuseSrvHeapIndex = realTexIndex + 1; // 根据子网格体对应的纹理索引来设置
		SaberSubMesh->NormalSrvHeapIndex = realTexIndex + 1;

		SaberSubMesh->DiffuseAlbedo = XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f);
		SaberSubMesh->FresnelR0 = XMFLOAT3(0.2f, 0.2f, 0.2f);
		SaberSubMesh->Roughness = 0.1f;

		mMaterials[submeshName] = std::move(SaberSubMesh);
	}

}
void SsaoApp::UpdateObjectTransforms() 
{
	for (auto& e : mAllRitems)
	{
		// 在修改 World 之前，先将其备份到 PrevWorld
		e->PrevWorld = e->World;

		// ... 然后在这里执行物体的旋转、平移，更新 e->World ...

		// 标记需要更新 CB
		e->NumFramesDirty = gNumFrameResources;
	}
}

void SsaoApp::BuildRenderItems()
{

	auto gridRitem = std::make_unique<RenderItem>();
	gridRitem->World = MathHelper::Identity4x4();
	XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
	gridRitem->ObjCBIndex = 0;
	gridRitem->Mat = mMaterials["tile0"].get();
	gridRitem->Geo = mGeometries["shapeGeo"].get();
	gridRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	gridRitem->IndexCount = gridRitem->Geo->DrawArgs["grid"].IndexCount;
	gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
	gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;

	mRitemLayer[(int)RenderLayer::Opaque].push_back(gridRitem.get());
	mAllRitems.push_back(std::move(gridRitem));

	//  grid 占用了 ObjCBIndex = 0
	// 因此 Saber 的 RenderItem 需要从 1 开始累加
	UINT objCBIndex = 1;

	const auto& SaberMeshes = Saber->GetMeshes();

	for (int i = 0; i < SaberMeshes.size(); ++i)
	{
		auto saberRitem = std::make_unique<RenderItem>();

		// 1. 设置世界矩阵 (World Matrix / ワールド行列)
		// 根据从 Assimp 导出的模型实际大小，你可能需要在这里调整缩放 (Scaling) 或平移 (Translation)
		XMStoreFloat4x4(&saberRitem->World, XMMatrixScaling(1.0f, 1.0f, 1.0f) * XMMatrixTranslation(0.0f, 0.0f, 0.0f));

		// Saber 模型的纹理不需要像地板那样平铺，所以保持单位矩阵即可
		XMStoreFloat4x4(&saberRitem->TexTransform, XMMatrixIdentity());

		// 每个子网格都需要一个独立的对象常量缓冲区索引
		saberRitem->ObjCBIndex = objCBIndex++;

		// 2. 绑定对应材质
		// 这里的命名必须与我们在上一步 mMaterials 中存入的 Key 完全一致
		std::string matName = "SaberSubMesh" + std::to_string(i);
		saberRitem->Mat = mMaterials[matName].get();

		// 3. 绑定唯一的全局大几何体
		// 注意：所有 Saber 的子网格都共享同一个大 Buffer！
		saberRitem->Geo = mGeometries["SaberGeo"].get();

		// 4. 设置图元拓扑类型 (Primitive Topology / プリミティブトポロジー)
		saberRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

		// 5. 【核心逻辑】提取子网格偏移量以发起精确的绘制调用 (Draw Call / ドローコール)
		// 这里的命名必须与我们在创建大 Buffer 时存入 DrawArgs 的 Key 完全一致
		std::string submeshName = "Submesh" + std::to_string(i);
		saberRitem->IndexCount = saberRitem->Geo->DrawArgs[submeshName].IndexCount;
		saberRitem->StartIndexLocation = saberRitem->Geo->DrawArgs[submeshName].StartIndexLocation;
		saberRitem->BaseVertexLocation = saberRitem->Geo->DrawArgs[submeshName].BaseVertexLocation;

		// 6. 放入不透明渲染层并移交所有权
		mRitemLayer[(int)RenderLayer::Opaque].push_back(saberRitem.get());
		mAllRitems.push_back(std::move(saberRitem));
	}
}

void SsaoApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

	auto objectCB = mCurrFrameResource->ObjectCB->Resource();

	// For each render item...
	for (size_t i = 0; i < ritems.size(); ++i)
	{
		auto ri = ritems[i];

		cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
		cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
		cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

		D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;

		cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);

		cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
	}
}



std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> SsaoApp::GetStaticSamplers()
{
	// Applications usually only need a handful of samplers.  So just define them all up front
	// and keep them available as part of the root signature.  

	const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
		0, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
		1, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
		2, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
		3, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
		4, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
		0.0f,                             // mipLODBias
		8);                               // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
		5, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
		0.0f,                              // mipLODBias
		8);                                // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC shadow(
		6, // shaderRegister
		D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressW
		0.0f,                               // mipLODBias
		16,                                 // maxAnisotropy
		D3D12_COMPARISON_FUNC_LESS_EQUAL,
		D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK);

	return {
		pointWrap, pointClamp,
		linearWrap, linearClamp,
		anisotropicWrap, anisotropicClamp,
		shadow
	};
}