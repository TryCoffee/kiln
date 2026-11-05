// mainwindow.hpp — Kiln's window. Never privileged: it surveys drives and
// inspects images as the user, and hands the write to kiln-helper (Linux:
// pkexec; Windows: UAC, events over a named pipe; macOS: in-process with an
// authopen descriptor).
#pragma once

#include "device.hpp"
#include "helper_protocol.hpp"
#include "image.hpp"
#include "windows.hpp"

#include <QFutureWatcher>
#include <QMainWindow>
#include <QProcess>
#include <QUrl>

#include <atomic>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLocalServer;
class QLocalSocket;
class QWinEventNotifier;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QTimer;

namespace kiln {

// What the worker thread learns about the chosen image.
struct Inspected {
    QString path;
    ImageInfo info;
    std::optional<WindowsIsoInfo> windows;
    QString error;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // For screenshots/tests: choose an image without the file dialog.
    void openImage(const QString& path);
    // For screenshots/tests: switch to making a Ventoy disk.
    void chooseVentoy();
    // Ask GitHub once whether a newer Kiln is out; a popup if so (after any running write).
    void checkForUpdates();

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void refreshDevices();
    void inspected();
    void updateState();
    void modeChanged();
    bool ventoyMode() const;
    void start();
    void cancel();
    void helperOutput();
    void helperBytes(const QByteArray& bytes);
    void helperFinished(int code, QProcess::ExitStatus status);
    void helperGone(const QString& why);  // the writer ended without a result: say so
#ifdef _WIN32
    void startWindowsHelper(const HelperRequest& req);
    void windowsHelperExited();
#endif
    void handleEvent(const HelperEvent& e);
    void jobFinished();
    void finishRun();
    void setBusy(bool busy);
    void log(const QString& line);
    QString helperPath() const;
    void loadSettings();
    void saveSettings() const;
    void showUpdate();

    QComboBox* devices_ = nullptr;
    QLabel* deviceNote_ = nullptr;
    QRadioButton* modeImage_ = nullptr;
    QRadioButton* modeVentoy_ = nullptr;
    QGroupBox* imageBox_ = nullptr;
    QLineEdit* imageEdit_ = nullptr;
    QPushButton* browse_ = nullptr;
    QLabel* imageInfo_ = nullptr;
    QGroupBox* windowsBox_ = nullptr;
    QCheckBox* bypass_ = nullptr;
    QCheckBox* noMsAccount_ = nullptr;
    QCheckBox* localAccount_ = nullptr;
    QLineEdit* accountName_ = nullptr;
    QCheckBox* noDataCollection_ = nullptr;
    QCheckBox* noBitlocker_ = nullptr;
    QCheckBox* declutter_ = nullptr;
    QGroupBox* ventoyBox_ = nullptr;
    QCheckBox* ventoySecureBoot_ = nullptr;
    QCheckBox* ventoyGpt_ = nullptr;
    QLineEdit* ventoyLabel_ = nullptr;
    QCheckBox* verify_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QTimer* poll_ = nullptr;

    std::vector<Device> deviceList_;
    std::optional<Inspected> image_;
    QFutureWatcher<Inspected> inspecting_;
    bool busy_ = false;
    bool closeWhenDone_ = false;
    QProcess* helper_ = nullptr;          // Linux: pkexec kiln-helper
#ifdef _WIN32
    QLocalServer* pipeServer_ = nullptr;  // the pipe the elevated helper reports on
    QLocalSocket* pipe_ = nullptr;
    void* helperProcess_ = nullptr;       // its process HANDLE
    unsigned long helperPid_ = 0;
    QWinEventNotifier* helperExit_ = nullptr;
#endif
    std::atomic<bool> cancelJob_{false};  // macOS: the in-process job
    QFutureWatcher<int> job_;
    QByteArray pending_;          // helper stdout not yet ending in a newline
    bool gotResult_ = false;
    QString notes_;               // helper messages worth repeating in the final dialog
    int stage_ = -1;              // current Stage, for the speed figure
    qint64 stageStartMs_ = 0;
    QString writingTo_;
    QString updateVersion_;       // a newer release not shown yet (held during a write)
    QUrl updatePage_;
};

}  // namespace kiln
