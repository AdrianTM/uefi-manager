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

    QString partitionName = cmd.getOut("df " + fileName + " --output=source | sed 1d");
    QString disk = "/dev/" + cmd.getOut("lsblk -no PKNAME " + partitionName);
    QString partition = partitionName.mid(partitionName.lastIndexOf(QRegularExpression("[0-9]+$")));

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
    this->adjustSize();
    refreshFrugal();
    refreshEntries();
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
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, &MainWindow::tabWidget_currentChanged);
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
    const bool isFrugalTab = ui->tabWidget->currentIndex() == Tab::Frugal;
    ui->pushNext->setVisible(isFrugalTab);
    ui->pushBack->setVisible(isFrugalTab);
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
            refreshFrugal();
            return {};
        }
        newDirectories.append(mountDir);
    }

    if (!cmd.procAsRoot("mount", {"/dev/" + part, mountDir})) {
        refreshFrugal();
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
        qDebug() << "ITEM" << item;
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

    QStringList bootorder;
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
    ui->stackedWidget->setCurrentIndex(Page::Location);
    ui->pushCancel->setEnabled(true);
    ui->pushBack->setEnabled(false);
    ui->pushNext->setEnabled(true);
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
        QMessageBox::critical(this, tr("EFI Stub Installer"), tr("No EFI System Partitions found."));
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
        QMessageBox::warning(this, tr("EFI Stub Installer"), tr("No EFI System Partition selected"));
        return {};
    }

    espMountPoint = mountPartition(selectedEsp);
    if (espMountPoint.isEmpty()) {
        QMessageBox::warning(this, tr("EFI Stub Installer"), tr("Could not mount selected EFI System Partition"));
        return {};
    }
    cmd.procAsRoot("rm", {"-f", espMountPoint + "/EFI/" + distro + "/frugal/vmlinuz"});
    cmd.procAsRoot("rm", {"-f", espMountPoint + "/EFI/" + distro + "/frugal/initrd.gz"});
    if (!checkSizeEsp()) {
        QMessageBox::critical(this, tr("EFI Stub Installer"),
                              tr("Not enough space on the EFI System Partition to install the frugal installation."));
        return {};
    }
    return selectedEsp;
}

void MainWindow::pushNext_clicked()
{
    if (ui->tabWidget->currentIndex() == Tab::Frugal) {
        if (ui->stackedWidget->currentIndex() == Page::Location) {
            ui->pushNext->setEnabled(false);
            if (!ui->comboDrive->currentText().isEmpty() && !ui->comboPartition->currentText().isEmpty()) {
                QString part = mountPartition(ui->comboPartition->currentText().section(' ', 0, 0));
                if (part.isEmpty()) {
                    QMessageBox::critical(
                        this, tr("EFI Stub Installer"),
                        tr("Could not mount partition. Please make sure you selected the correct partition."));
                    refreshFrugal();
                    return;
                }
                frugalDir = selectFrugalDirectory(part);
                if (!frugalDir.isEmpty()) {
                    ui->stackedWidget->setCurrentIndex(Page::Options);
                    ui->pushBack->setEnabled(true);
                } else {
                    QMessageBox::warning(this, tr("EFI Stub Installer"), tr("No directory selected"));
                    refreshFrugal();
                    return;
                }
                validateAndLoadOptions(frugalDir);
            }
        } else if (ui->stackedWidget->currentIndex() == Page::Options) {
            QString esp = selectESP();
            if (esp.isEmpty()) {
                return;
            }
            if (installEfiStub(esp)) {
                QMessageBox::information(this, tr("EFI Stub Installer"), tr("EFI stub installed successfully."));
            } else {
                QMessageBox::critical(this, tr("EFI Stub Installer"), tr("Failed to install EFI stub."));
            }
        }
    }
}

void MainWindow::pushAbout_clicked()
{
    this->hide();
    displayAboutMsgBox(
        tr("About %1") + tr("UEFI Manager"),
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
    if (ui->stackedWidget->currentIndex() == Page::Options) {
        ui->stackedWidget->setCurrentIndex(Page::Location);
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
        qDebug() << "Order:" << order;
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
