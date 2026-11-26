#include "cross_check.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTextStream>
#include <QRegExp>
#include <qgsvectorlayer.h>
#include <QgsVectorFileWriter.h>
#include <QgsFeature.h>
#include <QgsGeometry.h>
#include <QgsFields.h>
#include <QgsWkbTypes.h>
#include <QgsCoordinateReferenceSystem.h>

// -------------------------------------------------------
// 辅助：判断 error_type 是否是内容类错误
// -------------------------------------------------------
static bool isContentErrorType(const QString &errorType, const QString &errorMsg)
{
    // 明确不是内容类 → 不参与交叉验证修正
    if (errorType.contains("MISSING_SYMBOL", Qt::CaseInsensitive))
        return false;

    // 符号缺失 / GDB缺失注记 → 不修改
    if (errorMsg.contains(QString::fromUtf8("【符号缺失注记】")) ||
        errorMsg.contains(QString::fromUtf8("【GDB要素缺失注记】")) ||
        errorMsg.contains(QString::fromUtf8("注记缺失符号")) ||
        errorMsg.contains(QString::fromUtf8("缺失符号")))
        return false;

    // 内容相关错误
    if (errorType.contains("CONTENT_ERROR", Qt::CaseInsensitive) ||
        errorType.contains("MATCH_ERROR", Qt::CaseInsensitive) ||
        errorType.contains("content_mismatch", Qt::CaseInsensitive) ||
        errorType.contains("content_error", Qt::CaseInsensitive) ||
        errorType.contains("order_mismatch", Qt::CaseInsensitive) ||
        errorType.contains("partially_matched", Qt::CaseInsensitive))
        return true;

    // error_msg中的内容相关关键词
    if (errorMsg.contains(QString::fromUtf8("内容错误")) ||
        errorMsg.contains(QString::fromUtf8("内容不匹配")) ||
        errorMsg.contains(QString::fromUtf8("注记不匹配")) ||
        errorMsg.contains(QString::fromUtf8("顺序错误")) ||
        errorMsg.contains(QString::fromUtf8("回扫")))
        return true;

    return false;
}

// -------------------------------------------------------
// 辅助：从213 JSON文件读取内容错误FID（不依赖SHP）
// 213 JSON格式: { "groups": [{ "error_type": "...", "items": [{ "anno_fid": "...", "feature_id": "..." }] }] }
// anno_fid 与 200 匹配结果的主键一致；feature_id 仅用于兼容旧版 JSON。
// -------------------------------------------------------
static QList<CrossCheckFidRecord> readJsonErrorRecords(const QString &jsonPath,
                                                      const QString &shpPath)
{
    QList<CrossCheckFidRecord> records;
    if (!QFile::exists(jsonPath))
        return records;

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly))
        return records;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    QJsonObject root = doc.object();

    // 兼容两种格式：groups (213) / results (210)
    QJsonArray groups = root["groups"].toArray();
    if (groups.isEmpty())
        groups = root["results"].toArray();

    for (int i = 0; i < groups.size(); ++i)
    {
        QJsonObject group = groups[i].toObject();
        QString errorType = group["error_type"].toString();

        QJsonArray items = group["items"].toArray();
        for (int j = 0; j < items.size(); ++j)
        {
            QJsonObject item = items[j].toObject();
            QString fid = item["anno_fid"].toString();
            if (fid.isEmpty())
                fid = item["feature_id"].toString();
            if (fid.isEmpty())
                fid = item["symbol_short_id"].toString();
            if (fid.isEmpty())
                continue;

            QString name = item["character"].toString();
            // error_msg 用于 isContentErrorType 校验
            QString errMsg = item["notes"].toString();
            if (errMsg.isEmpty())
                errMsg = item["description"].toString();

            // 跳过非内容类错误（符号缺失等不参与交叉验证）
            if (!isContentErrorType(errorType, errMsg))
                continue;

            CrossCheckFidRecord rec;
            rec.fid = fid;
            rec.name = name;
            rec.matchStat = QString();  // 来自JSON的是错误记录
            rec.errorType = errorType;
            rec.shpPath = shpPath;
            records.append(rec);
        }
    }

    return records;
}

// -------------------------------------------------------
// 辅助：读取中间文件
// -------------------------------------------------------
static CrossCheckData readMiddleware(const QString &filePath)
{
    CrossCheckData data;
    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return data;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    QJsonObject root = doc.object();
    data.annotLayer = root["annot_layer"].toString();

    QJsonObject typesObj = root["types"].toObject();
    for (auto it = typesObj.constBegin(); it != typesObj.constEnd(); ++it)
    {
        const QString &typeName = it.key();
        QJsonArray arr = it.value().toArray();
        QList<CrossCheckFidRecord> records;
        for (int i = 0; i < arr.size(); ++i)
        {
            QJsonObject recObj = arr[i].toObject();
            CrossCheckFidRecord rec;
            rec.fid = recObj["fid"].toString();
            rec.name = recObj["name"].toString();
            rec.matchStat = recObj["match_stat"].toString();
            rec.errorType = recObj["error_type"].toString();
            rec.shpPath = recObj["shp"].toString();
            rec.jsonPath = recObj["json"].toString();
            records.append(rec);
        }
        data.types[typeName] = records;
    }
    return data;
}

// -------------------------------------------------------
// 辅助：写入中间文件
// -------------------------------------------------------
static void writeMiddleware(const QString &filePath, const CrossCheckData &data)
{
    QJsonObject root;
    root["annot_layer"] = data.annotLayer;

    QJsonObject typesObj;
    for (auto it = data.types.constBegin(); it != data.types.constEnd(); ++it)
    {
        QJsonArray arr;
        for (const CrossCheckFidRecord &rec : it.value())
        {
            QJsonObject recObj;
            recObj["fid"] = rec.fid;
            recObj["name"] = rec.name;
            recObj["match_stat"] = rec.matchStat;
            recObj["error_type"] = rec.errorType;
            recObj["shp"] = rec.shpPath;
            recObj["json"] = rec.jsonPath;
            arr.append(recObj);
        }
        typesObj[it.key()] = arr;
    }
    root["types"] = typesObj;

    QJsonDocument doc(root);
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        file.write(doc.toJson(QJsonDocument::Indented));
        file.close();
    }
}

// -------------------------------------------------------
// 辅助：从SHP中移除指定FID
// -------------------------------------------------------
static bool removeFidsFromShp(const QString &shpPath, const QSet<QString> &fidsToRemove)
{
    if (fidsToRemove.isEmpty() || !QFile::exists(shpPath))
        return false;

    QgsVectorLayer *layer = new QgsVectorLayer(shpPath, "temp", "ogr");
    if (!layer || !layer->isValid())
    {
        delete layer;
        return false;
    }

    QgsFields fields = layer->fields();
    int annoFidIdx = fields.indexOf("anno_fid");

    // 收集要保留的要素
    QList<QgsFeature> keepFeatures;
    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature feat;
    int removed = 0;
    int total = 0;
    while (it.nextFeature(feat))
    {
        total++;
        QString fidStr = annoFidIdx >= 0 ? feat.attribute(annoFidIdx).toString() : QString();
        if (fidsToRemove.contains(fidStr))
        {
            removed++;
            continue;
        }
        keepFeatures.append(feat);
    }

    // 读取原图层的CRS和几何类型
    QgsCoordinateReferenceSystem layerCrs = layer->crs();
    QgsWkbTypes::Type layerGeomType = layer->wkbType();

    delete layer;

    if (removed == 0)
        return false;

    // 写入临时文件再替换
    QString tmpPath = shpPath + ".crosscheck_tmp";

    // 先删除可能存在的临时文件
    QStringList tmpExts = {".shp", ".shx", ".dbf", ".prj", ".cpg"};
    for (const QString &ext : tmpExts)
        QFile::remove(tmpPath.replace(QRegExp("\\.[^.]+$"), "") + ext);

    QFileInfo fi(shpPath);
    tmpPath = fi.absolutePath() + "/" + fi.completeBaseName() + ".crosscheck_tmp.shp";

    std::unique_ptr<QgsVectorFileWriter> writer(
        new QgsVectorFileWriter(tmpPath, "UTF-8", fields, layerGeomType, layerCrs, "ESRI Shapefile"));

    if (writer->hasError() != QgsVectorFileWriter::NoError)
    {
        writer.reset();
        for (const QString &ext : tmpExts)
            QFile::remove(tmpPath.replace(QRegExp("\\.[^.]+$"), "") + ext);
        return false;
    }

    for (const QgsFeature &f : keepFeatures)
    {
        QgsFeature feat = f;
        writer->addFeature(feat);
    }
    writer.reset(); // 关闭文件

    // 替换原文件（保留 .prj 如果有）
    QString shpBase = shpPath;
    shpBase.replace(".shp", "");
    QString tmpBase = tmpPath;
    tmpBase.replace(".shp", "");

    QStringList exts = {".shp", ".shx", ".dbf"};
    for (const QString &ext : exts)
    {
        QFile::remove(shpBase + ext);
        QFile::rename(tmpBase + ext, shpBase + ext);
    }

    return true;
}

// -------------------------------------------------------
// 辅助：从JSON中移除指定FID
// -------------------------------------------------------
static bool removeFidsFromJson(const QString &jsonPath, const QSet<QString> &fidsToRemove)
{
    if (fidsToRemove.isEmpty() || !QFile::exists(jsonPath))
        return false;

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    QJsonObject root = doc.object();

    // 兼容 groups (213) 和 results (210) 两种格式
    QJsonArray groups = root["groups"].toArray();
    bool isGroupsFormat = !groups.isEmpty();
    if (!isGroupsFormat)
        groups = root["results"].toArray();

    QJsonArray newGroups;
    int removedGroups = 0;
    int removedItems = 0;

    for (int i = 0; i < groups.size(); ++i)
    {
        QJsonObject group = groups[i].toObject();
        QJsonArray items = group["items"].toArray();

        // 收集不在移除列表中的item
        QJsonArray keptItems;
        for (int j = 0; j < items.size(); ++j)
        {
            QJsonObject item = items[j].toObject();
            QString featId = item["anno_fid"].toString();
            if (featId.isEmpty())
                featId = item["feature_id"].toString();
            if (fidsToRemove.contains(featId))
            {
                removedItems++;
            }
            else
            {
                keptItems.append(item);
            }
        }

        // 如果整个group都没item了，整个group移除
        if (keptItems.isEmpty())
        {
            removedGroups++;
            continue;
        }

        // 只剩部分item，更新item_count
        group["items"] = keptItems;
        group["item_count"] = keptItems.size();
        newGroups.append(group);
    }

    if (removedGroups == 0 && removedItems == 0)
        return false;

    // 写回时保持原格式key名
    if (isGroupsFormat)
        root["groups"] = newGroups;
    else
        root["results"] = newGroups;

    // 更新summary
    QJsonObject summary = root["summary"].toObject();
    int totalItems = summary["total_items"].toInt() - removedItems;
    int totalGroups = summary["total_groups"].toInt() - removedGroups;
    summary["total_items"] = qMax(0, totalItems);
    summary["total_groups"] = qMax(0, totalGroups);
    root["summary"] = summary;

    // 更新info。兼容历史字符串格式和当前 { "description": ... } 格式。
    QJsonValue infoValue = root["info"];
    QJsonObject infoObject;
    if (infoValue.isObject())
        infoObject = infoValue.toObject();
    QString info = infoObject.isEmpty()
        ? infoValue.toString()
        : infoObject.value("description").toString();
    info += QString::fromUtf8("（交叉验证修正：删除%1组/%2条误报）")
                .arg(removedGroups).arg(removedItems);
    if (infoValue.isObject())
    {
        infoObject["description"] = info;
        root["info"] = infoObject;
    }
    else
    {
        root["info"] = info;
    }

    // 写回
    QFile outFile(jsonPath);
    if (outFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        outFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        outFile.close();
        return true;
    }
    return false;
}

// -------------------------------------------------------
// 主函数：执行跨类型交叉验证
// -------------------------------------------------------
bool performCrossTypeCheck(
    const QString& outputDir,
    const QString& annotLayerName,
    const QString& currentType,
    const QString& currentShpPath,
    const QString& currentJsonPath,
    QString& logDetails)
{
    // 1. 确定中间文件路径
    QString midPath = QDir(outputDir).filePath(
        QString("cross_check_%1.json").arg(annotLayerName));

    // 2. 读取或创建中间文件
    CrossCheckData midData = readMiddleware(midPath);
    if (midData.annotLayer.isEmpty())
        midData.annotLayer = annotLayerName;

    // 3. 从JSON读取当前type的内容错误FID（不依赖SHP，更可靠）
    //    213 JSON格式比SHP更稳定，避免字段名/编码/SHP空文件等问题
    QList<CrossCheckFidRecord> currentRecords = readJsonErrorRecords(currentJsonPath, currentShpPath);
    for (CrossCheckFidRecord &rec : currentRecords)
    {
        rec.jsonPath = currentJsonPath;
    }

    // 保留 saveFullMatchedToCrossCheck 预先写入的 FULLY_MATCHED 记录
    QList<CrossCheckFidRecord> existingFullMatched;
    if (midData.types.contains(currentType))
    {
        for (const CrossCheckFidRecord &rec : midData.types[currentType])
        {
            if (rec.matchStat == "FULLY_MATCHED")
                existingFullMatched.append(rec);
        }
    }
    midData.types[currentType] = currentRecords;
    midData.types[currentType].append(existingFullMatched);

    // 4. 交叉比对：找出在任意type中FULLY_MATCHED的FID
    QSet<QString> fullyMatchedFids;  // 在某个type中完美匹配的FID
    QMap<QString, QSet<QString>> oldestMatchedFids; // 除了当前type，已在历史type中完全匹配的
    // 记录每个FID匹配的type
    QMap<QString, QString> fidBestType;

    for (auto it = midData.types.constBegin(); it != midData.types.constEnd(); ++it)
    {
        const QString &typeName = it.key();
        for (const CrossCheckFidRecord &rec : it.value())
        {
            if (rec.matchStat == "FULLY_MATCHED")
            {
                fullyMatchedFids.insert(rec.fid);
                if (typeName != currentType)
                    oldestMatchedFids[typeName].insert(rec.fid);
                fidBestType[rec.fid] = typeName;
            }
        }
    }

    if (fullyMatchedFids.isEmpty())
    {
        // 没有完全匹配的FID，保存中间文件即可
        writeMiddleware(midPath, midData);
        return false;
    }

    // 5. 对每个type，找出需要移除的内容错误FID
    bool anyModified = false;
    QStringList detailLines;

    for (auto it = midData.types.begin(); it != midData.types.end(); ++it)
    {
        const QString &typeName = it.key();
        QList<CrossCheckFidRecord> &records = it.value();

        // 收集本type中需要移除的FID
        // 条件：FID在另一个type中是FULLY_MATCHED，而在本type中是内容错误
        QSet<QString> toRemove;
        // 收集被移除条目的名称（用于日志）
        QMap<QString, QString> removedName;   // fid → name
        QMap<QString, QString> removedCorrectType; // fid → 正确的type
        for (const CrossCheckFidRecord &rec : records)
        {
            if (fullyMatchedFids.contains(rec.fid) && fidBestType[rec.fid] != typeName)
            {
                toRemove.insert(rec.fid);
                removedName[rec.fid] = rec.name;
                removedCorrectType[rec.fid] = fidBestType[rec.fid];
            }
        }

        if (toRemove.isEmpty())
            continue;

        // 从SHP中移除
        QSet<QString> shpPaths;
        for (const CrossCheckFidRecord &rec : records)
        {
            if (toRemove.contains(rec.fid))
                shpPaths.insert(rec.shpPath);
        }
        for (const QString &shp : shpPaths)
        {
            if (removeFidsFromShp(shp, toRemove))
                anyModified = true;
        }

        // 从JSON中移除
        QSet<QString> jsonPaths;
        for (const CrossCheckFidRecord &rec : records)
        {
            if (toRemove.contains(rec.fid))
                jsonPaths.insert(rec.jsonPath);
        }
        for (const QString &jpath : jsonPaths)
        {
            if (removeFidsFromJson(jpath, toRemove))
                anyModified = true;
        }

        // 生成详情日志
        for (const QString &fid : toRemove)
        {
            QString correctType = removedCorrectType.value(fid, QStringLiteral("?"));
            QString name = removedName.value(fid, QStringLiteral("?"));
            // 只显示被修正文件的简短名称
            QString cleanType = typeName;
            QFileInfo mainFi(shpPaths.isEmpty() ? QString() : *shpPaths.begin());
            QString shortFile = mainFi.fileName();

            detailLines.append(QStringLiteral("  [%1] FID=%2 注记=\"%3\" → 在 %4 中正确，从 %5 中清除")
                .arg(shortFile, fid, name, correctType, cleanType));
        }
    }

    if (!detailLines.isEmpty())
    {
        logDetails = QStringLiteral("  交叉验证清除明细（共%1条跨类型误报）:\n%2")
            .arg(detailLines.size()).arg(detailLines.join(QStringLiteral("\n")));
    }

    // 6. 保存中间文件
    writeMiddleware(midPath, midData);

    return anyModified;
}

// -------------------------------------------------------
// 公共接口：从200匹配结果SHP的match_stat字段直接提取FULLY_MATCHED FID，
//           写入交叉验证中间文件
// -------------------------------------------------------
void saveFullMatchedToCrossCheck(
    const QString& outputDir,
    const QString& annotLayerName,
    const QString& currentType,
    const QString& finalJsonPath,
    const QString& matchResultShpPath,
    const QString& tempShpPath)
{
    // 1. 从200匹配结果SHP中读取 match_stat 字段，
    //    只有 match_stat == "FULLY_MATCHED" 的记录才算真正的完全匹配
    QList<CrossCheckFidRecord> fullMatched;
    {
        QgsVectorLayer *layer = new QgsVectorLayer(matchResultShpPath, "full_matched", "ogr");
        if (layer && layer->isValid())
        {
            QgsFields fields = layer->fields();
            int annoFidIdx = fields.indexOf("anno_fid");
            int matchStatIdx = fields.indexOf("match_stat");

            if (annoFidIdx >= 0)
            {
                QgsFeatureIterator it = layer->getFeatures();
                QgsFeature feat;
                while (it.nextFeature(feat))
                {
                    QString fid = feat.attribute(annoFidIdx).toString();
                    if (fid.isEmpty())
                        continue;
                    QString stat = matchStatIdx >= 0 ? feat.attribute(matchStatIdx).toString() : QString();
                    // 只接受明确 FULLY_MATCHED 的FID，不依赖 tempShpPath 差值计算
                    if (stat != QStringLiteral("FULLY_MATCHED"))
                        continue;

                    CrossCheckFidRecord rec;
                    rec.fid = fid;
                    rec.name = QString();
                    rec.matchStat = "FULLY_MATCHED";
                    rec.errorType = "FULLY_MATCHED";
                    rec.shpPath = tempShpPath;
                    rec.jsonPath = finalJsonPath;
                    fullMatched.append(rec);
                }
            }
            delete layer;
        }
    }

    if (fullMatched.isEmpty())
        return;

    // 2. 读/建中间文件
    QString midPath = QDir(outputDir).filePath(
        QString("cross_check_%1.json").arg(annotLayerName));
    CrossCheckData midData = readMiddleware(midPath);
    if (midData.annotLayer.isEmpty())
        midData.annotLayer = annotLayerName;

    // 3. 合并：保留已有FULLY_MATCHED，新增本次的（去重）
    QMap<QString, CrossCheckFidRecord> existingMap;
    if (midData.types.contains(currentType))
    {
        for (const CrossCheckFidRecord &r : midData.types[currentType])
        {
            if (r.matchStat == "FULLY_MATCHED")
                existingMap[r.fid] = r;
        }
    }
    for (const CrossCheckFidRecord &rec : fullMatched)
    {
        if (!existingMap.contains(rec.fid))
            existingMap[rec.fid] = rec;
    }

    // 4. 重建该type的记录列表
    QList<CrossCheckFidRecord> merged;
    for (auto it = existingMap.constBegin(); it != existingMap.constEnd(); ++it)
        merged.append(it.value());
    midData.types[currentType] = merged;

    writeMiddleware(midPath, midData);
}
