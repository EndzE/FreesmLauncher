// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 FreesmLauncher Contributors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

class MinecraftInstance;

struct OfflineFileEntry {
    QString diskPath;       // Absolute path on user's disk
    QString archivePath;    // Destination path inside ZIP (e.g., ".offline/libraries/...")
    qint64 size = 0;
};

struct OfflineExportOptions {
    bool includeLibraries = true;
    bool includeAssets = true;
    bool includeMetadata = true;
};

struct OfflineStats {
    int totalCount = 0;
    qint64 totalSize = 0;
    int libCount = 0;
    qint64 libSize = 0;
    int assetCount = 0;
    qint64 assetSize = 0;
    int metaCount = 0;
    qint64 metaSize = 0;
    int missingCount = 0;
};

class OfflineDependencyCollector {
public:
    explicit OfflineDependencyCollector(MinecraftInstance* instance, OfflineExportOptions options = {});

    bool collect(QList<OfflineFileEntry>& outFiles, QStringList& outMissingFiles);
    QByteArray createManifest(const QList<OfflineFileEntry>& files) const;
    static OfflineStats computeStats(const QList<OfflineFileEntry>& files, const QStringList& missing);

private:
    void collectMetadata(QList<OfflineFileEntry>& outFiles, QStringList& outMissing);
    void collectLibraries(QList<OfflineFileEntry>& outFiles, QStringList& outMissing);
    void collectAssets(QList<OfflineFileEntry>& outFiles, QStringList& outMissing);

    MinecraftInstance* m_instance;
    OfflineExportOptions m_options;
};
