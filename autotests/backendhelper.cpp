// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Test-only replacement for the encryption executables. Never mounts or
// encrypts anything: records argv/stdin and returns the requested exit status.
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <csignal>
#include <cstdio>
#include <poll.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    qputenv("LC_ALL", "C.UTF-8");
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments().mid(1);
    if (arguments.value(0) == QLatin1String("--process-test")) {
        QFile output;
        QFile error;
        if (!output.open(stdout, QIODevice::WriteOnly) || !error.open(stderr, QIODevice::WriteOnly)) {
            return 90;
        }
        output.write(arguments.value(1).toUtf8());
        error.write(arguments.value(2).toUtf8());
        output.flush();
        error.flush();
        if (arguments.contains(QStringLiteral("--crash"))) {
            std::raise(SIGTERM); // exercise abnormal termination without producing a core dump
        }
        return arguments.value(3).toInt();
    }
    if (arguments.value(0) == QLatin1String("--version-test")) {
        QFile output;
        if (!output.open(arguments.contains(QStringLiteral("--stderr")) ? stderr : stdout, QIODevice::WriteOnly)) {
            return 90;
        }
        output.write(arguments.value(1).toUtf8());
        return 0;
    }

    QString logPath;
    int exitCode = 73;
    for (const auto& argument : arguments) {
        if (argument.startsWith(QLatin1String("--test-log="))) {
            logPath = argument.mid(11);
        }
        if (argument.startsWith(QLatin1String("--test-exit="))) {
            exitCode = argument.mid(12).toInt();
        }
    }

    QJsonArray input;
    const int lines = arguments.contains(QStringLiteral("-init")) ? 2 : 1;
    for (int i = 0; i < lines; ++i) {
        QByteArray line;
        char ch;
        do {
            pollfd descriptor{STDIN_FILENO, POLLIN, 0};
            if (poll(&descriptor, 1, 2000) <= 0 || read(STDIN_FILENO, &ch, 1) != 1) {
                return 91;
            }
            line += ch;
        } while (ch != '\n' && line.size() < 4096);
        input.append(QString::fromUtf8(line));
    }

    const QJsonObject record{{QStringLiteral("arguments"), QJsonArray::fromStringList(arguments)},
        {QStringLiteral("input"), input},
        {QStringLiteral("frontend"), QString::fromUtf8(qgetenv("CRYFS_FRONTEND"))}};
    QFile log(logPath);
    if (!log.open(QIODevice::WriteOnly)) {
        return 92;
    }
    log.write(QJsonDocument(record).toJson());
    return exitCode;
}
