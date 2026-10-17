// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <KConfigGroup>
#include <KSharedConfig>

#include "kded/engine/fusebackend_p.h"

using namespace PlasmaVault;

// Replace only the mount operation when testing the shared safety gates. The
// initialize/import/permission/process implementations are production code.
class RecordingBackend : public FuseBackend {
public:
    using FuseBackend::checkVersion;
    using FuseBackend::hasProcessFinishedSuccessfully;
    using FuseBackend::process;
    using FuseBackend::removeDotDirectory;
    using FuseBackend::setupMountPoint;

    RecordingBackend()
    {
        // Even a regression in the close guard must not run a real unmounter.
        fusermountExecutable = QString::fromLocal8Bit(VAULT_TEST_HELPER);
    }

    bool initialized = false;
    int mounts = 0;
    bool isInitialized(const Device&) const override
    {
        return initialized;
    }
    bool isOpened(const MountPoint&) const override
    {
        return false;
    }
    QString name() const override
    {
        return QStringLiteral("recording");
    }
    FutureResult<> validateBackend() override
    {
        return AsynQt::makeReadyFuture(Result<>::success());
    }

protected:
    FutureResult<> mount(const Device&, const MountPoint&, const Vault::Payload&) override
    {
        ++mounts;
        return AsynQt::makeReadyFuture(Result<>::success());
    }
};

class BackendTest : public QObject {
    Q_OBJECT

    static bool writeFile(const QString& path, const QByteArray& contents = QByteArray("keep me"))
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
    }

private Q_SLOTS:
    void initializationGuards_data()
    {
        QTest::addColumn<bool>("importing");
        QTest::addColumn<bool>("initialized");
        QTest::addColumn<bool>("deviceHasData");
        QTest::addColumn<bool>("mountHasData");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("create-empty") << false << false << false << false << true;
        QTest::newRow("create-existing-encryption") << false << true << false << false << false;
        QTest::newRow("create-would-overwrite-storage") << false << false << true << false << false;
        QTest::newRow("create-would-hide-mount-data") << false << false << false << true << false;
        QTest::newRow("import-existing") << true << true << true << false << true;
        QTest::newRow("import-uninitialized") << true << false << true << false << false;
        QTest::newRow("import-empty-storage") << true << true << false << false << false;
        QTest::newRow("import-would-hide-mount-data") << true << true << true << true << false;
    }

    void initializationGuards()
    {
        QFETCH(bool, importing);
        QFETCH(bool, initialized);
        QFETCH(bool, deviceHasData);
        QFETCH(bool, mountHasData);
        QFETCH(bool, allowed);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString storage = temporary.filePath(QStringLiteral("storage"));
        const QString mount = temporary.filePath(QStringLiteral("mount"));
        QVERIFY(QDir().mkpath(storage));
        QVERIFY(QDir().mkpath(mount));
        if (deviceHasData) {
            QVERIFY(writeFile(storage + QStringLiteral("/precious")));
        }
        if (mountHasData) {
            QVERIFY(writeFile(mount + QStringLiteral("/precious")));
        }
        RecordingBackend backend;
        backend.initialized = initialized;
        const auto future = importing ? backend.import(QStringLiteral("test"), Device(storage), MountPoint(mount), {})
                                      : backend.initialize(QStringLiteral("test"), Device(storage), MountPoint(mount), {});
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        const auto result = future.result();
        QCOMPARE(bool(result), allowed);
        QCOMPARE(backend.mounts, allowed ? 1 : 0);
        if (!allowed) {
            QCOMPARE(result.error().code(), Error::BackendError);
        }
        QCOMPARE(QFile::exists(storage + QStringLiteral("/precious")), deviceHasData);
        QCOMPARE(QFile::exists(mount + QStringLiteral("/precious")), mountHasData);
    }

    void dotDirectoryCleanup_data()
    {
        QTest::addColumn<QString>("otherEntry");
        QTest::newRow("only-decoration") << QString();
        QTest::newRow("visible-file") << QStringLiteral("document");
        QTest::newRow("hidden-file") << QStringLiteral(".precious");
        QTest::newRow("subdirectory") << QStringLiteral("folder/");
    }

    void dotDirectoryCleanup()
    {
        QFETCH(QString, otherEntry);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto decoration = temporary.filePath(QStringLiteral(".directory"));
        QVERIFY(writeFile(decoration));
        if (otherEntry.endsWith(QLatin1Char('/'))) {
            QVERIFY(QDir().mkpath(temporary.filePath(otherEntry)));
        } else if (!otherEntry.isEmpty()) {
            QVERIFY(writeFile(temporary.filePath(otherEntry)));
        }
        RecordingBackend::removeDotDirectory(MountPoint(temporary.path()));
        QCOMPARE(QFile::exists(decoration), !otherEntry.isEmpty());
        if (!otherEntry.isEmpty()) {
            QVERIFY(QFileInfo::exists(temporary.filePath(otherEntry)));
        }
    }

    void mountPermissionsRequireOptIn()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        RecordingBackend backend;
        const MountPoint mount(temporary.path());
        const auto original = QFileInfo(temporary.path()).permissions();
        backend.lockMountPoint(mount, true);
        QCOMPARE(QFileInfo(temporary.path()).permissions(), original);
        const auto decoration = temporary.filePath(QStringLiteral(".directory"));
        QVERIFY(writeFile(decoration, "[Desktop Entry]\nIcon=folder\n"));
        backend.lockMountPoint(mount, true);
        QCOMPARE(QFileInfo(temporary.path()).permissions(), original);

        RecordingBackend::setupMountPoint(mount);
        const auto ownerAccess = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
        backend.lockMountPoint(mount, true);
        const auto locked = QFileInfo(temporary.path()).permissions() & ownerAccess;
        // Always restore access before assertions so temporary files can be removed.
        backend.lockMountPoint(mount, false);
        const auto unlocked = QFileInfo(temporary.path()).permissions() & ownerAccess;
        QCOMPARE(locked, QFileDevice::Permissions(QFileDevice::ExeOwner));
        QCOMPARE(unlocked, QFileDevice::Permissions(ownerAccess));
    }

    void closeUnopenedVaultDoesNotRunUnmount()
    {
        RecordingBackend backend;
        const auto future = backend.close(Device(QStringLiteral("unused")), MountPoint(QStringLiteral("unused")));
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        QVERIFY(!future.result());
        QCOMPARE(future.result().error().code(), Error::BackendError);
    }

    void versionChecks_data()
    {
        QTest::addColumn<QString>("output");
        QTest::addColumn<bool>("standardError");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("minimum") << QStringLiteral("backend 2.9.7") << false << true;
        QTest::newRow("newer-patch") << QStringLiteral("backend 2.9.10") << false << true;
        QTest::newRow("newer-major") << QStringLiteral("backend 3.0.0") << false << true;
        QTest::newRow("older") << QStringLiteral("backend 2.9.6") << false << false;
        QTest::newRow("missing-version") << QStringLiteral("not a version") << false << false;
        QTest::newRow("stderr-version") << QStringLiteral("backend 2.9.7") << true << true;
    }

    void versionChecks()
    {
        QFETCH(QString, output);
        QFETCH(bool, standardError);
        QFETCH(bool, accepted);
        RecordingBackend backend;
        QStringList arguments{QStringLiteral("--version-test"), output};
        if (standardError) {
            arguments << QStringLiteral("--stderr");
        }
        std::unique_ptr<QProcess> process(backend.process(QString::fromLocal8Bit(VAULT_TEST_HELPER), arguments, {}));
        const auto future = backend.checkVersion(process.get(), std::make_tuple(2, 9, 7));
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        QCOMPARE(future.result().first, accepted);
        QVERIFY(!future.result().second.isEmpty());
    }

    void missingExecutableFinishesWithError()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        RecordingBackend backend;
        std::unique_ptr<QProcess> process(backend.process(temporary.filePath(QStringLiteral("does-not-exist")), {}, {}));
        const auto future = backend.checkVersion(process.get(), std::make_tuple(2, 9, 7));
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        QVERIFY(!future.result().first);
    }

    void processResult_data()
    {
        QTest::addColumn<int>("exitCode");
        QTest::addColumn<QString>("errorOutput");
        QTest::addColumn<bool>("crash");
        QTest::addColumn<bool>("success");
        QTest::newRow("success-with-diagnostics") << 0 << QStringLiteral("diagnostic") << false << true;
        QTest::newRow("command-failure") << 7 << QStringLiteral("failure details") << false << false;
        QTest::newRow("nonempty-apostrophes") << 1 << QStringLiteral("use 'nonempty' to override") << false << false;
        QTest::newRow("nonempty-spaces") << 1 << QStringLiteral("non empty mount point") << false << false;
        QTest::newRow("abnormal-exit") << 0 << QStringLiteral("terminated") << true << false;
    }

    void processResult()
    {
        QFETCH(int, exitCode);
        QFETCH(QString, errorOutput);
        QFETCH(bool, crash);
        QFETCH(bool, success);
        RecordingBackend backend;
        QStringList arguments{QStringLiteral("--process-test"), QStringLiteral("output details"), errorOutput, QString::number(exitCode)};
        if (crash) {
            arguments << QStringLiteral("--crash");
        }
        std::unique_ptr<QProcess> process(backend.process(QString::fromLocal8Bit(VAULT_TEST_HELPER), arguments, {}));
        process->start();
        QVERIFY(process->waitForStarted(5000));
        // waitForFinished reports false on a crash even though the process ended.
        process->waitForFinished(5000);
        QCOMPARE(process->state(), QProcess::NotRunning);
        const auto result = RecordingBackend::hasProcessFinishedSuccessfully(process.get());
        QCOMPARE(bool(result), success);
        if (!success) {
            QCOMPARE(result.error().code(), Error::CommandError);
            QVERIFY(!result.error().message().isEmpty());
            if (errorOutput == QLatin1String("failure details") || crash) {
                QCOMPARE(result.error().out(), QStringLiteral("output details"));
                QCOMPARE(result.error().err(), errorOutput);
            } else {
                // The nonempty-directory cases have a dedicated user-facing
                // error rather than falling through to the generic command error.
                QVERIFY(result.error().out().isEmpty());
                QVERIFY(result.error().err().isEmpty());
            }
        }
    }

    void backendCommand_data()
    {
        QTest::addColumn<QString>("backendName");
        QTest::addColumn<QStringList>("prefix");
        QTest::addColumn<bool>("initialized");
        QTest::addColumn<int>("exitCode");
        QTest::newRow("encfs") << QStringLiteral("encfs") << QStringList{QStringLiteral("-S"), QStringLiteral("--standard")} << true << 73;
        QTest::newRow("cryfs-default") << QStringLiteral("cryfs") << QStringList{} << true << 73;
        QTest::newRow("cryfs-cipher") << QStringLiteral("cryfs") << QStringList{QStringLiteral("--cipher"), QStringLiteral("AES-256-GCM")} << true << 73;
        QTest::newRow("gocryptfs-open") << QStringLiteral("gocryptfs") << QStringList{QStringLiteral("-nonempty")} << true << 73;
        QTest::newRow("gocryptfs-initialize") << QStringLiteral("gocryptfs") << QStringList{QStringLiteral("-init")} << false << 73;
        QTest::newRow("encfs-success") << QStringLiteral("encfs") << QStringList{QStringLiteral("-S"), QStringLiteral("--standard")} << true << 0;
        QTest::newRow("cryfs-success") << QStringLiteral("cryfs") << QStringList{} << true << 0;
        QTest::newRow("gocryptfs-success") << QStringLiteral("gocryptfs") << QStringList{QStringLiteral("-nonempty")} << true << 0;
    }

    void backendCommand()
    {
        QFETCH(QString, backendName);
        QFETCH(QStringList, prefix);
        QFETCH(bool, initialized);
        QFETCH(int, exitCode);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto bin = temporary.filePath(QStringLiteral("bin"));
        QVERIFY(QDir().mkpath(bin));
        const QStringList executables{QStringLiteral("encfs"), QStringLiteral("encfsctl"), QStringLiteral("cryfs"),
            QStringLiteral("gocryptfs"), QStringLiteral("fusermount3"), QStringLiteral("fusermount")};
        for (const auto& executable : executables) {
            QVERIFY(QFile::link(QString::fromLocal8Bit(VAULT_TEST_HELPER), bin + QLatin1Char('/') + executable));
        }
        const auto oldPath = qgetenv("PATH");
        qputenv("PATH", bin.toLocal8Bit() + ':' + oldPath);
        // Restore PATH even if a test assertion returns early.
        const auto restorePath = qScopeGuard([oldPath] { qputenv("PATH", oldPath); });

        const auto logPath = temporary.filePath(QStringLiteral("command.json"));
        const QStringList options{QStringLiteral("--test-log=") + logPath, QStringLiteral("--test-exit=") + QString::number(exitCode), QStringLiteral("option with spaces")};
        const auto config = KSharedConfig::openConfig(PLASMAVAULT_CONFIG_FILE);
        const QString groupName = backendName == QLatin1String("cryfs") ? QStringLiteral("CryfsBackend")
            : backendName == QLatin1String("encfs")                     ? QStringLiteral("EncfsBackend")
                                                                        : QStringLiteral("GocryptfsBackend");
        KConfigGroup group(config, groupName);
        group.writeEntry("extraMountOptions", options);
        config->sync();
        const auto clearOptions = qScopeGuard([&group] { group.deleteEntry("extraMountOptions"); group.sync(); });

        const auto storage = temporary.filePath(QStringLiteral("storage with spaces;literal"));
        const auto mount = temporary.filePath(QStringLiteral("mount with spaces"));
        QVERIFY(QDir().mkpath(storage));
        QVERIFY(QDir().mkpath(mount));
        if (backendName == QLatin1String("gocryptfs") && initialized) {
            QVERIFY(writeFile(storage + QStringLiteral("/gocryptfs.conf"), "{}"));
        }
        const QString password = QString::fromUtf8("test-pässword;$(not-a-command)");
        Vault::Payload payload;
        payload[KEY_PASSWORD] = password;
        if (prefix.contains(QStringLiteral("--cipher"))) {
            payload[QByteArrayLiteral("cryfs-cipher")] = QStringLiteral("AES-256-GCM");
        }
        const auto backend = Backend::instance(backendName);
        QVERIFY(backend);
        const auto future = backend->open(Device(storage), MountPoint(mount), payload);
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        const auto result = future.result();
        QCOMPARE(bool(result), exitCode == 0);
        if (!result) {
            QCOMPARE(result.error().code(), Error::CommandError);
        }

        QFile log(logPath);
        QVERIFY(log.open(QIODevice::ReadOnly));
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(log.readAll(), &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        const auto record = document.object();
        QStringList expected = prefix;
        expected << storage;
        if (initialized) {
            expected << mount;
        }
        expected << options;
        QCOMPARE(record.value(QStringLiteral("arguments")).toArray(), QJsonArray::fromStringList(expected));
        QJsonArray expectedInput{password + QLatin1Char('\n')};
        if (!initialized) {
            expectedInput.append(password + QLatin1Char('\n'));
        }
        QCOMPARE(record.value(QStringLiteral("input")).toArray(), expectedInput);
        QVERIFY(!expected.contains(password));
        if (backendName == QLatin1String("cryfs")) {
            QCOMPARE(record.value(QStringLiteral("frontend")).toString(), QStringLiteral("noninteractive"));
        }
    }

    void unknownBackendIsRejected()
    {
        QVERIFY(!Backend::instance(QStringLiteral("not-a-backend")));
    }
};

int main(int argc, char** argv)
{
    // Set this before constructing the application or opening KSharedConfig.
    // Production backend code must never read or modify the user's vault config.
    QTemporaryDir config;
    if (!config.isValid()) {
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    QCoreApplication application(argc, argv);
    BackendTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "backendtest.moc"
