/**********************************************************************
 *
 **********************************************************************
 * Copyright (C) 2024-2025 MX Authors
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
#include "about.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
void setupDocDialog(QDialog &dialog, QTextBrowser *browser, const QString &title, bool largeWindow)
{
    dialog.setWindowTitle(title);
    if (largeWindow) {
        dialog.setWindowFlags(Qt::Window);
        dialog.resize(1000, 800);
    } else {
        dialog.resize(700, 600);
    }

    browser->setOpenExternalLinks(true);

    auto *btnClose = new QPushButton(QObject::tr("&Close"), &dialog);
    btnClose->setIcon(QIcon::fromTheme(QStringLiteral("window-close")));
    QObject::connect(btnClose, &QPushButton::clicked, &dialog, &QDialog::close);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(browser);
    layout->addWidget(btnClose);
}

void showHtmlDoc(const QString &url, const QString &title, bool largeWindow)
{
    QDialog dialog;
    auto *browser = new QTextBrowser(&dialog);
    setupDocDialog(dialog, browser, title, largeWindow);

    const QUrl sourceUrl = QUrl::fromUserInput(url);
    const QString localPath = sourceUrl.isLocalFile() ? sourceUrl.toLocalFile() : url;
    if (sourceUrl.isLocalFile() ? QFileInfo::exists(localPath) : QFileInfo::exists(url)) {
        browser->setSource(sourceUrl.isLocalFile() ? sourceUrl : QUrl::fromLocalFile(url));
    } else {
        browser->setText(QObject::tr("Could not load %1").arg(url));
        qWarning("Could not load HTML document: %s", qPrintable(url));
    }
    dialog.exec();
}
} // namespace

void displayDoc(const QString &url, const QString &title, bool largeWindow)
{
    showHtmlDoc(url, title, largeWindow);
}

void displayHelpDoc(const QString &path, const QString &title)
{
    showHtmlDoc(path, title, true);
}

void displayAboutMsgBox(const QString &title, const QString &message, const QString &licenseUrl,
                        const QString &licenseTitle)
{
    QMessageBox msgBox(QMessageBox::NoIcon, title, message);

    QPushButton *btnLicense = msgBox.addButton(QObject::tr("License"), QMessageBox::HelpRole);
    QPushButton *btnChangelog = msgBox.addButton(QObject::tr("Changelog"), QMessageBox::HelpRole);
    QPushButton *btnCancel = msgBox.addButton(QObject::tr("Cancel"), QMessageBox::NoRole);
    btnCancel->setIcon(QIcon::fromTheme("window-close"));

    msgBox.exec();

    if (msgBox.clickedButton() == btnLicense) {
        displayDoc(licenseUrl, licenseTitle);
    } else if (msgBox.clickedButton() == btnChangelog) {
        QDialog changelog;
        changelog.setWindowTitle(QObject::tr("Changelog"));
        auto *text = new QTextEdit(&changelog);
        text->setReadOnly(true);

        QString changelogPath
            = "/usr/share/doc/" + QFileInfo(QCoreApplication::applicationFilePath()).fileName() + "/changelog.gz";
        bool zcatExists = !QStandardPaths::findExecutable("zcat").isEmpty();
        bool changelogExists = QFileInfo::exists(changelogPath);

        if (zcatExists && changelogExists) {
            QProcess proc;
            proc.start("zcat", {changelogPath}, QIODevice::ReadOnly);
            if (proc.waitForStarted(3000) && proc.waitForFinished(3000)) {
                text->setText(proc.readAllStandardOutput());
            } else {
                text->setText(QObject::tr("Could not load changelog."));
            }
        } else {
            if (!changelogExists) {
                text->setText(QObject::tr("Error: Changelog file is missing."));
            } else if (!zcatExists) {
                text->setText(QObject::tr("Error: Required utility 'zcat' is missing."));
            }
        }

        auto *layout = new QVBoxLayout(&changelog);
        layout->addWidget(text);

        auto *btnClose = new QPushButton(QObject::tr("&Close"), &changelog);
        btnClose->setIcon(QIcon::fromTheme("window-close"));
        QObject::connect(btnClose, &QPushButton::clicked, &changelog, &QDialog::close);
        layout->addWidget(btnClose);
        changelog.setLayout(layout);
        changelog.resize(600, 500);
        changelog.exec();
    }
}
