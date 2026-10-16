#pragma once

#include <QDateTime>
#include <QVector>

#include <limits>

namespace tdlas {

// One completed TDLAS fitting result sampled at a wall-clock time.
struct PeakAbsorbancePoint
{
    QDateTime timestamp;
    double peakAbsorbance = std::numeric_limits<double>::quiet_NaN();
    double lightPercent = std::numeric_limits<double>::quiet_NaN();
};

// Stores all valid peak results for the current application session.  The
// complete history is kept for export; callers can request a time-limited
// copy for the live chart without dropping older samples.
class PeakAbsorbanceHistory
{
public:
    // Records all values belonging to the same completed fitting result.
    // Returns false if any of the three fields is invalid/non-finite.
    bool record(const QDateTime &timestamp, double peakAbsorbance,
                double lightPercent);
    bool record(double peakAbsorbance, double lightPercent);

    // Returns false for an invalid timestamp or a non-finite peak value.
    // Legacy overloads have no light measurement; exports leave it empty.
    bool record(const QDateTime &timestamp, double peakAbsorbance);

    // Convenience overload for live acquisition code.
    bool record(double peakAbsorbance);

    // Alias retained for callers that prefer append terminology.
    bool append(double peakAbsorbance,
                const QDateTime &timestamp = QDateTime::currentDateTime());

    void clear();
    qsizetype size() const;
    bool isEmpty() const;

    // Returns a copy in insertion order.  This is the complete session
    // history and is suitable for export.
    QVector<PeakAbsorbancePoint> points() const;

    // Returns points in [endTime - minutes, endTime].  A non-positive window
    // yields an empty vector.  The default end time is the current local time.
    QVector<PeakAbsorbancePoint> recent(
        int minutes,
        const QDateTime &endTime = QDateTime::currentDateTime()) const;

    // Alias retained for readability at call sites.
    QVector<PeakAbsorbancePoint> recentPoints(
        int minutes,
        const QDateTime &endTime = QDateTime::currentDateTime()) const;

    // Writes every recorded point as UTF-8 CSV text:
    // time,peakAbsorbance,lightPercent\n
    // The timestamp uses ISO 8601 with milliseconds and the number uses a
    // locale-independent decimal representation.
    bool saveToFile(const QString &filePath, QString *errorMessage = nullptr) const;

    // Alias retained for callers that want to make the text format explicit.
    bool saveToTextFile(const QString &filePath, QString *errorMessage = nullptr) const;

private:
    QVector<PeakAbsorbancePoint> m_points;
};

} // namespace tdlas
