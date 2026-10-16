#pragma once

#include <QMainWindow>
#include <QList>
#include <QString>
#include <QVector>

#include "core/peak_absorbance_history.h"
#include "core/peak_data_writer.h"
#include "daq/daqmx_acquisition.h"
#include "processing/tdlas_processing_worker.h"

#include <QtCharts/QChartView>
#include <QtCharts/QDateTimeAxis>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QDialog;
class QGroupBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class QWidget;

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
    void saveAcquisitionSettings();
    void startAcquisition();
    void stopAcquisition();
    void handleAcquisitionStarted();
    void handleAcquisitionStopped();
    void handleSamples(const QVector<double> &samples, double sampleRate);
    void handleProcessingResult(const tdlas::TDLASResult &result,
                                double sampleRate,
                                int processedCycles);
    void handleProcessingStatus(const QString &message);
    void handleProcessingError(const QString &message);
    void refreshCharts();
    void handleAcquisitionError(const QString &message);
    void toggleChartWindow();
    void restoreChartsToMainWindow();
    void togglePeakDataView();
    void choosePeakDataSavePath();
    void savePeakData();
    void handlePeakDisplayRangeChanged(int index);

private:
    void buildUi();
    void connectSignals();
    void loadAcquisitionSettings();
    void populateChannelsForDevice(int index);
    void setAcquisitionUiState(bool running);
    void setStatus(const QString &text, const QString &color);
    void updateChartAxes(const QVector<double> &samples);
    void updateProcessedChart(const tdlas::TDLASResult &result,
                              double sampleRate,
                              int displayOffsetSamples,
                              int displayWindowSamples);
    void updateFv1Chart(const tdlas::TDLASResult &result);
    void updateTimeAxes(double cycleDurationSeconds);
    void stopProcessing(bool drain);
    void moveChartsToSeparateWindow();
    void buildPeakDataPanel();
    void refreshPeakDataChart();
    void updatePeakSavingUi();
    void shutdownWorkers();

    tdlas::DaqmxAcquisition *m_acquisition = nullptr;
    tdlas::TDLASProcessingWorker *m_processingWorker = nullptr;
    QTimer *m_chartTimer = nullptr;
    tdlas::PeakDataWriter *m_peakWriter = nullptr;

    QList<tdlas::DaqmxDeviceInfo> m_devices;
    QComboBox *m_deviceCombo = nullptr;
    QComboBox *m_outputChannelCombo = nullptr;
    QComboBox *m_inputChannelCombo = nullptr;
    QComboBox *m_waveformCombo = nullptr;
    QPushButton *m_refreshDevicesButton = nullptr;
    QPushButton *m_saveSettingsButton = nullptr;
    QDoubleSpinBox *m_outputFrequencySpin = nullptr;
    QDoubleSpinBox *m_outputAmplitudeSpin = nullptr;
    QDoubleSpinBox *m_outputOffsetSpin = nullptr;
    QDoubleSpinBox *m_sampleRateSpin = nullptr;
    QSpinBox *m_samplesPerCycleSpin = nullptr;
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
    QPushButton *m_toggleChartWindowButton = nullptr;
    QPushButton *m_peakDataButton = nullptr;
    QChartView *m_rawChartView = nullptr;
    QChartView *m_processedChartView = nullptr;
    QChartView *m_fv1ChartView = nullptr;
    QGroupBox *m_rawChartGroup = nullptr;
    QGroupBox *m_processedChartGroup = nullptr;
    QWidget *m_displayPanel = nullptr;
    QVBoxLayout *m_displayLayout = nullptr;
    QStackedWidget *m_chartStack = nullptr;
    QWidget *m_waveformPage = nullptr;
    QVBoxLayout *m_waveformLayout = nullptr;
    QDialog *m_chartWindow = nullptr;
    bool m_chartWindowRestoring = false;
    bool m_chartsInSeparateWindow = false;
    QGroupBox *m_peakDataPanel = nullptr;
    QComboBox *m_peakRangeCombo = nullptr;
    QLabel *m_peakSavePathLabel = nullptr;
    QPushButton *m_peakSavePathButton = nullptr;
    QPushButton *m_peakSaveButton = nullptr;
    QLabel *m_peakSaveStatusLabel = nullptr;
    QChartView *m_peakChartView = nullptr;
    QLineSeries *m_peakSeries = nullptr;
    QDateTimeAxis *m_peakAxisX = nullptr;
    QValueAxis *m_peakAxisY = nullptr;
    QString m_peakSavePath;
    enum class PeakSavingState { Idle, Starting, Saving, Stopping };
    PeakSavingState m_peakSavingState = PeakSavingState::Idle;
    qint64 m_savedPeakPoints = 0;
    bool m_shuttingDown = false;
    QString m_peakSaveError;
    tdlas::PeakAbsorbanceHistory m_peakHistory;
    QLineSeries *m_rawSeries = nullptr;
    QLineSeries *m_processedSeries = nullptr;
    QLineSeries *m_fittedSeries = nullptr;
    QLineSeries *m_fv1Series = nullptr;
    QValueAxis *m_rawAxisX = nullptr;
    QValueAxis *m_rawAxisY = nullptr;
    QValueAxis *m_processedAxisX = nullptr;
    QValueAxis *m_processedAxisY = nullptr;
    QValueAxis *m_fv1AxisX = nullptr;
    QValueAxis *m_fv1AxisY = nullptr;
    QVector<double> m_rawDisplayBuffer;
    double m_displaySampleRate = 0.0;
    tdlas::TDLASResult m_latestResult;
    double m_latestResultSampleRate = 0.0;
    int m_processedCycles = 0;
    bool m_hasLatestResult = false;
};
