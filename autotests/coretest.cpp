// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QDBusMetaType>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "common/vaultinfo.h"
#include "kded/engine/commandresult.h"
#include "kded/engine/types.h"

using namespace PlasmaVault;

class CoreTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void vaultState_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<bool>("initialized");
        QTest::addColumn<bool>("opened");
        QTest::addColumn<bool>("busy");
        QTest::newRow("not-initialized") << int(VaultInfo::NotInitialized) << false << false << false;
        QTest::newRow("opened") << int(VaultInfo::Opened) << true << true << false;
        QTest::newRow("closed") << int(VaultInfo::Closed) << true << false << false;
        QTest::newRow("creating") << int(VaultInfo::Creating) << false << false << true;
        QTest::newRow("opening") << int(VaultInfo::Opening) << true << false << true;
        QTest::newRow("closing") << int(VaultInfo::Closing) << true << false << true;
        QTest::newRow("dismantling") << int(VaultInfo::Dismantling) << false << false << true;
        QTest::newRow("dismantled") << int(VaultInfo::Dismantled) << false << false << false;
        QTest::newRow("missing-device") << int(VaultInfo::DeviceMissing) << false << false << false;
        QTest::newRow("error") << int(VaultInfo::Error) << false << false << false;
    }

    void vaultState()
    {
        QFETCH(int, status);
        QFETCH(bool, initialized);
        QFETCH(bool, opened);
        QFETCH(bool, busy);
        VaultInfo info{};
        info.status = static_cast<VaultInfo::Status>(status);
        QCOMPARE(info.isInitialized(), initialized);
        QCOMPARE(info.isOpened(), opened);
        QCOMPARE(info.isBusy(), busy);
    }

    void dbusSignature()
    {
        qDBusRegisterMetaType<VaultInfo>();
        qDBusRegisterMetaType<VaultInfoList>();
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<VaultInfo>())), QByteArray("(sssqsasb)"));
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<VaultInfoList>())), QByteArray("a(sssqsasb)"));
        QCOMPARE(int(VaultInfo::DeviceMissing), 254);
        QCOMPARE(int(VaultInfo::Error), 255);
    }

    void pathsFollowFilesystemChanges()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString target = temporary.filePath(QStringLiteral("encrypted data"));
        const QString alias = temporary.filePath(QStringLiteral("alias"));
        const Device device(alias);
        const MountPoint mountPoint(alias);
        QCOMPARE(device.data(), alias);
        QCOMPARE(mountPoint.data(), alias);
        QVERIFY(QDir().mkpath(target));
        QVERIFY(QFile::link(target, alias));
        QCOMPARE(device.data(), QFileInfo(target).canonicalFilePath());
        QCOMPARE(mountPoint.data(), device.data());
        QVERIFY(device == Device(target));
        QCOMPARE(qHash(device), qHash(Device(target)));
    }

    void nonexistentPathsKeepTheirIdentity()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("not created"));
        QCOMPARE(Device(path + QLatin1Char('/')).data(), path);
        QCOMPARE(MountPoint(path + QLatin1Char('/')).data(), path);
        QVERIFY(MountPoint().isEmpty());
        QVERIFY(!MountPoint(path).isEmpty());
    }

    void resultTransitions_data()
    {
        QTest::addColumn<bool>("leftSuccess");
        QTest::addColumn<bool>("rightSuccess");
        QTest::newRow("success-success") << true << true;
        QTest::newRow("success-error") << true << false;
        QTest::newRow("error-success") << false << true;
        QTest::newRow("error-error") << false << false;
    }

    void resultTransitions()
    {
        QFETCH(bool, leftSuccess);
        QFETCH(bool, rightSuccess);
        auto left = leftSuccess ? Result<QString>::success(QStringLiteral("left"))
                                : Result<QString>::error(Error::BackendError, QStringLiteral("left error"));
        auto right = rightSuccess ? Result<QString>::success(QStringLiteral("right"))
                                  : Result<QString>::error(Error::CommandError, QStringLiteral("right error"));
        left.swap(right);
        QCOMPARE(bool(left), rightSuccess);
        QCOMPARE(bool(right), leftSuccess);
        QCOMPARE(left ? left.get() : left.error().message(), rightSuccess ? QStringLiteral("right") : QStringLiteral("right error"));
        QCOMPARE(right ? right.get() : right.error().message(), leftSuccess ? QStringLiteral("left") : QStringLiteral("left error"));
        const auto copy = left;
        auto moved = std::move(right);
        QCOMPARE(bool(copy), rightSuccess);
        QCOMPARE(bool(moved), leftSuccess);
        moved = copy;
        QCOMPARE(bool(moved), rightSuccess);
        QCOMPARE(moved ? moved.get() : moved.error().message(), copy ? copy.get() : copy.error().message());
    }

    void voidResultsPreserveErrorDetails()
    {
        auto result = Result<>::success();
        QVERIFY(bool(result));
        result = Result<>::error(Error::CommandError, QStringLiteral("failed"), QStringLiteral("stdout"), QStringLiteral("stderr"));
        QVERIFY(!result);
        const auto copy = result;
        QCOMPARE(copy.error().code(), Error::CommandError);
        QCOMPARE(copy.error().message(), QStringLiteral("failed"));
        QCOMPARE(copy.error().out(), QStringLiteral("stdout"));
        QCOMPARE(copy.error().err(), QStringLiteral("stderr"));
        result = Result<>::success();
        QVERIFY(bool(result));
        QVERIFY(!copy);
    }

    void errorFutureIsReady()
    {
        const auto future = errorResult(Error::DeviceError, QStringLiteral("missing"), QStringLiteral("out"), QStringLiteral("err"));
        QVERIFY(future.isFinished());
        const auto result = future.result();
        QVERIFY(!result);
        QCOMPARE(result.error().code(), Error::DeviceError);
        QCOMPARE(result.error().message(), QStringLiteral("missing"));
        QCOMPARE(result.error().out(), QStringLiteral("out"));
        QCOMPARE(result.error().err(), QStringLiteral("err"));
    }
};

QTEST_GUILESS_MAIN(CoreTest)
#include "coretest.moc"
