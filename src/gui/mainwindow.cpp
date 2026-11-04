// mainwindow.cpp
#include "mainwindow.hpp"

#include "blockio.hpp"
#include "helper_protocol.hpp"
#include "job.hpp"
#include "unattend.hpp"
#include "ventoy.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#ifdef _WIN32
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QUuid>
#include <QWinEventNotifier>

#include <windows.h>
#include <shellapi.h>
#endif

namespace kiln {
namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString stageText(Stage s) {
    switch (s) {
        case Stage::hashing: return MainWindow::tr("Checking the image");
        case Stage::writing: return MainWindow::tr("Writing");
        case Stage::syncing: return MainWindow::tr("Flushing to the drive");
        case Stage::verifying: return MainWindow::tr("Verifying");
        case Stage::splitting: return MainWindow::tr("Splitting install.wim");
        case Stage::customizing: return MainWindow::tr("Customizing Windows setup");
        case Stage::partitioning: return MainWindow::tr("Partitioning");
        case Stage::copying: return MainWindow::tr("Copying files");
    }
    return {};
}

// Stages with no meaningful byte count get a busy bar.
bool indeterminate(Stage s) {
    return s == Stage::syncing || s == Stage::partitioning || s == Stage::customizing;
}

// "hybrid ISO" -> "Hybrid ISO"
QString kindText(ImageKind k) {
    QString s = QString::fromUtf8(kind_name(k));
    if (!s.isEmpty()) s[0] = s[0].toUpper();
    return s;
}

QString describe(const Device& d) {
    QString label = qs(d.vendor.empty() ? d.model : d.vendor + " " + d.model).trimmed();
    if (label.isEmpty()) label = MainWindow::tr("Unnamed drive");
    return QStringLiteral("%1 — %2 (%3)").arg(label, qs(format_size(d.size_bytes)), qs(d.name));
}

// The Ventoy partition's name as typed; empty means Ventoy's default.
QString ventoyLabelText(const QLineEdit* e) {
    const QString t = e->text().trimmed();
    return t.isEmpty() ? QStringLiteral("Ventoy") : t;
}

Inspected inspect(const QString& path) {
    Inspected r;
    r.path = path;
    try {
        const std::string p = path.toStdString();
        r.info = inspect_image(p);
        r.windows = windows_iso_info(r.info, p);
    } catch (const std::exception& e) {
        r.error = qs(e.what());
    }
    return r;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("Kiln"));
    auto* central = new QWidget(this);
    auto* v = new QVBoxLayout(central);

    // Drive
    auto* driveBox = new QGroupBox(tr("Drive"));
    auto* dv = new QVBoxLayout(driveBox);
    auto* drow = new QHBoxLayout;
    devices_ = new QComboBox;
    devices_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* refresh = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), QString());
    if (refresh->icon().isNull()) refresh->setText(tr("Refresh"));
    refresh->setToolTip(tr("Look for drives again"));
    drow->addWidget(devices_);
    drow->addWidget(refresh);
    dv->addLayout(drow);
    deviceNote_ = new QLabel;
    deviceNote_->setWordWrap(true);
    dv->addWidget(deviceNote_);
    v->addWidget(driveBox);

    // What to write: an image, or Ventoy (which boots the ISO files copied onto it)
    auto* mrow = new QHBoxLayout;
    modeImage_ = new QRadioButton(tr("Write an image"));
    modeVentoy_ = new QRadioButton(tr("Make a Ventoy disk"));
    modeVentoy_->setToolTip(tr("Ventoy boots any ISO file you copy onto the drive afterwards, from a menu"));
    modeImage_->setChecked(true);
    mrow->addWidget(modeImage_);
    mrow->addWidget(modeVentoy_);
    mrow->addStretch();
    v->addLayout(mrow);

    // Image
    imageBox_ = new QGroupBox(tr("Image"));
    auto* iv = new QVBoxLayout(imageBox_);
    auto* irow = new QHBoxLayout;
    imageEdit_ = new QLineEdit;
    imageEdit_->setReadOnly(true);
    imageEdit_->setPlaceholderText(tr("An .iso or .img file"));
    browse_ = new QPushButton(tr("Choose…"));
    irow->addWidget(imageEdit_);
    irow->addWidget(browse_);
    iv->addLayout(irow);
    imageInfo_ = new QLabel;
    imageInfo_->setWordWrap(true);
    imageInfo_->setTextFormat(Qt::RichText);
    // Wrap at whatever width the window has instead of asking for the text's
    // full width (macOS's wider font clipped the line otherwise).
    imageInfo_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    iv->addWidget(imageInfo_);
    v->addWidget(imageBox_);

    // Windows setup tweaks (Rufus's set), shown for Windows ISOs only
    windowsBox_ = new QGroupBox(tr("Windows setup"));
    auto* wv = new QVBoxLayout(windowsBox_);
    bypass_ = new QCheckBox(tr("Skip the TPM 2.0, Secure Boot and 4 GB RAM requirements"));
    noMsAccount_ = new QCheckBox(tr("Allow setup without a Microsoft account"));
    auto* arow = new QHBoxLayout;
    localAccount_ = new QCheckBox(tr("Create a local account:"));
    accountName_ = new QLineEdit;
    accountName_->setPlaceholderText(tr("user name"));
    accountName_->setToolTip(tr("An administrator with an empty password, to be changed at first sign-in"));
    arow->addWidget(localAccount_);
    arow->addWidget(accountName_, 1);
    noDataCollection_ = new QCheckBox(tr("Answer the privacy questions with “no”"));
    noBitlocker_ = new QCheckBox(tr("No automatic BitLocker encryption"));
    declutter_ = new QCheckBox(tr("Declutter: OneDrive, Outlook, Teams, Copilot, news and ads off"));
    wv->addWidget(bypass_);
    wv->addWidget(noMsAccount_);
    wv->addLayout(arow);
    wv->addWidget(noDataCollection_);
    wv->addWidget(noBitlocker_);
    wv->addWidget(declutter_);
    windowsBox_->setVisible(false);
    v->addWidget(windowsBox_);

    // Ventoy
    ventoyBox_ = new QGroupBox(tr("Ventoy %1").arg(QString::fromLatin1(ventoy_version())));
    auto* vv = new QVBoxLayout(ventoyBox_);
    auto* vinfo = new QLabel(tr("The drive gets Ventoy's boot menu and an exFAT partition. Copy ISO files onto "
                                "that partition afterwards: Ventoy lists them at boot and starts the one you pick."));
    vinfo->setWordWrap(true);
    vinfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    ventoySecureBoot_ = new QCheckBox(tr("Secure Boot support (enrol Ventoy's key once, at the first boot)"));
    ventoyGpt_ = new QCheckBox(tr("GPT partition table (default MBR, which older PCs boot too)"));
    auto* lrow = new QHBoxLayout;
    ventoyLabel_ = new QLineEdit;
    ventoyLabel_->setMaxLength(11);
    ventoyLabel_->setPlaceholderText(QStringLiteral("Ventoy"));
    lrow->addWidget(new QLabel(tr("Name of the ISO partition:")));
    lrow->addWidget(ventoyLabel_, 1);
    vv->addWidget(vinfo);
    vv->addWidget(ventoySecureBoot_);
    vv->addWidget(ventoyGpt_);
    vv->addLayout(lrow);
    ventoyBox_->setVisible(false);
    v->addWidget(ventoyBox_);

    verify_ = new QCheckBox(tr("Read the drive back afterwards and compare (recommended)"));
    v->addWidget(verify_);

    status_ = new QLabel;
    status_->setWordWrap(true);
    bar_ = new QProgressBar;
    bar_->setRange(0, 1000);
    bar_->setValue(0);
    bar_->setTextVisible(false);
    v->addWidget(status_);
    v->addWidget(bar_);

    auto* brow = new QHBoxLayout;
    brow->addStretch();
    cancelButton_ = new QPushButton(tr("Cancel"));
    cancelButton_->setEnabled(false);
    startButton_ = new QPushButton(QIcon::fromTheme(QStringLiteral("media-flash")), tr("Write"));
    startButton_->setDefault(true);
    brow->addWidget(cancelButton_);
    brow->addWidget(startButton_);
    v->addLayout(brow);

    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(500);
    log_->setFixedHeight(fontMetrics().lineSpacing() * 6);
    v->addWidget(log_);

    setCentralWidget(central);
    setMinimumWidth(560);

    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(devices_, &QComboBox::currentIndexChanged, this, &MainWindow::updateState);
    connect(browse_, &QPushButton::clicked, this, [this] {
        QSettings s;
        const QString dir = s.value(QStringLiteral("lastDir"), QDir::homePath()).toString();
        const QString f = QFileDialog::getOpenFileName(this, tr("Choose an image"), dir,
                                                       tr("Disk images (*.iso *.img *.raw *.bin);;All files (*)"));
        if (!f.isEmpty()) openImage(f);
    });
    for (QCheckBox* c : {bypass_, noMsAccount_, localAccount_, noDataCollection_, noBitlocker_, declutter_, verify_})
        connect(c, &QCheckBox::toggled, this, &MainWindow::updateState);
    connect(accountName_, &QLineEdit::textChanged, this, &MainWindow::updateState);
    connect(ventoyLabel_, &QLineEdit::textChanged, this, &MainWindow::updateState);
    connect(modeVentoy_, &QRadioButton::toggled, this, &MainWindow::modeChanged);
    connect(startButton_, &QPushButton::clicked, this, &MainWindow::start);
    connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::cancel);
    connect(&inspecting_, &QFutureWatcher<Inspected>::finished, this, &MainWindow::inspected);
    connect(&job_, &QFutureWatcher<int>::finished, this, &MainWindow::jobFinished);

    poll_ = new QTimer(this);
    poll_->setInterval(2000);
    connect(poll_, &QTimer::timeout, this, &MainWindow::refreshDevices);
    poll_->start();

    loadSettings();
    refreshDevices();
    updateState();
}

void MainWindow::refreshDevices() {
    if (busy_) return;  // the list must not move under a running write
    std::vector<Device> now;
    try {
        now = list_devices();
    } catch (const std::exception& e) {
        log(tr("Could not list drives: %1").arg(qs(e.what())));
    }
    auto key = [](const std::vector<Device>& l) {
        QStringList k;
        for (const auto& d : l) k << qs(d.name + ":" + std::to_string(d.size_bytes) + ":" + std::to_string(d.mounts.size()));
        return k.join(QLatin1Char(','));
    };
    if (key(now) == key(deviceList_) && devices_->count() > 0) return;
    const QString selected = devices_->currentData().toString();
    deviceList_ = std::move(now);
    devices_->blockSignals(true);
    devices_->clear();
    for (const auto& d : deviceList_) devices_->addItem(QIcon::fromTheme(QStringLiteral("drive-removable-media-usb")), describe(d), qs(d.name));
    if (deviceList_.empty()) devices_->addItem(tr("No USB drive or SD card found"));
    const int at = devices_->findData(selected);
    devices_->setCurrentIndex(at >= 0 ? at : 0);
    devices_->blockSignals(false);
    updateState();
}

bool MainWindow::ventoyMode() const { return modeVentoy_->isChecked(); }

void MainWindow::modeChanged() {
    const bool ventoy = ventoyMode();
    imageBox_->setVisible(!ventoy);
    windowsBox_->setVisible(!ventoy && image_ && image_->error.isEmpty() && image_->windows.has_value());
    ventoyBox_->setVisible(ventoy);
    startButton_->setText(ventoy ? tr("Make Ventoy disk") : tr("Write"));
    updateState();
    QTimer::singleShot(0, this, [this] { resize(width(), sizeHint().height()); });
}

void MainWindow::chooseVentoy() { modeVentoy_->setChecked(true); }

void MainWindow::openImage(const QString& path) {
    modeImage_->setChecked(true);
    imageEdit_->setText(path);
    image_.reset();
    windowsBox_->setVisible(false);
    imageInfo_->setText(tr("Reading the image…"));
    QSettings().setValue(QStringLiteral("lastDir"), QFileInfo(path).absolutePath());
    inspecting_.setFuture(QtConcurrent::run(inspect, path));
    updateState();
}

void MainWindow::inspected() {
    const Inspected r = inspecting_.result();
    if (r.path != imageEdit_->text()) return;  // a newer choice is on its way
    image_ = r;
    const ImageInfo& i = r.info;
    if (!r.error.isEmpty()) {
        imageInfo_->setText(QStringLiteral("<span style='color:#c0392b'>%1</span>").arg(r.error.toHtmlEscaped()));
    } else if (r.windows) {
        const WindowsIsoInfo& w = *r.windows;
        const QString editions_text = w.editions.size() == 1 ? tr("1 edition")
                                                              : tr("%1 editions").arg(w.editions.size());
        imageInfo_->setText(tr("<b>Windows install ISO</b> · %1 · %2 · %3<br>Kiln builds the stick from its files "
                               "(GPT + FAT32, UEFI boot)%4. Needs a drive of at least %5.")
                                .arg(qs(i.volume_label).toHtmlEscaped(), qs(format_size(i.size_bytes)), editions_text,
                                     w.needs_split ? tr("; install.wim is split for FAT32") : QString(),
                                     qs(format_size(windows_stick_bytes_needed(w)))));
        QStringList editions;
        for (const auto& e : w.editions) editions << qs(e);
        imageInfo_->setToolTip(editions.join(QLatin1Char('\n')));
    } else if (raw_write_boots(i.kind)) {
        imageInfo_->setText(tr("<b>%1</b>%2 · %3<br>Written to the drive as it is.")
                                .arg(kindText(i.kind),
                                     i.volume_label.empty() ? QString() : QStringLiteral(" · ") + qs(i.volume_label).toHtmlEscaped(),
                                     qs(format_size(i.size_bytes))));
        imageInfo_->setToolTip({});
    } else {
        imageInfo_->setText(tr("<b>%1</b> · %2<br><span style='color:#c0392b'>Written as it is, this would not boot "
                               "(no partition table), and it is not a Windows install ISO.</span>")
                                .arg(kindText(i.kind), qs(format_size(i.size_bytes))));
        imageInfo_->setToolTip({});
    }
    windowsBox_->setVisible(!ventoyMode() && r.error.isEmpty() && r.windows.has_value());
    updateState();
    // Grow (or shrink) to the new content's height; the width stays the user's.
    QTimer::singleShot(0, this, [this] { resize(width(), sizeHint().height()); });
}

void MainWindow::updateState() {
    accountName_->setEnabled(localAccount_->isChecked() && !busy_);
    const int idx = devices_->currentIndex();
    const Device* dev = idx >= 0 && idx < static_cast<int>(deviceList_.size()) ? &deviceList_[static_cast<size_t>(idx)] : nullptr;
    if (dev && !dev->mounts.empty()) {
        QStringList m;
        for (const auto& x : dev->mounts) m << qs(x.mountpoint);
        deviceNote_->setText(tr("Mounted at %1: it will be unmounted.").arg(m.join(QStringLiteral(", "))));
    } else {
        deviceNote_->setText(dev ? QString() : tr("Plug in a USB drive or an SD card."));
    }
    if (busy_) return;

    QString problem;
    if (!dev) problem = tr("Choose a drive.");
    else if (dev->read_only) problem = tr("The drive is read-only (a lock switch on the card or adapter?).");
    else if (ventoyMode()) {
        const std::string why = ventoy_label_problem(ventoyLabelText(ventoyLabel_).toStdString());
        if (!why.empty()) problem = tr("Partition name: %1.").arg(qs(why));
        else if (ventoy_min_bytes() > dev->size_bytes)
            problem = tr("The drive is too small: Ventoy needs %1.").arg(qs(format_size(ventoy_min_bytes())));
        startButton_->setEnabled(problem.isEmpty());
        status_->setText(problem);
        return;
    } else if (!image_) problem = inspecting_.isRunning() ? QString() : tr("Choose an image.");
    else if (!image_->error.isEmpty()) problem = tr("The image cannot be used.");
    else if (!image_->windows && !raw_write_boots(image_->info.kind)) problem = tr("This image would not boot.");
    else {
        const uint64_t need = image_->windows ? windows_stick_bytes_needed(*image_->windows) : image_->info.size_bytes;
        if (need > dev->size_bytes)
            problem = tr("The drive is too small: this needs %1.").arg(qs(format_size(need)));
        else if (image_->windows && localAccount_->isChecked()) {
            std::string why;
            if (sanitize_account_name(accountName_->text().toStdString(), &why).empty())
                problem = tr("Local account: %1.").arg(qs(why));
        }
    }
    startButton_->setEnabled(problem.isEmpty() && image_.has_value() && dev);
    status_->setText(problem);
}

QString MainWindow::helperPath() const {
    const QByteArray env = qgetenv("KILN_HELPER");
    if (!env.isEmpty()) return QString::fromLocal8Bit(env);
#ifdef _WIN32
    const QString beside = QCoreApplication::applicationDirPath() + QStringLiteral("/kiln-helper.exe");
#else
    const QString beside = QCoreApplication::applicationDirPath() + QStringLiteral("/kiln-helper");
#endif
    if (QFileInfo::exists(beside)) return beside;
    return QStringLiteral(KILN_LIBEXEC_DIR "/kiln-helper");
}

void MainWindow::start() {
    const int idx = devices_->currentIndex();
    const bool ventoy = ventoyMode();
    if ((!ventoy && !image_) || idx < 0 || idx >= static_cast<int>(deviceList_.size())) return;
    const Device dev = deviceList_[static_cast<size_t>(idx)];

    HelperRequest req;
    req.device = dev.name;
    req.verify = verify_->isChecked();
    if (ventoy) {
        req.ventoy = true;
        req.ventoy_setup.secure_boot = ventoySecureBoot_->isChecked();
        req.ventoy_setup.gpt = ventoyGpt_->isChecked();
        req.ventoy_setup.label = ventoyLabelText(ventoyLabel_).toStdString();
    } else {
        req.image = image_->path.toStdString();
    }
    if (!ventoy && image_->windows) {
        req.custom.bypass_checks = bypass_->isChecked();
        req.custom.no_online_account = noMsAccount_->isChecked();
        if (localAccount_->isChecked()) req.custom.local_account = sanitize_account_name(accountName_->text().toStdString());
        req.custom.no_data_collection = noDataCollection_->isChecked();
        req.custom.no_bitlocker = noBitlocker_->isChecked();
        req.custom.declutter = declutter_->isChecked();
    }

    QString text = tr("Everything on <b>%1</b> will be erased.").arg(describe(dev).toHtmlEscaped());
    if (!dev.mounts.empty()) text += tr("<br>It is mounted now and will be unmounted first.");
#ifdef __APPLE__
    text += tr("<br><br>macOS will ask for an administrator's approval to write to the drive.");
#elif defined(_WIN32)
    text += tr("<br><br>Windows will ask for permission to write to the drive.");
#else
    text += tr("<br><br>Kiln will ask for your password to write to the drive.");
#endif
    QMessageBox box(QMessageBox::Warning, tr("Erase %1?").arg(qs(dev.path)), text, QMessageBox::Cancel, this);
    QPushButton* erase = box.addButton(ventoy ? tr("Erase and make Ventoy disk") : tr("Erase and write"),
                                       QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != erase) return;

    saveSettings();
    gotResult_ = false;
    notes_.clear();
    pending_.clear();
    stage_ = -1;
    writingTo_ = describe(dev);
    if (ventoy) {
        log(tr("Making %1 a Ventoy %2 disk").arg(writingTo_, QString::fromLatin1(ventoy_version())));
        notes_ = tr("\nCopy ISO files onto its “%1” partition.").arg(qs(req.ventoy_setup.label));
    } else {
        log(tr("Writing %1 to %2").arg(image_->path, writingTo_));
    }
    busy_ = true;
    setBusy(true);
    status_->setText(tr("Waiting for authorization…"));
    bar_->setRange(0, 0);
#ifdef __APPLE__
    // No root process on macOS: the job runs here, as the user, on a worker
    // thread; only the drive's descriptor comes from authopen (macOS's own
    // approval dialog).
    cancelJob_ = false;
    job_.setFuture(QtConcurrent::run([this, req] {
        auto held = std::make_shared<Fd>();
        JobIo io;
        io.image = req.image;
        io.cancel = &cancelJob_;
        io.on_event = [this](const HelperEvent& e) {
            QMetaObject::invokeMethod(this, [this, e] { handleEvent(e); }, Qt::QueuedConnection);
        };
        io.open_device = [held](const Device& d) {
            std::string why;
            const int fd = authopen_device(d.path, &why);
            if (fd < 0) throw std::runtime_error(why);
            *held = Fd(fd);
            return TargetSpec::descriptor(fd, d.path);
        };
        return run_write_job(req, io);
    }));
#elif defined(_WIN32)
    // Let "Waiting for authorization…" paint: ShellExecuteEx blocks while UAC asks.
    QTimer::singleShot(0, this, [this, req] { startWindowsHelper(req); });
#else
    QStringList args{helperPath()};
    for (const auto& a : helper_args(req)) args << qs(a);
    helper_ = new QProcess(this);
    helper_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(helper_, &QProcess::readyReadStandardOutput, this, &MainWindow::helperOutput);
    connect(helper_, &QProcess::readyReadStandardError, this, [this] {
        for (const QString& l : QString::fromLocal8Bit(helper_->readAllStandardError()).split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            log(l);
    });
    connect(helper_, &QProcess::finished, this, &MainWindow::helperFinished);
    helper_->start(QStringLiteral("pkexec"), args);
#endif
}

void MainWindow::cancel() {
    if (!busy_) return;
#ifdef __APPLE__
    cancelJob_ = true;
#elif defined(_WIN32)
    if (pipe_) pipe_->write("cancel\n");
#else
    if (helper_) helper_->write("cancel\n");
#endif
    cancelButton_->setEnabled(false);
    status_->setText(tr("Cancelling…"));
}

void MainWindow::helperOutput() {
    if (helper_) helperBytes(helper_->readAllStandardOutput());
}

void MainWindow::helperBytes(const QByteArray& bytes) {
    pending_ += bytes;
    qsizetype nl;
    while ((nl = pending_.indexOf('\n')) >= 0) {
        const std::string line = pending_.left(nl).toStdString();
        pending_.remove(0, nl + 1);
        if (const auto e = parse_event(line))
            handleEvent(*e);
        else
            log(qs(line));
    }
}

void MainWindow::handleEvent(const HelperEvent& event) {
    const HelperEvent* e = &event;
    using K = HelperEvent::Kind;
    switch (e->kind) {
        case K::stage: {
            const Progress& p = e->progress;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (static_cast<int>(p.stage) != stage_) {
                stage_ = static_cast<int>(p.stage);
                stageStartMs_ = now;
                log(stageText(p.stage) + QStringLiteral("…"));
            }
            if (indeterminate(p.stage) && p.done < p.total) {
                bar_->setRange(0, 0);
                status_->setText(stageText(p.stage) + QStringLiteral("…"));
            } else {
                bar_->setRange(0, 1000);
                bar_->setValue(p.total ? static_cast<int>(1000.0 * static_cast<double>(p.done) / static_cast<double>(p.total)) : 0);
                const double secs = static_cast<double>(now - stageStartMs_) / 1000.0;
                QString speed;
                if (secs > 1 && !indeterminate(p.stage))
                    speed = tr(" · %1 MB/s").arg(static_cast<double>(p.done) / secs / 1e6, 0, 'f', 1);
                status_->setText(tr("%1 · %2 of %3%4").arg(stageText(p.stage), qs(format_size(p.done)),
                                                           qs(format_size(p.total)), speed));
            }
            cancelButton_->setEnabled(true);
            break;
        }
        case K::msg:
            log(qs(e->text));
            if (e->text.find("unplug the drive") != std::string::npos) notes_ += QLatin1Char('\n') + qs(e->text);
            break;
        case K::ok: {
            gotResult_ = true;
            QString t = tr("Done: written in %1 s").arg(e->write_seconds, 0, 'f', 0);
            if (verify_->isChecked()) t += tr(", verified in %1 s").arg(e->verify_seconds, 0, 'f', 0);
            if (!e->sha256.empty()) t += tr(". Image SHA-256 %1").arg(qs(e->sha256));
            log(t + QLatin1Char('.'));
            status_->setText(tr("Done. The drive is ready to unplug."));
            bar_->setRange(0, 1000);
            bar_->setValue(1000);
            QMessageBox::information(this, tr("Done"), tr("%1 is ready.\n%2.").arg(writingTo_, t) + notes_);
            break;
        }
        case K::mismatch:
            gotResult_ = true;
            log(tr("Verification failed: %1").arg(qs(e->text)));
            status_->setText(tr("Verification FAILED."));
            QMessageBox::critical(this, tr("Verification failed"),
                                  tr("The drive does not hold what was written: %1.\n\nThe drive may be failing, or "
                                     "fake (smaller than it claims).").arg(qs(e->text)));
            break;
        case K::cancelled:
            gotResult_ = true;
            log(tr("Cancelled."));
            status_->setText(tr("Cancelled. The drive is left incomplete."));
            break;
        case K::error:
            gotResult_ = true;
            log(tr("Error: %1").arg(qs(e->text)));
            status_->setText(tr("Failed: %1").arg(qs(e->text)));
            QMessageBox::critical(this, tr("Kiln"), qs(e->text));
            break;
    }
}

void MainWindow::helperFinished(int code, QProcess::ExitStatus status) {
    helperOutput();
    if (!gotResult_) {
        // pkexec: 126 = the authentication dialog was dismissed, 127 = not authorized
        // or the helper could not be run.
        if (status != QProcess::NormalExit) helperGone(tr("The writer stopped unexpectedly."));
        else if (code == 126) helperGone(tr("Authorization was cancelled; nothing was written."));
        else if (code == 127) helperGone(tr("Not authorized, or the writer (%1) could not be started.").arg(helperPath()));
        else helperGone(tr("The writer exited with code %1.").arg(code));
    }
    helper_->deleteLater();
    helper_ = nullptr;
    finishRun();
}

void MainWindow::helperGone(const QString& why) {
    log(why);
    status_->setText(why);
    bar_->setRange(0, 1000);
    bar_->setValue(0);
}

#ifdef _WIN32
void MainWindow::startWindowsHelper(const HelperRequest& request) {
    HelperRequest req = request;
    // The helper writes its temporary files as the user, so the user's own temp
    // directory is the right place (and the only one the user's rights reach).
    req.temp_dir = QDir::toNativeSeparators(QDir::tempPath()).toStdString();

    const QString name = QStringLiteral("kiln-") + QUuid::createUuid().toString(QUuid::Id128);
    pipeServer_ = new QLocalServer(this);
    pipeServer_->setMaxPendingConnections(1);
    if (!pipeServer_->listen(name)) {
        helperGone(tr("Could not create the pipe to the writer: %1").arg(pipeServer_->errorString()));
        delete pipeServer_;
        pipeServer_ = nullptr;
        finishRun();
        return;
    }
    connect(pipeServer_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* s = pipeServer_->nextPendingConnection()) {
            // Only the process we started may report on this pipe.
            ULONG pid = 0;
            if (pipe_ || !GetNamedPipeClientProcessId(reinterpret_cast<HANDLE>(s->socketDescriptor()), &pid) ||
                pid != helperPid_) {
                s->abort();
                s->deleteLater();
                continue;
            }
            pipe_ = s;
            connect(pipe_, &QLocalSocket::readyRead, this, [this] { helperBytes(pipe_->readAll()); });
            pipeServer_->close();  // nobody else gets in
        }
    });

    std::vector<std::string> args = {"--pipe", name.toStdString()};
    for (const auto& a : helper_args(req)) args.push_back(a);
    const std::wstring params = QString::fromStdString(quote_windows_args(args)).toStdWString();
    const std::wstring file = QDir::toNativeSeparators(helperPath()).toStdWString();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = reinterpret_cast<HWND>(winId());
    sei.lpVerb = L"runas";
    sei.lpFile = file.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        const DWORD e = GetLastError();
        helperGone(e == ERROR_CANCELLED ? tr("Authorization was cancelled; nothing was written.")
                                        : tr("The writer (%1) could not be started (error %2).").arg(helperPath()).arg(e));
        delete pipeServer_;
        pipeServer_ = nullptr;
        finishRun();
        return;
    }
    helperProcess_ = sei.hProcess;
    helperPid_ = GetProcessId(sei.hProcess);
    status_->setText(tr("Starting…"));
    helperExit_ = new QWinEventNotifier(sei.hProcess, this);
    connect(helperExit_, &QWinEventNotifier::activated, this, &MainWindow::windowsHelperExited);
}

void MainWindow::windowsHelperExited() {
    // Its last lines may still be on their way through the pipe.
    for (int i = 0; pipe_ && i < 40 && pipe_->state() == QLocalSocket::ConnectedState; ++i) {
        pipe_->waitForReadyRead(50);
        helperBytes(pipe_->readAll());
    }
    if (pipe_) helperBytes(pipe_->readAll());
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(helperProcess_), &code);
    if (!gotResult_) helperGone(tr("The writer exited with code %1 without a result.").arg(code));
    helperExit_->setEnabled(false);
    helperExit_->deleteLater();
    helperExit_ = nullptr;
    CloseHandle(static_cast<HANDLE>(helperProcess_));
    helperProcess_ = nullptr;
    helperPid_ = 0;
    if (pipe_) pipe_->deleteLater();
    pipe_ = nullptr;
    if (pipeServer_) pipeServer_->deleteLater();
    pipeServer_ = nullptr;
    finishRun();
}
#endif

void MainWindow::jobFinished() {
    if (!gotResult_) {  // run_write_job always ends with a result event; this is a safety net
        log(tr("The writer stopped without a result."));
        status_->setText(tr("The writer stopped without a result."));
    }
    finishRun();
}

void MainWindow::finishRun() {
    busy_ = false;
    setBusy(false);
    refreshDevices();
    if (closeWhenDone_) close();
}

void MainWindow::setBusy(bool busy) {
    for (QWidget* w : std::initializer_list<QWidget*>{devices_, modeImage_, modeVentoy_, browse_, windowsBox_, ventoyBox_,
                                                      verify_, startButton_})
        w->setEnabled(!busy);
    cancelButton_->setEnabled(false);
    if (!busy) updateState();
}

void MainWindow::log(const QString& line) {
    log_->appendPlainText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss  ")) + line);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (!busy_) {
        saveSettings();
        e->accept();
        return;
    }
    e->ignore();
    if (QMessageBox::question(this, tr("Kiln"), tr("A write is running. Stop it and quit? The drive will be left "
                                                   "incomplete.")) != QMessageBox::Yes)
        return;
    closeWhenDone_ = true;
    cancel();
}

void MainWindow::loadSettings() {
    QSettings s;
    verify_->setChecked(s.value(QStringLiteral("verify"), true).toBool());
    bypass_->setChecked(s.value(QStringLiteral("tweaks/bypass"), true).toBool());  // Rufus's defaults
    noMsAccount_->setChecked(s.value(QStringLiteral("tweaks/noMsAccount"), true).toBool());
    localAccount_->setChecked(s.value(QStringLiteral("tweaks/localAccount"), false).toBool());
    accountName_->setText(s.value(QStringLiteral("tweaks/accountName")).toString());
    noDataCollection_->setChecked(s.value(QStringLiteral("tweaks/noDataCollection"), false).toBool());
    noBitlocker_->setChecked(s.value(QStringLiteral("tweaks/noBitlocker"), false).toBool());
    declutter_->setChecked(s.value(QStringLiteral("tweaks/declutter"), false).toBool());
    ventoySecureBoot_->setChecked(s.value(QStringLiteral("ventoy/secureBoot"), true).toBool());  // Ventoy's defaults
    ventoyGpt_->setChecked(s.value(QStringLiteral("ventoy/gpt"), false).toBool());
    ventoyLabel_->setText(s.value(QStringLiteral("ventoy/label")).toString());
}

void MainWindow::saveSettings() const {
    QSettings s;
    s.setValue(QStringLiteral("verify"), verify_->isChecked());
    s.setValue(QStringLiteral("tweaks/bypass"), bypass_->isChecked());
    s.setValue(QStringLiteral("tweaks/noMsAccount"), noMsAccount_->isChecked());
    s.setValue(QStringLiteral("tweaks/localAccount"), localAccount_->isChecked());
    s.setValue(QStringLiteral("tweaks/accountName"), accountName_->text());
    s.setValue(QStringLiteral("tweaks/noDataCollection"), noDataCollection_->isChecked());
    s.setValue(QStringLiteral("tweaks/noBitlocker"), noBitlocker_->isChecked());
    s.setValue(QStringLiteral("tweaks/declutter"), declutter_->isChecked());
    s.setValue(QStringLiteral("ventoy/secureBoot"), ventoySecureBoot_->isChecked());
    s.setValue(QStringLiteral("ventoy/gpt"), ventoyGpt_->isChecked());
    s.setValue(QStringLiteral("ventoy/label"), ventoyLabel_->text());
}

}  // namespace kiln
