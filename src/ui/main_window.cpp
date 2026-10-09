#include "ui/main_window.h"

#include "daq/daqmx_acquisition.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

struct DetectedSawtoothCycle
{
    QVector<double> samples;
    int offset = 0;
};

double median(QVector<double> values)
{
    if (values.isEmpty()) return 0.0;
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    double value = *middle;
    if (values.size() % 2 == 0) {
        const auto lowerMiddle = std::max_element(values.begin(), middle);
        value = (*lowerMiddle + value) * 0.5;
    }
    return value;
}

QVector<double> resampleCycle(const QVector<double> &source, int targetSize)
{
    if (source.size() == targetSize) return source;
    if (source.isEmpty() || targetSize <= 0) return {};
    if (source.size() == 1) return QVector<double>(targetSize, source.constFirst());

    QVector<double> result(targetSize);
    const double scale = static_cast<double>(source.size() - 1)
        / static_cast<double>(std::max(1, targetSize - 1));
    for (int index = 0; index < targetSize; ++index) {
        const double position = static_cast<double>(index) * scale;
        const int lastSourceIndex = static_cast<int>(source.size() - 1);
        const int lower = std::min(static_cast<int>(std::floor(position)), lastSourceIndex);
        const int upper = std::min(lower + 1, lastSourceIndex);
        const double fraction = position - static_cast<double>(lower);
        result[index] = source.at(lower) * (1.0 - fraction) + source.at(upper) * fraction;
    }
    return result;
}

bool detectCompleteSawtoothCycle(const QVector<double> &window,
                                 DetectedSawtoothCycle *detected)
{
    if (detected == nullptr || window.size() < 40) return false;

    const int sampleCount = window.size();
    QVector<double> smoothed(sampleCount);
    constexpr int smoothingRadius = 2;
    for (int index = 0; index < sampleCount; ++index) {
        const int begin = std::max(0, index - smoothingRadius);
        const int end = std::min(sampleCount - 1, index + smoothingRadius);
        double sum = 0.0;
        for (int sample = begin; sample <= end; ++sample) sum += window.at(sample);
        smoothed[index] = sum / static_cast<double>(end - begin + 1);
    }

    const auto valueRange = std::minmax_element(smoothed.cbegin(), smoothed.cend());
    const double signalRange = *valueRange.second - *valueRange.first;
    if (!(signalRange > 1e-6)) return false;

    QVector<double> drops(sampleCount - 1);
    for (int index = 0; index < drops.size(); ++index) {
        drops[index] = smoothed.at(index) - smoothed.at(index + 1);
    }
    const double typicalDrop = median(drops);
    QVector<double> deviations(drops.size());
    for (int index = 0; index < drops.size(); ++index) {
        deviations[index] = std::abs(drops.at(index) - typicalDrop);
    }
    const double mad = median(deviations);
    const double threshold = std::max(signalRange * 0.02,
                                      std::max(0.02, typicalDrop + 6.0 * mad));

    QVector<int> edgeCandidates;
    for (int index = 0; index < drops.size(); ++index) {
        if (drops.at(index) > threshold) edgeCandidates.append(index);
    }
    if (edgeCandidates.isEmpty()) return false;

    QVector<int> edges;
    int clusterBest = edgeCandidates.constFirst();
    for (int index = 1; index <= edgeCandidates.size(); ++index) {
        const bool endOfCluster = index == edgeCandidates.size()
            || edgeCandidates.at(index) - edgeCandidates.at(index - 1) > 3;
        if (endOfCluster) {
            edges.append(clusterBest + 1);
            if (index < edgeCandidates.size()) clusterBest = edgeCandidates.at(index);
        } else if (drops.at(edgeCandidates.at(index)) > drops.at(clusterBest)) {
            clusterBest = edgeCandidates.at(index);
        }
    }
    if (edges.size() < 2) return false;

    // NI-DAQmx validates the sample rate to be at least 10x the output
    // frequency, so a valid period can be short even when the display window
    // is large.  Keep the lower bound small enough for that case.
    const int minimumPeriod = std::max(8, sampleCount / 100);
    int selectedStart = -1;
    int selectedPeriod = 0;
    double selectedScore = std::numeric_limits<double>::max();
    for (int index = 0; index + 1 < edges.size(); ++index) {
        const int start = edges.at(index);
        const int period = edges.at(index + 1) - start;
        if (period < minimumPeriod || start < 0 || start + period > sampleCount) continue;

        // Prefer a cycle whose rising portion is predominantly monotonic.
        int nonRisingSamples = 0;
        for (int sample = start + 1; sample < start + period; ++sample) {
            if (smoothed.at(sample) + signalRange * 0.01 < smoothed.at(sample - 1)) {
                ++nonRisingSamples;
            }
        }
        const double score = static_cast<double>(nonRisingSamples)
            / static_cast<double>(period);
        if (score < selectedScore) {
            selectedScore = score;
            selectedStart = start;
            selectedPeriod = period;
        }
    }
    if (selectedStart < 0 || selectedPeriod <= 0) return false;

    detected->offset = selectedStart;
    detected->samples = window.mid(selectedStart, selectedPeriod);
    return !detected->samples.isEmpty();
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_acquisition(new tdlas::DaqmxAcquisition(this))
{
    buildUi();
    connectSignals();
    refreshDaqDevices();
    loadAcquisitionSettings();
    setAcquisitionUiState(false);
}

MainWindow::~MainWindow()
{
    stopAcquisition();
    m_acquisition->wait();
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("TDLAS 测氢 v1.4"));
    resize(1280, 820);
    setMinimumSize(980, 640);

    auto *centralWidget = new QWidget(this);
    auto *mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(16);

    auto *controlPanel = new QWidget(centralWidget);
    controlPanel->setFixedWidth(380);
    auto *controlLayout = new QVBoxLayout(controlPanel);
    controlLayout->setContentsMargins(0, 0, 0, 0);
    controlLayout->setSpacing(6);

    auto *configurationGroup = new QGroupBox(QStringLiteral("参数设置"), controlPanel);
    auto *configurationLayout = new QFormLayout(configurationGroup);
    configurationLayout->setVerticalSpacing(4);
    configurationLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    configurationLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto *deviceRow = new QWidget(configurationGroup);
    auto *deviceLayout = new QHBoxLayout(deviceRow);
    deviceLayout->setContentsMargins(0, 0, 0, 0);
    deviceLayout->setSpacing(6);
    m_deviceCombo = new QComboBox(deviceRow);
    m_deviceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_refreshDevicesButton = new QPushButton(QStringLiteral("刷新"), deviceRow);
    m_refreshDevicesButton->setToolTip(QStringLiteral("重新读取 NI MAX 当前识别到的设备和物理通道"));
    deviceLayout->addWidget(m_deviceCombo);
    deviceLayout->addWidget(m_refreshDevicesButton);

    m_outputChannelCombo = new QComboBox(configurationGroup);
    m_outputChannelCombo->setPlaceholderText(QStringLiteral("未配置输出通道"));

    m_inputChannelCombo = new QComboBox(configurationGroup);
    m_inputChannelCombo->setPlaceholderText(QStringLiteral("未发现 AI 输入通道"));

    m_waveformCombo = new QComboBox(configurationGroup);
    m_waveformCombo->addItem(QStringLiteral("正弦波"),
                             static_cast<int>(tdlas::OutputWaveform::Sine));
    m_waveformCombo->addItem(QStringLiteral("方波"),
                             static_cast<int>(tdlas::OutputWaveform::Square));
    m_waveformCombo->addItem(QStringLiteral("锯齿波"),
                             static_cast<int>(tdlas::OutputWaveform::Sawtooth));
    m_waveformCombo->addItem(QStringLiteral("三角波"),
                             static_cast<int>(tdlas::OutputWaveform::Triangle));

    m_outputFrequencySpin = new QDoubleSpinBox(configurationGroup);
    m_outputFrequencySpin->setRange(0.1, 100000.0);
    m_outputFrequencySpin->setDecimals(2);
    m_outputFrequencySpin->setSingleStep(100.0);
    m_outputFrequencySpin->setValue(1000.0);
    m_outputFrequencySpin->setSuffix(QStringLiteral(" Hz"));

    m_outputAmplitudeSpin = new QDoubleSpinBox(configurationGroup);
    m_outputAmplitudeSpin->setRange(0.0, 10.0);
    m_outputAmplitudeSpin->setDecimals(3);
    m_outputAmplitudeSpin->setSingleStep(0.1);
    m_outputAmplitudeSpin->setValue(0.1);
    m_outputAmplitudeSpin->setSuffix(QStringLiteral(" V"));

    m_outputOffsetSpin = new QDoubleSpinBox(configurationGroup);
    m_outputOffsetSpin->setRange(-10.0, 10.0);
    m_outputOffsetSpin->setDecimals(3);
    m_outputOffsetSpin->setSingleStep(0.1);
    m_outputOffsetSpin->setValue(0.0);
    m_outputOffsetSpin->setSuffix(QStringLiteral(" V"));

    m_sampleRateSpin = new QDoubleSpinBox(configurationGroup);
    m_sampleRateSpin->setRange(1.0, 2000000.0);
    m_sampleRateSpin->setDecimals(0);
    m_sampleRateSpin->setSingleStep(1000.0);
    m_sampleRateSpin->setValue(10000.0);
    m_sampleRateSpin->setSuffix(QStringLiteral(" S/s"));

    m_samplesPerCycleSpin = new QSpinBox(configurationGroup);
    m_samplesPerCycleSpin->setRange(1, 1000000);
    m_samplesPerCycleSpin->setSingleStep(100);
    m_samplesPerCycleSpin->setValue(1000);
    m_samplesPerCycleSpin->setSuffix(QStringLiteral(" 点"));
    m_samplesPerCycleSpin->setToolTip(
        QStringLiteral("原始波形显示窗口点数；锯齿波模式会在窗口内自动提取完整周期进行拟合"));

    m_minimumValueSpin = new QDoubleSpinBox(configurationGroup);
    m_minimumValueSpin->setRange(-10.0, 10.0);
    m_minimumValueSpin->setDecimals(2);
    m_minimumValueSpin->setValue(-10.0);
    m_minimumValueSpin->setSuffix(QStringLiteral(" V"));

    m_maximumValueSpin = new QDoubleSpinBox(configurationGroup);
    m_maximumValueSpin->setRange(-10.0, 10.0);
    m_maximumValueSpin->setDecimals(2);
    m_maximumValueSpin->setValue(10.0);
    m_maximumValueSpin->setSuffix(QStringLiteral(" V"));

    configurationLayout->addRow(QStringLiteral("采集卡"), deviceRow);
    configurationLayout->addRow(QStringLiteral("输出通道"), m_outputChannelCombo);
    configurationLayout->addRow(QStringLiteral("输出波形"), m_waveformCombo);
    configurationLayout->addRow(QStringLiteral("波形频率"), m_outputFrequencySpin);
    configurationLayout->addRow(QStringLiteral("波形幅值"), m_outputAmplitudeSpin);
    configurationLayout->addRow(QStringLiteral("波形偏移量"), m_outputOffsetSpin);
    configurationLayout->addRow(QStringLiteral("采集通道"), m_inputChannelCombo);
    configurationLayout->addRow(QStringLiteral("采样率"), m_sampleRateSpin);
    configurationLayout->addRow(QStringLiteral("显示窗口读取点数"), m_samplesPerCycleSpin);
    configurationLayout->addRow(QStringLiteral("量程下限"), m_minimumValueSpin);
    configurationLayout->addRow(QStringLiteral("量程上限"), m_maximumValueSpin);

    auto *commandGroup = new QGroupBox(QStringLiteral("采集控制"), controlPanel);
    auto *commandLayout = new QHBoxLayout(commandGroup);
    m_startButton = new QPushButton(QStringLiteral("开始采集"), commandGroup);
    m_stopButton = new QPushButton(QStringLiteral("停止采集"), commandGroup);
    m_saveSettingsButton = new QPushButton(QStringLiteral("保存参数"), commandGroup);
    m_startButton->setDefault(true);
    commandLayout->addWidget(m_startButton);
    commandLayout->addWidget(m_stopButton);
    commandLayout->addWidget(m_saveSettingsButton);

    auto *monitorGroup = new QGroupBox(QStringLiteral("数据监测"), controlPanel);
    auto *monitorLayout = new QFormLayout(monitorGroup);
    monitorLayout->setVerticalSpacing(3);
    m_statusIndicator = new QLabel(monitorGroup);
    m_sampleCountValue = new QLabel(QStringLiteral("0"), monitorGroup);
    m_latestValue = new QLabel(QStringLiteral("-- V"), monitorGroup);
    m_rangeValue = new QLabel(QStringLiteral("-- V"), monitorGroup);
    m_algorithmValue = new QLabel(QStringLiteral("等待 100000 点"), monitorGroup);
    m_peakValue = new QLabel(QStringLiteral("--"), monitorGroup);
    m_lightValue = new QLabel(QStringLiteral("--"), monitorGroup);
    // These labels still carry acquisition status internally, but are not
    // part of the compact, user-visible data monitor.
    m_statusIndicator->hide();
    m_sampleCountValue->hide();
    m_latestValue->hide();
    m_rangeValue->hide();
    m_algorithmValue->hide();
    // Keep the monitoring panel focused on the two requested algorithm
    // indicators.  The other labels remain available to the acquisition
    // logic but are intentionally not placed in the visible layout.
    monitorLayout->addRow(QStringLiteral("峰值吸光度"), m_peakValue);
    monitorLayout->addRow(QStringLiteral("光强百分比"), m_lightValue);

    auto *fv1Group = new QGroupBox(QStringLiteral("fv1 曲线"), controlPanel);
    auto *fv1Layout = new QVBoxLayout(fv1Group);
    // Let the preview fill only the interior rectangle, leaving a clear gap
    // below the title and around the group-box frame.
    fv1Layout->setContentsMargins(6, 6, 6, 6);
    auto *fv1Chart = new QChart();
    fv1Chart->setAnimationOptions(QChart::NoAnimation);
    fv1Chart->legend()->hide();
    fv1Chart->setMargins(QMargins(2, 2, 2, 2));
    m_fv1Series = new QLineSeries(fv1Chart);
    m_fv1Series->setColor(QColor(QStringLiteral("#5267a8")));
    fv1Chart->addSeries(m_fv1Series);
    m_fv1AxisX = new QValueAxis(fv1Chart);
    m_fv1AxisX->setTitleText(QStringLiteral("v_f"));
    m_fv1AxisX->setRange(-0.02, 0.02);
    m_fv1AxisX->setTickCount(4);
    m_fv1AxisY = new QValueAxis(fv1Chart);
    m_fv1AxisY->setTitleText(QStringLiteral("fv1"));
    m_fv1AxisY->setRange(0.0, 0.1);
    m_fv1AxisY->setTickCount(4);
    fv1Chart->addAxis(m_fv1AxisX, Qt::AlignBottom);
    fv1Chart->addAxis(m_fv1AxisY, Qt::AlignLeft);
    m_fv1Series->attachAxis(m_fv1AxisX);
    m_fv1Series->attachAxis(m_fv1AxisY);
    m_fv1ChartView = new QChartView(fv1Chart, fv1Group);
    m_fv1ChartView->setRenderHint(QPainter::Antialiasing);
    m_fv1ChartView->setMinimumSize(0, 0);
    m_fv1ChartView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    fv1Layout->addWidget(m_fv1ChartView);

    controlLayout->addWidget(configurationGroup);
    controlLayout->addWidget(commandGroup);
    controlLayout->addWidget(monitorGroup);
    controlLayout->addWidget(fv1Group, 1);
    controlLayout->addStretch();

    auto *displayPanel = new QWidget(centralWidget);
    auto *displayLayout = new QVBoxLayout(displayPanel);
    displayLayout->setContentsMargins(0, 0, 0, 0);
    displayLayout->setSpacing(12);

    auto createChartGroup = [displayPanel](const QString &title,
                                            QChartView **view,
                                            QLineSeries **series,
                                            QValueAxis **axisX,
                                            QValueAxis **axisY) {
        auto *group = new QGroupBox(title, displayPanel);
        auto *layout = new QVBoxLayout(group);
        auto *chart = new QChart();
        chart->setAnimationOptions(QChart::NoAnimation);
        chart->legend()->hide();
        chart->setMargins(QMargins(4, 4, 4, 4));

        *series = new QLineSeries(chart);
        chart->addSeries(*series);
        *axisX = new QValueAxis(chart);
        *axisY = new QValueAxis(chart);
        (*axisX)->setTitleText(QStringLiteral("时间 (s)"));
        (*axisY)->setTitleText(QStringLiteral("电压 (V)"));
        (*axisX)->setRange(0.0, 1.0);
        (*axisY)->setRange(-10.0, 10.0);
        chart->addAxis(*axisX, Qt::AlignBottom);
        chart->addAxis(*axisY, Qt::AlignLeft);
        (*series)->attachAxis(*axisX);
        (*series)->attachAxis(*axisY);

        *view = new QChartView(chart, group);
        (*view)->setRenderHint(QPainter::Antialiasing);
        (*view)->setMinimumHeight(240);
        layout->addWidget(*view);
        return group;
    };

    displayLayout->addWidget(createChartGroup(
        QStringLiteral("原始采集波形"), &m_rawChartView, &m_rawSeries,
        &m_rawAxisX, &m_rawAxisY), 1);
    displayLayout->addWidget(createChartGroup(
        QStringLiteral("吸光度与 Voigt 拟合"), &m_processedChartView, &m_processedSeries,
        &m_processedAxisX, &m_processedAxisY), 1);

    m_processedSeries->setName(QStringLiteral("吸光度"));
    m_processedSeries->setColor(QColor(QStringLiteral("#16794a")));
    m_fittedSeries = new QLineSeries(m_processedChartView->chart());
    m_fittedSeries->setName(QStringLiteral("Voigt 拟合"));
    m_fittedSeries->setColor(QColor(QStringLiteral("#d1495b")));
    m_processedChartView->chart()->addSeries(m_fittedSeries);
    m_fittedSeries->attachAxis(m_processedAxisX);
    m_fittedSeries->attachAxis(m_processedAxisY);
    m_processedChartView->chart()->legend()->setVisible(true);

    mainLayout->addWidget(controlPanel);
    mainLayout->addWidget(displayPanel, 1);
    setCentralWidget(centralWidget);
    statusBar()->showMessage(QStringLiteral("系统就绪"));
}

void MainWindow::connectSignals()
{
    connect(m_refreshDevicesButton, &QPushButton::clicked,
            this, &MainWindow::refreshDaqDevices);
    connect(m_deviceCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::handleDeviceSelectionChanged);
    connect(m_startButton, &QPushButton::clicked,
            this, &MainWindow::startAcquisition);
    connect(m_stopButton, &QPushButton::clicked,
            this, &MainWindow::stopAcquisition);
    connect(m_saveSettingsButton, &QPushButton::clicked,
            this, &MainWindow::saveAcquisitionSettings);
    connect(m_acquisition, &tdlas::DaqmxAcquisition::acquisitionStarted,
            this, &MainWindow::handleAcquisitionStarted);
    connect(m_acquisition, &tdlas::DaqmxAcquisition::acquisitionStopped,
            this, &MainWindow::handleAcquisitionStopped);
    connect(m_acquisition, &tdlas::DaqmxAcquisition::samplesReady,
            this, &MainWindow::handleSamples);
    connect(m_acquisition, &tdlas::DaqmxAcquisition::errorOccurred,
            this, &MainWindow::handleAcquisitionError);
}

void MainWindow::refreshDaqDevices()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("acquisition"));
    const QString currentDevice = m_deviceCombo->currentData().toString();
    const bool hasCurrentDevice = !currentDevice.isEmpty();
    const QString selectedDevice = hasCurrentDevice
        ? currentDevice : settings.value(QStringLiteral("device")).toString();
    const QString selectedInput = hasCurrentDevice
        ? m_inputChannelCombo->currentData().toString()
        : settings.value(QStringLiteral("inputChannel")).toString();
    const QString selectedOutput = hasCurrentDevice
        ? m_outputChannelCombo->currentData().toString()
        : settings.value(QStringLiteral("outputChannel")).toString();
    const bool hadOutputSelection = hasCurrentDevice;
    const bool hasSavedOutputSelection = settings.contains(QStringLiteral("outputChannel"));
    settings.endGroup();
    QString errorMessage;
    m_devices = tdlas::DaqmxAcquisition::enumerateDevices(&errorMessage);

    {
        const QSignalBlocker blocker(m_deviceCombo);
        m_deviceCombo->clear();
        for (const tdlas::DaqmxDeviceInfo &device : m_devices) {
            const QString label = device.productType.isEmpty()
                                      ? device.name
                                      : QStringLiteral("%1  (%2)")
                                            .arg(device.name, device.productType);
            m_deviceCombo->addItem(label, device.name);
        }

        int selectedIndex = m_deviceCombo->findData(selectedDevice);
        if (selectedIndex < 0 && m_deviceCombo->count() > 0) {
            selectedIndex = 0;
        }
        if (selectedIndex >= 0) {
            m_deviceCombo->setCurrentIndex(selectedIndex);
        }
    }

    if (m_deviceCombo->count() == 0) {
        m_deviceCombo->addItem(QStringLiteral("未检测到 NI-DAQmx 设备"));
        m_outputChannelCombo->clear();
        m_outputChannelCombo->addItem(QStringLiteral("无可用 AO 输出通道"));
        m_inputChannelCombo->clear();
        m_inputChannelCombo->addItem(QStringLiteral("无可用 AI 输入通道"));
        statusBar()->showMessage(errorMessage.isEmpty()
                                     ? QStringLiteral("未检测到可用采集卡")
                                     : errorMessage);
        return;
    }

    handleDeviceSelectionChanged(m_deviceCombo->currentIndex());

    const int deviceIndex = m_deviceCombo->currentIndex();
    if (deviceIndex >= 0 && deviceIndex < m_devices.size()) {
        const auto &device = m_devices.at(deviceIndex);
        const int inputIndex = m_inputChannelCombo->findData(selectedInput);
        if (inputIndex >= 0) {
            m_inputChannelCombo->setCurrentIndex(inputIndex);
        }
        const int outputIndex = m_outputChannelCombo->findData(selectedOutput);
        if ((hadOutputSelection || hasSavedOutputSelection) && outputIndex >= 0) {
            m_outputChannelCombo->setCurrentIndex(outputIndex);
        }
        statusBar()->showMessage(
            QStringLiteral("已识别 %1：AI %2 路，AO %3 路")
                .arg(device.productType.isEmpty() ? device.name : device.productType)
                .arg(device.analogInputChannels.size())
                .arg(device.analogOutputChannels.size()));
    }
}

void MainWindow::loadAcquisitionSettings()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("acquisition"));
    const QString device = settings.value(QStringLiteral("device")).toString();
    const int deviceIndex = m_deviceCombo->findData(device);
    if (deviceIndex >= 0) {
        m_deviceCombo->setCurrentIndex(deviceIndex);
        populateChannelsForDevice(deviceIndex);
    }

    const QString outputChannel = settings.value(QStringLiteral("outputChannel")).toString();
    const int outputIndex = m_outputChannelCombo->findData(outputChannel);
    if (outputIndex >= 0) m_outputChannelCombo->setCurrentIndex(outputIndex);

    const QString inputChannel = settings.value(QStringLiteral("inputChannel")).toString();
    const int inputIndex = m_inputChannelCombo->findData(inputChannel);
    if (inputIndex >= 0) m_inputChannelCombo->setCurrentIndex(inputIndex);

    const int waveformIndex = m_waveformCombo->findData(
        settings.value(QStringLiteral("waveform"), m_waveformCombo->currentData()).toInt());
    if (waveformIndex >= 0) m_waveformCombo->setCurrentIndex(waveformIndex);
    m_outputFrequencySpin->setValue(settings.value(QStringLiteral("outputFrequency"),
                                                    m_outputFrequencySpin->value()).toDouble());
    m_outputAmplitudeSpin->setValue(settings.value(QStringLiteral("outputAmplitude"),
                                                    m_outputAmplitudeSpin->value()).toDouble());
    m_outputOffsetSpin->setValue(settings.value(QStringLiteral("outputOffset"),
                                                 m_outputOffsetSpin->value()).toDouble());
    m_sampleRateSpin->setValue(settings.value(QStringLiteral("sampleRate"),
                                              m_sampleRateSpin->value()).toDouble());
    m_samplesPerCycleSpin->setValue(settings.value(QStringLiteral("samplesPerCycle"),
                                                    m_samplesPerCycleSpin->value()).toInt());
    m_minimumValueSpin->setValue(settings.value(QStringLiteral("minimumValue"),
                                                 m_minimumValueSpin->value()).toDouble());
    m_maximumValueSpin->setValue(settings.value(QStringLiteral("maximumValue"),
                                                 m_maximumValueSpin->value()).toDouble());
    settings.endGroup();
}

void MainWindow::saveAcquisitionSettings()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("acquisition"));
    settings.setValue(QStringLiteral("device"), m_deviceCombo->currentData());
    settings.setValue(QStringLiteral("outputChannel"), m_outputChannelCombo->currentData());
    settings.setValue(QStringLiteral("inputChannel"), m_inputChannelCombo->currentData());
    settings.setValue(QStringLiteral("waveform"), m_waveformCombo->currentData());
    settings.setValue(QStringLiteral("outputFrequency"), m_outputFrequencySpin->value());
    settings.setValue(QStringLiteral("outputAmplitude"), m_outputAmplitudeSpin->value());
    settings.setValue(QStringLiteral("outputOffset"), m_outputOffsetSpin->value());
    settings.setValue(QStringLiteral("sampleRate"), m_sampleRateSpin->value());
    settings.setValue(QStringLiteral("samplesPerCycle"), m_samplesPerCycleSpin->value());
    settings.setValue(QStringLiteral("minimumValue"), m_minimumValueSpin->value());
    settings.setValue(QStringLiteral("maximumValue"), m_maximumValueSpin->value());
    settings.endGroup();
    settings.sync();
    if (settings.status() == QSettings::NoError) {
        statusBar()->showMessage(QStringLiteral("参数已保存，下次启动将自动恢复"), 4000);
    } else {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("无法保存参数，请检查用户配置目录的写入权限。"));
    }
}

void MainWindow::handleDeviceSelectionChanged(int index)
{
    populateChannelsForDevice(index);
}

void MainWindow::populateChannelsForDevice(int index)
{
    const QSignalBlocker outputBlocker(m_outputChannelCombo);
    const QSignalBlocker inputBlocker(m_inputChannelCombo);
    m_outputChannelCombo->clear();
    m_inputChannelCombo->clear();

    if (index < 0 || index >= m_devices.size()) {
        m_outputChannelCombo->addItem(QStringLiteral("无可用 AO 输出通道"));
        m_inputChannelCombo->addItem(QStringLiteral("无可用 AI 输入通道"));
        return;
    }

    const tdlas::DaqmxDeviceInfo &device = m_devices.at(index);
    for (const QString &channel : device.analogOutputChannels) {
        m_outputChannelCombo->addItem(channel, channel);
    }
    m_outputChannelCombo->addItem(QStringLiteral("不使用输出通道"), QString());
    m_outputChannelCombo->setCurrentIndex(0);
    for (const QString &channel : device.analogInputChannels) {
        m_inputChannelCombo->addItem(channel, channel);
    }

    if (m_inputChannelCombo->count() > 0) {
        m_inputChannelCombo->setCurrentIndex(0);
    }
}

void MainWindow::startAcquisition()
{
    if (m_acquisition->isRunning()) {
        return;
    }

    if (m_inputChannelCombo->currentData().toString().trimmed().isEmpty()) {
        handleAcquisitionError(QStringLiteral("请选择有效的采集通道。"));
        return;
    }

    tdlas::DaqmxConfig config;
    config.outputPhysicalChannel = m_outputChannelCombo->currentData().toString();
    config.inputPhysicalChannel = m_inputChannelCombo->currentData().toString();
    config.outputWaveform = static_cast<tdlas::OutputWaveform>(
        m_waveformCombo->currentData().toInt());
    config.outputFrequency = m_outputFrequencySpin->value();
    config.outputAmplitude = m_outputAmplitudeSpin->value();
    config.outputOffset = m_outputOffsetSpin->value();
    config.sampleRate = m_sampleRateSpin->value();
    config.samplesPerCycle = m_samplesPerCycleSpin->value();
    config.minimumValue = m_minimumValueSpin->value();
    config.maximumValue = m_maximumValueSpin->value();

    if (!m_acquisition->configure(config)) {
        return;
    }

    // Keep the processing window aligned with the cycle size selected for
    // acquisition.  With the processor's default 500-point cycle this is
    // 100,000 points (10 cycles x 20 spectra), preserving the old behavior.
    auto processorConfig = m_processor.config();
    processorConfig.samplesPerCycle = config.samplesPerCycle;
    processorConfig.windowSamples = processorConfig.samplesPerCycle
        * processorConfig.averagedCyclesPerSpectrum
        * processorConfig.averagedSpectra;
    m_processor.setConfig(processorConfig);
    m_sawtoothCycleDetectionEnabled =
        config.outputWaveform == tdlas::OutputWaveform::Sawtooth;
    m_detectedCycleSamples = 0;

    setAcquisitionUiState(true);
    setStatus(QStringLiteral("正在启动"), QStringLiteral("#b06a00"));
    statusBar()->showMessage(QStringLiteral("正在创建 NI-DAQmx 采集任务..."));
    m_acquisition->start();
}

void MainWindow::stopAcquisition()
{
    if (!m_acquisition->isRunning()) {
        return;
    }

    m_stopButton->setEnabled(false);
    setStatus(QStringLiteral("正在停止"), QStringLiteral("#b06a00"));
    statusBar()->showMessage(QStringLiteral("正在安全停止采集..."));
    m_acquisition->requestStop();
}

void MainWindow::handleAcquisitionStarted()
{
    m_processor.reset();
    m_rawDisplayBuffer.clear();
    m_detectedCycleSamples = 0;
    m_processedSeries->clear();
    m_fittedSeries->clear();
    m_fv1Series->clear();
    m_algorithmValue->setText(QStringLiteral("等待 %1 点")
                                   .arg(m_processor.config().windowSamples));
    m_peakValue->setText(QStringLiteral("--"));
    m_lightValue->setText(QStringLiteral("--"));
    setAcquisitionUiState(true);
    setStatus(QStringLiteral("采集中"), QStringLiteral("#16794a"));
    statusBar()->showMessage(
        QStringLiteral("正在采集 %1，采样率 %2 S/s%3")
            .arg(m_inputChannelCombo->currentText())
            .arg(m_sampleRateSpin->value(), 0, 'f', 0)
            .arg(m_sawtoothCycleDetectionEnabled
                     ? QStringLiteral("，已启用完整锯齿周期检测")
                     : QString()));
}

void MainWindow::handleAcquisitionStopped()
{
    setAcquisitionUiState(false);
    setStatus(QStringLiteral("已停止"), QStringLiteral("#60646c"));
    statusBar()->showMessage(QStringLiteral("采集已停止"));
}

void MainWindow::handleSamples(const QVector<double> &samples, double sampleRate)
{
    if (samples.isEmpty()) {
        return;
    }

    m_displaySampleRate = sampleRate;
    // The raw chart intentionally keeps the user's display window.  It can
    // contain two clipped periods; fitting is performed only on the complete
    // sawtooth period found between two steep falling edges in that window.
    m_rawDisplayBuffer += samples;
    const int displaySamples = m_samplesPerCycleSpin->value();
    if (displaySamples <= 0 || m_rawDisplayBuffer.size() < displaySamples) {
        statusBar()->showMessage(
            QStringLiteral("等待显示窗口：%1 / %2 点，%3 S/s")
                .arg(m_rawDisplayBuffer.size())
                .arg(displaySamples)
                .arg(sampleRate, 0, 'f', 0));
        return;
    }

    const int completeSamples = (m_rawDisplayBuffer.size() / displaySamples) * displaySamples;
    QVector<double> latestDisplayWindow;
    tdlas::TDLASResult latestResult;
    bool hasResult = false;
    int latestCycleOffset = 0;
    for (int offset = 0; offset < completeSamples; offset += displaySamples) {
        const QVector<double> displayWindow = m_rawDisplayBuffer.mid(offset, displaySamples);
        latestDisplayWindow = displayWindow;

        QVector<double> fittingCycle = displayWindow;
        int cycleOffset = 0;
        if (m_sawtoothCycleDetectionEnabled) {
            DetectedSawtoothCycle detected;
            if (!detectCompleteSawtoothCycle(displayWindow, &detected)) continue;
            cycleOffset = detected.offset;
            if (m_detectedCycleSamples <= 0) {
                m_detectedCycleSamples = detected.samples.size();
                auto processorConfig = m_processor.config();
                processorConfig.samplesPerCycle = m_detectedCycleSamples;
                processorConfig.windowSamples = m_detectedCycleSamples
                    * processorConfig.averagedCyclesPerSpectrum
                    * processorConfig.averagedSpectra;
                m_processor.setConfig(processorConfig);
            }
            fittingCycle = resampleCycle(detected.samples, m_detectedCycleSamples);
        }

        tdlas::TDLASResult result;
        if (m_processor.appendSamples(fittingCycle, &result)) {
            latestResult = result;
            latestCycleOffset = cycleOffset;
            hasResult = true;
        }
    }
    m_rawDisplayBuffer.remove(0, completeSamples);

    if (latestDisplayWindow.isEmpty()) return;

    QVector<QPointF> points;
    points.reserve(latestDisplayWindow.size());
    for (qsizetype index = 0; index < latestDisplayWindow.size(); ++index) {
        points.append(QPointF(static_cast<double>(index) / sampleRate,
                             latestDisplayWindow.at(index)));
    }
    m_rawSeries->replace(points);
    updateTimeAxes(static_cast<double>(displaySamples) / sampleRate);
    updateChartAxes(latestDisplayWindow);

    const auto range = std::minmax_element(latestDisplayWindow.cbegin(), latestDisplayWindow.cend());
    m_sampleCountValue->setText(QString::number(latestDisplayWindow.size()));
    m_latestValue->setText(QStringLiteral("%1 V").arg(latestDisplayWindow.constLast(), 0, 'f', 6));
    m_rangeValue->setText(
        QStringLiteral("%1 ～ %2 V")
            .arg(*range.first, 0, 'f', 6)
            .arg(*range.second, 0, 'f', 6));
    if (hasResult) {
        updateProcessedChart(latestResult, sampleRate, latestCycleOffset, displaySamples);
        updateFv1Chart(latestResult);
        m_algorithmValue->setText(QStringLiteral("完成：检测周期 %1 点")
                                       .arg(latestResult.cycleSamples));
        m_peakValue->setText(QStringLiteral("%1").arg(latestResult.peakAbsorbance, 0, 'f', 6));
        m_lightValue->setText(QStringLiteral("%1 %").arg(latestResult.lightPercent, 0, 'f', 2));
        statusBar()->showMessage(latestResult.message);
    } else {
        const QString prefix = m_sawtoothCycleDetectionEnabled
            ? QStringLiteral("等待完整锯齿周期")
            : QStringLiteral("等待算法窗口");
        m_algorithmValue->setText(QStringLiteral("%1：%2 / %3 点")
                                       .arg(prefix)
                                       .arg(m_processor.bufferedSampleCount())
                                       .arg(m_processor.config().windowSamples));
        statusBar()->showMessage(QStringLiteral("实时更新：显示窗口 %1 点，%2 S/s")
                                     .arg(latestDisplayWindow.size())
                                     .arg(sampleRate, 0, 'f', 0));
    }
}

void MainWindow::updateChartAxes(const QVector<double> &samples)
{
    const auto range = std::minmax_element(samples.cbegin(), samples.cend());
    double minimum = *range.first;
    double maximum = *range.second;

    if (minimum == maximum) {
        const double margin = std::max(0.001, std::abs(minimum) * 0.05);
        minimum -= margin;
        maximum += margin;
    } else {
        const double margin = (maximum - minimum) * 0.05;
        minimum -= margin;
        maximum += margin;
    }

    m_rawAxisY->setRange(minimum, maximum);
}

void MainWindow::updateProcessedChart(const tdlas::TDLASResult &result,
                                      double sampleRate,
                                      int displayOffsetSamples,
                                      int displayWindowSamples)
{
    QVector<QPointF> absorbancePoints;
    QVector<QPointF> fittedPoints;
    absorbancePoints.reserve(result.voltage.size());
    fittedPoints.reserve(result.fittedVoltage.size());
    const QVector<double> &processedVoltage = result.voltage.isEmpty()
        ? result.absorbance : result.voltage;
    const QVector<double> &processedFit = result.fittedVoltage.isEmpty()
        ? result.fittedAbsorbance : result.fittedVoltage;
    for (qsizetype index = 0; index < result.frequencyCm1.size(); ++index) {
        const double time = static_cast<double>(displayOffsetSamples
                                                + result.sampleOffset + index) / sampleRate;
        absorbancePoints.append(QPointF(time, processedVoltage.at(index)));
        fittedPoints.append(QPointF(time, processedFit.at(index)));
    }
    m_processedSeries->replace(absorbancePoints);
    m_fittedSeries->replace(fittedPoints);
    if (!result.frequencyCm1.isEmpty()) {
        const auto yRange = std::minmax_element(processedVoltage.cbegin(),
                                                processedVoltage.cend());
        const auto fitRange = std::minmax_element(processedFit.cbegin(),
                                                  processedFit.cend());
        updateTimeAxes(static_cast<double>(displayWindowSamples) / sampleRate);
        m_processedAxisY->setRange(std::min(*yRange.first, *fitRange.first),
                                    std::max(*yRange.second, *fitRange.second));
    }
}

void MainWindow::updateTimeAxes(double cycleDurationSeconds)
{
    const double duration = std::max(cycleDurationSeconds, 1.0 / std::max(m_displaySampleRate, 1.0));
    m_rawAxisX->setRange(0.0, duration);
    m_processedAxisX->setRange(0.0, duration);
    m_rawAxisX->setTickCount(6);
    m_processedAxisX->setTickCount(6);
}

void MainWindow::updateFv1Chart(const tdlas::TDLASResult &result)
{
    const qsizetype pointCount = std::min(result.frequencyCm1.size(),
                                          result.fittedAbsorbance.size());
    if (pointCount <= 0) {
        m_fv1Series->clear();
        return;
    }

    QVector<QPointF> points;
    points.reserve(pointCount);
    for (qsizetype index = 0; index < pointCount; ++index) {
        points.append(QPointF(result.frequencyCm1.at(index),
                              result.fittedAbsorbance.at(index)));
    }
    m_fv1Series->replace(points);

    const auto frequencyRange = std::minmax_element(result.frequencyCm1.cbegin(),
                                                     result.frequencyCm1.cbegin() + pointCount);
    const auto fv1Range = std::minmax_element(result.fittedAbsorbance.cbegin(),
                                              result.fittedAbsorbance.cbegin() + pointCount);
    double xMinimum = *frequencyRange.first;
    double xMaximum = *frequencyRange.second;
    double yMinimum = *fv1Range.first;
    double yMaximum = *fv1Range.second;
    const double xPadding = xMaximum > xMinimum
        ? (xMaximum - xMinimum) * 0.05 : std::max(1e-6, std::abs(xMinimum) * 0.05);
    const double yPadding = yMaximum > yMinimum
        ? (yMaximum - yMinimum) * 0.08 : std::max(1e-6, std::abs(yMinimum) * 0.08);
    m_fv1AxisX->setRange(xMinimum - xPadding, xMaximum + xPadding);
    m_fv1AxisY->setRange(yMinimum - yPadding, yMaximum + yPadding);
}

void MainWindow::handleAcquisitionError(const QString &message)
{
    statusBar()->showMessage(message);
    QMessageBox::warning(this, QStringLiteral("NI-DAQmx 采集错误"), message);
}

void MainWindow::setAcquisitionUiState(bool running)
{
    m_deviceCombo->setEnabled(!running);
    m_outputChannelCombo->setEnabled(!running);
    m_inputChannelCombo->setEnabled(!running);
    m_waveformCombo->setEnabled(!running);
    m_refreshDevicesButton->setEnabled(!running);
    m_saveSettingsButton->setEnabled(!running);
    m_outputFrequencySpin->setEnabled(!running);
    m_outputAmplitudeSpin->setEnabled(!running);
    m_outputOffsetSpin->setEnabled(!running);
    m_sampleRateSpin->setEnabled(!running);
    m_samplesPerCycleSpin->setEnabled(!running);
    m_minimumValueSpin->setEnabled(!running);
    m_maximumValueSpin->setEnabled(!running);
    m_startButton->setEnabled(!running);
    m_stopButton->setEnabled(running);

    if (!running && !m_acquisition->isRunning()) {
        setStatus(QStringLiteral("就绪"), QStringLiteral("#60646c"));
    }
}

void MainWindow::setStatus(const QString &text, const QString &color)
{
    m_statusIndicator->setText(text);
    m_statusIndicator->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-weight: 600; }").arg(color));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_acquisition->isRunning()) {
        m_acquisition->requestStop();
        m_acquisition->wait();
    }
    event->accept();
}
