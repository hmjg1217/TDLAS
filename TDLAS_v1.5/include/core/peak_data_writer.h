#pragma once

#include "core/peak_absorbance_history.h"

#include <QMutex>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>

namespace tdlas {

// Performs all file access on a dedicated thread. Public methods only lock
// the short-lived queue mutex, so neither fitting nor chart updates wait on
// disk I/O. Each accepted point is written and flushed in insertion order.
class PeakDataWriter final : public QThread
{
    Q_OBJECT

public:
    explicit PeakDataWriter(QObject *parent = nullptr);
    ~PeakDataWriter() override;

    // Starts an asynchronous recording session. New/empty files receive the
    // three-column UTF-8 CSV header. Existing three-column files are appended;
    // incompatible files (including the old two-column format) are untouched.
    // Returns false if another session is active or the path is empty.
    bool startSaving(const QString &filePath);

    // Accepts a completed result from the UI or processing thread. Acceptance
    // is allowed while the file is opening. Invalid points, a full queue, and
    // calls after stopSaving() return false and are never silently dropped.
    bool enqueue(const PeakAbsorbancePoint &point);

    // Stops accepting points, then flushes the complete accepted queue before
    // ending the session. Does not block the calling thread.
    void stopSaving();

    // For application shutdown: request a drain and wait for the file to close.
    void drainAndWait();

    // True while opening, recording, or draining a session.
    bool isSaving() const;

signals:
    void savingStarted(const QString &filePath);
    void pointSaved(qint64 count);
    void savingStopped(const QString &filePath, qint64 count);
    void errorOccurred(const QString &message);

protected:
    void run() override;

private:
    mutable QMutex m_mutex;
    QWaitCondition m_condition;
    QQueue<PeakAbsorbancePoint> m_queue;
    QString m_filePath;
    bool m_sessionActive = false;
    bool m_accepting = false;
    bool m_closeRequested = false;
    static constexpr qsizetype kCapacityPoints = 100000;
};

} // namespace tdlas
