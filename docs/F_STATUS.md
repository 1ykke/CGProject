# F 组开发分支状态

本分支增加实验结果汇总脚本，读取 tools/export.py 输出的逐帧 frames.csv。

## 已实现
- 检查 CSV 必需字段。
- 按 preset、mode、width、height 分组。
- 计算样本数、中位数、最近秩 P95、最小值和最大值。
- 输出 summary.csv 与 summary.json。
- 拒绝缺失字段、非数值、NaN、无穷和负耗时。
- 提供独立 Python 单元测试。

## 使用方式
\`\`\`sh
python tools/analyze_results.py outputs/run-001/frames.csv --output outputs/run-001/analysis
\`\`\`

## 指标口径
P95 使用最近秩法：排序后取 ceil(0.95 × N) 对应的样本（按 1 起始计数）。分组键包含预设、采样模式和内部渲染分辨率，避免把不同实验条件混在一起。

## 尚未完成
- 目前输入仍由调用方提供真实逐帧数据。
- 尚未连接 C++ BenchmarkRunner 自动写出 CSV。
- 尚未实测真实渲染内核、上传和 UI 阶段耗时。
