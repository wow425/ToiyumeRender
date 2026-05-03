#pragma once

// 规定强制使用左手系
// Forward = +Z（全项目统一）
// 行向量 v * M


#include "Math/VectorMath.h"
#include "Math/Frustum.h"

namespace Math
{
    class BaseCamera
    {
    public:
        // 每帧更新函数：重新计算View，Proj，ViewProj等矩阵
        // 若每帧调用次数不唯一，会破环时间相关效果TAA
        void Update();

        // 基础控制接口
        void SetEyeAtUp(Vector3 eye, Vector3 at, Vector3 up); // 使用“眼点、焦点、上方向”设置相机（经典 LookAt）
        void SetLookDirection(Vector3 forward, Vector3 up);   // 使用“前方向、上方向”直接定义朝向
        void SetRotation(Quaternion basisRotation);           // 使用四元数 (Quaternion / クォータニオン) 设置旋转
        void SetPosition(Vector3 worldPos);                   // 设置相机在世界空间的位置
        void SetTransform(const AffineTransform& xform);      // 使用仿射变换矩阵设置相机位姿
        void SetTransform(const OrthogonalTransform& xform);  // 使用正交变换矩阵设置相机位姿

        // 获取属性的访问器 (Accessors)
        const Quaternion GetRotation() const { return m_CameraToWorld.GetRotation(); } // 获取相机旋转
        const Vector3 GetRightVec() const { return m_Basis.GetX(); }     // 获取相机本地坐标系的 X 轴（右）
        const Vector3 GetUpVec() const { return m_Basis.GetY(); }        // 获取相机本地坐标系的 Y 轴（上）
        const Vector3 GetForwardVec() const { return m_Basis.GetZ(); }   // 获取相机本地坐标系的 Z 轴   强制左手系！！
        const Vector3 GetPosition() const { return m_CameraToWorld.GetTranslation(); } // 获取世界坐标

        // 获取矩阵
        const Matrix4& GetViewMatrix() const { return m_ViewMatrix; }           // 获取视图矩阵 (World -> View)
        const Matrix4& GetProjMatrix() const { return m_ProjMatrix; }           // 获取投影矩阵 (View -> Clip)
        const Matrix4& GetViewProjMatrix() const { return m_ViewProjMatrix; }   // 获取视图投影矩阵 (World -> Clip)
        const Matrix4& GetReprojectionMatrix() const { return m_ReprojectMatrix; } // 获取重投影矩阵（用于 TAA 或 Motion Blur）


    protected:
        BaseCamera() : m_CameraToWorld(kIdentity), m_Basis(kIdentity) {}

        void SetProjMatrix(const Matrix4& ProjMat) { m_ProjMatrix = ProjMat; }

        OrthogonalTransform m_CameraToWorld; // 相机到世界的正交变换


        Matrix3 m_Basis;


        Matrix4 m_ViewMatrix;
        Matrix4 m_ProjMatrix;
        Matrix4 m_ViewProjMatrix;
        Matrix4 m_PreviousViewProjMatrix;   // 上一帧的 ViewProj 矩阵：用于计算当前像素与上一帧像素的位移差 (Velocity Buffer)。
        Matrix4 m_ReprojectMatrix;          // 重投影矩阵：将当前帧的裁剪空间坐标映射回上一帧的裁剪空间。
    };

    class Camera : public BaseCamera
    {
    public:
        Camera();

        // 控制“观察空间到投影空间 (View-to-Projection)”的矩阵
        // 设置透视矩阵参数：垂直视野、宽高比、近裁剪面距离、远裁剪面距离
        void SetPerspectiveMatrix(float verticalFovRadians, float aspectHeightOverWidth, float nearZClip, float farZClip);
        void SetFOV(float verticalFovInRadians) { m_VerticalFOV = verticalFovInRadians; UpdateProjMatrix(); }
        void SetAspectRatio(float heightOverWidth) { m_AspectRatio = heightOverWidth; UpdateProjMatrix(); }
        void SetZRange(float nearZ, float farZ) { m_NearClip = nearZ; m_FarClip = farZ; UpdateProjMatrix(); }

        void ReverseZ(bool enable) { m_ReverseZ = enable; UpdateProjMatrix(); }

        float GetFOV() const { return m_VerticalFOV; }      // 获取当前视野
        float GetNearClip() const { return m_NearClip; }    // 获取近裁剪面距离
        float GetFarClip() const { return m_FarClip; }      // 获取远裁剪面距离

        // 获取清除深度缓冲区时使用的默认值
        // 如果启用了反转 Z，清除值为 0.0；否则为 1.0
        float GetClearDepth() const { return m_ReverseZ ? 0.0f : 1.0f; }

    private:

        void UpdateProjMatrix(void);

        float m_VerticalFOV;    // 垂直视野角度（以弧度为单位）
        float m_AspectRatio;    // 长宽比
        float m_NearClip;       // 近裁剪面距离
        float m_FarClip;        // 远裁剪面距离


        bool m_ReverseZ;        // 反转 Z 轴标志：开启后将近平面映射为 Z=1，远平面映射为 Z=0，以提升远景的深度精度
        bool m_InfiniteZ;       // 无限远 Z 标志：将远裁剪面设置在无穷远处
    };

    // 相机放在eye， 朝向at， up确定头顶方向
    inline void BaseCamera::SetEyeAtUp(Vector3 eye, Vector3 at, Vector3 up)
    {
        // eye相机位置， at相机看向的目标点， up相机的上方向参考
        // forward=at−eye 相机朝向
        SetLookDirection(at - eye, up);

        SetPosition(eye);
    }

    inline void BaseCamera::SetPosition(Vector3 worldPos)
    {
        m_CameraToWorld.SetTranslation(worldPos);
    }

    // 从一个通用的仿射变换 (Affine Transform / アフィン変換) 重新推导相机的正交变换
    inline void BaseCamera::SetTransform(const AffineTransform& xform)
    {
        // 关键点：通过提取矩阵的基向量并强制正交化，防止因浮点运算累积导致的矩阵“切变”
        // 左手坐标系，+z为前方向
        SetLookDirection(xform.GetZ(), xform.GetY());
        SetPosition(xform.GetTranslation());
    }

    // 设置相机旋转：使用四元数 (Quaternion / クォータニオン)
    inline void BaseCamera::SetRotation(Quaternion basisRotation)
    {
        // 1. 规范化四元数，防止旋转缩放偏移
        // 2. 更新相机到世界的变换矩阵
        m_CameraToWorld.SetRotation(Normalize(basisRotation));
        // 3. 将旋转转换为 3x3 矩阵并缓存到 m_Basis（用于快速访问 Right/Up/Forward 向量）
        m_Basis = Matrix3(m_CameraToWorld.GetRotation());
    }

    // Camera 类的构造函数 使用左手系
    inline Camera::Camera() : m_ReverseZ(false), m_InfiniteZ(false)
    {
        // 默认关闭反转 Z (Reverse-Z)，默认视野 45 度 (PI/4)，长宽比 16:9，近平面 1.0，远平面 1000.0
        SetPerspectiveMatrix(XM_PIDIV4, 9.0f / 16.0f, 1.0f, 1000.0f);
    }

    // 设置透视投影矩阵参数
    inline void Camera::SetPerspectiveMatrix(float verticalFovRadians, float aspectHeightOverWidth, float nearZClip, float farZClip)
    {
        m_VerticalFOV = verticalFovRadians;
        m_AspectRatio = aspectHeightOverWidth;
        m_NearClip = nearZClip;
        m_FarClip = farZClip;

        // 重新计算并生成 m_ProjMatrix（投影矩阵）
        UpdateProjMatrix();

        // 初始化时，将上一帧的 ViewProj 矩阵设为当前矩阵，避免第一帧出现错误的运动矢量
        m_PreviousViewProjMatrix = m_ViewProjMatrix;
    }
} // namespace Math

