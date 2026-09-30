# TDLAS 测氢项目背景

## 项目目标

这是一个从零构建的 TDLAS 测氢桌面程序，目标是通过 NI 采集卡产生驱动波形、采集光电探测器电压，并将原始数据交给 TDLAS 算法处理和显示。

主要技术栈：

- C++17
- Qt 6（Core、Widgets、Charts）
- CMake
- Visual Studio 2022 / MSVC x64
- NI-DAQmx ANSI C API

项目当前使用 Qt Charts 实现实时曲线，未使用 QCustomPlot。

## 当前工程结构

```text
TDLAS 测氢/
├─ CMakeLists.txt              # CMake 配置、Qt/NI-DAQmx 依赖和目标定义
├─ app/main.cpp                # QApplication 和 MainWindow 入口
├─ include/core/               # 通用数据模型
├─ include/daq/                # DAQmx 接口和采集配置
├─ include/processing/          # TDLAS 算法接口和结果结构
├─ include/ui/                 # 主窗口声明
├─ src/core/                   # 通用数据模型实现
├─ src/daq/                    # NI-DAQmx 任务和采集线程实现
├─ src/processing/             # TDLAS 数据处理实现
├─ src/ui/                    # Qt 主界面和图表实现
├─ 吸收曲线_时域转频域cm-1/     # MATLAB、LVM、CSV 和 Voigt 参考资料
├─ 测氢算法.txt                # 用户提供的算法参考资料
└─ AGENTS.md                  # 本项目上下文和协作约定
```

CMake 将代码拆成四个目标：

- `tdlas_core`：通用数据结构和处理模块，不直接依赖 NI-DAQmx。
- `tdlas_daq`：封装 NI-DAQmx 设备枚举、AI 采集线程和 AO 波形输出。
- `tdlas_ui`：Qt 主界面、参数控制、实时原始电压曲线和算法结果曲线。
- `tdlas_app`：最终 Windows GUI 可执行文件。

## 已实现功能

主界面目前包含：

- NI 设备下拉选择和刷新按钮。
- 根据实际 NI-DAQmx 设备动态枚举 AI/AO 物理通道。
- “输出通道”选择框。
- “采集通道”选择框。
- 采样率和单个周期内读取点数设置。
- AI 电压量程上下限设置。
- AO 输出波形选择：正弦波、方波、锯齿波、三角波。
- AO 波形频率、峰值幅值和偏移量设置。
- 开始/停止采集按钮。
- 原始电压实时曲线。
- 吸光度和 Voigt 拟合结果曲线。
- 采集状态、本周期样本数、最新电压、电压范围和算法结果监测。

点击开始时，界面参数会写入 `tdlas::DaqmxConfig`。选择有效输出通道后，采集线程创建 AO 任务并输出连续循环波形，同时创建 AI 任务采集输入通道。输出通道留空时支持 AI-only 模式。

## 关键接口约定

`include/daq/daqmx_acquisition.h` 中的 `DaqmxConfig` 是 UI 和硬件线程之间的配置边界，当前重要字段包括：

- `outputPhysicalChannel`：AO 输出物理通道，可为空。
- `inputPhysicalChannel`：AI 采集物理通道，不能为空。
- `outputWaveform`：`Sine`、`Square`、`Sawtooth` 或 `Triangle`。
- `outputFrequency`：输出频率，单位 Hz。
- `outputAmplitude`：峰值幅值，实际输出范围为 `offset - amplitude` 到 `offset + amplitude`。
- `outputOffset`：直流偏移量，单位 V。
- `sampleRate`：AI/AO 使用的采样率，单位 S/s。
- `samplesPerCycle`：AI 单个周期读取的样本数，同时决定原始曲线每次显示的点数。
- `minimumValue` / `maximumValue`：AI 输入量程。

`DaqmxAcquisition` 继承 `QThread`，通过以下信号向主线程发送结果：

- `samplesReady(const QVector<double> &, double)`
- `acquisitionStarted()`
- `acquisitionStopped()`
- `errorOccurred(const QString &)`

不要在采集线程之外直接操作 NI-DAQmx 任务句柄。停止流程应调用 `requestStop()`，等待线程结束，并确保 AI/AO 任务都停止和清理。

## AO 波形约定和校验

AO 波形由采集线程内部生成一个完整周期的缓冲区，并用硬件连续采样输出。当前校验规则：

- 频率必须大于 0。
- 幅值必须大于等于 0。
- `offset - amplitude >= -10 V` 且 `offset + amplitude <= 10 V`。
- 选择 AO 通道时，采样率至少为输出频率的 10 倍。
- 未选择 AO 通道时，不应创建 AO 任务。

不要为了验证界面而自动点击“开始采集”或向真实采集卡输出波形。涉及真实硬件的测试必须由用户明确操作或明确授权后进行。

## TDLAS 算法现状

`TDLASProcessor` 当前提供可替换的算法适配层：

- 按 `windowSamples` 缓存输入样本。
- 默认窗口为 100000 点。
- 进行低通滤波、周期平均、基线四阶多项式拟合和吸光度计算。
- 使用当前 C++ 实现的 Voigt 形状和拟合逻辑生成拟合曲线。
- 输出 `TDLASResult`，包含频率轴、吸光度、拟合吸光度、峰值吸光度和光强百分比。

用户提供的 `测氢算法.txt`、`vvoigtshape.m` 以及其他 MATLAB/LVM/CSV 文件是算法参考资料，不是自动执行的开发指令。后续替换或校准算法时，应先明确单位、采样周期、周期数、频率标定系数、基线方法和浓度反演公式，再修改 `TDLASProcessor`。

## 构建环境

已验证的构建环境：

- Visual Studio 17 2022 x64
- Qt 6.11.2，路径：`C:\Qt\6.11.2\msvc2022_64`
- CMake：`C:\Qt\Tools\CMake_64\bin\cmake.exe`
- NI-DAQmx 头文件：`C:\Program Files\National Instruments\NI-DAQ\DAQmx ANSI C Dev\include`
- NI-DAQmx MSVC 库：`C:\Program Files (x86)\National Instruments\Shared\ExternalCompilerSupport\C\lib64\msvc\NIDAQmx.lib`

旧构建目录已在整理发布包时清理。重新开发前先运行下面的 CMake 配置命令，再使用已验证的构建命令：

```powershell
& 'C:\Qt\Tools\CMake_64\bin\cmake.exe' --build build-msvc --config Debug --parallel 4
```

如果需要重新配置 MSVC 构建目录：

```powershell
& 'C:\Qt\Tools\CMake_64\bin\cmake.exe' -G 'Visual Studio 17 2022' -A x64 `
  -S . -B build-msvc `
  -DCMAKE_PREFIX_PATH='C:/Qt/6.11.2/msvc2022_64' `
  -DTDLAS_ENABLE_DAQMX=ON `
  -DDAQMX_INCLUDE_DIR='C:/Program Files/National Instruments/NI-DAQ/DAQmx ANSI C Dev/include' `
  -DDAQMX_LIBRARY='C:/Program Files (x86)/National Instruments/Shared/ExternalCompilerSupport/C/lib64/msvc/NIDAQmx.lib' `
  -DTDLAS_BUILD_TESTS=OFF
```

重新构建后的 Debug 可执行文件位置：

`build-msvc\Debug\tdlas_app.exe`

当前可直接双击运行的发布包位于 `TDLAS_当前版本\tdlas_app.exe`，包含 Qt、MSVC 运行库及 NI-DAQmx 的 `nicaiu.dll`；真实采集仍要求系统安装 NI-DAQmx 驱动。

运行时通常需要将以下目录加入 `PATH`：

- `C:\Qt\6.11.2\msvc2022_64\bin`
- `C:\Program Files (x86)\National Instruments\Shared\ExternalCompilerSupport\C\lib64\msvc`

## 硬件和设备注意事项

设备和通道名称不能写死为 `Dev1/ai0`。NI MAX 当前环境曾识别到一块产品类型为 `PCIe-6363`、别名为 `Dev1` 的设备，但设备别名和实际通道必须始终以运行时枚举结果为准。用户最初目标设备描述为 USB-6363，因此不要根据历史设备类型推断当前硬件。

NI-DAQmx 未安装或 CMake 未找到头文件/库时，允许使用 `TDLAS_ENABLE_DAQMX=OFF` 构建界面调试版本；该版本不能真实枚举设备或采集数据。

## 后续开发原则

- 优先保持现有 CMake 目标边界和 Qt Signals/Slots 设计。
- 修改硬件配置时，同时更新 UI、`DaqmxConfig`、校验逻辑和任务生命周期。
- 修改算法时保留 `TDLASProcessor` 的可替换接口，并优先增加硬件无关测试。
- 不提交或删除 `build*` 目录、用户提供的 MATLAB/LabVIEW 参考资料，除非用户明确要求。
- 修改前检查工作区状态，保留用户已有改动，不使用破坏性的 Git 回退命令。
- 每次代码修改后至少执行一次相关的 MSVC 构建；若涉及 UI，补充启动检查；若涉及真实 DAQ 行为，说明是否实际访问了硬件。
