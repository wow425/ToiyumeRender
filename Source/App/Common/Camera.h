//***************************************************************************************
// Camera.h by Frank Luna (C) 2011 All Rights Reserved.
//   
// Simple first person style camera class that lets the viewer explore the 3D scene.
//   -It keeps track of the camera coordinate system relative to the world space
//    so that the view matrix can be constructed.  
//   -It keeps track of the viewing frustum of the camera so that the projection
//    matrix can be obtained.
//***************************************************************************************

#ifndef CAMERA_H
#define CAMERA_H

#include "d3dUtil.h"

class Camera
{
public:

	Camera();
	~Camera();

	// Get/Set world camera position.
	// 获取/设置世界相机位置。
	DirectX::XMVECTOR GetPosition()const;
	DirectX::XMFLOAT3 GetPosition3f()const;
	void SetPosition(float x, float y, float z);
	void SetPosition(const DirectX::XMFLOAT3& v);
	
	// Get camera basis vectors.
	//获取相机基向量。
	DirectX::XMVECTOR GetRight()const; // X
	DirectX::XMFLOAT3 GetRight3f()const;
	DirectX::XMVECTOR GetUp()const; // Y
	DirectX::XMFLOAT3 GetUp3f()const;
	DirectX::XMVECTOR GetLook()const; // Z
	DirectX::XMFLOAT3 GetLook3f()const;

	// Get frustum properties.
	// 获取视锥体属性
	float GetNearZ()const; // 近平面Z值
	float GetFarZ()const; // 远平面Z值
	float GetAspect()const; // 宽高比
	float GetFovY()const; // 垂直视场角
	float GetFovX()const;

	// Get near and far plane dimensions in view space coordinates.
	// 获取近平面和远平面在视空间坐标中的尺寸。
	float GetNearWindowWidth()const;
	float GetNearWindowHeight()const;
	float GetFarWindowWidth()const;
	float GetFarWindowHeight()const;
	
	// Set frustum.
	// 设置视锥体。
	void SetLens(float fovY, float aspect, float zn, float zf);

	// Define camera space via LookAt parameters.
	// 通过LookAt参数定义相机空间。
	void LookAt(DirectX::FXMVECTOR pos, DirectX::FXMVECTOR target, DirectX::FXMVECTOR worldUp);
	void LookAt(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& target, const DirectX::XMFLOAT3& up);

	// Get View/Proj matrices.
	// 获取视图/投影矩阵。
	DirectX::XMMATRIX GetView()const;
	DirectX::XMMATRIX GetProj()const;

	DirectX::XMFLOAT4X4 GetView4x4f()const;
	DirectX::XMFLOAT4X4 GetProj4x4f()const;

	// Strafe/Walk the camera a distance d.
	// 平移/行走相机距离d。
	void Strafe(float d);
	void Walk(float d);
	void Camera::Up(float d);
	void Camera::Down(float d);

	// Rotate the camera.
	// 旋转相机。
	void Pitch(float angle);
	void RotateY(float angle);

	// 翻滚roll
	void Roll(float angle);
	// After modifying camera position/orientation, call to rebuild the view matrix.
	// 修改相机位置/方向后，调用以重建视图矩阵。
	void UpdateViewMatrix();

private:

	// Camera coordinate system with coordinates relative to world space.
	// 相机坐标系，坐标相对于世界空间。
	DirectX::XMFLOAT3 mPosition = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 mRight = { 1.0f, 0.0f, 0.0f }; // x
	DirectX::XMFLOAT3 mUp = { 0.0f, 1.0f, 0.0f }; // y
	DirectX::XMFLOAT3 mLook = { 0.0f, 0.0f, 1.0f }; // z

	// Cache frustum properties.
	// 缓存视锥体属性。
	float mNearZ = 0.0f;
	float mFarZ = 0.0f;
	float mAspect = 0.0f;
	float mFovY = 0.0f;
	float mNearWindowHeight = 0.0f;
	float mFarWindowHeight = 0.0f;

	bool mViewDirty = true;

	// Cache View/Proj matrices.
	// 缓存视图/投影矩阵。
	DirectX::XMFLOAT4X4 mView = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 mProj = MathHelper::Identity4x4();
};

#endif // CAMERA_H