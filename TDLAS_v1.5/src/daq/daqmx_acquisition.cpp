#include "daq/daqmx_acquisition.h"

#include <QByteArray>
#include <QMutexLocker>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <functional>

#if TDLAS_HAS_DAQMX
#include <NIDAQmx.h>
#endif

namespace tdlas {

namespace {

#if TDLAS_HAS_DAQMX
bool readDaqmxString(const std::function<int32(char *, uInt32)> &reader,
                     QString *value,
                     QString *errorMessage)
{
    QByteArray buffer(4096, '\0');
    const int32 code = reader(buffer.data(), static_cast<uInt32>(buffer.size()));
    if (code < 0) {
        char errorBuffer[2048] = {};
        DAQmxGetExtendedErrorInfo(errorBuffer, sizeof(errorBuffer));
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("读取 NI-DAQmx 信息失败，错误码 %1：%2")
                                .arg(code)
                                .arg(QString::fromLocal8Bit(errorBuffer));
        }
        return false;
    }
    if (value != nullptr) {
        *value = QString::fromLocal8Bit(buffer.constData());
    }
    return true;
}
#endif

QVector<double> generateWaveform(OutputWaveform waveform,
                                 double frequency,
                                 double amplitude,
                                 double offset,
                                 double sampleRate)
{
    constexpr double kTwoPi = 6.28318530717958647692;
    // The AO sample clock remains at the user-selected sample rate.  Therefore
    // one complete cycle must contain exactly sampleRate / frequency samples;
    // adding an arbitrary minimum here changes the frequency being output
    // (for example, 10 kS/s and 1 kHz would become 312.5 Hz with 32 points).
    // validateConfig() guarantees at least ten samples per cycle when AO is
    // enabled, so rounding this ratio is sufficient and keeps the requested
    // frequency represented by the cyclic regeneration buffer.
    const int waveformSamples = std::max(2, qRound(sampleRate / frequency));
    QVector<double> values(waveformSamples);

    for (int index = 0; index < waveformSamples; ++index) {
        const double phase = static_cast<double>(index) / waveformSamples;
        double normalized = 0.0;
        switch (waveform) {
        case OutputWaveform::Square:
            normalized = phase < 0.5 ? 1.0 : -1.0;
            break;
        case OutputWaveform::Sawtooth:
            normalized = 2.0 * phase - 1.0;
            break;
        case OutputWaveform::Triangle:
            normalized = phase < 0.5
                             ? (4.0 * phase - 1.0)
                             : (3.0 - 4.0 * phase);
            break;
        case OutputWaveform::Sine:
        default:
            normalized = std::sin(kTwoPi * phase);
            break;
        }
        values[index] = offset + amplitude * normalized;
    }
    return values;
}

} // namespace

DaqmxAcquisition::DaqmxAcquisition(QObject *parent)
    : QThread(parent)
{
}

DaqmxAcquisition::~DaqmxAcquisition()
{
    requestStop();
    wait();
}

bool DaqmxAcquisition::validateConfig(const DaqmxConfig &config,
                                      QString *errorMessage) const
{
    if (config.inputPhysicalChannel.trimmed().isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("采集输入物理通道不能为空。");
        }
        return false;
    }
    if (config.sampleRate <= 0.0 || config.samplesPerCycle <= 0) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("采样率和单个周期读取点数必须大于 0。");
        }
        return false;
    }
    if (config.minimumValue >= config.maximumValue) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("输入量程下限必须小于上限。");
        }
        return false;
    }
    if (config.readTimeoutSeconds <= 0.0) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("读取超时时间必须大于 0。");
        }
        return false;
    }
    if (!config.outputPhysicalChannel.trimmed().isEmpty()) {
        if (config.outputFrequency <= 0.0) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("输出波形频率必须大于 0。 ");
            }
            return false;
        }
        if (config.outputAmplitude < 0.0) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("输出波形幅值不能小于 0。 ");
            }
            return false;
        }
        if (config.outputOffset - config.outputAmplitude < -10.0
            || config.outputOffset + config.outputAmplitude > 10.0) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral(
                    "输出波形范围必须保持在 -10 V 到 +10 V 之间。 ");
            }
            return false;
        }
        if (config.sampleRate < config.outputFrequency * 10.0) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral(
                    "输出采样率至少应为波形频率的 10 倍。 ");
            }
            return false;
        }
    }
    return true;
}

QList<DaqmxDeviceInfo> DaqmxAcquisition::enumerateDevices(QString *errorMessage)
{
    QList<DaqmxDeviceInfo> devices;

#if !TDLAS_HAS_DAQMX
    if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral(
            "当前构建未启用 NI-DAQmx，无法枚举采集卡。请重新配置并启用 NI-DAQmx。\n"
            "当前程序仍可用于界面调试。 ");
    }
    return devices;
#else
    QString deviceNames;
    if (!readDaqmxString(
            [](char *data, uInt32 size) { return DAQmxGetSysDevNames(data, size); },
            &deviceNames, errorMessage)) {
        return devices;
    }

    for (const QString &deviceName : deviceNames.split(',', Qt::SkipEmptyParts)) {
        const QString name = deviceName.trimmed();
        if (name.isEmpty()) {
            continue;
        }

        DaqmxDeviceInfo info;
        info.name = name;
        QByteArray deviceBytes = name.toLocal8Bit();

        QString productType;
        if (readDaqmxString(
                [&](char *data, uInt32 size) {
                    return DAQmxGetDevProductType(deviceBytes.constData(), data, size);
                },
                &productType, nullptr)) {
            info.productType = productType.trimmed();
        }

        QString aiChannels;
        if (readDaqmxString(
                [&](char *data, uInt32 size) {
                    return DAQmxGetDevAIPhysicalChans(deviceBytes.constData(), data, size);
                },
                &aiChannels, nullptr)) {
            info.analogInputChannels = aiChannels.split(',', Qt::SkipEmptyParts);
            for (QString &channel : info.analogInputChannels) {
                channel = channel.trimmed();
            }
        }

        QString aoChannels;
        if (readDaqmxString(
                [&](char *data, uInt32 size) {
                    return DAQmxGetDevAOPhysicalChans(deviceBytes.constData(), data, size);
                },
                &aoChannels, nullptr)) {
            info.analogOutputChannels = aoChannels.split(',', Qt::SkipEmptyParts);
            for (QString &channel : info.analogOutputChannels) {
                channel = channel.trimmed();
            }
        }

        devices.append(info);
    }

    if (devices.isEmpty() && errorMessage != nullptr) {
        *errorMessage = QStringLiteral(
            "NI-DAQmx 未返回可用设备。请确认采集卡已连接，并在 NI MAX 中完成自检。 ");
    }
    return devices;
#endif
}

bool DaqmxAcquisition::configure(const DaqmxConfig &config)
{
    QString errorMessage;
    if (!validateConfig(config, &errorMessage)) {
        emit errorOccurred(errorMessage);
        return false;
    }
    if (isRunning()) {
        emit errorOccurred(QStringLiteral("采集运行期间不能修改配置，请先停止采集。"));
        return false;
    }

    QMutexLocker locker(&m_configMutex);
    m_config = config;
    m_configured.store(true);
    return true;
}

DaqmxConfig DaqmxAcquisition::config() const
{
    QMutexLocker locker(&m_configMutex);
    return m_config;
}

bool DaqmxAcquisition::isConfigured() const
{
    return m_configured.load();
}

void DaqmxAcquisition::requestStop()
{
    m_stopRequested.store(true);
}

void DaqmxAcquisition::run()
{
    if (!isConfigured()) {
        emit errorOccurred(QStringLiteral("启动采集前必须先完成 DAQ 配置。"));
        emit acquisitionStopped();
        return;
    }

    const DaqmxConfig activeConfig = config();
    m_stopRequested.store(false);

#if !TDLAS_HAS_DAQMX
    Q_UNUSED(activeConfig);
    emit errorOccurred(QStringLiteral(
        "当前构建未启用 NI-DAQmx。请安装 NI-DAQmx C API 后重新配置 CMake。"));
    emit acquisitionStopped();
#else
    TaskHandle aiTaskHandle = nullptr;
    TaskHandle aoTaskHandle = nullptr;
    char errorBuffer[2048] = {};
    QVector<double> samples(activeConfig.samplesPerCycle);

    auto checkError = [&](int32 code, const char *operation) {
        if (code >= 0) {
            return true;
        }

        DAQmxGetExtendedErrorInfo(errorBuffer, sizeof(errorBuffer));
        emit errorOccurred(QStringLiteral("%1 失败，DAQmx 错误码 %2：%3")
                               .arg(QString::fromUtf8(operation))
                               .arg(code)
                               .arg(QString::fromLocal8Bit(errorBuffer)));
        return false;
    };

    int32 errorCode = 0;
    QVector<double> outputWaveform;
    if (!activeConfig.outputPhysicalChannel.trimmed().isEmpty()) {
        outputWaveform = generateWaveform(
            activeConfig.outputWaveform,
            activeConfig.outputFrequency,
            activeConfig.outputAmplitude,
            activeConfig.outputOffset,
            activeConfig.sampleRate);

        errorCode = DAQmxCreateTask("", &aoTaskHandle);
        if (checkError(errorCode, "DAQmxCreateTask(AO)")) {
            const QByteArray channel = activeConfig.outputPhysicalChannel.toLocal8Bit();
            errorCode = DAQmxCreateAOVoltageChan(
                aoTaskHandle,
                channel.constData(),
                "",
                -10.0,
                10.0,
                DAQmx_Val_Volts,
                nullptr);
            checkError(errorCode, "DAQmxCreateAOVoltageChan");
        }
        if (errorCode >= 0) {
            errorCode = DAQmxCfgSampClkTiming(
                aoTaskHandle,
                "",
                activeConfig.sampleRate,
                DAQmx_Val_Rising,
                DAQmx_Val_ContSamps,
                static_cast<uInt64>(outputWaveform.size()));
            checkError(errorCode, "DAQmxCfgSampClkTiming(AO)");
        }
        if (errorCode >= 0) {
            // Make the finite waveform buffer repeat continuously while the
            // acquisition thread is running.  This avoids relying on the
            // device's default regeneration mode.
            errorCode = DAQmxSetWriteRegenMode(aoTaskHandle, DAQmx_Val_AllowRegen);
            checkError(errorCode, "DAQmxSetWriteRegenMode(AO)");
        }
        if (errorCode >= 0) {
            int32 samplesWritten = 0;
            errorCode = DAQmxWriteAnalogF64(
                aoTaskHandle,
                static_cast<int32>(outputWaveform.size()),
                0,
                activeConfig.readTimeoutSeconds,
                DAQmx_Val_GroupByChannel,
                outputWaveform.constData(),
                &samplesWritten,
                nullptr);
            checkError(errorCode, "DAQmxWriteAnalogF64");
        }
    }

    if (errorCode >= 0) {
        errorCode = DAQmxCreateTask("", &aiTaskHandle);
        if (checkError(errorCode, "DAQmxCreateTask(AI)")) {
        const QByteArray channel = activeConfig.inputPhysicalChannel.toLocal8Bit();
        errorCode = DAQmxCreateAIVoltageChan(
            aiTaskHandle,
            channel.constData(),
            "",
            DAQmx_Val_Diff,
            activeConfig.minimumValue,
            activeConfig.maximumValue,
            DAQmx_Val_Volts,
            nullptr);
        checkError(errorCode, "DAQmxCreateAIVoltageChan");
        }
    }

    if (errorCode >= 0) {
        errorCode = DAQmxCfgSampClkTiming(
            aiTaskHandle,
            "",
            activeConfig.sampleRate,
            DAQmx_Val_Rising,
            DAQmx_Val_ContSamps,
            static_cast<uInt64>(activeConfig.samplesPerCycle));
        checkError(errorCode, "DAQmxCfgSampClkTiming");
    }

    if (errorCode >= 0) {
        if (aoTaskHandle != nullptr) {
            errorCode = DAQmxStartTask(aoTaskHandle);
            checkError(errorCode, "DAQmxStartTask(AO)");
        }
    }

    if (errorCode >= 0) {
        errorCode = DAQmxStartTask(aiTaskHandle);
        checkError(errorCode, "DAQmxStartTask(AI)");
    }

    if (errorCode >= 0) {
        emit acquisitionStarted();

        while (!m_stopRequested.load()) {
            int32 samplesRead = 0;
            errorCode = DAQmxReadAnalogF64(
                aiTaskHandle,
                activeConfig.samplesPerCycle,
                activeConfig.readTimeoutSeconds,
                DAQmx_Val_GroupByChannel,
                samples.data(),
                static_cast<uInt32>(samples.size()),
                &samplesRead,
                nullptr);

            if (!checkError(errorCode, "DAQmxReadAnalogF64")) {
                break;
            }
            if (samplesRead > 0) {
                emit samplesReady(samples.mid(0, samplesRead),
                                  activeConfig.sampleRate);
            }
        }

        errorCode = DAQmxStopTask(aiTaskHandle);
        if (errorCode < 0 && !m_stopRequested.load()) {
            checkError(errorCode, "DAQmxStopTask(AI)");
        }
    }

    if (aoTaskHandle != nullptr) {
        const int32 stopCode = DAQmxStopTask(aoTaskHandle);
        if (stopCode < 0 && !m_stopRequested.load()) {
            checkError(stopCode, "DAQmxStopTask(AO)");
        }
        checkError(DAQmxClearTask(aoTaskHandle), "DAQmxClearTask(AO)");
    }
    if (aiTaskHandle != nullptr) {
        checkError(DAQmxClearTask(aiTaskHandle), "DAQmxClearTask(AI)");
    }
    emit acquisitionStopped();
#endif
}

} // namespace tdlas
