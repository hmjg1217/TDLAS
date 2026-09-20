# TDLAS 测氢项目结构

```text
TDLAS测氢/
├─ CMakeLists.txt
├─ PROJECT_STRUCTURE.md
├─ app/
│  └─ main.cpp                 # 程序入口
├─ include/
│  ├─ core/                    # 通用数据结构、采集配置、错误类型
│  ├─ daq/                     # NI-DAQmx 采集接口
│  ├─ processing/              # TDLAS 算法接口、基线与浓度反演接口
│  └─ ui/                      # 主窗口和 Qt UI 相关接口
├─ src/
│  ├─ core/                    # 通用模块实现
│  ├─ daq/                     # DAQmx 任务、连续采集、采集线程
│  ├─ processing/              # 原始数据处理和算法适配层
│  └─ ui/                      # 主窗口、波形显示、控制面板
├─ resources/                  # Qt 资源、图标、样式表
├─ tests/                      # 单元测试和硬件无关测试
├─ cmake/                      # 后续放置 CMake 辅助模块
└─ 吸收曲线_时域转频域cm-1/   # 已有 MATLAB、LVM、CSV 资料，保持为数据/参考目录
```

## 模块边界

- `tdlas_core`：只放通用数据模型和模块间约定，不依赖 DAQmx。
- `tdlas_daq`：封装 USB-6363 的 NI-DAQmx 调用，负责连续采集和数据发送。
- `tdlas_ui`：负责 Qt 主窗口、采样率/通道配置、启动停止和实时曲线。
- `tdlas_app`：仅负责创建 `QApplication` 和主窗口。
- `tdlas_processor`：提供后续算法接入点，例如原始电压序列、基线校正、2f 信号和浓度结果。

## 配置说明

默认开启 `TDLAS_ENABLE_DAQMX`。如果开发机尚未安装 NI-DAQmx C API，可以先关闭硬件支持，验证 Qt 工程骨架：

```powershell
cmake -S . -B build -DTDLAS_ENABLE_DAQMX=OFF
cmake --build build --config Debug
```

如果 DAQmx 安装在非默认目录，可在配置时显式指定：

```powershell
cmake -S . -B build `
  -DDAQMX_INCLUDE_DIR="C:/path/to/include" `
  -DDAQMX_LIBRARY="C:/path/to/nidaqmx.lib"
```

实际通道名、采样率、触发方式和每次读取样本数不在 CMake 中固定，后续会由 UI 配置传入采集模块。
