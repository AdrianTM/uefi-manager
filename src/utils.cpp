#include "utils.h"

#include <QRegularExpression>
#include <algorithm>

namespace utils
{

QStringList sortKernelVersions(const QStringList &kernelFiles, bool reverse)
{
    auto versionCompare = [reverse](const QString &a, const QString &b) {
        QRegularExpression regex(R"((\d+)\.(\d+)(?:\.(\d+))?(-([a-z0-9]+[^-]*)?)?(-.*)?)");

        QRegularExpressionMatch matchA = regex.match(a);
        QRegularExpressionMatch matchB = regex.match(b);

        if (!matchA.hasMatch() && !matchB.hasMatch()) {
            return reverse ? a > b : a < b;
        }
        if (!matchA.hasMatch()) {
            return reverse;
        }
        if (!matchB.hasMatch()) {
            return !reverse;
        }

        int majorA = matchA.captured(1).toInt();
        int minorA = matchA.captured(2).toInt();
        int patchA = matchA.captured(3).isEmpty() ? 0 : matchA.captured(3).toInt();

        int majorB = matchB.captured(1).toInt();
        int minorB = matchB.captured(2).toInt();
        int patchB = matchB.captured(3).isEmpty() ? 0 : matchB.captured(3).toInt();

        if (majorA != majorB) {
            return reverse ? majorA > majorB : majorA < majorB;
        }
        if (minorA != minorB) {
            return reverse ? minorA > minorB : minorA < minorB;
        }
        if (patchA != patchB) {
            return reverse ? patchA > patchB : patchA < patchB;
        }

        return reverse ? matchA.captured(4) > matchB.captured(4) : matchA.captured(4) < matchB.captured(4);
    };

    QStringList sortedList = kernelFiles;
    std::sort(sortedList.begin(), sortedList.end(), versionCompare);
    return sortedList;
}

QString extractDiskFromPartition(const QString &partition)
{
    QRegularExpression regex("^(.*?)(\\d+)$");
    QRegularExpressionMatch match = regex.match(partition);
    QString diskDeviceName = match.hasMatch() ? match.captured(1) : partition;
    if ((diskDeviceName.startsWith("nvme") || diskDeviceName.startsWith("mmcblk"))
        && diskDeviceName.endsWith("p")) {
        diskDeviceName.chop(1);
    }
    return diskDeviceName;
}

} // namespace utils
