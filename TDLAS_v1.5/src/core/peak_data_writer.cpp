#include "core/peak_data_writer.h"

#include <QFile>
#include <QLocale>

#include <cmath>

namespace tdlas {
namespace {

const QByteArray kHeader("time,peakAbsorbance,lightPercent");

bool prepareFile(QFile &file, QString *errorMessage)
{
    if (!file.open(QIODevice::ReadWrite)) {
        *errorMessage = file.errorString();
        return false;
    }
    if (file.size() == 0) {
        const QByteArray header = kHeader + '\n';
        if (file.write(header) != header.size() || !file.flush()) {
            *errorMessage = file.errorString();
            return false;
        }
        return true;
    }

    QByteArray header = file.readLine(4096);
    if (header.startsWith("\xEF\xBB\xBF")) header.remove(0, 3);
    if (header.trimmed() != kHeader) {
        *errorMessage = QStringLiteral(
            "该文件不是“时间、峰值吸光度、光强百分比”三列格式。"
            "为保护原数据，请使用“保存地址”选择一个新文件。");
        return false;
    }
    if (!file.seek(file.size() - 1)) {
        *errorMessage = file.errorString();
        return false;
    }
    const QByteArray lastByte = file.read(1);
    if (lastByte.size() != 1 || !file.seek(file.size())) {
        *errorMessage = file.errorString();
        return false;
    }
    if (lastByte != "\n" && (file.write("\n", 1) != 1 || !file.flush())) {
        *errorMessage = file.errorString();
        return false;
    }
    return true;
}

QByteArray serializePoint(const PeakAbsorbancePoint &point)
{
    const QLocale locale = QLocale::c();
    return (point.timestamp.toString(Qt::ISODateWithMs)
            + QLatin1Char(',') + locale.toString(point.peakAbsorbance, 'g', 17)
            + QLatin1Char(',') + locale.toString(point.lightPercent, 'g', 17)
            + QLatin1Char('\n')).toUtf8();
}

} // namespace

PeakDataWriter::PeakDataWriter(QObject *parent)
    : QThread(parent)
{
}

PeakDataWriter::~PeakDataWriter()
{
    drainAndWait();
}

bool PeakDataWriter::startSaving(const QString &filePath)
{
    if (filePath.trimmed().isEmpty()) {
        emit errorOccurred(QStringLiteral("保存路径为空，请先选择保存地址。"));
        return false;
    }

    QMutexLocker locker(&m_mutex);
    if (m_sessionActive || isRunning()) return false;
    m_filePath = filePath;
    m_queue.clear();
    m_closeRequested = false;
    m_accepting = true;
    m_sessionActive = true;
    QThread::start();
    return true;
}

bool PeakDataWriter::enqueue(const PeakAbsorbancePoint &point)
{
    if (!point.timestamp.isValid() || !std::isfinite(point.peakAbsorbance)
        || !std::isfinite(point.lightPercent)) {
        emit errorOccurred(QStringLiteral("保存数据包含无效的时间、峰值吸光度或光强百分比。"));
        return false;
    }

    QString error;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_accepting || !m_sessionActive) return false;
        if (m_queue.size() >= kCapacityPoints) {
            m_accepting = false;
            m_closeRequested = true;
            m_condition.wakeAll();
            error = QStringLiteral(
                "保存队列已满，本条数据未能接收。已停止实时保存，"
                "正在写完之前接收的数据，请检查存储设备速度。");
        } else {
            m_queue.enqueue(point);
            m_condition.wakeOne();
            return true;
        }
    }
    emit errorOccurred(error);
    return false;
}

void PeakDataWriter::stopSaving()
{
    QMutexLocker locker(&m_mutex);
    m_accepting = false;
    m_closeRequested = true;
    m_condition.wakeAll();
}

void PeakDataWriter::drainAndWait()
{
    stopSaving();
    if (QThread::currentThread() != this) wait();
}

bool PeakDataWriter::isSaving() const
{
    QMutexLocker locker(&m_mutex);
    return m_sessionActive;
}

void PeakDataWriter::run()
{
    QString filePath;
    {
        QMutexLocker locker(&m_mutex);
        filePath = m_filePath;
    }

    QFile file(filePath);
    QString error;
    qint64 savedCount = 0;
    qsizetype failedPoints = 0;
    if (prepareFile(file, &error)) {
        emit savingStarted(filePath);
        for (;;) {
            PeakAbsorbancePoint point;
            {
                QMutexLocker locker(&m_mutex);
                while (m_queue.isEmpty() && !m_closeRequested) {
                    m_condition.wait(&m_mutex);
                }
                if (m_queue.isEmpty() && m_closeRequested) break;
                point = m_queue.dequeue();
            }
            const QByteArray row = serializePoint(point);
            if (file.write(row) != row.size() || !file.flush()) {
                error = file.errorString();
                failedPoints = 1;
                break;
            }
            emit pointSaved(++savedCount);
        }
    }
    file.close();

    {
        QMutexLocker locker(&m_mutex);
        m_accepting = false;
        m_sessionActive = false;
        failedPoints += m_queue.size();
        m_queue.clear();
    }
    if (!error.isEmpty()) {
        emit errorOccurred(QStringLiteral("无法保存到 %1：%2（%3 条数据未写入，已停止实时保存。）")
                               .arg(filePath, error).arg(failedPoints));
    }
    emit savingStopped(filePath, savedCount);
}

} // namespace tdlas
