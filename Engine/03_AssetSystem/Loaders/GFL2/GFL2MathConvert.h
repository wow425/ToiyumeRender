// ============================================================================
// 模块：GFL2MathConvert.h（Unity LH Y-Up <-> UE5 LH Z-Up 纯 CPU 正交变换工具）
// 架构层：Engine/03_AssetSystem/Loaders/GFL2（M0；零 D3D12 设备依赖，供 Loader 与 CTest 共用）
// 冻结约定（见 docs/AI/少前2复刻/AI施工/00_CONVENTIONS.md）：
//   - Unity LH Y-Up (+X Right, +Y Up, +Z Forward)
//   - UE5   LH Z-Up (+X Forward, +Y Right, +Z Up)
//   - 正交循环置换矩阵 P: (x_ue, y_ue, z_ue) = (z_u, x_u, y_u)，det(P) = +1
//   - 行向量左乘 (v' = v * M)：M_row_ue = P^T * M_row_unity * P，保持 det(M) = +1
//   - L2 SH 系数同样按正交基变换映射至 UE5 世界向量（机器精度 < 1e-6）
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <DirectXMath.h>
#include "00_Core/Math/VectorMath.h"

namespace GFL2::MathConv
{
	// 方向/位置循环置换：Unity (x, y, z) -> UE5 (z, x, y)
	inline DirectX::XMFLOAT3 UnityToUE5Vec3(float x_u, float y_u, float z_u)
	{
		return DirectX::XMFLOAT3(z_u, x_u, y_u);
	}

	inline DirectX::XMFLOAT3 UnityToUE5Vec3(const DirectX::XMFLOAT3& v)
	{
		return UnityToUE5Vec3(v.x, v.y, v.z);
	}

	// 将列存储 4x4 数组 cols[16]（列 0..2 为局部轴，cols[12..14] 为平移，即行向量转置格式）
	// 一步装入并共轭变换为 UE5 LH Z-Up 行向量左乘 Matrix4（M_row_ue = P^T * M_row_unity * P）。
	inline Math::Matrix4 UnityColsToUE5RowMatrix(const float cols[16])
	{
		// cols[4*r + c] 本身就是 Unity 行向量矩阵 M_row_u[r][c]
		// UE5 基下标 -> Unity 基下标：X_ue(0)->Z_u(2), Y_ue(1)->X_u(0), Z_ue(2)->Y_u(1)
		static constexpr int kSrc[3] = { 2, 0, 1 };

		DirectX::XMFLOAT4X4 out = {};
		for (int r = 0; r < 3; ++r)
		{
			const int sr = kSrc[r];
			for (int c = 0; c < 3; ++c)
			{
				const int sc = kSrc[c];
				out.m[r][c] = cols[sr * 4 + sc];
			}
			out.m[r][3] = 0.0f;
		}
		// 平移行（row 3）：(tx_ue, ty_ue, tz_ue) = (tz_u, tx_u, ty_u)
		for (int c = 0; c < 3; ++c)
			out.m[3][c] = cols[3 * 4 + kSrc[c]];
		out.m[3][3] = 1.0f;

		return Math::Matrix4(reinterpret_cast<const float*>(&out));
	}

	// 计算行向量矩阵下法线逆转置矩阵：v_ws = v * World, n_ws = n * WorldIT => WorldIT = Transpose(Inverse(World))
	inline Math::Matrix4 ComputeRowNormalMatrix(const Math::Matrix4& world)
	{
		DirectX::XMVECTOR det;
		const DirectX::XMMATRIX inv = DirectX::XMMatrixInverse(&det, static_cast<DirectX::XMMATRIX>(world));
		return Math::Matrix4(DirectX::XMMatrixTranspose(inv));
	}

	// 将 Unity L2 SH[7]（SHAr, SHAg, SHAb, SHBr, SHBg, SHBb, SHC）循环置换至 UE5 LH Z-Up 世界基。
	// 验证：对任意单位法线 N_u 与 N_ue = (N_u.z, N_u.x, N_u.y)，
	//       EvalSH_L2(outSH, N_ue) == EvalSH_L2(inSH, N_u)（浮点误差 < 1e-6）。
	inline void UnityToUE5SH_L2(const DirectX::XMFLOAT4 inSH[7], DirectX::XMFLOAT4 outSH[7])
	{
		const DirectX::XMFLOAT4& c_r = inSH[6]; // SHC.rgb -> (x^2 - y^2) 系数

		for (int ch = 0; ch < 3; ++ch)
		{
			const DirectX::XMFLOAT4& A_u = inSH[ch];     // (c_x, c_y, c_z, c_0)
			const DirectX::XMFLOAT4& B_u = inSH[3 + ch]; // (b0=xy, b1=yz, b2=z^2, b3=zx)
			const float b4 = (ch == 0) ? c_r.x : ((ch == 1) ? c_r.y : c_r.z);

			const float A = 0.5f * (B_u.z + b4);
			const float B = -0.5f * (B_u.z + 3.0f * b4);
			const float C = 0.5f * (B_u.z - b4);

			// L0+L1: (c_z, c_x, c_y, c_0 + A)
			outSH[ch] = DirectX::XMFLOAT4(A_u.z, A_u.x, A_u.y, A_u.w + A);
			// L2 B: (X*Y, Y*Z, Z^2, Z*X) <- (zx=b3, xy=b0, B, yz=b1)
			outSH[3 + ch] = DirectX::XMFLOAT4(B_u.w, B_u.x, B, B_u.y);

			if (ch == 0) outSH[6].x = C;
			else if (ch == 1) outSH[6].y = C;
			else outSH[6].z = C;
		}
		outSH[6].w = inSH[6].w;
	}

	// 按 Unity Shader 格式求值 L2 SH（可用于 CPU 测试校验与对照）
	inline DirectX::XMFLOAT3 EvaluateSH_L2(const DirectX::XMFLOAT4 sh[7], const DirectX::XMFLOAT3& n)
	{
		const float x = n.x, y = n.y, z = n.z;
		// L0 + L1
		float r = sh[0].x * x + sh[0].y * y + sh[0].z * z + sh[0].w;
		float g = sh[1].x * x + sh[1].y * y + sh[1].z * z + sh[1].w;
		float b = sh[2].x * x + sh[2].y * y + sh[2].z * z + sh[2].w;
		// L2 vB = (x*y, y*z, z*z, z*x)
		r += sh[3].x * (x * y) + sh[3].y * (y * z) + sh[3].z * (z * z) + sh[3].w * (z * x);
		g += sh[4].x * (x * y) + sh[4].y * (y * z) + sh[4].z * (z * z) + sh[4].w * (z * x);
		b += sh[5].x * (x * y) + sh[5].y * (y * z) + sh[5].z * (z * z) + sh[5].w * (z * x);
		// L2 vC = x*x - y*y
		const float vC = x * x - y * y;
		r += sh[6].x * vC;
		g += sh[6].y * vC;
		b += sh[6].z * vC;
		return DirectX::XMFLOAT3(r, g, b);
	}

	// 计算 UE5 LH Z-Up + Reverse-Z 紧致主方向光阴影正交矩阵（含阴影图纹素对齐防抖动）
	inline Math::Matrix4 ComputeUE5DirLightShadowMatrix(
		const DirectX::XMFLOAT3& boundsMin,
		const DirectX::XMFLOAT3& boundsMax,
		const DirectX::XMFLOAT3& dirToLightUE5,
		uint32_t shadowResolution = 4096u)
	{
		using namespace DirectX;
		const XMVECTOR bmin    = XMLoadFloat3(&boundsMin);
		const XMVECTOR bmax    = XMLoadFloat3(&boundsMax);
		const XMVECTOR center  = XMVectorScale(XMVectorAdd(bmin, bmax), 0.5f);
		const XMVECTOR extents = XMVectorScale(XMVectorSubtract(bmax, bmin), 0.5f);
		const float halfDiag   = XMVectorGetX(XMVector3Length(extents));
		const float orthoRad   = std::max(halfDiag * 1.15f, 1.10f);

		XMVECTOR toLight = XMLoadFloat3(&dirToLightUE5);
		const float lSq  = XMVectorGetX(XMVector3LengthSq(toLight));
		toLight = (lSq > 1e-8f)
			? XMVector3Normalize(toLight)
			: XMVector3Normalize(XMVectorSet(-0.738529f, 0.560575f, 0.374607f, 0.0f));
		const XMVECTOR lightFwd = XMVectorNegate(toLight);

		XMVECTOR upRef = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
		if (std::fabs(XMVectorGetZ(lightFwd)) > 0.98f)
			upRef = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
		const XMVECTOR right = XMVector3Normalize(XMVector3Cross(upRef, lightFwd));
		const XMVECTOR up    = XMVector3Normalize(XMVector3Cross(lightFwd, right));

		const float res        = static_cast<float>(std::max(shadowResolution, 256u));
		const float texelWorld = (2.0f * orthoRad) / res;
		const float cx = XMVectorGetX(XMVector3Dot(center, right));
		const float cy = XMVectorGetX(XMVector3Dot(center, up));
		const float cz = XMVectorGetX(XMVector3Dot(center, lightFwd));
		const float snappedX = std::floor(cx / texelWorld + 0.5f) * texelWorld;
		const float snappedY = std::floor(cy / texelWorld + 0.5f) * texelWorld;
		const XMVECTOR snappedCenter = XMVectorAdd(
			XMVectorAdd(XMVectorScale(right, snappedX), XMVectorScale(up, snappedY)),
			XMVectorScale(lightFwd, cz));

		const float camBackDist = orthoRad + 2.0f;
		const XMVECTOR eye      = XMVectorSubtract(snappedCenter, XMVectorScale(lightFwd, camBackDist));
		const float nearZ       = 0.25f;
		const float farZ        = camBackDist + orthoRad + 2.0f;

		XMFLOAT4 r0, r1, r2, r3;
		XMStoreFloat4(&r0, right);    r0.w = 0.0f;
		XMStoreFloat4(&r1, up);       r1.w = 0.0f;
		XMStoreFloat4(&r2, lightFwd); r2.w = 0.0f;
		XMStoreFloat4(&r3, eye);      r3.w = 1.0f;
		const XMMATRIX camToWorld(
			XMLoadFloat4(&r0), XMLoadFloat4(&r1), XMLoadFloat4(&r2), XMLoadFloat4(&r3));
		const XMMATRIX view = XMMatrixInverse(nullptr, camToWorld);

		const float invR = 1.0f / orthoRad;
		const float q1   = 1.0f / (nearZ - farZ);
		const float q2   = farZ / (farZ - nearZ);
		const XMMATRIX proj(
			XMVectorSet(invR, 0.0f, 0.0f, 0.0f),
			XMVectorSet(0.0f, invR, 0.0f, 0.0f),
			XMVectorSet(0.0f, 0.0f, q1,   0.0f),
			XMVectorSet(0.0f, 0.0f, q2,   1.0f));

		return Math::Matrix4(XMMatrixMultiply(view, proj));
	}

	// 将 Unity 世界->裁剪/阴影 行矩阵 (4 行) 左乘循环置换 P^T 映射至 UE5 世界->同裁剪空间：
	// pos_UE5 * M_UE5 = pos_Unity * M_Unity（仅重排第 0/1/2 行：UE X=Unity Z, UE Y=Unity X, UE Z=Unity Y）
	inline Math::Matrix4 UnityWorldToClipRowsToUE5(const float rows[16])
	{
		static constexpr int kSrcRow[3] = { 2, 0, 1 };
		DirectX::XMFLOAT4X4 out = {};
		for (int r = 0; r < 3; ++r)
		{
			const int sr = kSrcRow[r];
			for (int c = 0; c < 4; ++c)
				out.m[r][c] = rows[sr * 4 + c];
		}
		for (int c = 0; c < 4; ++c)
			out.m[3][c] = rows[3 * 4 + c];
		return Math::Matrix4(reinterpret_cast<const float*>(&out));
	}
}
