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

    m_instance->updateRuntimeContext();

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

    m_instance->updateRuntimeContext();
    auto runtimeContext = m_instance->runtimeContext();
    QString librariesBasePath = APPLICATION->metacache()->getBasePath("libraries");
    if (librariesBasePath.isEmpty()) {
        librariesBasePath = QDir("libraries").absolutePath();
    }
    QString cleanLibBase = QDir::cleanPath(librariesBasePath);
    QString cleanLocalLibBase = QDir::cleanPath(m_instance->getLocalLibraryPath());

    QStringList allJars;
    // 1. All classpath and native jars for current runtime
    QStringList jars, nativeJars;
    profile->getLibraryFiles(runtimeContext, jars, nativeJars, m_instance->getLocalLibraryPath(), m_instance->binRoot(), false);
    allJars.append(jars);
    allJars.append(nativeJars);

    // 2. Both 32-bit and 64-bit native jars (for cross-architecture portability)
    QStringList native32, native64;
    for (const auto& lib : profile->getLibraries()) {
        if (!lib) continue;
        QStringList j, nat;
        lib->getApplicableFiles(runtimeContext, j, nat, native32, native64, m_instance->getLocalLibraryPath());
    }
    for (const auto& lib : profile->getNativeLibraries()) {
        if (!lib) continue;
        QStringList j, nat;
        lib->getApplicableFiles(runtimeContext, j, nat, native32, native64, m_instance->getLocalLibraryPath());
    }
    allJars.append(native32);
    allJars.append(native64);

    // 3. Agents
    for (const auto& agent : profile->getAgents()) {
        if (agent.library) {
            QStringList agentJars, temp1, temp2, temp3;
            agent.library->getApplicableFiles(runtimeContext, agentJars, temp1, temp2, temp3, m_instance->getLocalLibraryPath());
            allJars.append(agentJars);
        }
    }

    // 4. Maven files (e.g. Forge installer, universal, shim)
    for (const auto& mf : profile->getMavenFiles()) {
        if (mf) {
            QStringList mavenJars, temp1, temp2, temp3;
            mf->getApplicableFiles(runtimeContext, mavenJars, temp1, temp2, temp3, m_instance->getLocalLibraryPath());
            allJars.append(mavenJars);
        }
    }

    // 5. Check for Forge client jar (generated on first launch or by processors)
    for (const auto& lib : profile->getLibraries()) {
        if (lib && lib->rawName().groupId() == "net.minecraftforge" && lib->rawName().artifactId() == "forge") {
            GradleSpecifier clientSpec = lib->rawName();
            clientSpec.setClassifier("client");
            QString clientRel = clientSpec.toPath();
            QString clientDisk = FS::PathCombine(librariesBasePath, clientRel);
            allJars.append(clientDisk);
        }
    }

    // 6. Inspect classifiers from mojangDownloads and nativeClassifiers
    QList<LibraryPtr> allLibs;
    if (profile->getMainJar()) allLibs.append(profile->getMainJar());
    allLibs.append(profile->getLibraries());
    allLibs.append(profile->getNativeLibraries());
    allLibs.append(profile->getMavenFiles());

    for (const auto& lib : allLibs) {
        if (!lib || lib->isLocal()) continue;
        QStringList classifiers = lib->nativeClassifiers().values();
        if (lib->mojangDownloads()) {
            classifiers.append(lib->mojangDownloads()->classifiers.keys());
        }
        for (const auto& classifier : classifiers) {
            GradleSpecifier spec = lib->rawName();
            spec.setClassifier(classifier);
            QString suffix = spec.toPath();
            if (suffix.contains("${arch}")) {
                for (const auto& arch : { "32", "64" }) {
                    auto s = suffix;
                    s.replace("${arch}", arch);
                    QString diskPath = FS::PathCombine(librariesBasePath, s);
                    if (QFileInfo::exists(diskPath)) {
                        allJars.append(diskPath);
                    }
                }
            } else {
                QString diskPath = FS::PathCombine(librariesBasePath, suffix);
                if (QFileInfo::exists(diskPath)) {
                    allJars.append(diskPath);
                }
            }
        }
    }

    QSet<QString> addedArchivePaths;
    for (auto jarPath : allJars) {
        jarPath = QDir::cleanPath(jarPath);
        QString relPath;
        if (jarPath.startsWith(cleanLibBase, Qt::CaseInsensitive)) {
            relPath = jarPath.mid(cleanLibBase.length());
            if (relPath.startsWith('/')) {
                relPath.remove(0, 1);
            }
        } else if (jarPath.startsWith(cleanLocalLibBase, Qt::CaseInsensitive)) {
            // Local library inside instance directory - will be exported with the instance files
            continue;
        } else {
            if (QDir::isRelativePath(jarPath)) {
                relPath = jarPath;
                jarPath = FS::PathCombine(cleanLibBase, relPath);
            } else {
                relPath = QFileInfo(jarPath).fileName();
            }
        }

        if (relPath.isEmpty()) {
            continue;
        }

        QString archivePath = ".offline/libraries/" + relPath;
        if (addedArchivePaths.contains(archivePath)) {
            continue;
        }
        addedArchivePaths.insert(archivePath);

        QFileInfo fi(jarPath);
        if (fi.exists() && fi.isFile()) {
            outFiles.append({ jarPath, archivePath, fi.size() });
        } else {
            outMissing.append(jarPath);
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
