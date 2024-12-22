/**********************************************************************
 *  mainwindow.cpp
 **********************************************************************
 * Copyright (C) 2024 MX Authors
 *
 * Authors: Adrian <adrian@mxlinux.org>
 *          MX Linux <http://mxlinux.org>
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this package. If not, see <http://www.gnu.org/licenses/>.
 **********************************************************************/

#include "mainwindow.h"
#include "qapplication.h"
#include "ui_mainwindow.h"

#include <QDebug>
#include <QFileDialog>
#include <QInputDialog>
#include <QListWidget>
#include <QScreen>
#include <QStorageInfo>
#include <QTextStream>

#include "about.h"
#include "cmd.h"

// Trying to map all the persitence type to values that make sense
// when passed to the kernel at boot time for frugal installation
const QMap<QString, QString> MainWindow::persistenceTypes = {{"persist_all", "persist_all"},
                                                             {"persist_root", "persist_root"},
                                                             {"persist_static", "persist_static"},
                                                             {"persist_static_root", "persist_static_root"},
                                                             {"p_static_root", "persist_static_root"},
                                                             {"persist_home", "persist_home"},
                                                             {"frugal_persist", "persist_all"},
                                                             {"frugal_root", "persist_root"},
                                                             {"frugal_static", "persist_static"},
                                                             {"frugal_static_root", "persist_static_root"},
                                                             {"f_static_root", "persist_static_root"},
                                                             {"frugal_home", "persist_home"},
                                                             {"frugal_only", "frugal_only"}};

MainWindow::MainWindow(const QCommandLineParser &arg_parser, QWidget *parent)
    : QDialog(parent),
      ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowFlags(Qt::Window);
    setWindowTitle(QApplication::applicationDisplayName());

    setup();
    setConnections();

    if (arg_parser.isSet("frugal")) {
        qDebug() << "Frugal mode";
        promptFrugalStubInstall();
        ui->tabWidget->setCurrentIndex(Tab::Frugal);
        refreshFrugal();
    }
}

MainWindow::~MainWindow()
{
    settings.setValue("geometry", saveGeometry());
    for (auto it = newMounts.rbegin(); it != newMounts.rend(); ++it) {
        cmd.procAsRoot("umount", {*it});
    }
    for (const QString &dir : newDirectories) {
        cmd.procAsRoot("rmdir", {dir});
    }
    delete ui;
}

void MainWindow::addUefiEntry(QListWidget *listEntries, QWidget *dialogUefi)
{
    // Mount all ESPs
    QStringList partList
        = cmd.getOutAsRoot(
                 "lsblk -no PATH,PARTTYPE | grep -iE 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b|0xef' | cut -d' ' -f1")
              .split("\n", Qt::SkipEmptyParts);

    for (const auto &device : qAsConst(partList)) {
        if (!cmd.proc("findmnt", {"-n", device})) {
            QString partName = device.section('/', -1);
            QString mountDir = "/boot/efi/" + partName;
            if (!QDir(mountDir).exists()) {
                cmd.procAsRoot("mkdir", {"-p", mountDir});
                newDirectories.append(mountDir);
                cmd.procAsRoot("mount", {device, mountDir});
                newMounts.append(mountDir);
            }
        }
    }

    QString initialPath = QFile::exists("/boot/efi/EFI") ? "/boot/efi/EFI" : "/boot/efi/";
    QString fileName
        = QFileDialog::getOpenFileName(dialogUefi, tr("Select EFI file"), initialPath, tr("EFI files (*.efi *.EFI)"));

    if (!QFile::exists(fileName)) {
        return;
    }

    QString partitionName = cmd.getOut("df " + fileName + " --output=source").split('\n').last().trimmed();
    QString disk = "/dev/" + cmd.getOut("lsblk -no PKNAME " + partitionName).trimmed();
    QString partition = partitionName.section(QRegularExpression("[0-9]+$"), -1);

    if (cmd.exitCode() != 0) {
        QMessageBox::critical(dialogUefi, tr("Error"), tr("Could not find the source mountpoint for %1").arg(fileName));
        return;
    }

    QString name = QInputDialog::getText(dialogUefi, tr("Set name"), tr("Enter the name for the UEFI menu item:"));
    if (name.isEmpty()) {
        name = "New entry";
    }

    fileName = "/EFI/" + fileName.section("/EFI/", 1);
    QString command = QString("efibootmgr -cL \"%1\" -d %2 -p %3 -l %4").arg(name, disk, partition, fileName);
    QString out = cmd.getOutAsRoot(command);

    if (cmd.exitCode() != 0) {
        QMessageBox::critical(dialogUefi, tr("Error"), tr("Something went wrong, could not add entry."));
        return;
    }

    QStringList outList = out.split('\n', Qt::SkipEmptyParts);
    listEntries->insertItem(0, outList.constLast());
    emit listEntries->itemSelectionChanged();
}

void MainWindow::checkDoneStub()
{
    bool allDone = !ui->comboDriveStub->currentText().isEmpty() && !ui->comboPartitionStub->currentText().isEmpty()
                   && !ui->comboKernel->currentText().isEmpty() && !ui->textEntryName->text().isEmpty();
    ui->pushNext->setEnabled(allDone);
}

// Clear existing widgets and layout from tabManageUefi
void MainWindow::clearEntryWidget()
{
    if (ui->tabManageUefi->layout() != nullptr) {
        QLayoutItem *child;
        while ((child = ui->tabManageUefi->layout()->takeAt(0))) {
            delete child->widget();
            delete child;
        }
    }
    delete ui->tabManageUefi->layout();
}

void MainWindow::centerWindow()
{
    const auto screenGeometry = QApplication::primaryScreen()->geometry();
    const auto x = (screenGeometry.width() - this->width()) / 2;
    const auto y = (screenGeometry.height() - this->height()) / 2;
    this->move(x, y);
}

void MainWindow::setup()
{
    auto size = this->size();
    if (settings.contains("geometry")) {
        restoreGeometry(settings.value("geometry").toByteArray());
        if (this->isMaximized()) { // Add option to resize if maximized
            this->resize(size);
            centerWindow();
        }
    }

    // Refresh appropriate tab content based on current tab
    const auto currentTab = ui->tabWidget->currentIndex();
    switch (currentTab) {
    case Tab::Entries:
        refreshEntries();
        break;
    case Tab::Frugal:
        refreshFrugal();
        break;
    case Tab::StubInstall:
        refreshStubInstall();
        break;
    }
}

void MainWindow::cmdStart()
{
    setCursor(QCursor(Qt::BusyCursor));
}

void MainWindow::cmdDone()
{
    setCursor(QCursor(Qt::ArrowCursor));
}

void MainWindow::setConnections()
{
    connect(&cmd, &Cmd::done, this, &MainWindow::cmdDone);
    connect(&cmd, &Cmd::started, this, &MainWindow::cmdStart);

    connect(ui->comboDrive, &QComboBox::currentTextChanged, this, &MainWindow::filterDrivePartitions);
    connect(ui->comboDriveStub, &QComboBox::currentTextChanged, this, &MainWindow::filterDrivePartitions);
    connect(ui->pushAbout, &QPushButton::clicked, this, &MainWindow::pushAbout_clicked);
    connect(ui->pushBack, &QPushButton::clicked, this, &MainWindow::pushBack_clicked);
    connect(ui->pushCancel, &QPushButton::pressed, this, &MainWindow::close);
    connect(ui->pushHelp, &QPushButton::clicked, this, &MainWindow::pushHelp_clicked);
    connect(ui->pushNext, &QPushButton::clicked, this, &MainWindow::pushNext_clicked);
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, &MainWindow::tabWidget_currentChanged);

    connect(ui->comboDriveStub, &QComboBox::currentTextChanged, this, &MainWindow::checkDoneStub);
    connect(ui->comboKernel, &QComboBox::currentTextChanged, this, &MainWindow::checkDoneStub);
    connect(ui->comboPartitionStub, &QComboBox::currentTextChanged, this, &MainWindow::checkDoneStub);
    connect(ui->textEntryName, &QLineEdit::textChanged, this, &MainWindow::checkDoneStub);
}

void MainWindow::toggleUefiActive(QListWidget *listEntries)
{
    auto currentItem = listEntries->currentItem();
    if (!currentItem) {
        return;
    }

    QString item = currentItem->text().section(' ', 0, 0).remove(QRegularExpression("^Boot"));
    QString rest = currentItem->text().section(' ', 1, -1);

    if (!item.contains(QRegularExpression(R"(^[0-9A-Z]{4}\*?$)"))) {
        return;
    }

    bool isActive = item.endsWith('*');
    if (isActive) {
        item.chop(1);
    }

    if (Cmd().procAsRoot("efibootmgr", {isActive ? "--inactive" : "--active", "-b", item})) {
        listEntries->currentItem()->setText(QString("Boot%1%2 %3").arg(item, isActive ? "" : "*", rest));
        listEntries->currentItem()->setBackground(isActive ? QBrush(Qt::gray) : QBrush());
    }

    emit listEntries->itemSelectionChanged();
}

void MainWindow::tabWidget_currentChanged()
{
    const int currentTab = ui->tabWidget->currentIndex();
    ui->pushNext->setVisible(currentTab == Tab::Frugal || currentTab == Tab::StubInstall);
    ui->pushBack->setVisible(currentTab == Tab::Frugal);

    switch (currentTab) {
    case Tab::Entries:
        refreshEntries();
        break;
    case Tab::Frugal:
        refreshFrugal();
        break;
    case Tab::StubInstall:
        refreshStubInstall();
        break;
    }
}

QString MainWindow::getBootLocation()
{
    QString partition = ui->comboPartitionStub->currentText().section(' ', 0, 0);
    QString mountPoint = getMountPoint(partition);
    if (mountPoint.isEmpty()) {
        mountPoint = mountPartition(partition);
    }
    if (mountPoint.isEmpty()) {
        qWarning() << "Failed to mount partition" << partition;
        return {};
    }

    // Check /etc/fstab for separate /boot partition
    QFile fstab(mountPoint + "/etc/fstab");
    if (!fstab.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Could not open" << fstab.fileName();
        return mountPoint;
    }

    QString bootPartition;
    QTextStream in(&fstab);
    QString line;
    while (in.readLineInto(&line)) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        QStringList fields = line.split(QRegularExpression("\\s+"));
        if (fields.size() >= 2 && fields.at(1) == "/boot") {
            bootPartition = fields.at(0);
            break;
        }
    }
    fstab.close();

    if (bootPartition.isEmpty()) {
        return mountPoint;
    }

    // If UUID is used, convert to device name
    if (bootPartition.startsWith("UUID=")) {
        QString uuid = bootPartition.mid(5);
        bootPartition = "/dev/disk/by-uuid/" + uuid;
    }

    // Mount the boot partition
    QString bootMountPoint = mountPartition(bootPartition);
    if (bootMountPoint.isEmpty()) {
        qWarning() << "Failed to mount boot partition" << bootPartition;
        return mountPoint;
    }

    return bootMountPoint;
}

bool MainWindow::copyKernel()
{
    if (espMountPoint.isEmpty()) {
        qWarning() << "ESP mount point is empty.";
        return false;
    }

    const bool isFrugal = ui->tabWidget->currentIndex() == Tab::Frugal;

    const QString subDir = isFrugal ? "/frugal" : "/stub";
    const QString targetPath = espMountPoint + "/EFI/" + distro + subDir;

    // Create target directory if it doesn't exist
    if (!QDir().exists(targetPath)) {
        if (!cmd.procAsRoot("mkdir", {"-p", targetPath})) {
            qWarning() << "Failed to create directory:" << targetPath;
            return false;
        }
    }

    // Copy kernel and initrd files
    const QString sourceDir = isFrugal ? frugalDir : getBootLocation();
    const QString kernelVersion = ui->comboKernel->currentText();
    const QString vmlinuz = QString("%1/vmlinuz%2").arg(sourceDir, isFrugal ? "" : "-" + kernelVersion);
    const QString initrd = QString("%1/initrd%2").arg(sourceDir, isFrugal ? ".gz" : ".img-" + kernelVersion);
    const QStringList filesToCopy = {vmlinuz, initrd};

    const QStringList targetFiles = {"/vmlinuz", "/initrd.img"};
    for (int i = 0; i < filesToCopy.size(); ++i) {
        const QString &file = filesToCopy.at(i);
        if (!QFile::exists(file)) {
            qWarning() << "Source file does not exist:" << file;
            return false;
        }
        const QString targetFile = targetPath + targetFiles.at(i);
        if (!cmd.procAsRoot("cp", {file, targetFile})) {
            qWarning() << "Failed to copy file:" << file << "to" << targetFile;
            return false;
        }
    }

    qInfo() << "Kernel and initrd files copied successfully to" << targetPath;
    return true;
}

bool MainWindow::installEfiStub(const QString &esp)
{
    if (esp.isEmpty() || !copyKernel()) {
        return false;
    }

    QString disk;
    for (const QString &drive : listDrive) {
        const QString driveName = drive.section(' ', 0, 0);
        if (esp.startsWith(driveName)) {
            disk = "/dev/" + driveName;
            break;
        }
    }
    QString part = esp.mid(esp.lastIndexOf(QRegularExpression("[0-9]+$")));

    if (disk.isEmpty() || part.isEmpty()) {
        return false;
    }

    const bool isFrugal = ui->tabWidget->currentIndex() == Tab::Frugal;
    const QString efiDir = isFrugal ? "frugal" : "stub";
    const QString entryName = isFrugal ? ui->textUefiEntryFrugal->text() : ui->textEntryName->text();

    QStringList args;
    args << "--disk" << disk << "--part" << part << "--create"
         << "--label" << '"' + entryName + '"' << "--loader"
         << QString("\"\\EFI\\%1\\%2\\vmlinuz\"").arg(distro, efiDir) << "--unicode";

    const QString initrd = QString("initrd=\\EFI\\%1\\%2\\initrd.img").arg(distro, efiDir);
    QString bootOptions;
    if (isFrugal) {
        bootOptions
            = QString("'bdir=%1 buuid=%2 %3 %4 %5'")
                  .arg(options.bdir, options.uuid, options.stringOptions, ui->comboFrugalMode->currentText(), initrd);
    } else {
        // Get drive and partition info
        QString rootDev = "/dev/" + ui->comboPartitionStub->currentText().section(' ', 0, 0);
        QString rootLabel = cmd.getOutAsRoot("blkid -s LABEL -o value " + rootDev).trimmed();
        QString root = !rootLabel.isEmpty() ? "LABEL=\"" + rootLabel + '"'
                                            : "UUID=" + cmd.getOutAsRoot("blkid -s UUID -o value " + rootDev).trimmed();

        if (isLuks(root)) {
            // ASSUMPTION: for MX we use "/dev/mapper/root.fsm"
            rootLabel = cmd.getOutAsRoot("blkid -s LABEL -o value /dev/mapper/root.fsm").trimmed();
            root = !rootLabel.isEmpty()
                       ? "LABEL=\"" + rootLabel + '"'
                       : "UUID=" + cmd.getOutAsRoot("blkid -s UUID -o value /dev/mapper/root.fsm").trimmed();
            root += QString(" rd.luks.uuid=%1=root.fsm").arg(getLuksUUID(rootDev));
        }
        bootOptions = QString("'root=%1 %2 %3'").arg(root, options.stringOptions, initrd);
    }

    if (!cmd.procAsRoot("efibootmgr", args << bootOptions)) {
        return false;
    }
    return true;
}

bool MainWindow::isLuks(const QString &part)
{
    return cmd.procAsRoot("cryptsetup", {"isLuks", part});
}

QString MainWindow::mountPartition(QString part)
{
    if (part.startsWith("/dev/")) {
        part = part.mid(5);
    }

    if (isLuks("/dev/" + part)) {
        if (cmd.run("lsblk -o NAME,MOUNTPOINT | grep -w " + part)) {
            return cmd.getOut("lsblk -o NAME,MOUNTPOINT | grep -A1 -w " + part + " | awk '{print $2}'").trimmed();
        }
        return openLuks("/dev/" + part);
    }

    QString mountDir = cmd.getOut("findmnt -nf --source /dev/" + part.section(" ", 0, 0)).section(" ", 0, 0);
    if (!mountDir.isEmpty()) {
        return mountDir;
    }

    mountDir = "/mnt/" + part;
    if (!QDir(mountDir).exists()) {
        if (!cmd.procAsRoot("mkdir", {mountDir})) {
            return {};
        }
        newDirectories.append(mountDir);
    }

    if (!cmd.procAsRoot("mount", {"/dev/" + part, mountDir})) {
        return {};
    }
    newMounts.append(mountDir);
    return mountDir;
}

// Add list of devices to comboLocation
void MainWindow::addDevToList()
{
    listDevices();
    auto *comboDrive = (ui->tabWidget->currentIndex() == Tab::Frugal) ? ui->comboDrive : ui->comboDriveStub;
    comboDrive->blockSignals(true);
    comboDrive->clear();
    comboDrive->blockSignals(false);
    comboDrive->addItems(listDrive);
}

bool MainWindow::checkSizeEsp()
{
    const bool isFrugal = ui->tabWidget->currentIndex() == Tab::Frugal;
    const QString sourceDir = isFrugal ? frugalDir : getBootLocation();
    qDebug() << "Source Dir:" << sourceDir;
    const QString vmlinuz
        = QString("%1/vmlinuz%2").arg(sourceDir, isFrugal ? "" : "-" + ui->comboKernel->currentText());
    const QString initrd
        = QString("%1/initrd%2").arg(sourceDir, isFrugal ? ".gz" : ".img-" + ui->comboKernel->currentText());
    qDebug() << "VMLINUZ:" << vmlinuz;
    qDebug() << "INITRD :" << initrd;
    const qint64 vmlinuzSize = QFile(vmlinuz).size();
    const qint64 initrdSize = QFile(initrd).size();
    const qint64 totalSize = vmlinuzSize + initrdSize;
    qDebug() << "Total needed:" << totalSize;

    const qint64 espFreeSpace = QStorageInfo(espMountPoint).bytesAvailable();
    qDebug() << "ESP Free    :" << espFreeSpace;
    if (totalSize > espFreeSpace) {
        return false;
    }
    return true;
}

void MainWindow::filterDrivePartitions()
{
    auto *comboDrive = (ui->tabWidget->currentIndex() == Tab::Frugal) ? ui->comboDrive : ui->comboDriveStub;
    auto *comboPartition = (ui->tabWidget->currentIndex() == Tab::Frugal) ? ui->comboPartition : ui->comboPartitionStub;

    comboPartition->blockSignals(true);
    comboPartition->clear();
    comboPartition->blockSignals(false);
    QString drive = comboDrive->currentText().section(' ', 0, 0);
    if (!drive.isEmpty()) {
        QStringList drivePart = listPart.filter(QRegularExpression("^" + drive));
        comboPartition->blockSignals(true);
        comboPartition->addItems(drivePart);
        comboPartition->blockSignals(false);
    }
    guessPartition();
}

void MainWindow::selectKernel()
{
    QDir bootDir {getBootLocation()};
    if (!bootDir.absolutePath().endsWith("/boot")) {
        bootDir.setPath(bootDir.absolutePath() + "/boot");
    }
    QStringList kernelFiles = bootDir.entryList({"vmlinuz-*"}, QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    std::transform(kernelFiles.begin(), kernelFiles.end(), kernelFiles.begin(),
                   [](const QString &file) { return file.mid(QStringLiteral("vmlinuz-").length()); });
    ui->comboKernel->clear();
    kernelFiles.sort();
    ui->comboKernel->addItems(kernelFiles);
    QString kernel = cmd.getOut("uname -r", true).trimmed();
    if (ui->comboKernel->findText(kernel) != -1) {
        ui->comboKernel->setCurrentText(kernel);
    }
}

void MainWindow::promptFrugalStubInstall()
{
    int ret = QMessageBox::question(this, tr("UEFI Installer"),
                                    tr("A recent frugal install has been detected. Do you wish to add a UEFI entry "
                                       "direct to your UEFI system menu?"),
                                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (ret == QMessageBox::No) {
        cmd.procAsRoot("/usr/lib/uefi-manager/uefimanager-lib", {"write_checkfile"});
        exit(EXIT_SUCCESS);
    } else {
        ui->tabWidget->setCurrentIndex(Tab::Frugal);
    }
}

void MainWindow::readBootEntries(QListWidget *listEntries, QLabel *textTimeout, QLabel *textBootNext,
                                 QLabel *textBootCurrent, QStringList *bootorder)
{
    QStringList entries = cmd.getOutAsRoot("efibootmgr").split('\n', Qt::SkipEmptyParts);
    QRegularExpression bootEntryRegex(R"(^Boot[0-9A-F]{4}\*?\s+)");

    for (const auto &item : qAsConst(entries)) {
        if (bootEntryRegex.match(item).hasMatch()) {
            auto *listItem = new QListWidgetItem(item);
            if (!item.contains("*")) {
                listItem->setBackground(QBrush(Qt::gray));
            }
            listEntries->addItem(listItem);
        } else if (item.startsWith("Timeout:")) {
            textTimeout->setText(tr("Timeout: %1 seconds").arg(item.section(' ', 1).trimmed()));
        } else if (item.startsWith("BootNext:")) {
            textBootNext->setText(tr("Boot Next: %1").arg(item.section(' ', 1).trimmed()));
        } else if (item.startsWith("BootCurrent:")) {
            textBootCurrent->setText(tr("Boot Current: %1").arg(item.section(' ', 1).trimmed()));
        } else if (item.startsWith("BootOrder:")) {
            *bootorder = item.section(' ', 1).split(',', Qt::SkipEmptyParts);
        }
    }
}

void MainWindow::refreshEntries()
{
    clearEntryWidget();

    auto *layout = new QGridLayout(ui->tabManageUefi);
    auto *listEntries = new QListWidget(ui->tabManageUefi);
    auto *textIntro = new QLabel(tr("You can use the Up/Down buttons, or drag & drop items to change boot order.\n"
                                    "- Items are listed in the boot order.\n"
                                    "- Grayed out lines are inactive."),
                                 ui->tabManageUefi);

    auto createButton = [&](const QString &text, const QString &iconName) {
        auto *button = new QPushButton(text, ui->tabManageUefi);
        button->setIcon(QIcon::fromTheme(iconName));
        return button;
    };

    auto *pushActive = createButton(tr("Set ac&tive"), "star-on");
    auto *pushAddEntry = createButton(tr("&Add entry"), "list-add");
    auto *pushBootNext = createButton(tr("Boot &next"), "go-next");
    auto *pushDown = createButton(tr("Move &down"), "arrow-down");
    auto *pushRemove = createButton(tr("&Remove entry"), "trash-empty");
    auto *pushResetNext = createButton(tr("Re&set next"), "edit-undo");
    auto *pushTimeout = createButton(tr("Change &timeout"), "timer-symbolic");
    auto *pushUp = createButton(tr("Move &up"), "arrow-up");

    auto *spacer = new QSpacerItem(0, 0, QSizePolicy::Fixed, QSizePolicy::Expanding);
    auto *textBootCurrent = new QLabel(ui->tabManageUefi);
    auto *textBootNext
        = new QLabel(tr("Boot Next: %1").arg(tr("not set, will boot using list order")), ui->tabManageUefi);
    auto *textTimeout = new QLabel(tr("Timeout: %1 seconds").arg("0"), ui->tabManageUefi);
    listEntries->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    disconnect(pushResetNext, &QPushButton::clicked, ui->tabManageUefi, nullptr);
    disconnect(pushTimeout, &QPushButton::clicked, this, nullptr);
    disconnect(pushAddEntry, &QPushButton::clicked, this, nullptr);
    disconnect(pushBootNext, &QPushButton::clicked, this, nullptr);
    disconnect(pushRemove, &QPushButton::clicked, this, nullptr);
    disconnect(pushActive, &QPushButton::clicked, ui->tabManageUefi, nullptr);
    disconnect(pushUp, &QPushButton::clicked, ui->tabManageUefi, nullptr);
    disconnect(pushDown, &QPushButton::clicked, ui->tabManageUefi, nullptr);
    disconnect(listEntries, &QListWidget::itemSelectionChanged, ui->tabManageUefi, nullptr);

    connect(pushResetNext, &QPushButton::clicked, ui->tabManageUefi, [textBootNext]() {
        if (Cmd().procAsRoot("efibootmg", {"-N"})) {
            textBootNext->setText(tr("Boot Next: %1").arg(tr("not set, will boot using list order")));
        }
    });
    connect(pushTimeout, &QPushButton::clicked, this,
            [this, textTimeout]() { setUefiTimeout(ui->tabManageUefi, textTimeout); });
    connect(pushAddEntry, &QPushButton::clicked, this,
            [this, listEntries]() { addUefiEntry(listEntries, ui->tabManageUefi); });
    connect(pushBootNext, &QPushButton::clicked, this,
            [listEntries, textBootNext]() { setUefiBootNext(listEntries, textBootNext); });
    connect(pushRemove, &QPushButton::clicked, this,
            [this, listEntries]() { removeUefiEntry(listEntries, ui->tabManageUefi); });
    connect(pushActive, &QPushButton::clicked, ui->tabManageUefi, [listEntries]() { toggleUefiActive(listEntries); });
    connect(pushUp, &QPushButton::clicked, ui->tabManageUefi, [listEntries]() {
        listEntries->model()->moveRow(QModelIndex(), listEntries->currentRow(), QModelIndex(),
                                      listEntries->currentRow() - 1);
    });
    connect(pushDown, &QPushButton::clicked, ui->tabManageUefi, [listEntries]() {
        listEntries->model()->moveRow(QModelIndex(), listEntries->currentRow() + 1, QModelIndex(),
                                      listEntries->currentRow()); // move next entry down
    });
    connect(listEntries, &QListWidget::itemSelectionChanged, ui->tabManageUefi,
            [listEntries, pushUp, pushDown, pushActive]() {
                pushUp->setEnabled(listEntries->currentRow() != 0);
                pushDown->setEnabled(listEntries->currentRow() != listEntries->count() - 1);
                if (listEntries->currentItem()->text().section(' ', 0, 0).endsWith('*')) {
                    pushActive->setText(tr("Set &inactive"));
                    pushActive->setIcon(QIcon::fromTheme("star-off"));
                } else {
                    pushActive->setText(tr("Set ac&tive"));
                    pushActive->setIcon(QIcon::fromTheme("star-on"));
                }
            });

    QStringList bootorder;
    readBootEntries(listEntries, textTimeout, textBootNext, textBootCurrent, &bootorder);
    sortUefiBootOrder(bootorder, listEntries);

    listEntries->setDragDropMode(QAbstractItemView::InternalMove);
    connect(listEntries->model(), &QAbstractItemModel::rowsMoved, this, [this, listEntries]() {
        saveBootOrder(listEntries);
        emit listEntries->itemSelectionChanged();
    });

    int row = 0;
    const int rowspan = 7;
    layout->addWidget(textIntro, row++, 0, 1, 2);
    layout->addWidget(listEntries, row, 0, rowspan, 1);
    layout->addWidget(pushRemove, row++, 1);
    layout->addWidget(pushAddEntry, row++, 1);
    layout->addWidget(pushUp, row++, 1);
    layout->addWidget(pushDown, row++, 1);
    layout->addWidget(pushActive, row++, 1);
    layout->addWidget(pushBootNext, row++, 1);
    layout->addItem(spacer, row++, 1);
    layout->addWidget(textBootCurrent, row++, 0);
    layout->addWidget(textTimeout, row, 0);
    layout->addWidget(pushTimeout, row++, 1);
    layout->addWidget(textBootNext, row, 0);
    layout->addWidget(pushResetNext, row++, 1);
    ui->tabManageUefi->setLayout(layout);

    ui->tabManageUefi->resize(this->size());
    ui->tabManageUefi->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    ui->pushNext->setHidden(true);
    ui->pushBack->setHidden(true);
}

void MainWindow::refreshFrugal()
{
    addDevToList();
    ui->stackedFrugal->setCurrentIndex(Page::Location);
    ui->pushCancel->setEnabled(true);
    ui->pushBack->setEnabled(false);
    ui->pushNext->setEnabled(true);
    ui->pushNext->setText(tr("Next"));
    ui->pushNext->setIcon(QIcon::fromTheme("go-next"));
}

void MainWindow::refreshStubInstall()
{
    addDevToList();
    ui->pushCancel->setEnabled(true);
    ui->pushNext->setText(tr("Install"));
    ui->pushNext->setIcon(QIcon::fromTheme("run-install"));
    ui->textEntryName->setText(getDistroName(true));
    if (!ui->comboDriveStub->currentText().isEmpty() && !ui->comboPartitionStub->currentText().isEmpty()
        && !ui->comboKernel->currentText().isEmpty() && !ui->textEntryName->text().isEmpty()) {
        ui->pushNext->setEnabled(true);
    }
}

bool MainWindow::readGrubEntry()
{
    QFile grubEntryFile(frugalDir + "/grub.entry");
    if (!grubEntryFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("UEFI Installer"), tr("Failed to open grub.entry file."));
        return false;
    }
    options.bdir.clear();
    options.entryName.clear();
    options.persistenceType.clear();
    options.uuid.clear();
    options.stringOptions.clear();

    QTextStream in(&grubEntryFile);
    QString line;
    while (in.readLineInto(&line)) {
        line = line.trimmed();
        if (line.startsWith("menuentry")) {
            options.entryName = line.section('"', 1, 1).trimmed();
        } else if (line.startsWith("search")) {
            options.uuid = line.section("--fs-uuid", 1, 1).trimmed();
        } else if (line.startsWith("linux")) {
            QStringList optionsList = line.split(' ').mid(1); // Skip the first "linux" element
            for (const QString &option : optionsList) {
                if (option.startsWith("bdir=")) {
                    options.bdir = option.section('=', 1, 1).trimmed();
                } else if (persistenceTypes.contains(option)) {
                    options.persistenceType = persistenceTypes[option];
                } else if (!option.startsWith("buuid=") && !option.endsWith("vmlinuz")) {
                    options.stringOptions.append(option + ' ');
                }
            }
        }
    }
    options.stringOptions = options.stringOptions.trimmed();
    grubEntryFile.close();
    return true;
}

void MainWindow::loadStubOption()
{
    options.bdir.clear();
    options.persistenceType.clear();
    options.entryName = ui->textEntryName->text();
    options.uuid.clear();
    options.stringOptions = ui->textKernelOptions->text();
}

QString MainWindow::openLuks(const QString &partition)
{
    QString uuid;
    if (!cmd.procAsRoot("cryptsetup", {"luksUUID", partition}, &uuid) || uuid.trimmed().isEmpty()) {
        QMessageBox::critical(this, tr("Error"), tr("Could not retrieve UUID for %1").arg(partition));
        return {};
    }
    const QString mountPoint = "luks-" + uuid.trimmed();

    bool ok;
    QByteArray pass = QInputDialog::getText(this, this->windowTitle(),
                                            tr("Enter password to unlock %1 encrypted partition:").arg(partition),
                                            QLineEdit::Password, QString(), &ok)
                          .toUtf8();

    if (!ok || pass.isEmpty()) {
        QMessageBox::critical(this, tr("Error"), tr("Password entry cancelled or empty for %1").arg(partition));
        return {};
    }

    // Try to open the LUKS container
    if (!cmd.procAsRoot("cryptsetup", {"luksOpen", "--allow-discards", partition, mountPoint, "-"}, nullptr, &pass)) {
        QMessageBox::critical(this, tr("Error"), tr("Could not open %1 LUKS container").arg(partition));
        pass.fill(static_cast<char>(0xA5 & 0xFF));
        return {};
    }
    pass.fill(static_cast<char>(0xA5 & 0xFF));

    return mountPoint;
}

void MainWindow::sortUefiBootOrder(const QStringList &order, QListWidget *list)
{
    if (order.isEmpty()) {
        return;
    }

    int index = 0;
    for (const auto &orderItem : order) {
        auto items = list->findItems("Boot" + orderItem, Qt::MatchStartsWith);
        if (items.isEmpty()) {
            continue;
        }

        auto *item = items.constFirst();
        list->takeItem(list->row(item));
        list->insertItem(index, item);
        ++index;
    }

    list->setCurrentRow(0);
    list->currentItem()->setSelected(true);
    emit list->itemSelectionChanged();
}

QString MainWindow::getDistroName(bool pretty, const QString &mountPoint) const
{
    QFile file(QString("%1etc/initrd_release").arg(mountPoint));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return pretty ? "MX Linux" : "MX";
    }
    QTextStream in(&file);
    QString line;
    QString distroName;

    const QString searchTerm = pretty ? "PRETTY_NAME=" : "NAME=";

    while (!in.atEnd()) {
        line = in.readLine();
        if (line.startsWith(searchTerm)) {
            distroName = line.section('=', 1, 1).remove('"').trimmed();
            break;
        }
    }

    file.close();
    return distroName;
}

QString MainWindow::getLuksUUID(const QString &part)
{
    return cmd.getOutAsRoot("cryptsetup luksUUID " + part);
}

QString MainWindow::getMountPoint(const QString &partition)
{
    QString command;
    if (partition.startsWith("/dev/")) {
        command = QString("lsblk -no MOUNTPOINT %1").arg(partition);
    } else {
        command = QString("lsblk -no MOUNTPOINT /dev/%1").arg(partition);
    }
    QString mountPoint = cmd.getOut(command).trimmed();

    return mountPoint;
}

void MainWindow::getGrubOptions(const QString &mountPoint)
{
    QFile grubFile(QString("%1etc/default/grub").arg(mountPoint));
    if (!grubFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Could not open grub file for reading.";
        return;
    }

    const QString grubConfig = grubFile.readAll();
    grubFile.close();

    QRegularExpression regex("GRUB_CMDLINE_LINUX_DEFAULT=\"([^\"]*)\"");
    QRegularExpressionMatch match = regex.match(grubConfig);

    if (match.hasMatch()) {
        QString bootOptions = match.captured(1);
        if (!bootOptions.isEmpty()) {
            ui->textKernelOptions->setText(bootOptions);
        } else {
            qWarning() << "Captured boot options are empty.";
        }
    } else {
        qWarning() << "No match found for GRUB_CMDLINE_LINUX_DEFAULT.";
    }
}

// Try to guess root partition by checking partition labels and types
void MainWindow::guessPartition()
{
    const bool isFrugal = ui->tabWidget->currentIndex() == Tab::Frugal;
    auto *comboPartition = isFrugal ? ui->comboPartition : ui->comboPartitionStub;

    if (ui->tabWidget->currentIndex() == Tab::StubInstall) {
        disconnect(ui->comboPartitionStub, nullptr, this, nullptr);
        connect(ui->comboPartitionStub, &QComboBox::currentTextChanged, this, [this]() {
            if (!ui->comboPartitionStub->currentText().isEmpty()) {
                const QString mountPoint = mountPartition(ui->comboPartitionStub->currentText().section(' ', 0, 0));
                if (mountPoint.isEmpty()) {
                    return;
                }
                getGrubOptions(mountPoint);
                selectKernel();
            }
        });
    }

    const int partitionCount = comboPartition->count();

    // Known identifiers for Linux root partitions
    const QString rootMXLabel = "rootMX";
    const QStringList linuxPartTypes = {
        "0x83",                                 // Linux native partition
        "0fc63daf-8483-4772-8e79-3d69d8477de4", // Linux filesystem
        "44479540-F297-41B2-9AF7-D131D5F0458A", // Linux root (x86)
        "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709"  // Linux root (x86-64)
    };

    // Helper function to search partitions matching a command pattern
    auto findPartition = [&](const QString &command) -> bool {
        for (int index = 0; index < partitionCount; ++index) {
            const QString part = comboPartition->itemText(index).section(' ', 0, 0);
            if (cmd.runAsRoot(command.arg(part), nullptr, nullptr, true)) {
                comboPartition->setCurrentIndex(index);
                return true;
            }
        }
        return false;
    };

    // Try to find partition with rootMX* label
    if (findPartition(QString("lsblk -ln -o LABEL /dev/%1 | grep -q %2").arg("%1", rootMXLabel))) {
        return;
    }

    // Fall back to checking for any Linux partition type
    findPartition(QString("lsblk -ln -o PARTTYPE /dev/%1 | grep -qEi '%2'").arg("%1", linuxPartTypes.join('|')));
}

void MainWindow::listDevices()
{
    static bool firstRun {true};
    if (firstRun) {
        firstRun = false;
        QString cmd_str(
            "lsblk -ln -o NAME,SIZE,LABEL,MODEL -d -e 2,11 -x NAME | grep -E '^x?[h,s,v].[a-z]|^mmcblk|^nvme'");
        listDrive = cmd.getOut(cmd_str).split('\n', Qt::SkipEmptyParts);

        cmd_str = "lsblk -ln -o NAME,SIZE,FSTYPE,MOUNTPOINT,LABEL -e 2,11 -x NAME | grep -E "
                  "'^x?[h,s,v].[a-z][0-9]|^mmcblk[0-9]+p|^nvme[0-9]+n[0-9]+p'";
        listPart = cmd.getOut(cmd_str).split('\n', Qt::SkipEmptyParts);
    }
}

void MainWindow::validateAndLoadOptions(const QString &frugalDir)
{
    QDir dir(frugalDir);
    const QStringList requiredFiles = {"vmlinuz", "linuxfs", "grub.entry"};
    const QStringList existingFiles = dir.entryList(requiredFiles, QDir::Files);

    QStringList missingFiles;
    missingFiles.reserve(requiredFiles.size());
    for (const QString &file : requiredFiles) {
        if (!existingFiles.contains(file)) {
            missingFiles.append(file);
        }
    }

    if (!missingFiles.isEmpty()) {
        QMessageBox::critical(this, tr("UEFI Installer"),
                              tr("Are you sure this is the MX or antiX Frugal installation location?\nMissing "
                                 "mandatory files in directory: ")
                                  + missingFiles.join(", "));
        ui->pushBack->click();
        return;
    }

    bool success = readGrubEntry();
    if (!success) {
        QMessageBox::critical(this, tr("UEFI Installer"), tr("Failed to read grub.entry file."));
        ui->pushBack->click();
        return;
    }

    if (!options.persistenceType.isEmpty()) {
        ui->comboFrugalMode->setCurrentText(options.persistenceType);
    }
    if (!options.entryName.isEmpty()) {
        ui->textUefiEntryFrugal->setText(options.entryName);
    }
    ui->textOptionsFrugal->setText(options.stringOptions);
    ui->pushNext->setEnabled(true);
    ui->pushNext->setText(tr("Install"));
    ui->pushNext->setIcon(QIcon::fromTheme("run-install"));
}

QString MainWindow::selectFrugalDirectory(const QString &partition)
{
    return QFileDialog::getExistingDirectory(this, tr("Select Frugal Directory"), partition, QFileDialog::ShowDirsOnly);
}

QString MainWindow::selectESP()
{
    QStringList espList;
    espList.reserve(2);
    for (const QString &part : listPart) {
        if (cmd.runAsRoot("lsblk -ln -o PARTTYPE /dev/" + part.section(' ', 0, 0)
                          + "| grep -qiE 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b|0xef'")) {
            espList.append(part.section(' ', 0, 0));
        }
    }

    if (espList.isEmpty()) {
        QMessageBox::critical(this, QApplication::applicationDisplayName(), tr("No EFI System Partitions found."));
        return {};
    }

    bool ok;
    QInputDialog dialog(this);
    dialog.setWindowTitle(tr("Select EFI System Partition"));
    dialog.setLabelText(tr("EFI System Partitions:"));
    dialog.setComboBoxItems(espList);
    dialog.setMinimumWidth(400);
    dialog.resize(dialog.minimumWidth(), dialog.height());
    QString selectedEsp;
    if (dialog.exec() == QDialog::Accepted) {
        selectedEsp = dialog.textValue();
        ok = true;
    } else {
        ok = false;
    }

    if (!ok || selectedEsp.isEmpty()) {
        QMessageBox::warning(this, QApplication::applicationDisplayName(), tr("No EFI System Partition selected"));
        return {};
    }

    espMountPoint = mountPartition(selectedEsp);
    if (espMountPoint.isEmpty()) {
        QMessageBox::warning(this, QApplication::applicationDisplayName(),
                             tr("Could not mount selected EFI System Partition"));
        return {};
    }

    const bool isFrugal = ui->tabWidget->currentIndex() == Tab::Frugal;
    const QString subDir = isFrugal ? "/frugal" : "/stub";
    const QString targetPath = espMountPoint + "/EFI/" + distro + subDir;
    cmd.procAsRoot("rm", {"-f", targetPath + "/vmlinuz"});
    cmd.procAsRoot("rm", {"-f", targetPath + "/initrd.{gz,img}"});
    if (!checkSizeEsp()) {
        QMessageBox::critical(this, QApplication::applicationDisplayName(),
                              tr("Not enough space on the EFI System Partition to copy the kernel and initrd files."));
        return {};
    }
    return selectedEsp;
}

void MainWindow::pushNext_clicked()
{
    if (ui->tabWidget->currentIndex() == Tab::Frugal) {
        if (ui->stackedFrugal->currentIndex() == Page::Location) {
            ui->pushNext->setEnabled(false);
            if (!ui->comboDrive->currentText().isEmpty() && !ui->comboPartition->currentText().isEmpty()) {
                QString part = mountPartition(ui->comboPartition->currentText().section(' ', 0, 0));
                if (part.isEmpty()) {
                    QMessageBox::critical(
                        this, QApplication::applicationDisplayName(),
                        tr("Could not mount partition. Please make sure you selected the correct partition."));
                    refreshFrugal();
                    return;
                }
                frugalDir = selectFrugalDirectory(part);
                if (!frugalDir.isEmpty()) {
                    ui->stackedFrugal->setCurrentIndex(Page::Options);
                    ui->pushBack->setEnabled(true);
                } else {
                    QMessageBox::warning(this, QApplication::applicationDisplayName(), tr("No directory selected"));
                    refreshFrugal();
                    return;
                }
                validateAndLoadOptions(frugalDir);
            }
        } else if (ui->stackedFrugal->currentIndex() == Page::Options) {
            QString esp = selectESP();
            if (esp.isEmpty()) {
                return;
            }
            if (installEfiStub(esp)) {
                QMessageBox::information(this, QApplication::applicationDisplayName(),
                                         tr("EFI stub installed successfully."));
            } else {
                QMessageBox::critical(this, QApplication::applicationDisplayName(), tr("Failed to install EFI stub."));
            }
        }
    } else if (ui->tabWidget->currentIndex() == Tab::StubInstall) {
        if (ui->comboDriveStub->currentText().isEmpty() || ui->comboPartitionStub->currentText().isEmpty()
            || ui->textEntryName->text().isEmpty()) {
            QMessageBox::warning(this, QApplication::applicationDisplayName(), tr("All fields are required"));
            return;
        }
        QString part = mountPartition(ui->comboPartitionStub->currentText().section(' ', 0, 0));
        if (part.isEmpty()) {
            QMessageBox::critical(
                this, QApplication::applicationDisplayName(),
                tr("Could not mount partition. Please make sure you selected the correct partition."));
            refreshStubInstall();
            return;
        }

        loadStubOption();

        QString esp = selectESP();
        if (esp.isEmpty()) {
            QMessageBox::critical(this, QApplication::applicationDisplayName(), tr("Could not select ESP"));
            refreshStubInstall();
            return;
        }
        if (installEfiStub(esp)) {
            QMessageBox::information(this, QApplication::applicationDisplayName(),
                                     tr("EFI stub installed successfully."));
        } else {
            QMessageBox::critical(this, QApplication::applicationDisplayName(), tr("Failed to install EFI stub."));
            refreshStubInstall();
        }
    }
}

void MainWindow::pushAbout_clicked()
{
    this->hide();
    displayAboutMsgBox(
        tr("About %1").arg(QApplication::applicationDisplayName()),
        R"(<p align="center"><b><h2>UEFI Manager</h2></b></p><p align="center">)" + tr("Version: ")
            + QApplication::applicationVersion() + "</p><p align=\"center\"><h3>" + tr("Description goes here")
            + R"(</h3></p><p align="center"><a href="http://mxlinux.org">http://mxlinux.org</a><br /></p><p align="center">)"
            + tr("Copyright (c) MX Linux") + "<br /><br /></p>",
        "/usr/share/doc/uefi-manager/license.html", tr("%1 License").arg(this->windowTitle()));

    this->show();
}

void MainWindow::pushHelp_clicked()
{
    const QString url = "https://forum.mxlinux.org";
    displayDoc(url, tr("%1 Help").arg(this->windowTitle()));
}

void MainWindow::pushBack_clicked()
{
    if (ui->stackedFrugal->currentIndex() == Page::Options) {
        ui->stackedFrugal->setCurrentIndex(Page::Location);
        ui->pushBack->setEnabled(false);
        ui->pushNext->setText(tr("Next"));
        ui->pushNext->setIcon(QIcon::fromTheme("go-next"));
        ui->pushNext->setEnabled(true);
    }
}

void MainWindow::saveBootOrder(const QListWidget *list)
{
    QStringList orderList;
    orderList.reserve(list->count());
    for (int i = 0; i < list->count(); ++i) {
        QString item = list->item(i)->text().section(' ', 0, 0);
        item.remove(QRegularExpression("^Boot|\\*$"));
        if (item.contains(QRegularExpression("^[0-9A-Z]{4}$"))) {
            orderList.append(item);
        }
    }

    QString order = orderList.join(',');
    if (!cmd.procAsRoot("efibootmgr", {"-o", order})) {
        QMessageBox::critical(this, tr("Error"), tr("Something went wrong, could not save boot order."));
    }
}

void MainWindow::setUefiTimeout(QWidget *uefiDialog, QLabel *textTimeout)
{
    bool ok = false;
    ushort initialTimeout = textTimeout->text().section(' ', 1, 1).toUInt();
    ushort newTimeout = QInputDialog::getInt(uefiDialog, tr("Set timeout"), tr("Timeout in seconds:"), initialTimeout,
                                             0, 65535, 1, &ok);

    if (ok && Cmd().procAsRoot("efibootmgr", {"-t", QString::number(newTimeout)})) {
        textTimeout->setText(tr("Timeout: %1 seconds").arg(newTimeout));
    }
}

void MainWindow::setUefiBootNext(QListWidget *listEntries, QLabel *textBootNext)
{
    if (auto currentItem = listEntries->currentItem()) {
        QString item = currentItem->text().section(' ', 0, 0);
        item.remove(QRegularExpression("^Boot"));
        item.remove(QRegularExpression(R"(\*$)"));

        if (QRegularExpression("^[0-9A-Z]{4}$").match(item).hasMatch()
            && Cmd().procAsRoot("efibootmgr", {"-n", item})) {
            textBootNext->setText(tr("Boot Next: %1").arg(item));
        }
    }
}

void MainWindow::removeUefiEntry(QListWidget *listEntries, QWidget *uefiDialog)
{
    auto *currentItem = listEntries->currentItem();
    if (!currentItem) {
        return;
    }

    QString itemText = currentItem->text();
    if (QMessageBox::Yes
        != QMessageBox::question(uefiDialog, tr("Removal confirmation"),
                                 tr("Are you sure you want to delete this boot entry?\n%1").arg(itemText))) {
        return;
    }

    QString item = itemText.section(' ', 0, 0);
    item.remove(QRegularExpression("^Boot"));
    item.remove(QRegularExpression(R"(\*$)"));

    if (!item.contains(QRegularExpression("^[0-9A-Z]{4}$"))) {
        return;
    }

    if (Cmd().procAsRoot("efibootmgr", {"-B", "-b", item})) {
        delete currentItem;
    }
    emit listEntries->itemSelectionChanged();
}
