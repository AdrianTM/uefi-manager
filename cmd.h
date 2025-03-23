#pragma once

#include <QProcess>

class QTextStream;

enum struct Elevate { No, Yes };
enum struct Quiet { No, Yes };

class Cmd : public QProcess
{
    Q_OBJECT
public:
    explicit Cmd(QObject *parent = nullptr);
    bool proc(const QString &cmd, const QStringList &args = {}, QString *output = nullptr,
              const QByteArray *input = nullptr, Quiet quiet = Quiet::No, Elevate elevate = Elevate::No);
    bool procAsRoot(const QString &cmd, const QStringList &args = {}, QString *output = nullptr,
                    const QByteArray *input = nullptr, Quiet quiet = Quiet::No);
    bool run(const QString &cmd, QString *output = nullptr, const QByteArray *input = nullptr, Quiet quiet = Quiet::No,
             Elevate elevate = Elevate::No);
    bool runAsRoot(const QString &cmd, QString *output = nullptr, const QByteArray *input = nullptr,
                   Quiet quiet = Quiet::No);
    [[nodiscard]] QString getOut(const QString &cmd, Quiet quiet = Quiet::No, Elevate elevate = Elevate::No);
    [[nodiscard]] QString getOutAsRoot(const QString &cmd, Quiet quiet = Quiet::No);

signals:
    void done();
    void errorAvailable(const QString &err);
    void outputAvailable(const QString &out);

private:
    QString out_buffer;
    QString asRoot;
    QString helper;
};
