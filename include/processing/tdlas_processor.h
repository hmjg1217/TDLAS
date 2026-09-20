#pragma once

#include <QVector>
#include <QString>

#include <limits>

namespace tdlas {

struct TDLASProcessorConfig
{
    int windowSamples = 100000;
    int samplesPerCycle = 500;
    int averagedCyclesPerSpectrum = 10;
    int averagedSpectra = 20;
};

struct TDLASResult
{
    QVector<double> frequencyCm1;
    QVector<double> absorbance;
    QVector<double> fittedAbsorbance;
    double peakAbsorbance = std::numeric_limits<double>::quiet_NaN();
    double lightPercent = std::numeric_limits<double>::quiet_NaN();
    bool valid = false;
    QString message;
};

class TDLASProcessor
{
public:
    explicit TDLASProcessor(const TDLASProcessorConfig &config = {});

    void setConfig(const TDLASProcessorConfig &config);
    TDLASProcessorConfig config() const;
    void reset();
    bool appendSamples(const QVector<double> &samples, TDLASResult *result);
    int bufferedSampleCount() const;

private:
    TDLASResult processWindow(const QVector<double> &window) const;

    TDLASProcessorConfig m_config;
    QVector<double> m_buffer;
};

} // namespace tdlas
