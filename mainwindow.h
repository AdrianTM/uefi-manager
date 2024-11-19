/**********************************************************************
 *  mainwindow.h
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
#pragma once

#include <QCommandLineParser>
#include <QMessageBox>
#include <QSettings>

#include "cmd.h"

namespace Ui
{
class MainWindow;
}

namespace Tab
{
enum { Location, Options };
}

class MainWindow : public QDialog
{
    Q_OBJECT

public:
    explicit MainWindow(const QCommandLineParser &arg_parser, QWidget *parent = nullptr);
    ~MainWindow() override;
    void centerWindow();
    void setup();

private slots:
    void cmdDone();
    void cmdStart();
    void pushAbout_clicked();
    void pushHelp_clicked();
    void pushBack_clicked();
    void pushNext_clicked();
    void setConnections();

private:
    Ui::MainWindow *ui;
    Cmd cmd;
    QString distro = getDistroName();
    QString espMountPoint;
    QString frugalDir;
    QSettings settings;
    QStringList listDrive;
    QStringList listPart;
    QStringList newDirectories;
    QStringList newMounts;

    struct Options {
        QString entryName;
        QString uuid;
        QString bdir;
        QString stringOptions;
        QString persistenceType;
    } options;

    [[nodiscard]] QString getDistroName();
    [[nodiscard]] QString mountPartition(QString part);
    [[nodiscard]] QString selectESP();
    [[nodiscard]] QString selectFrugalDirectory(const QString &part);
    [[nodiscard]] bool checkSizeEsp();
    [[nodiscard]] bool copyKernel();
    [[nodiscard]] bool installUefiStub(const QString &esp);
    [[nodiscard]] bool readGrubEntry();
    void addDevToList();
    void filterDrivePartitions();
    void promptFrugalStubInstall();
    void validateAndLoadOptions(const QString &frugalDir);
};
