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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_acquisition(new tdlas::DaqmxAcquisition(this))
{
    buildUi();
    connectSignals();
    refreshDaqDevices();
    setAcquisitionUiState(false);
}

MainWindow::~MainWindow()
{
    stopAcquisition();
    m_acquisition->wait();
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("TDLAS 测氢"));
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
    controlLayout->setSpacing(12);

    auto *configurationGroup = new QGroupBox(QStringLiteral("参数设置"), controlPanel);
    auto *configurationLayout = new QFormLayout(configurationGroup);
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

    m_samplesPerReadSpin = new QSpinBox(configurationGroup);
    m_samplesPerReadSpin->setRange(1, 1000000);
    m_samplesPerReadSpin->setSingleStep(100);
    m_samplesPerReadSpin->setValue(1000);
    m_samplesPerReadSpin->setSuffix(QStringLiteral(" 点"));

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
    configurationLayout->addRow(QStringLiteral("每次读取"), m_samplesPerReadSpin);
    configurationLayout->addRow(QStringLiteral("量程下限"), m_minimumValueSpin);
    configurationLayout->addRow(QStringLiteral("量程上限"), m_maximumValueSpin);

    auto *commandGroup = new QGroupBox(QStringLiteral("采集控制"), controlPanel);
    auto *commandLayout = new QHBoxLayout(commandGroup);
    m_startButton = new QPushButton(QStringLiteral("开始采集"), commandGroup);
    m_stopButton = new QPushButton(QStringLiteral("停止采集"), commandGroup);
    m_startButton->setDefault(true);
    commandLayout->addWidget(m_startButton);
    commandLayout->addWidget(m_stopButton);

    auto *monitorGroup = new QGroupBox(QStringLiteral("数据监测"), controlPanel);
    auto *monitorLayout = new QFormLayout(monitorGroup);
    m_statusIndicator = new QLabel(monitorGroup);
    m_sampleCountValue = new QLabel(QStringLiteral("0"), monitorGroup);
    m_latestValue = new QLabel(QStringLiteral("-- V"), monitorGroup);
    m_rangeValue = new QLabel(QStringLiteral("-- V"), monitorGroup);
    m_algorithmValue = new QLabel(QStringLiteral("等待 100000 点"), monitorGroup);
    m_peakValue = new QLabel(QStringLiteral("--"), monitorGroup);
    m_lightValue = new QLabel(QStringLiteral("--"), monitorGroup);
    monitorLayout->addRow(QStringLiteral("采集状态"), m_statusIndicator);
    monitorLayout->addRow(QStringLiteral("本批样本"), m_sampleCountValue);
    monitorLayout->addRow(QStringLiteral("最新电压"), m_latestValue);
    monitorLayout->addRow(QStringLiteral("电压范围"), m_rangeValue);
    monitorLayout->addRow(QStringLiteral("算法缓存"), m_algorithmValue);
    monitorLayout->addRow(QStringLiteral("峰值吸光度"), m_peakValue);
    monitorLayout->addRow(QStringLiteral("光强百分比"), m_lightValue);

    controlLayout->addWidget(configurationGroup);
    controlLayout->addWidget(commandGroup);
    controlLayout->addWidget(monitorGroup);
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
        const bool processed = title.contains(QStringLiteral("吸光度"));
        (*axisX)->setTitleText(processed ? QStringLiteral("频率偏移 (cm^-1)")
                                         : QStringLiteral("采样点"));
        (*axisY)->setTitleText(processed ? QStringLiteral("吸光度")
                                         : QStringLiteral("电压 (V)"));
        (*axisX)->setRange(0.0, 1000.0);
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
        &m_rawAxisX, &m_rawAxisY));
    displayLayout->addWidget(createChartGroup(
        QStringLiteral("吸光度与 Voigt 拟合"), &m_processedChartView, &m_processedSeries,
        &m_processedAxisX, &m_processedAxisY));
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
    const QString selectedDevice = m_deviceCombo->currentData().toString();
    const QString selectedInput = m_inputChannelCombo->currentData().toString();
    const QString selectedOutput = m_outputChannelCombo->currentData().toString();
    const bool hadOutputSelection = m_outputChannelCombo->count() > 0;
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
        if (hadOutputSelection && outputIndex >= 0) {
            m_outputChannelCombo->setCurrentIndex(outputIndex);
        }
        statusBar()->showMessage(
            QStringLiteral("已识别 %1：AI %2 路，AO %3 路")
                .arg(device.productType.isEmpty() ? device.name : device.productType)
                .arg(device.analogInputChannels.size())
                .arg(device.analogOutputChannels.size()));
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
    config.samplesPerRead = m_samplesPerReadSpin->value();
    config.minimumValue = m_minimumValueSpin->value();
    config.maximumValue = m_maximumValueSpin->value();

    if (!m_acquisition->configure(config)) {
        return;
    }

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
    m_processedSeries->clear();
    m_fittedSeries->clear();
    m_algorithmValue->setText(QStringLiteral("等待 100000 点"));
    m_peakValue->setText(QStringLiteral("--"));
    m_lightValue->setText(QStringLiteral("--"));
    setAcquisitionUiState(true);
    setStatus(QStringLiteral("采集中"), QStringLiteral("#16794a"));
    statusBar()->showMessage(
        QStringLiteral("正在采集 %1，采样率 %2 S/s")
            .arg(m_inputChannelCombo->currentText())
            .arg(m_sampleRateSpin->value(), 0, 'f', 0));
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

    QVector<QPointF> points;
    points.reserve(samples.size());
    for (qsizetype index = 0; index < samples.size(); ++index) {
        points.append(QPointF(static_cast<double>(index), samples.at(index)));
    }
    m_rawSeries->replace(points);

    m_rawAxisX->setRange(0.0, std::max<qsizetype>(1, samples.size() - 1));
    updateChartAxes(samples);

    tdlas::TDLASResult result;
    if (m_processor.appendSamples(samples, &result)) {
        updateProcessedChart(result);
        m_algorithmValue->setText(QStringLiteral("完成"));
        m_peakValue->setText(QStringLiteral("%1").arg(result.peakAbsorbance, 0, 'f', 6));
        m_lightValue->setText(QStringLiteral("%1 %").arg(result.lightPercent, 0, 'f', 2));
        statusBar()->showMessage(result.message);
    } else {
        m_algorithmValue->setText(QStringLiteral("%1 / 100000 点")
                                       .arg(m_processor.bufferedSampleCount()));
    }

    const auto range = std::minmax_element(samples.cbegin(), samples.cend());
    m_sampleCountValue->setText(QString::number(samples.size()));
    m_latestValue->setText(QStringLiteral("%1 V").arg(samples.constLast(), 0, 'f', 6));
    m_rangeValue->setText(
        QStringLiteral("%1 ～ %2 V")
            .arg(*range.first, 0, 'f', 6)
            .arg(*range.second, 0, 'f', 6));
    statusBar()->showMessage(QStringLiteral("实时更新：%1 点，%2 S/s")
                                 .arg(samples.size())
                                 .arg(sampleRate, 0, 'f', 0));
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

void MainWindow::updateProcessedChart(const tdlas::TDLASResult &result)
{
    QVector<QPointF> absorbancePoints;
    QVector<QPointF> fittedPoints;
    absorbancePoints.reserve(result.frequencyCm1.size());
    fittedPoints.reserve(result.frequencyCm1.size());
    for (qsizetype index = 0; index < result.frequencyCm1.size(); ++index) {
        absorbancePoints.append(QPointF(result.frequencyCm1.at(index),
                                        result.absorbance.at(index)));
        fittedPoints.append(QPointF(result.frequencyCm1.at(index),
                                    result.fittedAbsorbance.at(index)));
    }
    m_processedSeries->replace(absorbancePoints);
    m_fittedSeries->replace(fittedPoints);
    if (!result.frequencyCm1.isEmpty()) {
        const auto xRange = std::minmax_element(result.frequencyCm1.cbegin(),
                                                result.frequencyCm1.cend());
        const auto yRange = std::minmax_element(result.absorbance.cbegin(),
                                                result.absorbance.cend());
        const auto fitRange = std::minmax_element(result.fittedAbsorbance.cbegin(),
                                                  result.fittedAbsorbance.cend());
        m_processedAxisX->setRange(*xRange.first, *xRange.second);
        m_processedAxisY->setRange(std::min(*yRange.first, *fitRange.first),
                                    std::max(*yRange.second, *fitRange.second));
    }
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
    m_outputFrequencySpin->setEnabled(!running);
    m_outputAmplitudeSpin->setEnabled(!running);
    m_outputOffsetSpin->setEnabled(!running);
    m_sampleRateSpin->setEnabled(!running);
    m_samplesPerReadSpin->setEnabled(!running);
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
