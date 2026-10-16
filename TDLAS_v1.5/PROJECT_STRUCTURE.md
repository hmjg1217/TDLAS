# TDLAS 测氢 v1.5 项目结构

```text
TDLAS_v1.5/
├─ CMakeLists.txt
├─ app/                       # QApplication 和程序入口
├─ include/                   # core、daq、processing、ui 接口
├─ src/                       # 通用模块、DAQmx、算法和 Qt UI 实现
├─ tests/                     # 硬件无关测试
├─ build-msvc/                # 本地 MSVC 构建目录（不提交）
├─ build-tests/               # 本地测试构建目录（不提交）
├─ TDLAS_v1.5发布包/           # v1.5 可直接运行的 Release 包及依赖
└─ 吸收曲线_时域转频域cm-1/    # MATLAB、LVM、CSV 和 Voigt 参考资料
```

## 模块边界

- `tdlas_core`：通用数据模型、峰值历史、TDLAS 算法和独立处理线程，不依赖 DAQmx。
- `tdlas_daq`：封装 NI-DAQmx 设备枚举、连续采集和 AO 波形输出。
- `tdlas_ui`：负责 Qt 主窗口、采样率/通道配置、实时曲线、图表独立窗口和峰值数据窗口。
- `tdlas_app`：创建 `QApplication` 和主窗口。

## 配置说明

默认开启 `TDLAS_ENABLE_DAQMX`。如果开发机尚未安装 NI-DAQmx C API，可以先关闭硬件支持验证 Qt 工程：

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.11.2/msvc2022_64" `
  -DTDLAS_ENABLE_DAQMX=OFF
cmake --build build-msvc --config Debug --parallel 4
```

实际设备和通道名称始终由运行时枚举结果决定，程序不会写死设备别名。
