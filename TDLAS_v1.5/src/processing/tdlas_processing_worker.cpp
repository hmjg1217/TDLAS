#include "processing/tdlas_processing_worker.h"

#include <QMetaType>

#include <algorithm>
#include <cmath>
#include <limits>

namespace tdlas {
namespace {

QVector<double> linearResample(const QVector<double> &source, int count)
{
    if (count <= 0 || source.isEmpty()) return {};
    if (source.size() == count) return source;
    if (source.size() == 1) return QVector<double>(count, source.constFirst());

    QVector<double> result(count);
    const double scale = static_cast<double>(source.size() - 1) / static_cast<double>(count - 1);
    for (int i = 0; i < count; ++i) {
        const double position = static_cast<double>(i) * scale;
        const int left = std::min(static_cast<int>(std::floor(position)),
                                  static_cast<int>(source.size()) - 2);
        const double fraction = position - static_cast<double>(left);
        result[i] = source.at(left) * (1.0 - fraction) + source.at(left + 1) * fraction;
    }
    return result;
}

} // namespace

TDLASProcessingWorker::TDLASProcessingWorker(QObject *parent)
    : QThread(parent), m_processor(m_pendingConfig)
{
    qRegisterMetaType<tdlas::TDLASResult>("tdlas::TDLASResult");
}

TDLASProcessingWorker::~TDLASProcessingWorker()
{
    stop();
}

void TDLASProcessingWorker::configure(const TDLASProcessorConfig &config,
                                       bool detectSawtooth,
                                       int targetCycleSamples,
                                       double expectedFrequencyHz)
{
    QMutexLocker locker(&m_mutex);
    if (isRunning()) {
        emit errorOccurred(QStringLiteral("TDLAS processing configuration cannot change while running."));
        return;
    }
    m_pendingConfig = config;
    m_detectSawtooth = detectSawtooth;
    m_targetCycleSamples = std::max(0, targetCycleSamples);
    m_expectedFrequencyHz = std::isfinite(expectedFrequencyHz) && expectedFrequencyHz > 0.0
        ? expectedFrequencyHz : 0.0;
    if (m_detectSawtooth) {
        if (m_targetCycleSamples > 1) m_pendingConfig.samplesPerCycle = m_targetCycleSamples;
        m_pendingConfig.windowSamples = std::max(
            1, m_pendingConfig.samplesPerCycle
                * std::max(1, m_pendingConfig.averagedCyclesPerSpectrum)
                * std::max(1, m_pendingConfig.averagedSpectra));
    }
}

void TDLASProcessingWorker::start(QThread::Priority priority)
{
    QMutexLocker locker(&m_mutex);
    if (isRunning()) return;
    m_closeRequested = false;
    m_drainOnClose = true;
    m_stopRequested = false;
    m_accepting = true;
    m_queue.clear();
    m_queuedSamples = 0;
    resetDetector();
    m_processor.setConfig(m_pendingConfig);
    QThread::start(priority);
}

bool TDLASProcessingWorker::enqueueSamples(const QVector<double> &samples,
                                            double sampleRate)
{
    if (samples.isEmpty()) return true;
    QString overflowMessage;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_accepting || m_stopRequested || m_closeRequested) return false;
        if (samples.size() > m_capacitySamples - m_queuedSamples) {
            overflowMessage = QStringLiteral(
                "TDLAS processing queue overflow (%1/%2 samples); input block rejected.")
                .arg(m_queuedSamples).arg(m_capacitySamples);
        } else {
            m_queue.enqueue(InputBlock{samples, sampleRate});
            m_queuedSamples += samples.size();
            m_condition.wakeOne();
            return true;
        }
    }
    emit errorOccurred(overflowMessage);
    emit statusChanged(overflowMessage);
    return false;
}

void TDLASProcessingWorker::closeInput(bool drain)
{
    QString message;
    {
        QMutexLocker locker(&m_mutex);
        m_accepting = false;
        m_closeRequested = true;
        m_drainOnClose = drain;
        if (!drain && !m_queue.isEmpty()) {
            const qsizetype dropped = m_queuedSamples;
            m_queue.clear();
            m_queuedSamples = 0;
            message = QStringLiteral(
                "TDLAS input closed immediately; %1 queued samples were discarded.").arg(dropped);
        }
        m_condition.wakeAll();
    }
    if (!message.isEmpty()) {
        emit errorOccurred(message);
        emit statusChanged(message);
    }
}

void TDLASProcessingWorker::stop()
{
    {
        QMutexLocker locker(&m_mutex);
        m_accepting = false;
        m_stopRequested = true;
        m_condition.wakeAll();
    }
    if (isRunning() && QThread::currentThread() != this) wait();
}

qsizetype TDLASProcessingWorker::backlogSamples() const
{
    QMutexLocker locker(&m_mutex);
    return m_queuedSamples;
}

qsizetype TDLASProcessingWorker::capacitySamples() const
{
    QMutexLocker locker(&m_mutex);
    return m_capacitySamples;
}

void TDLASProcessingWorker::setCapacitySamples(qsizetype capacity)
{
    QMutexLocker locker(&m_mutex);
    if (isRunning() || capacity < 1) return;
    m_capacitySamples = capacity;
}

void TDLASProcessingWorker::run()
{
    emit processingStarted();
    emit statusChanged(QStringLiteral("TDLAS processing thread started."));

    for (;;) {
        InputBlock block;
        {
            QMutexLocker locker(&m_mutex);
            while (m_queue.isEmpty() && !m_stopRequested
                   && !(m_closeRequested && !m_drainOnClose)) {
                if (m_closeRequested && m_drainOnClose) break;
                m_condition.wait(&m_mutex);
            }
            if (m_stopRequested) break;
            if (m_queue.isEmpty()) {
                if (m_closeRequested) break;
                continue;
            }
            block = m_queue.dequeue();
            m_queuedSamples -= block.samples.size();
        }
        processBlock(block);
    }

    if (m_stopRequested && backlogSamples() > 0) {
        const QString message = QStringLiteral(
            "TDLAS processing stopped with %1 queued samples pending.").arg(backlogSamples());
        emit statusChanged(message);
    }
    emit statusChanged(QStringLiteral("TDLAS processing thread stopped."));
    emit processingStopped();
}

void TDLASProcessingWorker::processBlock(const InputBlock &block)
{
    if (!m_detectSawtooth) {
        TDLASResult result;
        if (m_processor.appendSamples(block.samples, &result) && result.valid) {
            emit resultReady(result, block.sampleRate, ++m_processedCycles);
        }
        return;
    }
    m_sawtoothBuffer += block.samples;
    processSawtoothBuffer(block.sampleRate);
}

void TDLASProcessingWorker::processSawtoothBuffer(double sampleRate)
{
    if (m_sawtoothBuffer.size() < 3) return;

    int expectedPeriodSamples = m_targetCycleSamples;
    if (expectedPeriodSamples <= 1 && m_expectedFrequencyHz > 0.0
        && std::isfinite(sampleRate) && sampleRate > 0.0) {
        const double period = sampleRate / m_expectedFrequencyHz;
        if (std::isfinite(period) && period >= 4.0
            && period <= static_cast<double>(std::numeric_limits<int>::max() / 4)) {
            expectedPeriodSamples = static_cast<int>(std::lround(period));
        }
    }

    // Allow a reset to span a short run of samples instead of requiring one
    // unusually large point-to-point drop. The upper bound keeps the scan
    // cost predictable if a bad frequency setting is supplied.
    const int resetSearchSpan = expectedPeriodSamples > 1
        ? std::clamp(static_cast<int>(std::ceil(expectedPeriodSamples * 0.2)), 2, 64)
        : 8;
    const int maximumUnanchoredSamples = expectedPeriodSamples > 1
        ? expectedPeriodSamples * 2 + resetSearchSpan + 2
        : 200000;
    const int maximumAnchoredSamples = expectedPeriodSamples > 1
        ? expectedPeriodSamples * 3 + resetSearchSpan + 2
        : 200000;

    double minimum = m_sawtoothBuffer.constFirst();
    double maximum = minimum;
    if (m_sawtoothBuffer.size() >= 32) {
        // A single transient can otherwise inflate the full-range threshold
        // enough to hide every real reset. Use a lightly trimmed range once
        // there are enough samples for percentile estimates to be meaningful.
        QVector<double> ordered = m_sawtoothBuffer;
        const int lowIndex = (static_cast<int>(ordered.size()) - 1) * 2 / 100;
        const int highIndex = (static_cast<int>(ordered.size()) - 1) * 98 / 100;
        std::nth_element(ordered.begin(), ordered.begin() + lowIndex, ordered.end());
        minimum = ordered.at(lowIndex);
        std::nth_element(ordered.begin(), ordered.begin() + highIndex, ordered.end());
        maximum = ordered.at(highIndex);
    } else {
        for (double value : m_sawtoothBuffer) {
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
    }
    const double threshold = std::max(1e-9, (maximum - minimum) * 0.15);
    int anchor = m_cycleAnchor;
    int scanBegin = std::max(1, anchor + 1);
    for (int i = scanBegin; i + 1 < m_sawtoothBuffer.size(); ++i) {
        const int searchEnd = std::min(i + resetSearchSpan,
                                       static_cast<int>(m_sawtoothBuffer.size()) - 2);
        int valleyIndex = i;
        double valleyValue = m_sawtoothBuffer.at(i);
        for (int candidate = i + 1; candidate <= searchEnd; ++candidate) {
            const double value = m_sawtoothBuffer.at(candidate);
            if (value < valleyValue) {
                valleyValue = value;
                valleyIndex = candidate;
            }
        }

        // Measure the drop over a short interval so a rounded or bandwidth-
        // limited reset is still found. Confirm that the signal has reached
        // the valley before treating it as a reset edge.
        const double drop = m_sawtoothBuffer.at(i - 1) - valleyValue;
        if (drop < threshold) continue;
        if (std::abs(m_sawtoothBuffer.at(valleyIndex + 1) - valleyValue)
            > threshold * 0.8) continue;

        const int resetIndex = valleyIndex;

        if (anchor < 0) {
            anchor = resetIndex;
            i = resetIndex;
            continue;
        }

        const int length = resetIndex - anchor;
        if (expectedPeriodSamples > 1) {
            const int minimumPeriod = std::max(4, expectedPeriodSamples / 2);
            const int maximumPeriod = expectedPeriodSamples + expectedPeriodSamples / 2;
            if (length < minimumPeriod) {
                // Ignore an early noise edge without moving the cycle anchor.
                i = resetIndex;
                continue;
            }
            if (length > maximumPeriod) {
                // A missed edge must not turn several physical periods into
                // one stretched cycle. Re-anchor here and resume on the next
                // plausible period.
                anchor = resetIndex;
                if (!m_detectorResyncMessageShown) {
                    emit statusChanged(QStringLiteral(
                        "锯齿波复位沿间隔超出预期，已重新同步并继续搜索。"));
                    m_detectorResyncMessageShown = true;
                }
                i = resetIndex;
                continue;
            }
        }

        if (length >= 4) {
            processCycle(m_sawtoothBuffer.mid(anchor, length), sampleRate);
        }
        anchor = resetIndex;
        m_detectorResyncMessageShown = false;
        i = resetIndex;
    }

    if (anchor >= 0) {
        if (m_sawtoothBuffer.size() - anchor > maximumAnchoredSamples) {
            // The reset detector has lost lock. Drop its stale search history
            // and wait for a fresh pair of edges, while bounding memory and
            // the work done for each subsequent acquisition block.
            anchor = -1;
            if (!m_detectorResyncMessageShown) {
                emit statusChanged(QStringLiteral(
                    "锯齿波复位沿暂未稳定识别，已清理检测缓存并继续搜索。"));
                m_detectorResyncMessageShown = true;
            }
        }
    }

    if (anchor >= 0) {
        m_sawtoothBuffer = m_sawtoothBuffer.mid(anchor);
        m_cycleAnchor = 0;
    } else {
        if (m_sawtoothBuffer.size() > maximumUnanchoredSamples) {
            m_sawtoothBuffer = m_sawtoothBuffer.mid(
                m_sawtoothBuffer.size() - maximumUnanchoredSamples);
        }
        m_cycleAnchor = -1;
    }
}

void TDLASProcessingWorker::processCycle(const QVector<double> &cycle,
                                          double sampleRate)
{
    int count = m_targetCycleSamples > 1 ? m_targetCycleSamples
                                         : m_pendingConfig.samplesPerCycle;
    if (m_targetCycleSamples <= 1 && m_processedCycles == 0 && cycle.size() > 1) {
        // The actual period is determined from the first pair of falling
        // edges.  Adapting the processor here keeps 10 kS/s (50 points per
        // period) and 100 kS/s (500 points per period) equally efficient.
        count = cycle.size();
        m_pendingConfig.samplesPerCycle = count;
        m_pendingConfig.windowSamples = std::max(
            1, count * std::max(1, m_pendingConfig.averagedCyclesPerSpectrum)
                * std::max(1, m_pendingConfig.averagedSpectra));
        m_processor.setConfig(m_pendingConfig);
    }
    const QVector<double> normalized = resampleCycle(cycle);
    if (normalized.isEmpty()) return;
    TDLASResult result;
    ++m_processedCycles;
    if (m_processor.appendSamples(normalized, &result) && result.valid)
        emit resultReady(result, sampleRate, m_processedCycles);
    if (m_processedCycles % std::max(1, m_pendingConfig.averagedCyclesPerSpectrum) == 0) {
        emit statusChanged(QStringLiteral("TDLAS cycles processed: %1; queue: %2 samples")
                           .arg(m_processedCycles).arg(backlogSamples()));
    }
}

QVector<double> TDLASProcessingWorker::resampleCycle(const QVector<double> &cycle) const
{
    const int count = m_targetCycleSamples > 1 ? m_targetCycleSamples
                                                : m_pendingConfig.samplesPerCycle;
    return linearResample(cycle, count);
}

void TDLASProcessingWorker::resetDetector()
{
    m_sawtoothBuffer.clear();
    m_cycleAnchor = -1;
    m_processedCycles = 0;
    m_detectorResyncMessageShown = false;
}

} // namespace tdlas
