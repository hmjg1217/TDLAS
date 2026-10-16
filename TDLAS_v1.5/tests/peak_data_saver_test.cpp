#include "core/peak_absorbance_history.h"
#include "core/peak_data_writer.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QLocale>
#include <QSemaphore>
#include <QTemporaryDir>

#include <atomic>
#include <limits>

namespace {

bool check(bool condition, const char *description)
{
    if (!condition) qWarning() << description;
    return condition;
}

QByteArray readFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Cannot read test output" << filePath << file.errorString();
        return {};
    }
    return file.readAll().replace("\r\n", "\n");
}

bool runHistoryCase()
{
    tdlas::PeakAbsorbanceHistory history;
    const QDateTime now = QDateTime::fromString(
        QStringLiteral("2026-10-15T10:30:45.123+08:00"), Qt::ISODateWithMs);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    if (!check(!history.record(QDateTime{}, 0.001, 80.0), "Invalid timestamp was accepted")
        || !check(!history.record(now, nan, 80.0), "NaN peak was accepted")
        || !check(!history.record(now, infinity, 80.0), "Infinite peak was accepted")
        || !check(!history.record(now, 0.001, nan), "NaN light percentage was accepted")
        || !check(!history.record(now, 0.001, infinity), "Infinite light percentage was accepted")
        || !check(history.isEmpty(), "Invalid points changed the history")
        || !check(history.record(now.addSecs(-3601), 0.001, 70.0), "Old point was rejected")
        || !check(history.record(now.addSecs(-3600), 0.002, 75.0), "Boundary point was rejected")
        || !check(history.record(now, 0.003, 80.0), "Current point was rejected")
        || !check(history.record(now.addSecs(1), 0.004, 85.0), "Future point was rejected")) {
        return false;
    }

    const auto recent = history.recent(60, now);
    if (!check(history.size() == 4, "History lost points")
        || !check(recent.size() == 2, "Recent history has an incorrect time window")
        || !check(recent[0].timestamp == now.addSecs(-3600), "Lower time boundary is not inclusive")
        || !check(recent[1].timestamp == now, "Upper time boundary is not inclusive")
        || !check(recent[1].peakAbsorbance == 0.003 && recent[1].lightPercent == 80.0,
                  "Peak and light percentage are not associated with the same result")
        || !check(history.recent(0, now).isEmpty(), "Zero time window is not empty")
        || !check(history.recent(60, QDateTime{}).isEmpty(), "Invalid end timestamp is not empty")) {
        return false;
    }

    history.clear();
    return check(history.isEmpty() && history.size() == 0, "History clear failed");
}

bool runHistoryFileCase()
{
    QTemporaryDir directory;
    if (!check(directory.isValid(), "Temporary output directory failed")) return false;

    tdlas::PeakAbsorbanceHistory history;
    const QDateTime timestamp = QDateTime::fromString(
        QStringLiteral("2026-10-15T10:30:45.123+08:00"), Qt::ISODateWithMs);
    if (!check(history.record(timestamp, 0.0012345678901234567, 80.125),
               "File test point was rejected")) return false;

    QString error = QStringLiteral("stale error");
    const QString filePath = directory.filePath(QStringLiteral("peak.txt"));
    const QLocale previousLocale;
    QLocale::setDefault(QLocale(QLocale::German));
    const bool saved = history.saveToFile(filePath, &error);
    QLocale::setDefault(previousLocale);
    if (!check(saved && error.isEmpty(), "History export failed or kept a stale error")) {
        return false;
    }

    const QByteArray contents = readFile(filePath);
    if (!check(contents.startsWith("time,peakAbsorbance,lightPercent\n"), "Unexpected export header")
        || !check(contents.contains("2026-10-15T10:30:45.123+08:00,"),
                  "Export lost timestamp precision or time zone")
        || !check(contents.contains("0.0012345678901234567,80.125\n"),
                  "Export is locale-dependent or loses numeric precision")
        || !check(contents.count('\n') == 2, "Export has an incorrect number of rows")
        || !check(!history.saveToFile(QString{}, &error) && !error.isEmpty(),
                  "Empty export path did not return an error")
        || !check(!history.saveToFile(directory.path(), &error) && !error.isEmpty(),
                  "Directory export path did not return an error")) {
        return false;
    }
    return true;
}

struct WriterObservations
{
    QSemaphore started;
    QSemaphore saved;
    std::atomic<int> errorCount{0};
    std::atomic<qint64> savedCount{0};
    std::atomic<qint64> stoppedCount{-1};
    std::atomic<bool> wrongThread{false};

    void connectTo(tdlas::PeakDataWriter &writer)
    {
        QObject::connect(&writer, &tdlas::PeakDataWriter::savingStarted, &writer,
                         [this, &writer](const QString &) {
                             if (QThread::currentThread() != &writer) wrongThread = true;
                             started.release();
                         }, Qt::DirectConnection);
        QObject::connect(&writer, &tdlas::PeakDataWriter::pointSaved, &writer,
                         [this, &writer](qint64 count) {
                             if (QThread::currentThread() != &writer) wrongThread = true;
                             savedCount = count;
                             saved.release();
                         }, Qt::DirectConnection);
        QObject::connect(&writer, &tdlas::PeakDataWriter::savingStopped, &writer,
                         [this, &writer](const QString &, qint64 count) {
                             if (QThread::currentThread() != &writer) wrongThread = true;
                             stoppedCount = count;
                         }, Qt::DirectConnection);
        QObject::connect(&writer, &tdlas::PeakDataWriter::errorOccurred, &writer,
                         [this](const QString &message) {
                             if (!message.isEmpty()) ++errorCount;
                         }, Qt::DirectConnection);
    }
};

bool runLiveWriterCase()
{
    QTemporaryDir directory;
    if (!check(directory.isValid(), "Temporary writer directory failed")) return false;

    WriterObservations observations;
    tdlas::PeakDataWriter writer;
    observations.connectTo(writer);
    const QString filePath = directory.filePath(QStringLiteral("live.txt"));
    const QDateTime timestamp = QDateTime::fromString(
        QStringLiteral("2026-10-15T10:30:45.123+08:00"), Qt::ISODateWithMs);
    const tdlas::PeakAbsorbancePoint first{timestamp, 0.00125, 80.125};
    if (!check(!writer.enqueue(first), "Writer accepted a point before recording")
        || !check(!writer.startSaving(QString{}), "Writer accepted an empty path")
        || !check(observations.errorCount == 1, "Empty path did not report an error")
        || !check(writer.startSaving(filePath), "Writer failed to start")
        || !check(writer.isSaving(), "Opening writer is not marked active")
        || !check(!writer.startSaving(filePath), "Writer started a second concurrent session")
        || !check(writer.enqueue(first), "Writer rejected a point while opening")
        || !check(observations.started.tryAcquire(1, 5000), "Writer did not open its file")
        || !check(observations.saved.tryAcquire(1, 5000), "Writer did not save a live result")) {
        return false;
    }

    const QByteArray header("time,peakAbsorbance,lightPercent\n");
    const QByteArray firstRow("2026-10-15T10:30:45.123+08:00,0.00125,80.125\n");
    // Read the file while the writer remains active. A one-off export or a
    // buffer that only flushes on stop cannot satisfy this assertion.
    if (!check(writer.isSaving(), "Writer stopped after a single result")
        || !check(readFile(filePath) == header + firstRow,
                  "Live result was not flushed with all three columns")) return false;

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    if (!check(!writer.enqueue({QDateTime{}, 0.001, 80.0}), "Writer accepted an invalid timestamp")
        || !check(!writer.enqueue({timestamp, nan, 80.0}), "Writer accepted a NaN peak")
        || !check(!writer.enqueue({timestamp, 0.001, infinity}), "Writer accepted infinite light")
        || !check(observations.errorCount == 4, "Invalid writer points did not report errors")
        || !check(writer.enqueue({timestamp.addSecs(1), 0.0025, 0.0}), "Writer rejected zero light")
        || !check(writer.enqueue({timestamp.addSecs(2), 0.00375, 100.0}), "Writer rejected full light")) {
        return false;
    }
    writer.stopSaving();
    if (!check(!writer.enqueue(first), "Writer accepted a point after stop")
        || !check(writer.wait(5000), "Writer did not finish draining")
        || !check(!writer.isSaving(), "Stopped writer is still active")
        || !check(observations.stoppedCount == 3, "Writer did not drain every accepted result")
        || !check(!observations.wrongThread, "File operations did not run on the writer thread")) {
        return false;
    }

    const QByteArray rows = firstRow
        + "2026-10-15T10:30:46.123+08:00,0.0025000000000000001,0\n"
        + "2026-10-15T10:30:47.123+08:00,0.0037499999999999999,100\n";
    if (!check(readFile(filePath) == header + rows, "Stopped writer lost or reordered results")
        || !check(writer.startSaving(filePath), "Writer could not restart on the same file")
        || !check(writer.enqueue(first), "Restarted writer rejected a result")) return false;
    writer.drainAndWait();
    return check(readFile(filePath) == header + rows + firstRow,
                 "Restarting overwrote data or duplicated the header")
        && check(observations.stoppedCount == 1, "Restarted session count was not reset");
}

bool runWriterDrainCase()
{
    QTemporaryDir directory;
    if (!check(directory.isValid(), "Temporary drain directory failed")) return false;

    WriterObservations observations;
    const QString filePath = directory.filePath(QStringLiteral("drain.txt"));
    constexpr int kPointCount = 512;
    const QDateTime timestamp = QDateTime::fromString(
        QStringLiteral("2026-10-15T10:30:45.123+08:00"), Qt::ISODateWithMs);
    {
        tdlas::PeakDataWriter writer;
        observations.connectTo(writer);
        if (!check(writer.startSaving(filePath), "Drain writer could not start")) return false;
        for (int index = 0; index < kPointCount; ++index) {
            if (!check(writer.enqueue({timestamp.addMSecs(index),
                                       static_cast<double>(index),
                                       80.0 + static_cast<double>(index) / 1000.0}),
                       "Drain writer rejected a queued result")) return false;
        }
        // The destructor must close the session only after writing its queue.
    }
    const auto lines = readFile(filePath).split('\n');
    if (!check(lines.size() == kPointCount + 2, "Shutdown lost queued results")
        || !check(observations.stoppedCount == kPointCount, "Shutdown count does not match accepted results")
        || !check(observations.errorCount == 0, "Drain writer reported an error")) return false;
    for (int index = 0; index < kPointCount; ++index) {
        const auto columns = lines[index + 1].split(',');
        if (!check(columns.size() == 3, "Drain file is not three-column data")
            || !check(columns[0] == timestamp.addMSecs(index).toString(Qt::ISODateWithMs).toUtf8(),
                      "Queued timestamps were reordered")
            || !check(columns[1].toDouble() == static_cast<double>(index),
                      "Queued peak values were reordered")
            || !check(columns[2].toDouble() == 80.0 + static_cast<double>(index) / 1000.0,
                      "Queued light values do not match their result")) return false;
    }
    return true;
}

bool runWriterErrorCase()
{
    QTemporaryDir directory;
    if (!check(directory.isValid(), "Temporary error directory failed")) return false;

    // The synchronous export can create a legacy-input fixture through a
    // missing light measurement, but its header is always the new format.
    // Use a repository fixture for the genuinely old two-column format.
    const QString oldFilePath = directory.filePath(QStringLiteral("old.txt"));
    if (!QFile::copy(QStringLiteral(TDLAS_TEST_DATA_DIR) + QStringLiteral("/peak_legacy.txt"),
                     oldFilePath)) return check(false, "Cannot copy legacy file fixture");
    const QByteArray original = readFile(oldFilePath);

    WriterObservations observations;
    tdlas::PeakDataWriter writer;
    observations.connectTo(writer);
    if (!check(writer.startSaving(oldFilePath), "Legacy-file writer did not accept async start")
        || !check(writer.wait(5000), "Legacy-file writer did not stop")
        || !check(observations.errorCount == 1, "Legacy file did not report a format error")
        || !check(observations.stoppedCount == 0, "Legacy file reported saved points")
        || !check(readFile(oldFilePath) == original, "Legacy file was modified")
        || !check(writer.startSaving(directory.path()), "Directory writer did not accept async start")
        || !check(writer.wait(5000), "Directory writer did not stop")
        || !check(observations.errorCount == 2, "Directory path did not report a file error")
        || !check(!writer.isSaving(), "Failed writer remains active")) return false;

    const QString validFilePath = directory.filePath(QStringLiteral("recovered.txt"));
    if (!check(writer.startSaving(validFilePath), "Writer could not recover after a file error")) return false;
    writer.drainAndWait();
    return check(readFile(validFilePath) == "time,peakAbsorbance,lightPercent\n",
                 "Recovered writer did not produce a valid empty file")
        && check(observations.errorCount == 2, "Recovered writer reported a new error");
}

bool runExistingFileCase()
{
    QTemporaryDir directory;
    if (!check(directory.isValid(), "Temporary append directory failed")) return false;

    const QByteArray header("time,peakAbsorbance,lightPercent");
    const QByteArray row("2026-10-15T10:30:45.123+08:00,0.00125,80.125\n");
    const QByteArray existingRow("2026-10-15T10:30:44.123+08:00,0.0025,81.5");
    const QList<QByteArray> originals = {
        QByteArray{},
        header,
        header + '\n' + existingRow,
        QByteArray("\xEF\xBB\xBF") + header + "\r\n" + existingRow + "\r\n"
    };
    const QDateTime timestamp = QDateTime::fromString(
        QStringLiteral("2026-10-15T10:30:45.123+08:00"), Qt::ISODateWithMs);

    for (qsizetype index = 0; index < originals.size(); ++index) {
        const QString path = directory.filePath(QStringLiteral("append_%1.txt").arg(index));
        const QByteArray original = originals[index];
        {
            QFile fixture(path);
            if (!check(fixture.open(QIODevice::WriteOnly)
                           && fixture.write(original) == original.size(),
                       "Cannot create existing-file fixture")) return false;
        }

        WriterObservations observations;
        tdlas::PeakDataWriter writer;
        observations.connectTo(writer);
        if (!check(writer.startSaving(path), "Writer rejected compatible existing file")
            || !check(writer.enqueue({timestamp, 0.00125, 80.125}),
                      "Existing-file writer rejected result")) return false;
        writer.drainAndWait();

        QByteArray expected = original;
        if (expected.isEmpty()) expected = header + '\n';
        else if (!expected.endsWith('\n')) expected += '\n';
        expected += row;
        // Use raw bytes here so adding a BOM or normalizing the prior file's
        // line endings would be detected as an unexpected content change.
        QFile output(path);
        if (!check(output.open(QIODevice::ReadOnly), "Cannot read appended file")
            || !check(output.readAll() == expected,
                      "Append altered existing data or omitted its line separator")
            || !check(observations.errorCount == 0 && observations.stoppedCount == 1,
                      "Compatible existing-file session did not finish successfully")) return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    return runHistoryCase() && runHistoryFileCase()
            && runLiveWriterCase() && runWriterDrainCase() && runWriterErrorCase()
            && runExistingFileCase()
        ? 0 : 1;
}
