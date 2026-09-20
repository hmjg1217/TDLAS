#pragma once

#include <QMainWindow>
#include <QList>
#include <QVector>

#include "daq/daqmx_acquisition.h"
#include "processing/tdlas_processor.h"

#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void refreshDaqDevices();
    void handleDeviceSelectionChanged(int index);
    void startAcquisition();
    void stopAcquisition();
    void handleAcquisitionStarted();
    void handleAcquisitionStopped();
    void handleSamples(const QVector<double> &samples, double sampleRate);
    void handleAcquisitionError(const QString &message);

private:
    void buildUi();
    void connectSignals();
    void populateChannelsForDevice(int index);
    void setAcquisitionUiState(bool running);
    void setStatus(const QString &text, const QString &color);
    void updateChartAxes(const QVector<double> &samples);
    void updateProcessedChart(const tdlas::TDLASResult &result);

    tdlas::DaqmxAcquisition *m_acquisition = nullptr;
    tdlas::TDLASProcessor m_processor;

    QList<tdlas::DaqmxDeviceInfo> m_devices;
    QComboBox *m_deviceCombo = nullptr;
    QComboBox *m_outputChannelCombo = nullptr;
    QComboBox *m_inputChannelCombo = nullptr;
    QComboBox *m_waveformCombo = nullptr;
    QPushButton *m_refreshDevicesButton = nullptr;
    QDoubleSpinBox *m_outputFrequencySpin = nullptr;
    QDoubleSpinBox *m_outputAmplitudeSpin = nullptr;
    QDoubleSpinBox *m_outputOffsetSpin = nullptr;
    QDoubleSpinBox *m_sampleRateSpin = nullptr;
    QSpinBox *m_samplesPerReadSpin = nullptr;
    QDoubleSpinBox *m_minimumValueSpin = nullptr;
    QDoubleSpinBox *m_maximumValueSpin = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QLabel *m_statusIndicator = nullptr;
    QLabel *m_sampleCountValue = nullptr;
    QLabel *m_latestValue = nullptr;
    QLabel *m_rangeValue = nullptr;
    QLabel *m_algorithmValue = nullptr;
    QLabel *m_peakValue = nullptr;
    QLabel *m_lightValue = nullptr;
    QChartView *m_rawChartView = nullptr;
    QChartView *m_processedChartView = nullptr;
    QLineSeries *m_rawSeries = nullptr;
    QLineSeries *m_processedSeries = nullptr;
    QLineSeries *m_fittedSeries = nullptr;
    QValueAxis *m_rawAxisX = nullptr;
    QValueAxis *m_rawAxisY = nullptr;
    QValueAxis *m_processedAxisX = nullptr;
    QValueAxis *m_processedAxisY = nullptr;
};
