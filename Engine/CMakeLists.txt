# ==========================================
# 收集 Engine 下所有的源码文件
# ==========================================
# 递归获取所有 .cpp 和 .h 文件（方便起见使用 GLOB_RECURSE）
file(GLOB_RECURSE ENGINE_SOURCES
    "*.cpp"
    "*.h"
    "*.hpp"
)

# ==========================================
# Engine 静态库
# ==========================================
add_library(TooiyumeEngine STATIC ${ENGINE_SOURCES})

# ==========================================
# Shader 编译
# ==========================================
compile_shader(
    StaticMeshVS
    Shader/02_Deferred/StaticMeshVS.hlsl
    MainVS
    vs_6_6
)

compile_shader(
    GBufferPS
    Shader/02_Deferred/GBufferPS.hlsl
    MainPS
    ps_6_6
)
# ==========================================
# 链接Shader
# ==========================================
add_dependencies(TooiyumeEngine
    StaticMeshVS_Shader
    GBufferPS_Shader
)

target_include_directories(TooiyumeEngine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${CMAKE_BINARY_DIR}/ShaderCache"
)

target_include_directories(TooiyumeEngine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/ShaderCache
)
# ==========================================
# PCH (Precompiled Header)
# ==========================================
target_precompile_headers(TooiyumeEngine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/00_Core/PCH.h"
)

# ==========================================
# Include 路径设置 
# ==========================================
target_include_directories(TooiyumeEngine PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
)


# ==========================================
# 本地第三方库：PIX (从根目录的 ThirdParty 中引入)
# ==========================================
set(PIX_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/PIX")

# 包含 PIX 的头文件
target_include_directories(TooiyumeEngine PUBLIC "${PIX_DIR}/include")

# 链接 PIX 的静态库 (假设您开发的是 64 位程序)
target_link_directories(TooiyumeEngine PUBLIC "${PIX_DIR}/lib")

# ==========================================
# 链接所有依赖项
# ==========================================
target_link_libraries(TooiyumeEngine PUBLIC
    Microsoft::DirectXTex
    Microsoft::DirectXMesh
    Microsoft::DirectX-Headers
    WinPixEventRuntime    # PIX 的导入库
    dxcompiler            # vcpkg 安装的 DXC 库
    d3d12                 # Windows SDK 自带
    dxgi
    dxguid
)

