#include "cmd.h"

#include <QApplication>
#include <QDebug>
#include <QEventLoop>
#include <QFile>

#include <unistd.h>

Cmd::Cmd(QObject *parent)
    : QProcess(parent),
      asRoot {QFile::exists("/usr/bin/pkexec") ? "/usr/bin/pkexec" : "/usr/bin/gksu"},
      helper {"/usr/lib/" + QApplication::applicationName() + "/helper"}
{
    connect(this, &Cmd::readyReadStandardOutput, [this] { emit outputAvailable(readAllStandardOutput()); });
    connect(this, &Cmd::readyReadStandardError, [this] { emit errorAvailable(readAllStandardError()); });
    connect(this, &Cmd::outputAvailable, [this](const QString &out) { out_buffer += out; });
    connect(this, &Cmd::errorAvailable, [this](const QString &out) { out_buffer += out; });
}

QString Cmd::getOut(const QString &cmd, Quiet quiet, Elevate elevate)
{
    QString output;
    run(cmd, &output, nullptr, quiet, elevate);
    return output;
}

QString Cmd::getOutAsRoot(const QString &cmd, Quiet quiet)
{
    return getOut(cmd, quiet, Elevate::Yes);
}

bool Cmd::proc(const QString &cmd, const QStringList &args, QString *output, const QByteArray *input, Quiet quiet,
               Elevate elevate)
{
    out_buffer.clear();
    connect(this, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &Cmd::done);
    if (this->state() != QProcess::NotRunning) {
        qDebug() << "Process already running:" << this->program() << this->arguments();
        return false;
    }
    if (quiet == Quiet::No) {
        qDebug() << cmd << args;
    }
    QEventLoop loop;
    connect(this, &Cmd::done, &loop, &QEventLoop::quit);
    if (elevate == Elevate::Yes && getuid() != 0) {
        QStringList cmdAndArgs = QStringList() << helper << cmd << args;
        start(asRoot, {cmdAndArgs});
    } else {
        start(cmd, args);
    }
    if (input) {
        write(*input);
    }
    closeWriteChannel();
    loop.exec();
    if (output) {
        *output = out_buffer.trimmed();
    }
    return (exitStatus() == QProcess::NormalExit && exitCode() == 0);
}

bool Cmd::procAsRoot(const QString &cmd, const QStringList &args, QString *output, const QByteArray *input, Quiet quiet)
{
    return proc(cmd, args, output, input, quiet, Elevate::Yes);
}

bool Cmd::run(const QString &cmd, QString *output, const QByteArray *input, Quiet quiet, Elevate elevate)
{
    if (elevate == Elevate::Yes && getuid() != 0) {
        return proc(asRoot, {helper, cmd}, output, input, quiet);
    } else {
        return proc("/bin/bash", {"-c", cmd}, output, input, quiet);
    }
}

bool Cmd::runAsRoot(const QString &cmd, QString *output, const QByteArray *input, Quiet quiet)
{
    return run(cmd, output, input, quiet, Elevate::Yes);
}
