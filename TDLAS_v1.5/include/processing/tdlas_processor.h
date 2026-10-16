#pragma once

#include <QVector>
#include <QString>

#include <limits>

namespace tdlas {

struct TDLASProcessorConfig
{
    int windowSamples = 10000;
    int samplesPerCycle = 500;
    int averagedCyclesPerSpectrum = 20;
    int averagedSpectra = 1;
};

struct TDLASResult
{
    QVector<double> frequencyCm1;
    QVector<double> absorbance;
    // Equivalent of fv1 = vvoigtshape(ppar, v_f), evaluated on frequencyCm1.
    QVector<double> fittedAbsorbance;
    // Voltage-domain representations of the absorbance and Voigt curves.
    // The UI can use these when the processed chart is configured to show
    // voltage on its vertical axis.
    QVector<double> voltage;
    QVector<double> fittedVoltage;
    // Index of the first processed point within one acquisition cycle.
    int sampleOffset = 0;
    int cycleSamples = 0;
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
