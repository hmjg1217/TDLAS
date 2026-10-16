#include "ui/main_window.h"

#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cmath>
#include <functional>
#include <limits>

namespace {

bool check(bool condition, const char *description)
{
    if (!condition) qWarning() << description;
    return condition;
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 5000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    return condition();
}

QStringList readLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

QDialog *visibleDialog(const QString &title)
{
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (auto *dialog = qobject_cast<QDialog *>(widget)) {
            if (dialog->isVisible() && dialog->windowTitle() == title) return dialog;
        }
    }
    return nullptr;
}

bool deliverResult(MainWindow &window, double peak, double light, bool valid = true)
{
    tdlas::TDLASResult result;
    result.valid = valid;
    result.peakAbsorbance = peak;
    result.lightPercent = light;
    return QMetaObject::invokeMethod(&window, "handleProcessingResult", Qt::DirectConnection,
                                     Q_ARG(tdlas::TDLASResult, result),
                                     Q_ARG(double, 100000.0), Q_ARG(int, 20));
}

bool selectSavePath(QPushButton *button, const QString &path)
{
    // Use Qt's non-native file picker inside the test process; no desktop or
    // real acquisition controls are involved.
    bool selected = false;
    QTimer selector;
    selector.setInterval(10);
    QObject::connect(&selector, &QTimer::timeout, &selector, [&]() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *dialog = qobject_cast<QFileDialog *>(widget)) {
                if (!dialog->isVisible()) continue;
                dialog->selectFile(path);
                selected = QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                selector.stop();
                return;
            }
        }
    });
    // A failed selector must not leave a modal dialog hanging in CI.
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &timeout, []() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *dialog = qobject_cast<QFileDialog *>(widget)) dialog->reject();
        }
    });
    selector.start();
    timeout.start(5000);
    button->click();
    return selected;
}

bool runUiCase(const QString &filePath)
{
    MainWindow window;
    window.show();
    QApplication::processEvents();

    auto *stack = window.findChild<QStackedWidget *>(QStringLiteral("chartStack"));
    auto *peakPanel = window.findChild<QGroupBox *>(QStringLiteral("peakDataPanel"));
    auto *peakButton = window.findChild<QPushButton *>(QStringLiteral("peakDataButton"));
    auto *pathButton = window.findChild<QPushButton *>(QStringLiteral("peakSavePathButton"));
    auto *saveButton = window.findChild<QPushButton *>(QStringLiteral("peakSaveButton"));
    if (!check(stack && peakPanel && peakButton && pathButton && saveButton,
               "Missing embedded peak panel controls")) return false;

    QWidget *waveformPage = stack->currentWidget();
    if (!check(waveformPage != peakPanel, "Peak panel replaced initial waveform view")) return false;
    peakButton->click();
    if (!check(stack->currentWidget() == peakPanel && peakPanel->isVisible(),
               "Peak view is not embedded in the chart area")
        || !check(visibleDialog(QStringLiteral("峰值数据")) == nullptr,
                  "Peak view opened a separate dialog")) return false;

    auto *rangeCombo = peakPanel->findChild<QComboBox *>();
    if (!check(rangeCombo && rangeCombo->count() == 2, "Missing peak display range")) return false;
    rangeCombo->setCurrentIndex(1);
    rangeCombo->setCurrentIndex(0);
    peakButton->click();
    if (!check(stack->currentWidget() == waveformPage && waveformPage->isVisible(),
               "Returning to waveform view failed")) return false;

    if (!check(QMetaObject::invokeMethod(&window, "toggleChartWindow", Qt::DirectConnection),
               "Cannot detach waveform view")) return false;
    QApplication::processEvents();
    if (!check(visibleDialog(QStringLiteral("原始波形与拟合图")) != nullptr,
               "Existing waveform detachment no longer works")) return false;
    peakButton->click();
    if (!check(stack->currentWidget() == peakPanel,
               "Peak view did not return to embedded chart area")
        || !check(visibleDialog(QStringLiteral("原始波形与拟合图")) == nullptr,
                  "Detached waveform dialog stayed open on peak view switch")) return false;

    // Results before enabling saving belong only to the chart history.
    if (!check(deliverResult(window, 0.099, 50.0), "Cannot inject synthetic fitting result")) return false;
    saveButton->click();
    if (!check(!QFile::exists(filePath), "Saving without an address created a file")
        || !check(visibleDialog(QStringLiteral("选择峰值数据保存地址")) == nullptr,
                  "Data save duplicated the address picker")) return false;
    if (!check(selectSavePath(pathButton, filePath), "Cannot choose temporary save path")
        || !check(!QFile::exists(filePath), "Selecting an address saved data prematurely")) return false;

    saveButton->click();
    // Arrive immediately after the click, while the worker may still be
    // opening the file. This first accepted result must not be lost.
    if (!check(deliverResult(window, 0.00125, 81.5), "Cannot inject first streaming result")
        || !check(waitUntil([&]() { return readLines(filePath).size() == 2; }),
                  "First result was not flushed while saving was active")
        || !check(!pathButton->isEnabled(), "Address can change during an active save")) return false;

    QStringList lines = readLines(filePath);
    const QStringList columns = lines.at(1).trimmed().split(QLatin1Char(','));
    if (!check(lines.at(0).trimmed() == QStringLiteral("time,peakAbsorbance,lightPercent"),
               "Streaming file does not have the three-column header")
        || !check(columns.size() == 3, "Streaming row is not three columns")
        || !check(QDateTime::fromString(columns.at(0), Qt::ISODateWithMs).isValid(),
                  "Streaming row has no valid timestamp")
        || !check(std::abs(columns.at(1).toDouble() - 0.00125) < 1e-12,
                  "Streaming peak value does not match its result")
        || !check(std::abs(columns.at(2).toDouble() - 81.5) < 1e-12,
                  "Streaming light percentage does not match its result")) return false;

    // Save every valid result even while the peak panel is hidden and the
    // chart repaint timer is stopped (no acquisition is ever started).
    peakButton->click();
    for (int index = 0; index < 5; ++index) {
        if (!deliverResult(window, 0.002 + index * 0.001, 82.0 + index)) return false;
    }
    if (!check(deliverResult(window, 99.0, 99.0, false), "Cannot inject invalid result")
        || !check(deliverResult(window, 99.0, std::numeric_limits<double>::quiet_NaN()),
                  "Cannot inject invalid light result")
        || !check(waitUntil([&]() { return readLines(filePath).size() == 7; }),
                  "Hidden-view results were lost or invalid results were saved")) return false;

    // Stopping is ordered after the accepted queue. Further results are not
    // saved until explicitly enabled again.
    if (!check(waitUntil([&]() { return saveButton->isEnabled(); }),
               "Save button did not finish opening")) return false;
    saveButton->click();
    if (!check(deliverResult(window, 0.25, 25.0), "Cannot inject stopped result")
        || !check(waitUntil([&]() { return pathButton->isEnabled(); }),
                  "Save stop did not drain and re-enable address selection")
        || !check(readLines(filePath).size() == 7, "A result was saved after stop")) return false;

    // Exercise a fast restart right after the stop notification. The new
    // session must become active even as the previous thread is returning
    // from its run() function.
    saveButton->click();
    if (!check(waitUntil([&]() { return saveButton->text() == QStringLiteral("停止保存"); }),
               "Rapid restart did not activate a new saving session")
        || !check(deliverResult(window, 0.5, 90.0), "Cannot inject resumed result")
        || !check(waitUntil([&]() { return readLines(filePath).size() == 8; }),
                  "Resuming save did not append without duplicating history")) return false;

    // Close immediately after queueing: shutdown must finish accepted writes.
    for (int index = 0; index < 10; ++index) {
        if (!deliverResult(window, 0.6 + index, 91.0)) return false;
    }
    window.close();
    return check(readLines(filePath).size() == 18, "Application close lost queued save records");
}

} // namespace

int main(int argc, char **argv)
{
#if TDLAS_HAS_DAQMX
    Q_UNUSED(argc);
    Q_UNUSED(argv);
    qWarning() << "UI test requires TDLAS_ENABLE_DAQMX=OFF; refusing hardware enumeration";
    return 77;
#else
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("TDLAS-Hardware-Free-Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("MainWindowTest"));
    QTemporaryDir directory;
    if (!check(directory.isValid(), "Temporary test directory failed")) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    return runUiCase(directory.filePath(QStringLiteral("live_peaks.txt"))) ? 0 : 1;
#endif
}
