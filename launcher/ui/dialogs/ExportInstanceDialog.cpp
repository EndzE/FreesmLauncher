// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
 *  Copyright (C) 2023 TheKodeToad <TheKodeToad@proton.me>
 *  Copyright (c) 2023 Trial97 <alexandru.tripon97@gmail.com>
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
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *      Copyright 2013-2021 MultiMC Contributors
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#include "ExportInstanceDialog.h"
#include <BaseInstance.h>
#include <MMCZip.h>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QMessageBox>
#include <QtConcurrent>
#include "FileIgnoreProxy.h"
#include "QObjectPtr.h"
#include "archive/ExportToZipTask.h"
#include "Application.h"
#include "meta/Index.h"
#include "minecraft/Component.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/MinecraftLoadAndCheck.h"
#include "minecraft/OfflineDependencyCollector.h"
#include "minecraft/PackProfile.h"
#include "tasks/SequentialTask.h"
#include "StringUtils.h"
#include "ui/dialogs/CustomMessageBox.h"
#include "ui/dialogs/ProgressDialog.h"
#include "ui_ExportInstanceDialog.h"

#include <FileSystem.h>
#include <icons/IconList.h>
#include <QDebug>
#include <QFileInfo>
#include <QPushButton>
#include <QSaveFile>
#include <QSortFilterProxyModel>
#include <QStack>
#include <functional>
#include "Application.h"
#include "SeparatorPrefixTree.h"

ExportInstanceDialog::ExportInstanceDialog(BaseInstance* instance, QWidget* parent)
    : QDialog(parent), m_ui(new Ui::ExportInstanceDialog), m_instance(instance)
{
    m_ui->setupUi(this);
    auto model = new QFileSystemModel(this);
    model->setIconProvider(&m_icons);
    auto root = instance->instanceRoot();
    m_proxyModel = new FileIgnoreProxy(root, this);
    m_proxyModel->setSourceModel(model);
    auto prefix = QDir(instance->instanceRoot()).relativeFilePath(instance->gameRoot());
    for (auto path : { "logs", "crash-reports", ".cache", ".fabric", ".quilt" }) {
        m_proxyModel->ignoreFilesWithPath().insert(FS::PathCombine(prefix, path));
    }
    m_proxyModel->ignoreFilesWithName().append({ ".DS_Store", "thumbs.db", "Thumbs.db" });
    m_proxyModel->loadBlockedPathsFromFile(ignoreFileName());

    m_ui->treeView->setModel(m_proxyModel);
    m_ui->treeView->setRootIndex(m_proxyModel->mapFromSource(model->index(root)));
    m_ui->treeView->sortByColumn(0, Qt::AscendingOrder);

    connect(m_proxyModel, &QAbstractItemModel::rowsInserted, this, &ExportInstanceDialog::rowsInserted);

    model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs | QDir::Hidden);
    model->setRootPath(root);
    auto headerView = m_ui->treeView->header();
    headerView->setSectionResizeMode(QHeaderView::ResizeToContents);
    headerView->setSectionResizeMode(0, QHeaderView::Stretch);

    m_ui->buttonBox->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    m_ui->buttonBox->button(QDialogButtonBox::Ok)->setText(tr("OK"));

    auto mcInstance = dynamic_cast<MinecraftInstance*>(m_instance);
    if (!mcInstance) {
        m_ui->offlineGroupBox->hide();
    } else {
        connect(m_ui->exportOfflineCheckBox, &QCheckBox::toggled, this, &ExportInstanceDialog::offlineExportToggled);
        connect(m_ui->includeLibrariesCheckBox, &QCheckBox::toggled, this, &ExportInstanceDialog::updateOfflineStats);
        connect(m_ui->includeAssetsCheckBox, &QCheckBox::toggled, this, &ExportInstanceDialog::updateOfflineStats);
        connect(m_ui->includeMetadataCheckBox, &QCheckBox::toggled, this, &ExportInstanceDialog::updateOfflineStats);

        offlineExportToggled(true);
    }
}

ExportInstanceDialog::~ExportInstanceDialog()
{
    if (m_statsWatcher) {
        m_statsWatcher->cancel();
        m_statsWatcher->waitForFinished();
        delete m_statsWatcher;
        m_statsWatcher = nullptr;
    }
    delete m_ui;
}

/// Save icon to instance's folder is needed
void SaveIcon(BaseInstance* m_instance)
{
    auto iconKey = m_instance->iconKey();
    auto iconList = APPLICATION->icons();
    auto mmcIcon = iconList->icon(iconKey);
    if (!mmcIcon || mmcIcon->isBuiltIn()) {
        return;
    }
    auto path = mmcIcon->getFilePath();
    if (!path.isNull()) {
        QFileInfo inInfo(path);
        FS::copy(path, FS::PathCombine(m_instance->instanceRoot(), inInfo.fileName()))();
        return;
    }
    auto& image = mmcIcon->m_images[mmcIcon->type()];
    auto& icon = image.icon;
    auto sizes = icon.availableSizes();
    if (sizes.size() == 0) {
        return;
    }
    auto areaOf = [](QSize size) { return size.width() * size.height(); };
    QSize largest = sizes[0];
    // find variant with largest area
    for (auto size : sizes) {
        if (areaOf(largest) < areaOf(size)) {
            largest = size;
        }
    }
    auto pixmap = icon.pixmap(largest);
    pixmap.save(FS::PathCombine(m_instance->instanceRoot(), iconKey + ".png"));
}

bool ExportInstanceDialog::validateAndConfirmOffline()
{
    if (!m_ui->exportOfflineCheckBox->isChecked()) {
        return true;
    }

    auto mcInstance = dynamic_cast<MinecraftInstance*>(m_instance);
    if (!mcInstance) {
        return true;
    }

    OfflineExportOptions options;
    options.includeLibraries = m_ui->includeLibrariesCheckBox->isChecked();
    options.includeAssets = m_ui->includeAssetsCheckBox->isChecked();
    options.includeMetadata = m_ui->includeMetadataCheckBox->isChecked();

    OfflineDependencyCollector collector(mcInstance, options);
    QList<OfflineFileEntry> offlineFiles;
    QStringList missingFiles;
    collector.collect(offlineFiles, missingFiles);

    if (missingFiles.isEmpty()) {
        return true;
    }

    QMessageBox box(this);
    box.setWindowTitle(tr("Missing Offline Dependencies"));
    box.setIcon(QMessageBox::Warning);
    box.setText(tr("Instance missing main libraries that is required to play offline. Instance should be run once on online machine.\n\n"
                   "Do you want to continue exporting anyway?"));
    auto exportAnywayButton = box.addButton(tr("Export Anyway"), QMessageBox::AcceptRole);
    auto cancelButton = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancelButton);

    box.exec();
    return box.clickedButton() == exportAnywayButton;
}

void ExportInstanceDialog::doExport()
{
    auto name = FS::RemoveInvalidFilenameChars(m_instance->name());

    const QString output = QFileDialog::getSaveFileName(this, tr("Export %1").arg(m_instance->name()),
                                                        FS::PathCombine(QDir::homePath(), name + ".zip"), "Zip (*.zip)", nullptr);
    if (output.isEmpty()) {
        return;
    }

    SaveIcon(m_instance);

    auto files = QFileInfoList();
    if (!MMCZip::collectFileListRecursively(m_instance->instanceRoot(), nullptr, &files,
                                            std::bind(&FileIgnoreProxy::filterFile, m_proxyModel, std::placeholders::_1))) {
        QMessageBox::warning(this, tr("Error"), tr("Unable to export instance"));
        return;
    }

    auto task = makeShared<MMCZip::ExportToZipTask>(output, m_instance->instanceRoot(), files, "", true);

    if (m_ui->exportOfflineCheckBox->isChecked()) {
        auto mcInstance = dynamic_cast<MinecraftInstance*>(m_instance);
        if (mcInstance) {
            OfflineExportOptions options;
            options.includeLibraries = m_ui->includeLibrariesCheckBox->isChecked();
            options.includeAssets = m_ui->includeAssetsCheckBox->isChecked();
            options.includeMetadata = m_ui->includeMetadataCheckBox->isChecked();

            OfflineDependencyCollector collector(mcInstance, options);
            QList<OfflineFileEntry> offlineFiles;
            QStringList missingFiles;
            collector.collect(offlineFiles, missingFiles);

            QByteArray manifestData = collector.createManifest(offlineFiles);
            task->addExtraFile(".offline/manifest.json", manifestData);

            for (const auto& file : offlineFiles) {
                task->addExtraDiskFile(file.diskPath, file.archivePath);
            }
        }
    }

    connect(task.get(), &Task::failed, this,
            [this, output](QString reason) { CustomMessageBox::selectable(this, tr("Error"), reason, QMessageBox::Critical)->show(); });
    connect(task.get(), &Task::finished, this, [task] { task->deleteLater(); });

    ProgressDialog progress(this);
    progress.setSkipButton(true, tr("Abort"));
    auto result = progress.execWithTask(task.get());
    QDialog::done(result);
}

void ExportInstanceDialog::offlineExportToggled(bool checked)
{
    m_ui->includeLibrariesCheckBox->setEnabled(checked);
    m_ui->includeAssetsCheckBox->setEnabled(checked);
    m_ui->includeMetadataCheckBox->setEnabled(checked);
    if (!checked) {
        m_ui->offlineWarningLabel->setVisible(false);
    }
    updateOfflineStats();
}

void ExportInstanceDialog::updateOfflineStats()
{
    if (!m_ui->exportOfflineCheckBox->isChecked()) {
        m_ui->offlineInfoLabel->setText(tr("Includes all libraries, assets, and component metadata required to launch this instance offline without internet access."));
        m_ui->offlineWarningLabel->setVisible(false);
        return;
    }

    auto mcInstance = dynamic_cast<MinecraftInstance*>(m_instance);
    if (!mcInstance) {
        m_ui->offlineWarningLabel->setVisible(false);
        return;
    }

    m_ui->offlineInfoLabel->setText(tr("Calculating offline dependencies..."));
    m_ui->offlineWarningLabel->setVisible(false);

    OfflineExportOptions options;
    options.includeLibraries = m_ui->includeLibrariesCheckBox->isChecked();
    options.includeAssets = m_ui->includeAssetsCheckBox->isChecked();
    options.includeMetadata = m_ui->includeMetadataCheckBox->isChecked();

    if (m_statsWatcher) {
        m_statsWatcher->cancel();
        m_statsWatcher->waitForFinished();
        delete m_statsWatcher;
        m_statsWatcher = nullptr;
    }

    auto future = QtConcurrent::run(QThreadPool::globalInstance(), [mcInstance, options]() {
        OfflineDependencyCollector collector(mcInstance, options);
        QList<OfflineFileEntry> files;
        QStringList missing;
        collector.collect(files, missing);

        return OfflineDependencyCollector::computeStats(files, missing);
    });

    m_statsWatcher = new QFutureWatcher<OfflineStats>(this);
    connect(m_statsWatcher, &QFutureWatcher<OfflineStats>::finished, this, [this]() {
        if (!m_statsWatcher || m_statsWatcher->isCanceled()) {
            return;
        }
        OfflineStats stats = m_statsWatcher->result();
        if (!m_ui->exportOfflineCheckBox->isChecked()) {
            m_ui->offlineWarningLabel->setVisible(false);
            return;
        }

        QString info = tr("Includes ~%1 of assets (%2 files), ~%3 of libraries (%4 files), and %5 metadata files (~%6) (Total: ~%7).")
                           .arg(StringUtils::humanReadableFileSize(stats.assetSize), QString::number(stats.assetCount),
                                StringUtils::humanReadableFileSize(stats.libSize), QString::number(stats.libCount),
                                QString::number(stats.metaCount),
                                StringUtils::humanReadableFileSize(stats.metaSize),
                                StringUtils::humanReadableFileSize(stats.totalSize));
        m_ui->offlineInfoLabel->setText(info);

        if (stats.missingCount > 0) {
            m_ui->offlineWarningLabel->setText(
                "<span style=\"color: #e67e22; font-weight: bold;\">⚠️ Note:</span> "
                "<span style=\"color: #e67e22; font-weight: bold;\">Instance is missing required offline dependencies (e.g. not launched online yet). "
                "Instance should be run once on online machine before exporting.</span>");
            m_ui->offlineWarningLabel->setVisible(true);
        } else {
            m_ui->offlineWarningLabel->setVisible(false);
        }
    });
    m_statsWatcher->setFuture(future);
}


void ExportInstanceDialog::done(int result)
{
    m_proxyModel->saveBlockedPathsToFile(ignoreFileName());
    if (result == QDialog::Accepted) {
        if (!validateAndConfirmOffline()) {
            return;
        }
        doExport();
        return;
    }
    QDialog::done(result);
}

void ExportInstanceDialog::rowsInserted(QModelIndex parent, int top, int bottom)
{
    // WARNING: possible off-by-one?
    for (int i = top; i < bottom; i++) {
        auto node = m_proxyModel->index(i, 0, parent);
        if (m_proxyModel->shouldExpand(node)) {
            auto expNode = node.parent();
            if (!expNode.isValid()) {
                continue;
            }
            m_ui->treeView->expand(node);
        }
    }
}

QString ExportInstanceDialog::ignoreFileName()
{
    return FS::PathCombine(m_instance->instanceRoot(), ".packignore");
}
