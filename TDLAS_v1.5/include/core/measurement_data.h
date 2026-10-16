#pragma once

#include <QVector>

namespace tdlas {

struct MeasurementData
{
    QVector<double> samples;
    double sampleRate = 0.0;
};

} // namespace tdlas
