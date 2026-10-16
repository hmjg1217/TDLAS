#include "core/peak_absorbance_history.h"

#include <QFile>
#include <QLocale>
#include <QTextStream>

#include <cmath>

namespace tdlas {

bool PeakAbsorbanceHistory::record(const QDateTime &timestamp,
                                   double peakAbsorbance,
                                   double lightPercent)
{
    if (!timestamp.isValid() || !std::isfinite(peakAbsorbance)
        || !std::isfinite(lightPercent)) return false;
    m_points.append(PeakAbsorbancePoint{timestamp, peakAbsorbance, lightPercent});
    return true;
}

bool PeakAbsorbanceHistory::record(double peakAbsorbance, double lightPercent)
{
    return record(QDateTime::currentDateTime(), peakAbsorbance, lightPercent);
}

bool PeakAbsorbanceHistory::record(const QDateTime &timestamp,
                                   double peakAbsorbance)
{
    if (!timestamp.isValid() || !std::isfinite(peakAbsorbance)) return false;
    m_points.append(PeakAbsorbancePoint{timestamp, peakAbsorbance});
    return true;
}

bool PeakAbsorbanceHistory::record(double peakAbsorbance)
{
    return record(QDateTime::currentDateTime(), peakAbsorbance);
}

bool PeakAbsorbanceHistory::append(double peakAbsorbance,
                                   const QDateTime &timestamp)
{
    return record(timestamp, peakAbsorbance);
}

void PeakAbsorbanceHistory::clear()
{
    m_points.clear();
}

qsizetype PeakAbsorbanceHistory::size() const
{
    return m_points.size();
}

bool PeakAbsorbanceHistory::isEmpty() const
{
    return m_points.isEmpty();
}

QVector<PeakAbsorbancePoint> PeakAbsorbanceHistory::points() const
{
    return m_points;
}

QVector<PeakAbsorbancePoint> PeakAbsorbanceHistory::recent(
    int minutes,
    const QDateTime &endTime) const
{
    if (minutes <= 0 || !endTime.isValid()) return {};

    const QDateTime beginTime = endTime.addSecs(-static_cast<qint64>(minutes) * 60);
    QVector<PeakAbsorbancePoint> result;
    result.reserve(m_points.size());
    for (const PeakAbsorbancePoint &point : m_points) {
        if (point.timestamp >= beginTime && point.timestamp <= endTime) {
            result.append(point);
        }
    }
    return result;
}

QVector<PeakAbsorbancePoint> PeakAbsorbanceHistory::recentPoints(
    int minutes,
    const QDateTime &endTime) const
{
    return recent(minutes, endTime);
}

bool PeakAbsorbanceHistory::saveToFile(const QString &filePath,
                                       QString *errorMessage) const
{
    if (errorMessage != nullptr) errorMessage->clear();
    if (filePath.trimmed().isEmpty()) {
        if (errorMessage != nullptr) *errorMessage = QStringLiteral("保存路径为空。");
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorMessage != nullptr) *errorMessage = file.errorString();
        return false;
    }

    QTextStream stream(&file);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif
    stream << QStringLiteral("time,peakAbsorbance,lightPercent\n");
    const QLocale locale = QLocale::c();
    for (const PeakAbsorbancePoint &point : m_points) {
        stream << point.timestamp.toString(Qt::ISODateWithMs)
               << QLatin1Char(',')
               << locale.toString(point.peakAbsorbance, 'g', 17)
               << QLatin1Char(',');
        if (std::isfinite(point.lightPercent)) {
            stream << locale.toString(point.lightPercent, 'g', 17);
        }
        stream << QLatin1Char('\n');
    }
    stream.flush();
    if (stream.status() != QTextStream::Ok || !file.flush()) {
        if (errorMessage != nullptr) *errorMessage = file.errorString();
        file.close();
        return false;
    }
    file.close();
    if (file.error() != QFile::NoError) {
        if (errorMessage != nullptr) *errorMessage = file.errorString();
        return false;
    }
    return true;
}

bool PeakAbsorbanceHistory::saveToTextFile(const QString &filePath,
                                           QString *errorMessage) const
{
    return saveToFile(filePath, errorMessage);
}

} // namespace tdlas
