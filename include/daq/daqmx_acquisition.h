#pragma once

#include <QMutex>
#include <QList>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVector>

#include <atomic>

namespace tdlas {

struct DaqmxDeviceInfo
{
    QString name;
    QString productType;
    QStringList analogInputChannels;
    QStringList analogOutputChannels;
};

enum class OutputWaveform
{
    Sine,
    Square,
    Sawtooth,
    Triangle
};

struct DaqmxConfig
{
    // Leave this empty to acquire AI data without generating an AO waveform.
    QString outputPhysicalChannel;
    QString inputPhysicalChannel;
    OutputWaveform outputWaveform = OutputWaveform::Sine;
    double outputFrequency = 1000.0;
    double outputAmplitude = 0.1;
    double outputOffset = 0.0;
    double sampleRate = 10000.0;
    int samplesPerRead = 1000;
    double minimumValue = -10.0;
    double maximumValue = 10.0;
    double readTimeoutSeconds = 1.0;
};

class DaqmxAcquisition final : public QThread
{
    Q_OBJECT

public:
    explicit DaqmxAcquisition(QObject *parent = nullptr);
    ~DaqmxAcquisition() override;

    bool configure(const DaqmxConfig &config);
    DaqmxConfig config() const;
    bool isConfigured() const;

    static QList<DaqmxDeviceInfo> enumerateDevices(QString *errorMessage = nullptr);

public slots:
    void requestStop();

signals:
    void samplesReady(const QVector<double> &samples, double sampleRate);
    void acquisitionStarted();
    void acquisitionStopped();
    void errorOccurred(const QString &message);

protected:
    void run() override;

private:
    bool validateConfig(const DaqmxConfig &config, QString *errorMessage) const;

    mutable QMutex m_configMutex;
    DaqmxConfig m_config;
    std::atomic_bool m_stopRequested{false};
    std::atomic_bool m_configured{false};
};

} // namespace tdlas
