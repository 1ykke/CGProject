# E 组桌面端验收

以包含 PR #2 的上游 main 为基线。桌面端已实现相机旋转与缩放、行星选择、Compare、双侧同步像素放大和三个演示预设。验收以真实渲染窗口为准。

## Release 构建

```sh
cmake -S . -B build-studio -DPLANET_BUILD_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-studio --config Release
ctest --test-dir build-studio -C Release --output-on-failure
```

macOS 运行 `build-studio/planet_studio`（Metal）。Windows 使用 Visual Studio 生成器时运行 `build-studio/Release/planet_studio.exe`（OpenGL）。

## 手动检查

| 操作 | 通过条件 |
| --- | --- |
| 在地球与土星预设分别选择 1x / 4x SSAA，再选择 4x SSAA / 4x MSAA | 两侧比较同一场景与视角，改变左或右的方法只切换对应一侧 |
| 点击两侧相同位置、分隔线旁及四个角，再用方向键调整选择 | 两侧取样位置一致，9、17、25 像素区域都留在各自画面内，Details 对应当前像素 |
| 在 Retina 或 Windows 125% / 150% 缩放下缩放窗口并重复取样 | 鼠标指向的像素与取样框一致，右键旋转与滚轮缩放仍有效 |
| 在 Compare 运行动画或碰撞场景，随后退出 Compare | 比较期间暂停播放并隐藏 Events，退出后可恢复播放和查看 Events |
| 在单图 Pixel 页关闭 Inspect 后点击，再返回 Design 试画 | Pixel 页点击不改变地形，Design 可正常雕刻 |
| 等待 Frame cost 出结果，依次移动取样框、调整 Region、改变视角 | 前两项保持现有计时，改变视角后重新测量，显示对应方法和着色次数 |
| 保存项目、重新打开并切换预设 | 自定义外观保留，碰撞演示临时状态不会写回项目 |

记录系统版本、显示缩放、提交号、Release 构建结果并附两张预设截图。PR #2 的 macOS 验证可作为对照，最新提交仍需复验。PR #3 在审查中，合入后再验收处理历史及展开状态。
