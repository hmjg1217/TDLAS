#pragma once

#include "processing/tdlas_processor.h"

#include <QMutex>
#include <QMetaType>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>

namespace tdlas {

// Owns the TDLAS processor and runs the complete signal path on a dedicated
// thread.  Input blocks are retained in a bounded queue; enqueueSamples()
// returns false when accepting the block would exceed the configured capacity
// so that callers can report back-pressure instead of silently losing data.
class TDLASProcessingWorker final : public QThread
{
    Q_OBJECT

public:
    explicit TDLASProcessingWorker(QObject *parent = nullptr);
    ~TDLASProcessingWorker() override;

    void configure(const TDLASProcessorConfig &config,
                   bool detectSawtooth = true,
                   int targetCycleSamples = 0,
                   double expectedFrequencyHz = 0.0);

    // Starts the processing thread. Configuration should be supplied before
    // start(); calling configure while stopped is always safe.
    void start(QThread::Priority priority = QThread::InheritPriority);

    // Enqueue a DAQ block in acquisition order. Returns false on overflow or
    // after closeInput()/stop() has been requested.
    bool enqueueSamples(const QVector<double> &samples, double sampleRate = 0.0);

    // Stop accepting new blocks. With drain=true, queued blocks are processed
    // before the worker exits; with drain=false, queued blocks are discarded
    // deliberately and an error/status signal is emitted.
    void closeInput(bool drain = true);
    void stop();

    qsizetype backlogSamples() const;
    qsizetype capacitySamples() const;
    void setCapacitySamples(qsizetype capacity);

signals:
    void resultReady(const tdlas::TDLASResult &result,
                     double sampleRate,
                     int processedCycles);
    void statusChanged(const QString &message);
    void errorOccurred(const QString &message);
    void processingStarted();
    void processingStopped();

protected:
    void run() override;

private:
    struct InputBlock {
        QVector<double> samples;
        double sampleRate = 0.0;
    };

    void processBlock(const InputBlock &block);
    void processSawtoothBuffer(double sampleRate);
    void processCycle(const QVector<double> &cycle, double sampleRate);
    QVector<double> resampleCycle(const QVector<double> &cycle) const;
    void resetDetector();

    mutable QMutex m_mutex;
    QWaitCondition m_condition;
    QQueue<InputBlock> m_queue;
    qsizetype m_queuedSamples = 0;
    qsizetype m_capacitySamples = 4000000;
    bool m_accepting = true;
    bool m_closeRequested = false;
    bool m_drainOnClose = true;
    bool m_stopRequested = false;

    TDLASProcessorConfig m_pendingConfig;
    bool m_detectSawtooth = true;
    int m_targetCycleSamples = 0;
    double m_expectedFrequencyHz = 0.0;
    bool m_detectorResyncMessageShown = false;

    // All fields below are accessed only by run() and its helpers.
    TDLASProcessor m_processor;
    QVector<double> m_sawtoothBuffer;
    int m_cycleAnchor = -1;
    int m_processedCycles = 0;
};

} // namespace tdlas

Q_DECLARE_METATYPE(tdlas::TDLASResult)
