/*

Pencil2D - Traditional Animation Software
Copyright (C) 2012-2020 Matthew Chiawen Chang
Copyright (C) 2026-2099 Oliver S. Larsen

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

*/

#include "catch.hpp"
#include "qminiz.h"

#include <cstring>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <cstdlib>

// https://github.com/pencil2d/pencil/issues/2009
TEST_CASE("QMiniZ::CompressFolder")
{
    SECTION("Verify robustness against paths with different roots")
    {
        QTemporaryDir tempDir;
        REQUIRE(tempDir.isValid());

        QString subFolder = tempDir.path() + "/bar/";
        QDir().mkpath(subFolder);
        QFile file(subFolder + "biz.mp3");
        file.open(QIODevice::WriteOnly);
        file.close();

        QString zipPath = tempDir.path() + "/test.pclx";

        // Simulate root mismatch by using canonical path for file
        // but non-canonical path for the base
        QString canonicalFile = QFileInfo(subFolder + "biz.mp3").canonicalFilePath();
        QString nonCanonicalBase = tempDir.path();

        QList<QString> filesToZip = { canonicalFile };

        Status st = MiniZ::compressFolder(zipPath, nonCanonicalBase, filesToZip, "application/x-pencil2d-pclx");
        REQUIRE(st.ok());

        mz_zip_archive zip;
        memset(&zip, 0, sizeof(zip));
        REQUIRE(mz_zip_reader_init_file(&zip, zipPath.toUtf8().data(), 0));

        int fileIndex = mz_zip_reader_locate_file(&zip, "bar/biz.mp3", nullptr, 0);
        REQUIRE(fileIndex >= 0);

        mz_zip_reader_end(&zip);
    }
}

TEST_CASE("QMiniZ failures preserve an existing archive")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QString destination = directory.filePath("project.pclx");
    QFile original(destination);
    REQUIRE(original.open(QIODevice::WriteOnly));
    REQUIRE(original.write("original project bytes") > 0);
    original.close();
    auto status = MiniZ::compressFolder(destination, directory.path(),
        {directory.filePath("missing asset.png")}, "application/x-pencil2d-pclx");
    REQUIRE_FALSE(status.ok());
    REQUIRE(original.open(QIODevice::ReadOnly));
    REQUIRE(original.readAll() == QByteArray("original project bytes"));
}

TEST_CASE("QMiniZ interrupted archive child", "[.archive-crash-child]")
{
    QString root = qEnvironmentVariable("PENCIL_TEST_ARCHIVE_ROOT");
    REQUIRE_FALSE(root.isEmpty());
    MiniZ::compressFolder(QDir(root).filePath("project.pclx"), root,
        {QDir(root).filePath("asset.bin")}, "application/x-pencil2d-pclx", [] { std::_Exit(77); });
    FAIL("Crash fixture did not reach native archive write");
}

TEST_CASE("QMiniZ process death before archive publication preserves original")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QFile asset(directory.filePath("asset.bin"));
    REQUIRE(asset.open(QIODevice::WriteOnly));
    REQUIRE(asset.write(QByteArray(65536, 'a')) == 65536);
    asset.close();
    QString destination = directory.filePath("project.pclx");
    REQUIRE(MiniZ::compressFolder(destination, directory.path(), {asset.fileName()}, "application/x-pencil2d-pclx").ok());
    QFile original(destination);
    REQUIRE(original.open(QIODevice::ReadOnly));
    QByteArray before = original.readAll();
    original.close();
    REQUIRE(asset.open(QIODevice::WriteOnly | QIODevice::Truncate));
    REQUIRE(asset.write(QByteArray(65536, 'b')) == 65536);
    asset.close();
    QProcess child;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("PENCIL_TEST_ARCHIVE_ROOT", directory.path());
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(), {"QMiniZ interrupted archive child"});
    REQUIRE(child.waitForFinished(15000));
    REQUIRE(child.exitCode() == 77);
    REQUIRE(original.open(QIODevice::ReadOnly));
    REQUIRE(original.readAll() == before);
    original.close();
    REQUIRE(MiniZ::sanityCheck(destination).ok());
    REQUIRE(MiniZ::compressFolder(destination, directory.path(), {asset.fileName()}, "application/x-pencil2d-pclx").ok());
    REQUIRE(original.open(QIODevice::ReadOnly));
    REQUIRE(original.readAll() != before);
    REQUIRE(MiniZ::sanityCheck(destination).ok());
}
