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

#include "OfflineDependencyCollector.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include "Application.h"
#include "BuildConfig.h"
#include "FileSystem.h"
#include "Json.h"
#include "meta/BaseEntity.h"
#include "meta/Index.h"
#include "meta/Version.h"
#include "minecraft/AssetsUtils.h"
#include "minecraft/Component.h"
#include "minecraft/LaunchProfile.h"
#include "minecraft/Library.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "net/HttpMetaCache.h"

OfflineDependencyCollector::OfflineDependencyCollector(MinecraftInstance* instance, OfflineExportOptions options)
    : m_instance(instance), m_options(options)
{}

bool OfflineDependencyCollector::collect(QList<OfflineFileEntry>& outFiles, QStringList& outMissingFiles)
{
    outFiles.clear();
    outMissingFiles.clear();

    if (!m_instance) {
        return false;
    }

    auto packProfile = m_instance->getPackProfile();
    if (packProfile && !packProfile->isLoaded()) {
        packProfile->load();
    }

    collectMetadata(outFiles, outMissingFiles);
    collectLibraries(outFiles, outMissingFiles);
    collectAssets(outFiles, outMissingFiles);

    return outMissingFiles.isEmpty();
}

void OfflineDependencyCollector::collectMetadata(QList<OfflineFileEntry>& outFiles, QStringList& outMissing)
{
    if (!m_options.includeMetadata) {
        return;
    }

    auto packProfile = m_instance->getPackProfile();
    if (!packProfile) {
        return;
    }

    if (!packProfile->isLoaded()) {
        packProfile->load();
    }

    QSet<QString> addedPaths;
    QString metaBasePath = QDir("meta").absolutePath();

    auto checkAndAdd = [&](const QString& relPath, bool required) {
        if (addedPaths.contains(relPath)) {
            return;
        }
        addedPaths.insert(relPath);

        QString diskPath = FS::PathCombine(metaBasePath, relPath);
        QFileInfo fi(diskPath);
        if (fi.exists() && fi.isFile()) {
            outFiles.append({ diskPath, ".offline/meta/" + relPath, fi.size() });
        } else if (required) {
            outMissing.append(diskPath);
        }
    };

    // Master metadata index (if present)
    checkAndAdd("index.json", false);

    for (int i = 0; i < packProfile->rowCount(); ++i) {
        auto comp = packProfile->getComponent(i);
        if (!comp) {
            continue;
        }

        QString uid = comp->getID();
        QString version = comp->getVersion();
        if (uid.isEmpty()) {
            continue;
        }

        // Component index (e.g. meta/net.minecraft/index.json)
        checkAndAdd(uid + "/index.json", false);

        // Component version file (e.g. meta/net.minecraft/1.16.5.json)
        if (!version.isEmpty()) {
            checkAndAdd(uid + "/" + version + ".json", true);
        }
    }
}

void OfflineDependencyCollector::collectLibraries(QList<OfflineFileEntry>& outFiles, QStringList& outMissing)
{
    if (!m_options.includeLibraries) {
        return;
    }

    auto packProfile = m_instance->getPackProfile();
    if (!packProfile) {
        return;
    }

    if (!packProfile->isLoaded()) {
        packProfile->load();
    }

    // Try loading component metadata from local disk into memory if not loaded
    bool hasUnloadedComponent = false;
    for (int i = 0; i < packProfile->rowCount(); ++i) {
        auto comp = packProfile->getComponent(i);
        if (!comp) {
            continue;
        }
        if (!comp->m_metaVersion && !comp->m_file) {
            auto metaVersion = APPLICATION->metadataIndex()->get(comp->m_uid, comp->m_version);
            if (metaVersion && !metaVersion->isLoaded()) {
                metaVersion->loadLocalFile();
            }
            if (metaVersion && metaVersion->isLoaded()) {
                comp->m_metaVersion = metaVersion;
            } else {
                hasUnloadedComponent = true;
            }
        }
    }
    packProfile->invalidateLaunchProfile();

    auto profile = packProfile->getProfile();
    if (!profile || hasUnloadedComponent) {
        outMissing.append(QObject::tr("Required game libraries (instance has never been launched)."));
        if (!profile) {
            return;
        }
    }

    if (packProfile->rowCount() > 0 && (profile->getLibraries().isEmpty() || !profile->getMainJar())) {
        outMissing.append(QObject::tr("Required game libraries (instance has never been launched)."));
    }

    QSet<QString> addedArchivePaths;
    auto runtimeContext = m_instance->runtimeContext();
    QString librariesBasePath = APPLICATION->metacache()->getBasePath("libraries");
    if (librariesBasePath.isEmpty()) {
        librariesBasePath = QDir("libraries").absolutePath();
    }

    QList<LibraryPtr> libPool;
    if (profile->getMainJar()) {
        libPool.append(profile->getMainJar());
    }
    libPool.append(profile->getLibraries());
    libPool.append(profile->getNativeLibraries());
    libPool.append(profile->getMavenFiles());
    for (const auto& agent : profile->getAgents()) {
        if (agent.library) {
            libPool.append(agent.library);
        }
    }

    for (const auto& lib : libPool) {
        if (!lib) {
            continue;
        }

        // Local libraries are already in the instance folder
        if (lib->isLocal()) {
            continue;
        }

        if (lib->isNative()) {
            QString rawSuffix = lib->storageSuffix(runtimeContext);
            QStringList requiredSuffixes;

            if (rawSuffix.contains("${arch}")) {
                QString natArch = runtimeContext.javaArchitecture.isEmpty() ? "64" : runtimeContext.javaArchitecture;
                auto s = rawSuffix;
                s.replace("${arch}", natArch);
                requiredSuffixes.append(s);
            } else if (!rawSuffix.contains("INVALID")) {
                requiredSuffixes.append(rawSuffix);
            }

            for (const auto& suffix : requiredSuffixes) {
                QString archivePath = ".offline/libraries/" + suffix;
                if (addedArchivePaths.contains(archivePath)) {
                    continue;
                }
                addedArchivePaths.insert(archivePath);

                QString diskPath = FS::PathCombine(librariesBasePath, suffix);
                QFileInfo fi(diskPath);
                if (fi.exists() && fi.isFile()) {
                    outFiles.append({ diskPath, archivePath, fi.size() });
                } else {
                    outMissing.append(diskPath);
                }
            }

            // Also inspect cached native classifiers for cross-platform portability
            QStringList extraClassifiers = lib->nativeClassifiers().values();
            if (lib->mojangDownloads()) {
                extraClassifiers.append(lib->mojangDownloads()->classifiers.keys());
            }

            for (const auto& classifier : extraClassifiers) {
                GradleSpecifier spec = lib->rawName();
                spec.setClassifier(classifier);
                QString suffix = spec.toPath();
                if (suffix.contains("${arch}")) {
                    for (const auto& arch : { "32", "64" }) {
                        auto s = suffix;
                        s.replace("${arch}", arch);
                        QString archivePath = ".offline/libraries/" + s;
                        if (addedArchivePaths.contains(archivePath)) {
                            continue;
                        }
                        QString diskPath = FS::PathCombine(librariesBasePath, s);
                        QFileInfo fi(diskPath);
                        if (fi.exists() && fi.isFile()) {
                            addedArchivePaths.insert(archivePath);
                            outFiles.append({ diskPath, archivePath, fi.size() });
                        }
                    }
                } else {
                    QString archivePath = ".offline/libraries/" + suffix;
                    if (addedArchivePaths.contains(archivePath)) {
                        continue;
                    }
                    QString diskPath = FS::PathCombine(librariesBasePath, suffix);
                    QFileInfo fi(diskPath);
                    if (fi.exists() && fi.isFile()) {
                        addedArchivePaths.insert(archivePath);
                        outFiles.append({ diskPath, archivePath, fi.size() });
                    }
                }
            }
        } else {
            // Classpath or normal library
            QString suffix = lib->storageSuffix(runtimeContext);
            if (suffix.isEmpty()) {
                continue;
            }

            QString archivePath = ".offline/libraries/" + suffix;
            if (addedArchivePaths.contains(archivePath)) {
                continue;
            }
            addedArchivePaths.insert(archivePath);

            QString diskPath = FS::PathCombine(librariesBasePath, suffix);
            QFileInfo fi(diskPath);
            if (fi.exists() && fi.isFile()) {
                outFiles.append({ diskPath, archivePath, fi.size() });
            } else {
                outMissing.append(diskPath);
            }
        }
    }
}

void OfflineDependencyCollector::collectAssets(QList<OfflineFileEntry>& outFiles, QStringList& outMissing)
{
    if (!m_options.includeAssets) {
        return;
    }

    auto packProfile = m_instance->getPackProfile();
    if (!packProfile) {
        return;
    }

    if (!packProfile->isLoaded()) {
        packProfile->load();
    }

    auto profile = packProfile->getProfile();
    if (!profile) {
        outMissing.append(QObject::tr("Required game assets (instance has never been launched)."));
        return;
    }

    auto assets = profile->getMinecraftAssets();
    if (!assets || assets->id.isEmpty()) {
        if (packProfile->rowCount() > 0) {
            outMissing.append(QObject::tr("Game assets (instance has never been launched)."));
        }
        return;
    }

    QString assetIndexId = assets->id;
    QString assetsRoot = QDir("assets").absolutePath();

    // 1. Index file
    QString indexRelPath = "indexes/" + assetIndexId + ".json";
    QString indexDiskPath = FS::PathCombine(assetsRoot, indexRelPath);
    QString indexArchivePath = ".offline/assets/" + indexRelPath;

    QFileInfo indexFi(indexDiskPath);
    if (indexFi.exists() && indexFi.isFile()) {
        outFiles.append({ indexDiskPath, indexArchivePath, indexFi.size() });
    } else {
        outMissing.append(indexDiskPath);
        return;
    }

    // 2. Objects
    AssetsIndex index;
    if (!AssetsUtils::loadAssetsIndexJson(assetIndexId, indexDiskPath, index)) {
        outMissing.append(indexDiskPath);
        return;
    }

    QSet<QString> addedObjectArchivePaths;
    for (const auto& obj : index.objects) {
        QString relPath = "objects/" + obj.hash.left(2) + "/" + obj.hash;
        QString archivePath = ".offline/assets/" + relPath;
        if (addedObjectArchivePaths.contains(archivePath)) {
            continue;
        }
        addedObjectArchivePaths.insert(archivePath);

        QString diskPath = FS::PathCombine(assetsRoot, relPath);
        QFileInfo fi(diskPath);
        if (fi.exists() && fi.isFile() && (obj.size <= 0 || fi.size() == obj.size)) {
            outFiles.append({ diskPath, archivePath, fi.size() });
        } else {
            outMissing.append(diskPath);
        }
    }
}

QByteArray OfflineDependencyCollector::createManifest(const QList<OfflineFileEntry>& files) const
{
    QJsonObject root;
    root["formatVersion"] = 1;
    root["createdAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root["launcherVersion"] = QString("%1 %2").arg(BuildConfig.LAUNCHER_DISPLAYNAME, BuildConfig.printableVersionString());
    root["instanceName"] = m_instance->name();

    QJsonArray componentsArray;
    auto packProfile = m_instance->getPackProfile();
    if (packProfile) {
        for (int i = 0; i < packProfile->rowCount(); ++i) {
            auto comp = packProfile->getComponent(i);
            if (!comp) {
                continue;
            }
            QJsonObject compObj;
            compObj["uid"] = comp->getID();
            compObj["version"] = comp->getVersion();
            componentsArray.append(compObj);
        }
    }
    root["components"] = componentsArray;

    auto profile = packProfile ? packProfile->getProfile() : nullptr;
    auto assets = profile ? profile->getMinecraftAssets() : nullptr;
    root["assetIndex"] = assets ? assets->id : QString();

    int librariesCount = 0;
    int assetsCount = 0;
    int metaCount = 0;
    qint64 totalUncompressedSize = 0;

    for (const auto& file : files) {
        totalUncompressedSize += file.size;
        if (file.archivePath.startsWith(".offline/libraries/")) {
            librariesCount++;
        } else if (file.archivePath.startsWith(".offline/assets/")) {
            assetsCount++;
        } else if (file.archivePath.startsWith(".offline/meta/")) {
            metaCount++;
        }
    }

    QJsonObject statsObj;
    statsObj["librariesCount"] = librariesCount;
    statsObj["assetsCount"] = assetsCount;
    statsObj["metaCount"] = metaCount;
    statsObj["totalUncompressedSize"] = totalUncompressedSize;
    root["stats"] = statsObj;

    QJsonDocument doc(root);
    return doc.toJson(QJsonDocument::Indented);
}

OfflineStats OfflineDependencyCollector::computeStats(const QList<OfflineFileEntry>& files, const QStringList& missing)
{
    OfflineStats stats;
    stats.totalCount = static_cast<int>(files.size());
    stats.missingCount = static_cast<int>(missing.size());

    for (const auto& file : files) {
        stats.totalSize += file.size;
        if (file.archivePath.startsWith(".offline/libraries/")) {
            stats.libSize += file.size;
            stats.libCount++;
        } else if (file.archivePath.startsWith(".offline/assets/")) {
            stats.assetSize += file.size;
            stats.assetCount++;
        } else if (file.archivePath.startsWith(".offline/meta/")) {
            stats.metaSize += file.size;
            stats.metaCount++;
        }
    }
    return stats;
}
