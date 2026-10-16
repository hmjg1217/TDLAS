#include "ui/main_window.h"

#include "daq/daqmx_acquisition.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QStandardPaths>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QDateTimeAxis>
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
    , m_processingWorker(new tdlas::TDLASProcessingWorker(this))
    , m_chartTimer(new QTimer(this))
    , m_peakWriter(new tdlas::PeakDataWriter(this))
{
    m_peakWriter->setObjectName(QStringLiteral("PeakDataWriter"));
    m_chartTimer->setInterval(50);
    m_chartTimer->setSingleShot(false);
    buildUi();
    connectSignals();
    refreshDaqDevices();
    loadAcquisitionSettings();
    setAcquisitionUiState(false);
}

MainWindow::~MainWindow()
{
    shutdownWorkers();
    restoreChartsToMainWindow();
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("TDLAS 测氢 v1.5"));
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
    m_algorithmValue = new QLabel(QStringLiteral("等待 20 个完整周期"), monitorGroup);
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
    m_displayPanel = displayPanel;
    m_displayLayout = new QVBoxLayout(displayPanel);
    m_displayLayout->setContentsMargins(0, 0, 0, 0);
    m_displayLayout->setSpacing(12);

    // Switch the right-hand chart area in place; keep all acquisition
    // controls (and the fv1 preview) in the main window.
    auto *chartControlLayout = new QHBoxLayout();
    chartControlLayout->setContentsMargins(0, 0, 0, 0);
    chartControlLayout->addStretch();
    m_peakDataButton = new QPushButton(QStringLiteral("峰值数据"), displayPanel);
    m_peakDataButton->setObjectName(QStringLiteral("peakDataButton"));
    m_peakDataButton->setCheckable(true);
    m_peakDataButton->setToolTip(QStringLiteral("在右侧图表区域切换峰值数据，不打开独立窗口"));
    chartControlLayout->addWidget(m_peakDataButton);
    m_toggleChartWindowButton = new QPushButton(QStringLiteral("切换到独立窗口"), displayPanel);
    m_toggleChartWindowButton->setToolTip(
        QStringLiteral("只将原始波形和吸光度拟合图切换到独立窗口"));
    chartControlLayout->addWidget(m_toggleChartWindowButton);
    m_displayLayout->addLayout(chartControlLayout);

    m_chartStack = new QStackedWidget(displayPanel);
    m_chartStack->setObjectName(QStringLiteral("chartStack"));
    m_displayLayout->addWidget(m_chartStack, 1);
    m_waveformPage = new QWidget(m_chartStack);
    m_waveformLayout = new QVBoxLayout(m_waveformPage);
    m_waveformLayout->setContentsMargins(0, 0, 0, 0);
    m_waveformLayout->setSpacing(12);
    m_chartStack->addWidget(m_waveformPage);

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

    m_rawChartGroup = createChartGroup(
        QStringLiteral("原始采集波形"), &m_rawChartView, &m_rawSeries,
        &m_rawAxisX, &m_rawAxisY);
    m_processedChartGroup = createChartGroup(
        QStringLiteral("吸光度与 Voigt 拟合"), &m_processedChartView, &m_processedSeries,
        &m_processedAxisX, &m_processedAxisY);
    m_waveformLayout->addWidget(m_rawChartGroup, 1);
    m_waveformLayout->addWidget(m_processedChartGroup, 1);
    buildPeakDataPanel();
    m_chartStack->setCurrentWidget(m_waveformPage);

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
    connect(m_toggleChartWindowButton, &QPushButton::clicked,
            this, &MainWindow::toggleChartWindow);
    connect(m_peakDataButton, &QPushButton::clicked,
            this, &MainWindow::togglePeakDataView);
    connect(m_peakWriter, &tdlas::PeakDataWriter::savingStarted, this,
            [this](const QString &) {
                if (m_peakSavingState == PeakSavingState::Starting)
                    m_peakSavingState = PeakSavingState::Saving;
                updatePeakSavingUi();
            }, Qt::QueuedConnection);
    connect(m_peakWriter, &tdlas::PeakDataWriter::pointSaved, this,
            [this](qint64 count) {
                m_savedPeakPoints = count;
                updatePeakSavingUi();
            }, Qt::QueuedConnection);
    connect(m_peakWriter, &tdlas::PeakDataWriter::savingStopped, this,
            [this](const QString &, qint64 count) {
                m_savedPeakPoints = count;
                m_peakSavingState = PeakSavingState::Stopping;
                updatePeakSavingUi();
            }, Qt::QueuedConnection);
    connect(m_peakWriter, &QThread::finished, this,
            [this]() {
                // Join the completed thread before enabling a new session.
                // All file writes have already finished at this point.
                m_peakWriter->wait();
                m_peakSavingState = PeakSavingState::Idle;
                updatePeakSavingUi();
            }, Qt::QueuedConnection);
    connect(m_peakWriter, &tdlas::PeakDataWriter::errorOccurred, this,
            [this](const QString &message) {
                m_peakWriter->stopSaving();
                m_peakSavingState = m_peakWriter->isRunning()
                    ? PeakSavingState::Stopping : PeakSavingState::Idle;
                m_peakSaveError = message;
                updatePeakSavingUi();
                statusBar()->showMessage(QStringLiteral("实时保存失败：%1").arg(message));
                if (!m_shuttingDown)
                    QMessageBox::warning(this, QStringLiteral("峰值数据保存失败"), message);
            }, Qt::QueuedConnection);
    connect(m_acquisition, &tdlas::DaqmxAcquisition::acquisitionStarted,
            this, &MainWindow::handleAcquisitionStarted);
    connect(m_acquisition, &tdlas::DaqmxAcquisition::acquisitionStopped,
            this, &MainWindow::handleAcquisitionStopped);
    connect(m_acquisition, &tdlas::DaqmxAcquisition::samplesReady,
            this, &MainWindow::handleSamples);
    connect(m_acquisition, &tdlas::DaqmxAcquisition::errorOccurred,
            this, &MainWindow::handleAcquisitionError);
    connect(m_processingWorker, &tdlas::TDLASProcessingWorker::resultReady,
            this, &MainWindow::handleProcessingResult, Qt::QueuedConnection);
    connect(m_processingWorker, &tdlas::TDLASProcessingWorker::statusChanged,
            this, &MainWindow::handleProcessingStatus, Qt::QueuedConnection);
    connect(m_processingWorker, &tdlas::TDLASProcessingWorker::errorOccurred,
            this, &MainWindow::handleProcessingError, Qt::QueuedConnection);
    connect(m_chartTimer, &QTimer::timeout, this, &MainWindow::refreshCharts);
}

void MainWindow::toggleChartWindow()
{
    if (m_peakDataButton->isChecked()) return;
    if (m_chartsInSeparateWindow) {
        restoreChartsToMainWindow();
    } else {
        moveChartsToSeparateWindow();
    }
}

void MainWindow::moveChartsToSeparateWindow()
{
    if (m_chartsInSeparateWindow) {
        if (m_chartWindow != nullptr) {
            m_chartWindow->show();
            m_chartWindow->raise();
            m_chartWindow->activateWindow();
        }
        return;
    }
    if (m_waveformLayout == nullptr || m_rawChartGroup == nullptr
        || m_processedChartGroup == nullptr) {
        return;
    }

    if (m_chartWindow == nullptr) {
        m_chartWindow = new QDialog(this);
        m_chartWindow->setWindowTitle(QStringLiteral("原始波形与拟合图"));
        m_chartWindow->setAttribute(Qt::WA_DeleteOnClose, false);
        m_chartWindow->setModal(false);
        m_chartWindow->resize(1100, 760);
        auto *chartLayout = new QVBoxLayout(m_chartWindow);
        chartLayout->setContentsMargins(12, 12, 12, 12);
        chartLayout->setSpacing(12);
        connect(m_chartWindow, &QDialog::finished, this,
                [this](int) { restoreChartsToMainWindow(); });
    }

    m_waveformLayout->removeWidget(m_rawChartGroup);
    m_waveformLayout->removeWidget(m_processedChartGroup);
    auto *chartLayout = qobject_cast<QVBoxLayout *>(m_chartWindow->layout());
    if (chartLayout == nullptr) return;
    chartLayout->addWidget(m_rawChartGroup, 1);
    chartLayout->addWidget(m_processedChartGroup, 1);
    m_rawChartGroup->show();
    m_processedChartGroup->show();
    m_chartsInSeparateWindow = true;
    m_toggleChartWindowButton->setText(QStringLiteral("返回主窗口"));
    m_toggleChartWindowButton->setToolTip(
        QStringLiteral("将原始波形和吸光度拟合图放回主窗口"));
    m_chartWindow->show();
    m_chartWindow->raise();
    m_chartWindow->activateWindow();
}

void MainWindow::restoreChartsToMainWindow()
{
    if (m_chartWindowRestoring) return;
    m_chartWindowRestoring = true;
    if (m_chartWindow != nullptr) {
        if (auto *chartLayout = qobject_cast<QVBoxLayout *>(m_chartWindow->layout())) {
            chartLayout->removeWidget(m_rawChartGroup);
            chartLayout->removeWidget(m_processedChartGroup);
        }
        m_chartWindow->hide();
    }
    if (m_waveformLayout != nullptr) {
        m_waveformLayout->removeWidget(m_rawChartGroup);
        m_waveformLayout->removeWidget(m_processedChartGroup);
        m_waveformLayout->addWidget(m_rawChartGroup, 1);
        m_waveformLayout->addWidget(m_processedChartGroup, 1);
        m_rawChartGroup->show();
        m_processedChartGroup->show();
    }
    m_chartsInSeparateWindow = false;
    if (m_toggleChartWindowButton != nullptr) {
        m_toggleChartWindowButton->setText(QStringLiteral("切换到独立窗口"));
        m_toggleChartWindowButton->setToolTip(
            QStringLiteral("只将原始波形和吸光度拟合图切换到独立窗口"));
    }
    m_chartWindowRestoring = false;
}

void MainWindow::buildPeakDataPanel()
{
    if (m_peakDataPanel != nullptr) return;

    m_peakDataPanel = new QGroupBox(QStringLiteral("峰值数据"), m_chartStack);
    m_peakDataPanel->setObjectName(QStringLiteral("peakDataPanel"));
    m_chartStack->addWidget(m_peakDataPanel);

    auto *layout = new QVBoxLayout(m_peakDataPanel);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *controlLayout = new QHBoxLayout();
    controlLayout->addWidget(new QLabel(QStringLiteral("显示范围"), m_peakDataPanel));
    m_peakRangeCombo = new QComboBox(m_peakDataPanel);
    m_peakRangeCombo->addItem(QStringLiteral("最近 30 分钟"), 30);
    m_peakRangeCombo->addItem(QStringLiteral("最近 1 小时"), 60);
    m_peakRangeCombo->setToolTip(QStringLiteral("只改变图表显示范围，不影响实时保存"));
    controlLayout->addWidget(m_peakRangeCombo);
    controlLayout->addStretch();
    m_peakSavePathButton = new QPushButton(QStringLiteral("保存地址"), m_peakDataPanel);
    m_peakSavePathButton->setObjectName(QStringLiteral("peakSavePathButton"));
    m_peakSavePathButton->setToolTip(QStringLiteral("选择保存文件；更换地址前请先停止保存"));
    m_peakSaveButton = new QPushButton(QStringLiteral("数据保存"), m_peakDataPanel);
    m_peakSaveButton->setObjectName(QStringLiteral("peakSaveButton"));
    m_peakSaveButton->setToolTip(QStringLiteral("开启后逐条保存新拟合结果，再次点击停止；已有三列文件会继续追加"));
    controlLayout->addWidget(m_peakSavePathButton);
    controlLayout->addWidget(m_peakSaveButton);
    layout->addLayout(controlLayout);

    auto *pathLayout = new QHBoxLayout();
    pathLayout->addWidget(new QLabel(QStringLiteral("保存地址"), m_peakDataPanel));
    m_peakSavePathLabel = new QLabel(QStringLiteral("尚未选择"), m_peakDataPanel);
    m_peakSavePathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_peakSavePathLabel->setWordWrap(true);
    m_peakSavePathLabel->setMinimumWidth(0);
    m_peakSavePathLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_peakSavePathLabel->setToolTip(QStringLiteral("按“保存地址”选择 TXT 文件路径和文件名"));
    pathLayout->addWidget(m_peakSavePathLabel, 1);
    layout->addLayout(pathLayout);
    m_peakSaveStatusLabel = new QLabel(m_peakDataPanel);
    m_peakSaveStatusLabel->setObjectName(QStringLiteral("peakSaveStatusLabel"));
    m_peakSaveStatusLabel->setWordWrap(true);
    layout->addWidget(m_peakSaveStatusLabel);
    updatePeakSavingUi();

    auto *chart = new QChart();
    chart->setAnimationOptions(QChart::NoAnimation);
    chart->legend()->hide();
    chart->setMargins(QMargins(4, 4, 4, 4));
    m_peakSeries = new QLineSeries(chart);
    m_peakSeries->setName(QStringLiteral("峰值吸光度"));
    m_peakSeries->setColor(QColor(QStringLiteral("#8a4f9e")));
    chart->addSeries(m_peakSeries);

    m_peakAxisX = new QDateTimeAxis(chart);
    m_peakAxisX->setTitleText(QStringLiteral("时间"));
    m_peakAxisX->setFormat(QStringLiteral("HH:mm:ss"));
    m_peakAxisX->setTickCount(7);
    m_peakAxisY = new QValueAxis(chart);
    m_peakAxisY->setTitleText(QStringLiteral("峰值吸光度"));
    m_peakAxisY->setRange(0.0, 1.0);
    m_peakAxisY->setTickCount(6);
    chart->addAxis(m_peakAxisX, Qt::AlignBottom);
    chart->addAxis(m_peakAxisY, Qt::AlignLeft);
    m_peakSeries->attachAxis(m_peakAxisX);
    m_peakSeries->attachAxis(m_peakAxisY);

    m_peakChartView = new QChartView(chart, m_peakDataPanel);
    m_peakChartView->setRenderHint(QPainter::Antialiasing);
    m_peakChartView->setMinimumSize(0, 0);
    m_peakChartView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(m_peakChartView, 1);

    connect(m_peakRangeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::handlePeakDisplayRangeChanged);
    connect(m_peakSavePathButton, &QPushButton::clicked,
            this, &MainWindow::choosePeakDataSavePath);
    connect(m_peakSaveButton, &QPushButton::clicked,
            this, &MainWindow::savePeakData);

    auto *peakChartTimer = new QTimer(this);
    peakChartTimer->setInterval(1000);
    connect(peakChartTimer, &QTimer::timeout, this, [this]() {
        if (m_peakDataPanel->isVisible()) refreshPeakDataChart();
    });
    peakChartTimer->start();
}

void MainWindow::togglePeakDataView()
{
    const bool showPeakData = m_peakDataButton->isChecked();
    if (showPeakData && m_chartsInSeparateWindow) restoreChartsToMainWindow();
    m_chartStack->setCurrentWidget(showPeakData ? static_cast<QWidget *>(m_peakDataPanel)
                                               : m_waveformPage);
    m_peakDataButton->setText(showPeakData ? QStringLiteral("返回波形与拟合")
                                          : QStringLiteral("峰值数据"));
    // Peak data always stays embedded, including after restoring detached charts.
    m_toggleChartWindowButton->setEnabled(!showPeakData);
    if (showPeakData) refreshPeakDataChart();
}

void MainWindow::choosePeakDataSavePath()
{
    if (m_peakSavingState != PeakSavingState::Idle || m_shuttingDown) return;
    QString initialPath = m_peakSavePath;
    if (initialPath.isEmpty()) {
        QString documentsPath = QStandardPaths::writableLocation(
            QStandardPaths::DocumentsLocation);
        if (documentsPath.isEmpty()) documentsPath = QStringLiteral(".");
        initialPath = documentsPath + QStringLiteral("/peak_absorbance.txt");
    }

    QString selectedPath = QFileDialog::getSaveFileName(
        this, QStringLiteral("选择峰值数据保存地址"), initialPath,
        QStringLiteral("TXT 文本文件 (*.txt);;所有文件 (*.*)"), nullptr,
        QFileDialog::DontConfirmOverwrite);
    if (selectedPath.isEmpty()) return;
    if (QFileInfo(selectedPath).suffix().isEmpty()) selectedPath += QStringLiteral(".txt");

    m_peakSavePath = selectedPath;
    if (m_peakSavePathLabel != nullptr) {
        m_peakSavePathLabel->setText(m_peakSavePath);
        m_peakSavePathLabel->setToolTip(m_peakSavePath);
    }
}

void MainWindow::savePeakData()
{
    if (m_shuttingDown) return;
    if (m_peakSavingState == PeakSavingState::Starting
        || m_peakSavingState == PeakSavingState::Saving) {
        m_peakSavingState = PeakSavingState::Stopping;
        updatePeakSavingUi();
        m_peakWriter->stopSaving();
        return;
    }
    if (m_peakSavingState != PeakSavingState::Idle) return;
    if (m_peakSavePath.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("请先通过“保存地址”选择文件，再点击“数据保存”开启实时保存。"));
        return;
    }
    m_savedPeakPoints = 0;
    m_peakSaveError.clear();
    m_peakSavingState = PeakSavingState::Starting;
    updatePeakSavingUi();
    if (!m_peakWriter->startSaving(m_peakSavePath)) {
        m_peakSavingState = m_peakWriter->isRunning()
            ? PeakSavingState::Stopping : PeakSavingState::Idle;
        m_peakSaveError = QStringLiteral("上一次保存尚未结束，请稍后重试。");
        updatePeakSavingUi();
    }
}

void MainWindow::updatePeakSavingUi()
{
    if (!m_peakSaveButton || !m_peakSaveStatusLabel) return;
    const bool idle = m_peakSavingState == PeakSavingState::Idle;
    const bool saving = m_peakSavingState == PeakSavingState::Starting
        || m_peakSavingState == PeakSavingState::Saving;
    m_peakSavePathButton->setEnabled(idle && !m_shuttingDown);
    m_peakSaveButton->setEnabled((idle || saving) && !m_shuttingDown);
    QString status;
    switch (m_peakSavingState) {
    case PeakSavingState::Idle:
        m_peakSaveButton->setText(QStringLiteral("数据保存"));
        status = m_savedPeakPoints > 0
            ? QStringLiteral("保存已停止，本次已保存 %1 条。再次开启将继续追加新结果。").arg(m_savedPeakPoints)
            : QStringLiteral("未开启保存。开启后逐条保存：时间、峰值吸光度、光强百分比 (%)。");
        break;
    case PeakSavingState::Starting:
        m_peakSaveButton->setText(QStringLiteral("停止保存"));
        status = QStringLiteral("正在打开保存文件，新结果已进入保存队列…");
        break;
    case PeakSavingState::Saving:
        m_peakSaveButton->setText(QStringLiteral("停止保存"));
        status = QStringLiteral("实时保存中，本次已保存 %1 条（光强单位：%）。").arg(m_savedPeakPoints);
        break;
    case PeakSavingState::Stopping:
        m_peakSaveButton->setText(QStringLiteral("正在停止…"));
        status = QStringLiteral("正在写完已排队的数据，已保存 %1 条…").arg(m_savedPeakPoints);
        break;
    }
    if (!m_peakSaveError.isEmpty())
        status = QStringLiteral("保存失败：%1（本次已保存 %2 条）").arg(m_peakSaveError).arg(m_savedPeakPoints);
    m_peakSaveStatusLabel->setText(status);
}

void MainWindow::handlePeakDisplayRangeChanged(int)
{
    refreshPeakDataChart();
}

void MainWindow::refreshPeakDataChart()
{
    if (m_peakDataPanel == nullptr || m_peakRangeCombo == nullptr
        || m_peakSeries == nullptr || m_peakAxisX == nullptr || m_peakAxisY == nullptr) {
        return;
    }

    const int minutes = std::max(1, m_peakRangeCombo->currentData().toInt());
    const QDateTime endTime = QDateTime::currentDateTime();
    const QDateTime beginTime = endTime.addSecs(-static_cast<qint64>(minutes) * 60);
    const QVector<tdlas::PeakAbsorbancePoint> points =
        m_peakHistory.recent(minutes, endTime);

    QVector<QPointF> chartPoints;
    chartPoints.reserve(points.size());
    for (const tdlas::PeakAbsorbancePoint &point : points) {
        chartPoints.append(QPointF(
            static_cast<qreal>(point.timestamp.toMSecsSinceEpoch()),
            point.peakAbsorbance));
    }
    m_peakSeries->replace(chartPoints);
    m_peakAxisX->setRange(beginTime, endTime);

    if (points.isEmpty()) {
        m_peakAxisY->setRange(0.0, 1.0);
        return;
    }

    auto valueRange = std::minmax_element(
        points.cbegin(), points.cend(),
        [](const tdlas::PeakAbsorbancePoint &left,
           const tdlas::PeakAbsorbancePoint &right) {
            return left.peakAbsorbance < right.peakAbsorbance;
        });
    double minimum = valueRange.first->peakAbsorbance;
    double maximum = valueRange.second->peakAbsorbance;
    const double padding = maximum > minimum
        ? (maximum - minimum) * 0.1
        : std::max(0.001, std::abs(minimum) * 0.1);
    minimum -= padding;
    maximum += padding;
    if (!(maximum > minimum)) maximum = minimum + 1.0;
    m_peakAxisY->setRange(minimum, maximum);
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

    tdlas::TDLASProcessorConfig processorConfig;
    processorConfig.samplesPerCycle = config.samplesPerCycle;
    processorConfig.averagedCyclesPerSpectrum = 20;
    processorConfig.averagedSpectra = 1;
    processorConfig.windowSamples = processorConfig.samplesPerCycle
        * processorConfig.averagedCyclesPerSpectrum;
    const bool detectSawtooth = config.outputWaveform == tdlas::OutputWaveform::Sawtooth;
    m_processingWorker->configure(processorConfig, detectSawtooth, 0,
                                  config.outputFrequency);
    m_processingWorker->setCapacitySamples(4000000);
    m_processingWorker->start();

    setAcquisitionUiState(true);
    setStatus(QStringLiteral("正在启动"), QStringLiteral("#b06a00"));
    statusBar()->showMessage(QStringLiteral("正在创建 NI-DAQmx 采集任务..."));
    m_acquisition->start();
}

void MainWindow::stopAcquisition()
{
    if (!m_acquisition->isRunning()) {
        stopProcessing(true);
        setAcquisitionUiState(false);
        return;
    }

    m_stopButton->setEnabled(false);
    setStatus(QStringLiteral("正在停止"), QStringLiteral("#b06a00"));
    statusBar()->showMessage(QStringLiteral("正在安全停止采集..."));
    m_acquisition->requestStop();
}

void MainWindow::handleAcquisitionStarted()
{
    m_rawDisplayBuffer.clear();
    m_hasLatestResult = false;
    m_processedCycles = 0;
    m_latestResult = tdlas::TDLASResult{};
    m_processedSeries->clear();
    m_fittedSeries->clear();
    m_fv1Series->clear();
    m_algorithmValue->setText(QStringLiteral("处理线程已启动，等待 20 个周期"));
    m_peakValue->setText(QStringLiteral("--"));
    m_lightValue->setText(QStringLiteral("--"));
    setAcquisitionUiState(true);
    setStatus(QStringLiteral("采集中"), QStringLiteral("#16794a"));
    statusBar()->showMessage(
        QStringLiteral("正在采集 %1，采样率 %2 S/s%3")
            .arg(m_inputChannelCombo->currentText())
            .arg(m_sampleRateSpin->value(), 0, 'f', 0)
            .arg(m_outputChannelCombo->currentData().toString().isEmpty()
                     ? QString()
                     : QStringLiteral("，后台线程保留完整周期")));
    m_chartTimer->start();
}

void MainWindow::handleAcquisitionStopped()
{
    stopProcessing(true);
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
    m_rawDisplayBuffer += samples;
    const int displaySamples = m_samplesPerCycleSpin->value();
    const int limit = std::max(1, displaySamples);
    if (m_rawDisplayBuffer.size() > limit)
        m_rawDisplayBuffer.remove(0, m_rawDisplayBuffer.size() - limit);
    if (!m_processingWorker->enqueueSamples(samples, sampleRate)) {
        handleProcessingError(QStringLiteral("处理队列已满，未能接收采集数据。"));
        m_acquisition->requestStop();
    }
}

void MainWindow::handleProcessingResult(const tdlas::TDLASResult &result,
                                         double sampleRate,
                                         int processedCycles)
{
    m_latestResult = result;
    m_latestResultSampleRate = sampleRate;
    m_processedCycles = processedCycles;
    m_hasLatestResult = result.valid;
    const tdlas::PeakAbsorbancePoint point{
        QDateTime::currentDateTime(), result.peakAbsorbance, result.lightPercent};
    if (result.valid && m_peakHistory.record(point.timestamp, point.peakAbsorbance,
                                             point.lightPercent)) {
        // Queue every result independently of chart visibility and redraw rate.
        // Starting is included so results arriving while the file opens are kept.
        if (m_peakSavingState == PeakSavingState::Starting
            || m_peakSavingState == PeakSavingState::Saving) {
            if (!m_peakWriter->enqueue(point)) {
                m_peakSavingState = PeakSavingState::Stopping;
                updatePeakSavingUi();
            }
        }
        if (m_peakDataPanel->isVisible()) refreshPeakDataChart();
    }
}

void MainWindow::handleProcessingStatus(const QString &message)
{
    if (!message.isEmpty()) statusBar()->showMessage(message);
}

void MainWindow::handleProcessingError(const QString &message)
{
    if (!message.isEmpty()) statusBar()->showMessage(message);
}

void MainWindow::refreshCharts()
{
    if (m_rawDisplayBuffer.isEmpty() || m_displaySampleRate <= 0.0) return;
    const QVector<double> source = m_rawDisplayBuffer;
    const int pixelWidth = std::max(200, m_rawChartView->viewport()->width());
    const int binCount = std::min(pixelWidth, static_cast<int>(source.size()));
    QVector<QPointF> points;
    points.reserve(binCount * 2);
    for (int bin = 0; bin < binCount; ++bin) {
        const int begin = bin * source.size() / binCount;
        const int end = std::max(begin + 1,
                                 static_cast<int>((bin + 1) * source.size() / binCount));
        auto first = source.cbegin() + begin;
        auto last = source.cbegin() + std::min(end, static_cast<int>(source.size()));
        const auto minIt = std::min_element(first, last);
        const auto maxIt = std::max_element(first, last);
        const auto appendPoint = [&](const QVector<double>::const_iterator it) {
            points.append(QPointF(static_cast<double>(it - source.cbegin()) / m_displaySampleRate,
                                  *it));
        };
        if (minIt < maxIt) { appendPoint(minIt); appendPoint(maxIt); }
        else { appendPoint(maxIt); appendPoint(minIt); }
    }
    m_rawSeries->replace(points);
    updateTimeAxes(static_cast<double>(source.size()) / m_displaySampleRate);
    updateChartAxes(source);
    m_sampleCountValue->setText(QString::number(source.size()));
    m_latestValue->setText(QStringLiteral("%1 V").arg(source.constLast(), 0, 'f', 6));
    const auto range = std::minmax_element(source.cbegin(), source.cend());
    m_rangeValue->setText(QStringLiteral("%1 ～ %2 V").arg(*range.first, 0, 'f', 6)
                          .arg(*range.second, 0, 'f', 6));
    if (m_hasLatestResult) {
        updateProcessedChart(m_latestResult, m_latestResultSampleRate, 0,
                             std::max(1, m_latestResult.cycleSamples));
        updateFv1Chart(m_latestResult);
        m_algorithmValue->setText(QStringLiteral("完成：%1 个周期，当前周期 %2 点")
                                       .arg(m_processedCycles).arg(m_latestResult.cycleSamples));
        m_peakValue->setText(QStringLiteral("%1").arg(m_latestResult.peakAbsorbance, 0, 'f', 6));
        m_lightValue->setText(QStringLiteral("%1 %").arg(m_latestResult.lightPercent, 0, 'f', 2));
    } else {
        m_algorithmValue->setText(QStringLiteral("后台处理：队列 %1 / %2 点")
                                       .arg(m_processingWorker->backlogSamples())
                                       .arg(m_processingWorker->capacitySamples()));
    }
}

void MainWindow::stopProcessing(bool drain)
{
    if (!m_processingWorker) return;
    m_chartTimer->stop();
    m_processingWorker->closeInput(drain);
    if (drain) m_processingWorker->wait();
    else m_processingWorker->stop();
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
    if (!m_shuttingDown)
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
    shutdownWorkers();
    event->accept();
}

void MainWindow::shutdownWorkers()
{
    if (m_shuttingDown) return;
    m_shuttingDown = true;
    // Stop hardware first, then deliver pending DAQ blocks and fitting results.
    // This keeps already-completed results in the same ordered save queue.
    if (m_acquisition->isRunning()) {
        m_acquisition->requestStop();
        m_acquisition->wait();
    }
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    stopProcessing(true);
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    // Wait only at shutdown; normal result handling never performs file I/O.
    m_peakWriter->drainAndWait();
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    updatePeakSavingUi();
}
