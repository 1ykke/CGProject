# E 组开发分支状态

本分支在公共 E/F 基础上增加屏幕像素到世界空间拾取射线的计算。

## 已实现
- 使用左上角为原点的 framebuffer 像素坐标。
- 使用像素中心采样，支持宽高比和垂直视场角。
- 依据 CameraController 的 eye/target 构造相机基向量。
- 处理相机视线接近世界 Y 轴时的退化叉积。
- 对非法尺寸、越界像素和非法 FOV 明确报错。
- 提供独立 C++ 测试。

## 尚未完成
- GLFW 窗口与鼠标事件绑定。
- 内容区域偏移、DPI/Retina framebuffer 尺寸换算。
- Metal 纹理上传和 Dear ImGui 展示。
- 实际场景对象选择与选中状态 UI。

## 坐标约定
调用方必须先把鼠标位置转换为 framebuffer 局部像素坐标，再传入 screen_pixel_ray。不要直接把逻辑窗口坐标当作 Retina framebuffer 坐标。
