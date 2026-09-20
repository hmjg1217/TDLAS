#include "processing/tdlas_processor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace tdlas {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct Biquad { double b0; double b1; double b2; double a1; double a2; };

Biquad makeLowPass(double normalizedFrequency, double q)
{
    const double omega = kPi * normalizedFrequency;
    const double alpha = std::sin(omega) / (2.0 * q);
    const double cosine = std::cos(omega);
    const double denominator = 1.0 + alpha;
    return {(1.0 - cosine) / (2.0 * denominator),
            (1.0 - cosine) / denominator,
            (1.0 - cosine) / (2.0 * denominator),
            -2.0 * cosine / denominator,
            (1.0 - alpha) / denominator};
}

void applyBiquad(QVector<double> *values, const Biquad &filter)
{
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    for (double &value : *values) {
        const double output = filter.b0 * value + filter.b1 * x1 + filter.b2 * x2
            - filter.a1 * y1 - filter.a2 * y2;
        x2 = x1;
        x1 = value;
        y2 = y1;
        y1 = output;
        value = output;
    }
}

void applyZeroPhaseLowPass(QVector<double> *values)
{
    const std::array<Biquad, 2> filters = {
        makeLowPass(0.1, 0.541196100146197),
        makeLowPass(0.1, 1.306562964876377)};
    for (const Biquad &filter : filters) applyBiquad(values, filter);
    std::reverse(values->begin(), values->end());
    for (const Biquad &filter : filters) applyBiquad(values, filter);
    std::reverse(values->begin(), values->end());
}

bool solveLinearSystem(std::array<std::array<double, 6>, 5> matrix,
                       std::array<double, 5> *solution)
{
    for (int column = 0; column < 5; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 5; ++row) {
            if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column])) pivot = row;
        }
        if (std::abs(matrix[pivot][column]) < 1e-18) return false;
        std::swap(matrix[column], matrix[pivot]);
        const double divisor = matrix[column][column];
        for (int value = column; value <= 5; ++value) matrix[column][value] /= divisor;
        for (int row = 0; row < 5; ++row) {
            if (row == column) continue;
            const double factor = matrix[row][column];
            for (int value = column; value <= 5; ++value) matrix[row][value] -= factor * matrix[column][value];
        }
    }
    for (int index = 0; index < 5; ++index) (*solution)[index] = matrix[index][5];
    return true;
}

QVector<double> polynomialFit4(const QVector<double> &x, const QVector<double> &y)
{
    std::array<std::array<double, 6>, 5> normal{};
    for (qsizetype index = 0; index < x.size(); ++index) {
        std::array<double, 5> powers{};
        powers[0] = 1.0;
        for (int power = 1; power < 5; ++power) powers[power] = powers[power - 1] * x.at(index);
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 5; ++column) normal[row][column] += powers[row] * powers[column];
            normal[row][5] += powers[row] * y.at(index);
        }
    }
    std::array<double, 5> coefficients{};
    if (!solveLinearSystem(normal, &coefficients)) return {};
    return QVector<double>(coefficients.begin(), coefficients.end());
}

double evaluatePolynomial(const QVector<double> &coefficients, double x)
{
    double value = 0.0;
    for (qsizetype index = coefficients.size(); index-- > 0;) value = value * x + coefficients.at(index);
    return value;
}

double voigtShape(const std::array<double, 4> &parameters, double frequency)
{
    static constexpr std::array<double, 4> a = {-1.2150, -1.3509, -1.2150, -1.3509};
    static constexpr std::array<double, 4> b = {1.2359, 0.3786, -1.2359, -0.3786};
    static constexpr std::array<double, 4> c = {-0.3085, 0.5906, -0.3085, 0.5906};
    static constexpr std::array<double, 4> d = {0.0210, -1.1858, -0.0210, 1.1858};
    const double gaussianFwhm = std::max(parameters[2], 1e-9);
    const double ratio = std::max(parameters[3], 0.0) / gaussianFwhm * std::sqrt(std::log(2.0));
    const double y = 2.0 * std::sqrt(std::log(2.0)) * (frequency - parameters[0]);
    const double phiG = 2.0 / gaussianFwhm * std::sqrt(std::log(2.0) / kPi);
    double profile = 0.0;
    for (int index = 0; index < 4; ++index) {
        const double numerator = c[index] * (ratio - a[index]) + d[index] * (y / gaussianFwhm - b[index]);
        const double denominator = std::pow(ratio - a[index], 2.0) + std::pow(y / gaussianFwhm - b[index], 2.0);
        profile += phiG * numerator / denominator;
    }
    return profile * parameters[1];
}

double fitError(const std::array<double, 4> &parameters,
                const QVector<double> &x, const QVector<double> &y)
{
    double error = 0.0;
    for (qsizetype index = 0; index < x.size(); ++index) {
        const double difference = voigtShape(parameters, x.at(index)) - y.at(index);
        error += difference * difference;
    }
    return error;
}

std::array<double, 4> fitVoigt(const QVector<double> &x, const QVector<double> &y)
{
    std::array<double, 4> parameters = {0.0, 0.004, 0.05, 0.05};
    const std::array<double, 4> lower = {-0.5, 0.0, 0.001, 0.001};
    const std::array<double, 4> upper = {0.5, 0.2, 0.5, 0.5};
    double error = fitError(parameters, x, y);
    std::array<double, 4> steps = {0.02, 0.002, 0.02, 0.02};
    for (int iteration = 0; iteration < 80; ++iteration) {
        bool improved = false;
        for (int parameter = 0; parameter < 4; ++parameter) {
            for (double direction : {-1.0, 1.0}) {
                auto candidate = parameters;
                candidate[parameter] = std::clamp(candidate[parameter] + direction * steps[parameter], lower[parameter], upper[parameter]);
                const double candidateError = fitError(candidate, x, y);
                if (candidateError < error) {
                    parameters = candidate;
                    error = candidateError;
                    improved = true;
                }
            }
        }
        if (!improved) for (double &step : steps) step *= 0.55;
        if (*std::max_element(steps.cbegin(), steps.cend()) < 1e-6) break;
    }
    return parameters;
}

} // namespace

TDLASProcessor::TDLASProcessor(const TDLASProcessorConfig &config) : m_config(config)
{
    m_buffer.reserve(m_config.windowSamples);
}

void TDLASProcessor::setConfig(const TDLASProcessorConfig &config)
{
    m_config = config;
    reset();
    m_buffer.reserve(m_config.windowSamples);
}

TDLASProcessorConfig TDLASProcessor::config() const { return m_config; }
void TDLASProcessor::reset() { m_buffer.clear(); }
int TDLASProcessor::bufferedSampleCount() const { return static_cast<int>(m_buffer.size()); }

bool TDLASProcessor::appendSamples(const QVector<double> &samples, TDLASResult *result)
{
    if (result != nullptr) *result = TDLASResult{};
    if (m_config.windowSamples <= 0 || m_config.samplesPerCycle <= 0 || m_config.averagedCyclesPerSpectrum <= 0 || m_config.averagedSpectra <= 0) {
        if (result != nullptr) result->message = QStringLiteral("Invalid TDLAS configuration.");
        return false;
    }
    m_buffer += samples;
    if (m_buffer.size() < m_config.windowSamples) return false;
    const QVector<double> window = m_buffer.mid(m_buffer.size() - m_config.windowSamples);
    m_buffer.clear();
    if (result == nullptr) return true;
    *result = processWindow(window);
    return result->valid;
}

TDLASResult TDLASProcessor::processWindow(const QVector<double> &window) const
{
    TDLASResult result;
    const int samplesPerCycle = m_config.samplesPerCycle;
    const int cyclesPerSpectrum = m_config.averagedCyclesPerSpectrum;
    const int spectra = m_config.averagedSpectra;
    const int samplesPerSpectrum = samplesPerCycle * cyclesPerSpectrum;
    const int usableSamples = samplesPerSpectrum * spectra;
    if (window.size() < usableSamples) {
        result.message = QStringLiteral("Insufficient samples for a TDLAS window.");
        return result;
    }

    QVector<double> filtered = window.mid(window.size() - usableSamples);
    applyZeroPhaseLowPass(&filtered);
    QVector<double> accumulatedAbsorbance;
    QVector<double> accumulatedFrequency;

    for (int spectrumIndex = 0; spectrumIndex < spectra; ++spectrumIndex) {
        QVector<double> cycleAverage(samplesPerCycle, 0.0);
        const int spectrumOffset = spectrumIndex * samplesPerSpectrum;
        for (int cycle = 0; cycle < cyclesPerSpectrum; ++cycle) {
            const int cycleOffset = spectrumOffset + cycle * samplesPerCycle;
            for (int sample = 0; sample < samplesPerCycle; ++sample) {
                cycleAverage[sample] += filtered.at(cycleOffset + sample) / static_cast<double>(cyclesPerSpectrum);
            }
        }
        const auto maximum = std::max_element(cycleAverage.cbegin(), cycleAverage.cend());
        const int peakIndex = static_cast<int>(std::distance(cycleAverage.cbegin(), maximum));
        QVector<double> doubled = cycleAverage;
        doubled += cycleAverage;
        QVector<double> aligned(samplesPerCycle);
        for (int sample = 0; sample < samplesPerCycle; ++sample) aligned[sample] = doubled.at(peakIndex + sample) + 4.2;

        const int begin = std::max(0, static_cast<int>(std::floor(0.3 * samplesPerCycle)) - 1);
        const int end = std::min(samplesPerCycle, static_cast<int>(std::floor(0.9 * samplesPerCycle)));
        QVector<double> y;
        for (int sample = begin; sample <= end; ++sample) y.append(aligned.at(sample));
        QVector<double> x(y.size());
        for (qsizetype index = 0; index < x.size(); ++index) x[index] = static_cast<double>(index + 1);

        QVector<double> baselineX;
        QVector<double> baselineY;
        const int firstEnd = std::min(static_cast<int>(y.size()), static_cast<int>(std::floor(0.35 * y.size())));
        const int secondBegin = std::max(0, static_cast<int>(y.size()) - static_cast<int>(std::floor(0.4 * y.size())) - 1);
        for (int index = 0; index < firstEnd; ++index) { baselineX.append(x.at(index)); baselineY.append(y.at(index)); }
        for (int index = secondBegin; index < y.size(); ++index) { baselineX.append(x.at(index)); baselineY.append(y.at(index)); }

        const QVector<double> polynomial = polynomialFit4(baselineX, baselineY);
        if (polynomial.size() != 5) { result.message = QStringLiteral("Baseline fit failed."); return result; }
        QVector<double> absorbance(y.size());
        for (qsizetype index = 0; index < y.size(); ++index) {
            const double baseline = evaluatePolynomial(polynomial, x.at(index));
            absorbance[index] = std::log(std::max(baseline, 1e-12) / std::max(y.at(index), 1e-12));
        }

        const QVector<double> frequencyPolynomial = {0.609383083614659, 0.0241628513601265, 5.82968569053455e-06, -3.02716372873655e-09};
        QVector<double> frequency(y.size());
        const auto absorptionPeak = std::max_element(absorbance.cbegin(), absorbance.cend());
        const int absorptionPeakIndex = static_cast<int>(std::distance(absorbance.cbegin(), absorptionPeak));
        const double peakFrequency = evaluatePolynomial(frequencyPolynomial, x.at(absorptionPeakIndex));
        for (qsizetype index = 0; index < frequency.size(); ++index) {
            frequency[index] = (evaluatePolynomial(frequencyPolynomial, x.at(index)) - peakFrequency) * 0.8761 / 30.0;
        }

        if (spectrumIndex == 0) {
            accumulatedAbsorbance = absorbance;
            accumulatedFrequency = frequency;
            result.lightPercent = (aligned.constLast() - aligned.at(49)) / 2.0 * 100.0;
        } else {
            for (qsizetype index = 0; index < absorbance.size(); ++index) accumulatedAbsorbance[index] += absorbance.at(index);
        }
    }

    for (double &value : accumulatedAbsorbance) value /= static_cast<double>(spectra);
    const std::array<double, 4> fitParameters = fitVoigt(accumulatedFrequency, accumulatedAbsorbance);
    result.frequencyCm1 = accumulatedFrequency;
    result.absorbance = accumulatedAbsorbance;
    result.fittedAbsorbance.resize(accumulatedFrequency.size());
    for (qsizetype index = 0; index < accumulatedFrequency.size(); ++index) result.fittedAbsorbance[index] = voigtShape(fitParameters, accumulatedFrequency.at(index));
    result.peakAbsorbance = *std::max_element(result.fittedAbsorbance.cbegin(), result.fittedAbsorbance.cend());
    result.valid = true;
    result.message = QStringLiteral("TDLAS complete: peak absorbance = %1").arg(result.peakAbsorbance, 0, 'f', 6);
    return result;
}

} // namespace tdlas
