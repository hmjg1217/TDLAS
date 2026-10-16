#include "processing/tdlas_processing_worker.h"

#include <QCoreApplication>
#include <QDebug>

#include <algorithm>

namespace {

QVector<double> makeSawtooth(int periodSamples, int periods, int resetSamples)
{
    const int rampSamples = periodSamples - resetSamples;
    QVector<double> signal(periodSamples * periods);
    for (int index = 0; index < signal.size(); ++index) {
        const int phase = index % periodSamples;
        if (phase < rampSamples) {
            const double fraction = rampSamples > 1
                ? static_cast<double>(phase) / static_cast<double>(rampSamples - 1)
                : 0.0;
            signal[index] = 2.6 + 0.4 * fraction;
        } else {
            const int resetIndex = phase - rampSamples;
            const double fraction = static_cast<double>(resetIndex + 1)
                / static_cast<double>(resetSamples);
            signal[index] = 3.0 - 0.4 * fraction;
        }
    }
    return signal;
}

bool enqueueInBlocks(tdlas::TDLASProcessingWorker &worker,
                     const QVector<double> &signal,
                     double sampleRate)
{
    // Deliberately use a non-period-aligned block size to exercise boundaries.
    constexpr int kReadBlockSamples = 997;
    for (int offset = 0; offset < signal.size(); offset += kReadBlockSamples) {
        if (!worker.enqueueSamples(signal.mid(
                offset, std::min(kReadBlockSamples,
                                 static_cast<int>(signal.size()) - offset)),
                sampleRate)) {
            return false;
        }
    }
    return true;
}

void configureWorker(tdlas::TDLASProcessingWorker &worker,
                     int periodSamples,
                     double sampleRate)
{
    tdlas::TDLASProcessorConfig config;
    config.samplesPerCycle = 1000;
    config.averagedCyclesPerSpectrum = 20;
    config.averagedSpectra = 1;
    config.windowSamples = 20000;
    worker.configure(config, true, 0,
                     sampleRate / static_cast<double>(periodSamples));
}

bool runCase(int periodSamples,
             double sampleRate,
             int resetSamples,
             bool injectOutlier = false)
{
    tdlas::TDLASProcessingWorker worker;
    configureWorker(worker, periodSamples, sampleRate);

    int resultCount = 0;
    int lastProcessedCycles = 0;
    QObject::connect(&worker, &tdlas::TDLASProcessingWorker::resultReady,
                     &worker,
                     [&resultCount, &lastProcessedCycles](const tdlas::TDLASResult &result,
                                                           double,
                                                           int processedCycles) {
                         if (result.valid) {
                             ++resultCount;
                             lastProcessedCycles = processedCycles;
                         }
                     },
                     Qt::DirectConnection);

    worker.start();
    QVector<double> signal = makeSawtooth(periodSamples, 42, resetSamples);
    if (injectOutlier) signal[periodSamples * 8 + 10] = -5.0;
    const bool enqueued = enqueueInBlocks(worker, signal, sampleRate);
    worker.closeInput(true);
    worker.wait();

    if (!enqueued || resultCount != 2 || lastProcessedCycles != 40) {
        qWarning() << "period" << periodSamples << "reset samples" << resetSamples
                   << "resultCount" << resultCount
                   << "lastProcessedCycles" << lastProcessedCycles;
        return false;
    }
    return true;
}

bool runRecoveryCase()
{
    constexpr int kPeriodSamples = 50;
    constexpr double kSampleRate = 10000.0;
    tdlas::TDLASProcessingWorker worker;
    configureWorker(worker, kPeriodSamples, kSampleRate);

    int resultCount = 0;
    int lastProcessedCycles = 0;
    QObject::connect(&worker, &tdlas::TDLASProcessingWorker::resultReady,
                     &worker,
                     [&resultCount, &lastProcessedCycles](const tdlas::TDLASResult &result,
                                                           double,
                                                           int processedCycles) {
                         if (result.valid) {
                             ++resultCount;
                             lastProcessedCycles = processedCycles;
                         }
                     },
                     Qt::DirectConnection);

    worker.start();
    bool enqueued = enqueueInBlocks(worker,
                                    makeSawtooth(kPeriodSamples, 42, 8),
                                    kSampleRate);
    // Simulate a temporary loss of visible reset edges long enough to force a
    // detector resynchronization. A high, flat segment avoids creating a
    // false falling edge at the transition into the pause.
    enqueued = enqueued && enqueueInBlocks(worker, QVector<double>(100000, 3.0),
                                           kSampleRate);
    enqueued = enqueued && enqueueInBlocks(worker,
                                           makeSawtooth(kPeriodSamples, 42, 8),
                                           kSampleRate);
    worker.closeInput(true);
    worker.wait();

    if (!enqueued || resultCount != 4 || lastProcessedCycles != 80) {
        qWarning() << "recovery resultCount" << resultCount
                   << "lastProcessedCycles" << lastProcessedCycles;
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    return runCase(50, 10000.0, 1)
            && runCase(500, 100000.0, 1)
            && runCase(50, 10000.0, 8)
            && runCase(50, 10000.0, 1, true)
            && runRecoveryCase()
        ? 0 : 1;
}
