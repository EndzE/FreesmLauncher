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
#include "Version.h"
#include "archive/ArchiveReader.h"
#include "meta/BaseEntity.h"
#include "meta/Index.h"
#include "meta/JsonFormat.h"
#include "meta/Version.h"
#include "minecraft/AssetsUtils.h"
#include "minecraft/Component.h"
#include "minecraft/LaunchProfile.h"
#include "minecraft/Library.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/OneSixVersionFormat.h"
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

    ensureDependenciesResolved();

    collectMetadata(outFiles, outMissingFiles);
    collectLibraries(outFiles, outMissingFiles);
    collectAssets(outFiles, outMissingFiles);

    return outMissingFiles.isEmpty();
}

void OfflineDependencyCollector::ensureDependenciesResolved()
{
    auto packProfile = m_instance->getPackProfile();
    if (!packProfile) {
        return;
    }
    if (!packProfile->isLoaded()) {
        packProfile->load();
    }

    bool addedAny = false;
    for (int i = 0; i < packProfile->rowCount(); ++i) {
        auto comp = packProfile->getComponent(i);
        if (!comp) continue;

        QList<Meta::Require> requiresToCheck;
        for (const auto& req : comp->m_cachedRequires) {
            requiresToCheck.append(req);
        }
        auto vfile = comp->getVersionFile();
        if (vfile) {
            for (const auto& req : vfile->m_requires) {
                if (!requiresToCheck.contains(req)) {
                    requiresToCheck.append(req);
                }
            }
        }

        for (const auto& req : requiresToCheck) {
            if (req.uid.isEmpty() || packProfile->getComponent(req.uid)) {
                continue;
            }
            QString version = !req.equalsVersion.isEmpty() ? req.equalsVersion : req.suggests;
            if (version.isEmpty()) {
                if (req.uid == "org.lwjgl") {
                    version = "2.9.1";
                } else if (req.uid == "org.lwjgl3") {
                    version = "3.1.2";
                }
            }
            if (!version.isEmpty()) {
                auto newComp = makeShared<Component>(packProfile, req.uid);
                newComp->m_version = version;
                newComp->m_dependencyOnly = true;
                packProfile->appendComponent(newComp);
                addedAny = true;
            }
        }
    }

    if (addedAny) {
        packProfile->saveNow();
        packProfile->invalidateLaunchProfile();
    }
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

        // Component version file (e.g. meta/net.minecraft/1.16.5.json)
        if (!version.isEmpty()) {
            QString relVersionPath = uid + "/" + version + ".json";
            QString diskVersionPath = FS::PathCombine(metaBasePath, relVersionPath);
            if (!QFileInfo::exists(diskVersionPath)) {
                auto vfile = comp->getVersionFile();
                if (!vfile && comp->m_metaVersion) {
                    vfile = comp->m_metaVersion->data();
                }
                if (vfile) {
                    FS::ensureFilePathExists(diskVersionPath);
                    auto doc = OneSixVersionFormat::versionFileToJson(vfile);
                    FS::write(diskVersionPath, doc.toJson(QJsonDocument::Indented));
                }
            }
            checkAndAdd(relVersionPath, true);
        }

        // Component index (e.g. meta/net.minecraft/index.json)
        checkAndAdd(uid + "/index.json", false);
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
        if (lib && (lib->rawName().groupId() == "net.minecraftforge" || lib->rawName().groupId() == "net.neoforged") && lib->rawName().artifactId() == "forge") {
            GradleSpecifier clientSpec = lib->rawName();
            clientSpec.setClassifier("client");
            QString clientRel = clientSpec.toPath();
            QString clientDisk = FS::PathCombine(librariesBasePath, clientRel);
            if (QFileInfo::exists(clientDisk)) {
                allJars.append(clientDisk);
            }
        }
    }

    // 6. Inspect install_profile.json in any installer jars (NeoForge / Forge installer)
    for (const auto& jarCandidate : allJars) {
        if (!jarCandidate.contains("installer", Qt::CaseInsensitive) || !jarCandidate.endsWith(".jar", Qt::CaseInsensitive)) {
            continue;
        }
        QString jarPath = jarCandidate;
        if (QDir::isRelativePath(jarPath)) {
            jarPath = FS::PathCombine(cleanLibBase, jarPath);
        }
        if (!QFileInfo::exists(jarPath)) {
            continue;
        }

        try {
            MMCZip::ArchiveReader reader(jarPath);
            auto ipFile = reader.goToFile("install_profile.json");
            if (!ipFile) {
                ipFile = reader.goToFile("/install_profile.json");
            }
            if (ipFile) {
                QByteArray ipBytes = ipFile->readAll();
                if (!ipBytes.isEmpty()) {
                    auto doc = Json::requireDocument(ipBytes, "install_profile.json");
                    auto root = Json::requireObject(doc, "install_profile.json");

                    // 1. Data section: e.g. MOJMAPS, PATCHED, etc.
                    if (root.contains("data") && root["data"].isObject()) {
                        auto dataObj = root["data"].toObject();
                        for (auto it = dataObj.begin(); it != dataObj.end(); ++it) {
                            QString key = it.key();
                            // Skip temporary intermediate bundler files created during extraction that are deleted immediately
                            if (key.endsWith("_UNPACKED", Qt::CaseInsensitive) || key.contains("UNPACKED", Qt::CaseInsensitive)) {
                                continue;
                            }
                            QString valStr;
                            if (it.value().isObject()) {
                                auto sideObj = it.value().toObject();
                                if (sideObj.contains("client")) {
                                    valStr = sideObj["client"].toString();
                                }
                            } else if (it.value().isString()) {
                                valStr = it.value().toString();
                            }
                            if (valStr.startsWith('[') && valStr.endsWith(']')) {
                                GradleSpecifier spec(valStr.mid(1, valStr.length() - 2));
                                if (spec.valid()) {
                                    QString diskPath = FS::PathCombine(librariesBasePath, spec.toPath());
                                    // PATCHED is the primary required output generated by processors on launch.
                                    // Other data entries (MOJMAPS, MC_SLIM, etc.) are only included if they exist on disk.
                                    if (key == "PATCHED" || QFileInfo::exists(diskPath)) {
                                        allJars.append(diskPath);
                                    }
                                }
                            }
                        }
                    }

                    // 2. Processors section
                    if (root.contains("processors") && root["processors"].isArray()) {
                        for (const auto& pVal : root["processors"].toArray()) {
                            if (!pVal.isObject()) continue;
                            auto pObj = pVal.toObject();
                            if (pObj.contains("sides") && pObj["sides"].isArray()) {
                                bool forClient = false;
                                for (const auto& s : pObj["sides"].toArray()) {
                                    if (s.toString() == "client") {
                                        forClient = true;
                                        break;
                                    }
                                }
                                if (!forClient) continue;
                            }

                            if (pObj.contains("jar")) {
                                GradleSpecifier spec(pObj["jar"].toString());
                                if (spec.valid()) {
                                    QString diskPath = FS::PathCombine(librariesBasePath, spec.toPath());
                                    if (QFileInfo::exists(diskPath)) {
                                        allJars.append(diskPath);
                                    }
                                }
                            }

                            if (pObj.contains("classpath") && pObj["classpath"].isArray()) {
                                for (const auto& cpVal : pObj["classpath"].toArray()) {
                                    GradleSpecifier spec(cpVal.toString());
                                    if (spec.valid()) {
                                        QString diskPath = FS::PathCombine(librariesBasePath, spec.toPath());
                                        if (QFileInfo::exists(diskPath)) {
                                            allJars.append(diskPath);
                                        }
                                    }
                                }
                            }

                            if (pObj.contains("args") && pObj["args"].isArray()) {
                                for (const auto& argVal : pObj["args"].toArray()) {
                                    QString argStr = argVal.toString();
                                    if (argStr.startsWith('[') && argStr.endsWith(']')) {
                                        GradleSpecifier spec(argStr.mid(1, argStr.length() - 2));
                                        if (spec.valid()) {
                                            QString diskPath = FS::PathCombine(librariesBasePath, spec.toPath());
                                            if (QFileInfo::exists(diskPath)) {
                                                allJars.append(diskPath);
                                            }
                                        }
                                    }
                                }
                            }

                            if (pObj.contains("outputs") && pObj["outputs"].isObject()) {
                                auto outObj = pObj["outputs"].toObject();
                                for (auto it = outObj.begin(); it != outObj.end(); ++it) {
                                    QString outStr = it.value().toString();
                                    if (outStr.startsWith('[') && outStr.endsWith(']')) {
                                        GradleSpecifier spec(outStr.mid(1, outStr.length() - 2));
                                        if (spec.valid()) {
                                            QString diskPath = FS::PathCombine(librariesBasePath, spec.toPath());
                                            if (QFileInfo::exists(diskPath)) {
                                                allJars.append(diskPath);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // 3. Libraries in install_profile
                    if (root.contains("libraries") && root["libraries"].isArray()) {
                        for (const auto& libVal : root["libraries"].toArray()) {
                            if (!libVal.isObject()) continue;
                            auto libObj = libVal.toObject();
                            if (libObj.contains("name")) {
                                GradleSpecifier spec(libObj["name"].toString());
                                if (spec.valid()) {
                                    allJars.append(FS::PathCombine(librariesBasePath, spec.toPath()));
                                }
                            }
                        }
                    }
                }
            }
        } catch (...) {
            // Ignore unreadable or corrupted installer jars
        }
    }

    // 7. Direct component fallbacks (covers unlaunched instances or missing installer jars)
    QString mcVersion = packProfile->getComponentVersion("net.minecraft");
    for (int i = 0; i < packProfile->rowCount(); ++i) {
        auto comp = packProfile->getComponent(i);
        if (!comp) continue;
        QString uid = comp->getID();
        QString version = comp->getVersion();

        if (uid == "net.minecraftforge") {
            // Forge client jar (for modern Forge 1.13+)
            bool isModernForge = !mcVersion.isEmpty() && (::Version(mcVersion) >= ::Version("1.13"));
            if (isModernForge) {
                // Try composite version first (e.g. 1.20.1-47.4.10), then direct version
                QString fullVer = version.contains(mcVersion) ? version : QString("%1-%2").arg(mcVersion, version);
                GradleSpecifier clientSpec(QString("net.minecraftforge:forge:%1:client").arg(fullVer));
                GradleSpecifier legacySpec(QString("net.minecraftforge:forge:%1:client").arg(version));

                QString clientDisk = FS::PathCombine(librariesBasePath, clientSpec.toPath());
                QString legacyDisk = FS::PathCombine(librariesBasePath, legacySpec.toPath());

                if (QFileInfo::exists(clientDisk)) {
                    allJars.append(clientDisk);
                } else if (QFileInfo::exists(legacyDisk)) {
                    allJars.append(legacyDisk);
                } else {
                    // Not found on disk: instance is unlaunched, record expected client jar as missing
                    allJars.append(clientDisk);
                }
            }

            // Mojang mappings for modern Forge (1.20+)
            if (!mcVersion.isEmpty()) {
                GradleSpecifier mapSpec(QString("net.minecraft:client:%1:mappings@txt").arg(mcVersion));
                if (mapSpec.valid()) {
                    QString mapDisk = FS::PathCombine(librariesBasePath, mapSpec.toPath());
                    if (QFileInfo::exists(mapDisk)) {
                        allJars.append(mapDisk);
                    }
                }
            }
        } else if (uid == "net.neoforged") {
            // NeoForge patched client jar:
            // 1. NeoForge 1.20.1 (47.1.x): net.neoforged:forge:<mcVersion>-<version>:client
            // 2. NeoForge 20.2 - 20.4: net.neoforged:neoforge:<version>:client
            // 3. NeoForge 20.5+ / 21.x+: net.neoforged:minecraft-client-patched:<version>
            QString fullVer = (!mcVersion.isEmpty() && !version.contains(mcVersion))
                                  ? QString("%1-%2").arg(mcVersion, version)
                                  : version;

            GradleSpecifier neo1201Spec(QString("net.neoforged:forge:%1:client").arg(fullVer));
            GradleSpecifier neoLegacySpec(QString("net.neoforged:forge:%1:client").arg(version));
            GradleSpecifier neoforgeClientSpec(QString("net.neoforged:neoforge:%1:client").arg(version));
            GradleSpecifier patchedSpec(QString("net.neoforged:minecraft-client-patched:%1").arg(version));

            QString neo1201Disk = FS::PathCombine(librariesBasePath, neo1201Spec.toPath());
            QString neoLegacyDisk = FS::PathCombine(librariesBasePath, neoLegacySpec.toPath());
            QString neoDisk = FS::PathCombine(librariesBasePath, neoforgeClientSpec.toPath());
            QString patchedDisk = FS::PathCombine(librariesBasePath, patchedSpec.toPath());

            if (QFileInfo::exists(neo1201Disk)) {
                allJars.append(neo1201Disk);
            } else if (QFileInfo::exists(neoDisk)) {
                allJars.append(neoDisk);
            } else if (QFileInfo::exists(patchedDisk)) {
                allJars.append(patchedDisk);
            } else if (QFileInfo::exists(neoLegacyDisk)) {
                allJars.append(neoLegacyDisk);
            } else {
                // Not found on disk: instance is unlaunched, record expected client jar as missing
                if (mcVersion == "1.20.1" || version.startsWith("47.")) {
                    allJars.append(neo1201Disk);
                } else if (version.startsWith("20.2") || version.startsWith("20.3") || version.startsWith("20.4")) {
                    allJars.append(neoDisk);
                } else {
                    allJars.append(patchedDisk);
                }
            }
            // NeoForge Mojang mappings
            if (!mcVersion.isEmpty()) {
                GradleSpecifier mapSpec(QString("net.minecraft:client:%1:mappings@txt").arg(mcVersion));
                if (mapSpec.valid()) {
                    QString mapDisk = FS::PathCombine(librariesBasePath, mapSpec.toPath());
                    if (QFileInfo::exists(mapDisk)) {
                        allJars.append(mapDisk);
                    }
                }
            }
        }
    }

    // 8. Inspect classifiers from mojangDownloads and nativeClassifiers
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
