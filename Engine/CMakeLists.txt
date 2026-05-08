# ==========================================
# 收集 Engine 下所有的源码文件
# ==========================================
# 递归获取所有 .cpp 和 .h 文件（方便起见使用 GLOB_RECURSE）
file(GLOB_RECURSE ENGINE_SOURCES
    "*.cpp"
    "*.h"
    "*.hpp"
)

# 编译为静态库
add_library(TooiyumeEngine STATIC ${ENGINE_SOURCES})

# ==========================================
# Include 路径设置 (满足 #include "00_Core/..." 格式)
# ==========================================
target_include_directories(TooiyumeEngine PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
)


# 1. 包含我们自己写的 CMake 模块 (相当于 #include "cmake/CompileShader.cmake")
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileShader.cmake)

# 2. 收集所有的 Shader 源码路径
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/11_Shaders")

# 3. 逐个调用刚才定义的函数，编译你的 Shader
# 语法: compile_shader(源文件路径 ShaderModel 入口函数 "宏定义列表")

# 编译 Vertex Shader
compile_shader(
    "${SHADER_DIR}/DefaultVS.hlsl" 
    "vs_6_6" 
    "MainVS" 
    "ENABLE_INSTANCING=1;DEBUG" # 宏定义项
)

# 编译 Pixel Shader
compile_shader(
    "${SHADER_DIR}/DefaultPS.hlsl" 
    "ps_6_6" 
    "MainPS" 
    "USE_PBR=1"
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