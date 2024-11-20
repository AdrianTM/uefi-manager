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
#include <QScreen>
#include <QScrollBar>
#include <QStorageInfo>
#include <QTextStream>
#include <QTimer>

#include "about.h"
#include "cmd.h"

MainWindow::MainWindow(const QCommandLineParser &arg_parser, QWidget *parent)
    : QDialog(parent),
      ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowFlags(Qt::Window);
    if (arg_parser.isSet("frugal")) {
        qDebug() << "Frugal mode";
        promptFrugalStubInstall();
    }
    setup();
    setConnections();
    addDevToList();
}

MainWindow::~MainWindow()
{
    settings.setValue("geometry", saveGeometry());
    for (auto it = newMounts.rbegin(); it != newMounts.rend(); ++it) {
        cmd.procAsRoot("umount", {*it});
    }
    for (const auto &dir : newDirectories) {
        cmd.procAsRoot("rmdir", {dir});
    }
    delete ui;
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
        if (this->isMaximized()) { // add option to resize if maximized
            this->resize(size);
            centerWindow();
        }
    }
    this->adjustSize();
    ui->stackedWidget->setCurrentIndex(Tab::Location);
    ui->pushCancel->setEnabled(true);
    ui->pushBack->setEnabled(false);
    ui->pushNext->setEnabled(true);
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
    connect(ui->pushAbout, &QPushButton::clicked, this, &MainWindow::pushAbout_clicked);
    connect(ui->pushBack, &QPushButton::clicked, this, &MainWindow::pushBack_clicked);
    connect(ui->pushCancel, &QPushButton::pressed, this, &MainWindow::close);
    connect(ui->pushHelp, &QPushButton::clicked, this, &MainWindow::pushHelp_clicked);
    connect(ui->pushNext, &QPushButton::clicked, this, &MainWindow::pushNext_clicked);
}
bool MainWindow::copyKernel()
{
    if (espMountPoint.isEmpty()) {
        return false;
    }
    const QString targetPath = espMountPoint + "/EFI/" + distro + "/frugal/";
    if (!QFile::exists(targetPath) && !cmd.procAsRoot("mkdir", {"-p", targetPath})) {
        return false;
    }
    const QStringList filesToCopy = {frugalDir + "/vmlinuz", frugalDir + "/initrd.gz"};
    for (const QString &file : filesToCopy) {
        if (!cmd.procAsRoot("cp", {file, targetPath})) {
            return false;
        }
    }
    return true;
}

bool MainWindow::installUefiStub(const QString &esp)
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

    QStringList args;
    args << "--disk" << disk << "--part" << part << "--create"
         << "--label" << '"' + ui->textUefiEntry->text() + '"' << "--loader"
         << QString("\"\\EFI\\%1\\frugal\\vmlinuz\"").arg(distro) << "--unicode"
         << QString("'bdir=%1 buuid=%2 %3 %4 initrd=/EFI/%5/frugal/initrd.gz'")
                .arg(options.bdir, options.uuid, options.stringOptions, ui->comboFrugalMode->currentText(), distro);

    if (!cmd.procAsRoot("efibootmgr", args)) {
        return false;
    }

    return true;
}

QString MainWindow::mountPartition(QString part)
{
    if (part.startsWith("/dev/")) {
        part = part.mid(5);
    }

    QString mountDir = cmd.getOut("findmnt -nf --source /dev/" + part).section(" ", 0, 0);
    if (!mountDir.isEmpty()) {
        return mountDir;
    }

    mountDir = "/mnt/" + part;
    if (!QDir(mountDir).exists()) {
        if (!cmd.procAsRoot("mkdir", {mountDir})) {
            setup();
            return {};
        }
        newDirectories.append(mountDir);
    }

    if (!cmd.procAsRoot("mount", {"/dev/" + part, mountDir})) {
        setup();
        return {};
    }
    newMounts.append(mountDir);
    return mountDir;
}

// Add list of devices to comboLocation
void MainWindow::addDevToList()
{
    QString cmd_str("lsblk -ln -o NAME,SIZE,LABEL,MODEL -d -e 2,11 -x NAME | grep -E '^x?[h,s,v].[a-z]|^mmcblk|^nvme'");
    listDrive = cmd.getOut(cmd_str).split('\n', Qt::SkipEmptyParts);

    cmd_str = "lsblk -ln -o NAME,SIZE,FSTYPE,MOUNTPOINT,LABEL -e 2,11 -x NAME | grep -E "
              "'^x?[h,s,v].[a-z][0-9]|^mmcblk[0-9]+p|^nvme[0-9]+n[0-9]+p'";
    listPart = cmd.getOut(cmd_str).split('\n', Qt::SkipEmptyParts);
    ui->comboDrive->clear();
    ui->comboDrive->addItems(listDrive);
    filterDrivePartitions();
}

bool MainWindow::checkSizeEsp()
{
    QString vmlinuzPath = frugalDir + "/vmlinuz";
    QString initrdPath = frugalDir + "/initrd.gz";
    qint64 vmlinuzSize = QFile(vmlinuzPath).size();
    qint64 initrdSize = QFile(initrdPath).size();
    qint64 totalSize = vmlinuzSize + initrdSize;

    qint64 espFreeSpace = QStorageInfo(espMountPoint).bytesAvailable();
    if (totalSize > espFreeSpace) {
        return false;
    }
    return true;
}

void MainWindow::filterDrivePartitions()
{
    ui->comboPartition->clear();
    QString drive = ui->comboDrive->currentText().section(' ', 0, 0);
    if (!drive.isEmpty()) {
        QStringList drivePart = listPart.filter(QRegularExpression("^" + drive));
        ui->comboPartition->addItems(drivePart);
    }
}

void MainWindow::promptFrugalStubInstall()
{
    int ret = QMessageBox::question(this, tr("UEFI Installer"),
                                    tr("A recent frugal install has been detected. Do you wish to add a UEFI entry "
                                       "direct to your UEFI system menu?"),
                                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (ret == QMessageBox::No) {
        cmd.procAsRoot("/usr/lib/uefi-stub-installer/uefistub--lib", {"write_checkfile"});
        QTimer::singleShot(0, qApp, &QApplication::quit);
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
                } else if (option.startsWith("persist_all") || option.startsWith("persist_root")
                           || option.startsWith("persist_static") || option.startsWith("persist_static_root")
                           || option.startsWith("p_static_root") || option.startsWith("persist_home")) {
                    options.persistenceType = option;
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

QString MainWindow::getDistroName()
{
    QFile file("/etc/initrd_release");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return "MX"; // Default to MX
    }
    QTextStream in(&file);
    QString line;
    QString distroName;

    while (!in.atEnd()) {
        line = in.readLine();
        if (line.startsWith("NAME=")) {
            distroName = line.section('=', 1, 1).remove('"').trimmed();
            break;
        }
    }

    file.close();
    return distroName;
}

void MainWindow::validateAndLoadOptions(const QString &frugalDir)
{
    QString cmd_str("ls -1 " + frugalDir + " | grep -E 'vmlinuz|grub\\.entry|linuxfs'");
    QStringList listFiles = cmd.getOut(cmd_str).split('\n', Qt::SkipEmptyParts);
    QStringList requiredFiles = {"vmlinuz", "linuxfs", "grub.entry"};

    QStringList missingFiles;
    for (const QString &requiredFile : requiredFiles) {
        if (!listFiles.contains(requiredFile)) {
            missingFiles.append(requiredFile);
        }
    }

    if (!missingFiles.isEmpty()) {
        QMessageBox::critical(
            this, tr("UEFI Installer"),
            tr("Are you sure this is the Frugal installation location?\nMissing mandatory files in directory: ")
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
        ui->textUefiEntry->setText(options.entryName);
    }
    ui->textOptions->setText(options.stringOptions);
    ui->pushNext->setEnabled(true);
    ui->pushNext->setText(tr("Install"));
    ui->pushNext->setIcon(QIcon::fromTheme("install"));
}

QString MainWindow::selectFrugalDirectory(const QString &part)
{
    return QFileDialog::getExistingDirectory(this, tr("Select Frugal Directory"), part, QFileDialog::ShowDirsOnly);
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
        QMessageBox::critical(this, tr("UEFI Stub Installer"), tr("No EFI System Partitions found."));
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
        QMessageBox::warning(this, tr("UEFI Stub Installer"), tr("No EFI System Partition selected"));
        return {};
    }

    espMountPoint = mountPartition(selectedEsp);
    if (espMountPoint.isEmpty()) {
        QMessageBox::warning(this, tr("UEFI Stub Installer"), tr("Could not mount selected EFI System Partition"));
        return {};
    }
    cmd.procAsRoot("rm", {"-f", espMountPoint + "/EFI/frugal/vmlinuz"});
    cmd.procAsRoot("rm", {"-f", espMountPoint + "/EFI/frugal/initrd.gz"});
    if (!checkSizeEsp()) {
        QMessageBox::critical(this, tr("UEFI Stub Installer"),
                              tr("Not enough space on the EFI System Partition to install the frugal installation."));
        return {};
    }
    return selectedEsp;
}

void MainWindow::pushNext_clicked()
{
    if (ui->stackedWidget->currentIndex() == Tab::Location) {
        ui->pushNext->setEnabled(false);
        if (!ui->comboDrive->currentText().isEmpty() && !ui->comboPartition->currentText().isEmpty()) {
            QString part = mountPartition(ui->comboPartition->currentText().section(' ', 0, 0));
            if (part.isEmpty()) {
                QMessageBox::critical(
                    this, tr("UEFI Stub Installer"),
                    tr("Could not mount partition. Please make sure you selected the correct partition."));
                setup();
                return;
            }
            frugalDir = selectFrugalDirectory(part);
            if (!frugalDir.isEmpty()) {
                ui->stackedWidget->setCurrentIndex(Tab::Options);
                ui->pushBack->setEnabled(true);
            } else {
                QMessageBox::warning(this, tr("UEFI Stub Installer"), tr("No directory selected"));
                setup();
                return;
            }
            validateAndLoadOptions(frugalDir);
        }
    } else if (ui->stackedWidget->currentIndex() == Tab::Options) {
        QString esp = selectESP();
        if (esp.isEmpty()) {
            return;
        }
        if (installUefiStub(esp)) {
            QMessageBox::information(this, tr("UEFI Stub Installer"), tr("UEFI stub installed successfully."));
        } else {
            QMessageBox::critical(this, tr("UEFI Stub Installer"), tr("Failed to install UEFI stub."));
        }
    }
}

void MainWindow::pushAbout_clicked()
{
    this->hide();
    displayAboutMsgBox(
        tr("About %1") + tr("Uefi Installer"),
        R"(<p align="center"><b><h2>Uefi Installer</h2></b></p><p align="center">)" + tr("Version: ")
            + QApplication::applicationVersion() + "</p><p align=\"center\"><h3>" + tr("Description goes here")
            + R"(</h3></p><p align="center"><a href="http://mxlinux.org">http://mxlinux.org</a><br /></p><p align="center">)"
            + tr("Copyright (c) MX Linux") + "<br /><br /></p>",
        "/usr/share/doc/uefi-stub-installer/license.html", tr("%1 License").arg(this->windowTitle()));

    this->show();
}

void MainWindow::pushHelp_clicked()
{
    const QString url = "https://forum.mxlinux.org";
    displayDoc(url, tr("%1 Help").arg(this->windowTitle()));
}

void MainWindow::pushBack_clicked()
{
    if (ui->stackedWidget->currentIndex() == Tab::Options) {
        ui->stackedWidget->setCurrentIndex(Tab::Location);
        ui->pushBack->setEnabled(false);
        ui->pushNext->setText(tr("Next"));
        ui->pushNext->setIcon(QIcon::fromTheme("go-next"));
        ui->pushNext->setEnabled(true);
    }
}
