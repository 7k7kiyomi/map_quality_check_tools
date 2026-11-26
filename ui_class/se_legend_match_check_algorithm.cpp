#include "se_legend_match_check_algorithm.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include <QString>
#include <QFile>
#include <QTextStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QDir>
#include <QFileInfo>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#ifdef _WIN32
#include <spdlog/sinks/wincolor_sink.h>
#endif
#include <cmath>
#include <memory>
#include <cstring>

// 安全日志宏定义，防止崩溃（直接检查logger是否存在）
#define SAFE_LOG_INFO(...) do { try { auto logger = spdlog::get("legend_match_check"); if (logger) { logger->info(__VA_ARGS__); } } catch(...) {} } while(0)
#define SAFE_LOG_ERROR(...) do { try { auto logger = spdlog::get("legend_match_check"); if (logger) { logger->error(__VA_ARGS__); } } catch(...) {} } while(0)
#define SAFE_LOG_WARN(...) do { try { auto logger = spdlog::get("legend_match_check"); if (logger) { logger->warn(__VA_ARGS__); } } catch(...) {} } while(0)

static constexpr bool kCheckStrokeDas = false; // stroke_das数组由PDF/SHP导出方式影响较大，默认不参与质检。

// 清理日志器资源的辅助函数
static void CleanupLogger() {
    try {
        spdlog::drop("legend_match_check");
        spdlog::set_default_logger(nullptr);
    } catch (...) {
        // 忽略清理异常
    }
}

struct CsvStandardFeature {
    QString elementName;
    QString symbolName;
    QString stroked;
    QString strokeCol;
    QString strokeWid;
    QString strokeDas;
    QString isDashed;
    QString fillColor;
    QString sizeOrWidth;
    QString sourcePdf;
    QString sourceRow;
    QString splitPart;
};

static QStringList SplitCsvLine(const QString& line) {
    QStringList cells;
    QString current;
    bool inQuotes = false;
    for (int i = 0; i < line.size(); ++i) {
        QChar ch = line.at(i);
        if (ch == '"') {
            if (inQuotes && i + 1 < line.size() && line.at(i + 1) == '"') {
                current.append('"');
                ++i;
            } else {
                inQuotes = !inQuotes;
            }
        } else if (ch == ',' && !inQuotes) {
            cells.append(current);
            current.clear();
        } else {
            current.append(ch);
        }
    }
    cells.append(current);
    return cells;
}

static QString NormalizeCsvText(QString text) {
    if (!text.isEmpty() && text.at(0).unicode() == 0xFEFF) {
        text.remove(0, 1);
    }
    return text.trimmed();
}

static QStringList SplitFilterValues(QString value) {
    value.replace("，", ",");
    QStringList result = value.split(",", QString::SkipEmptyParts);
    for (QString& item : result) {
        item = item.trimmed();
    }
    if (result.isEmpty() && !value.trimmed().isEmpty()) {
        result << value.trimmed();
    }
    return result;
}

static void AddUniqueFilterValue(QStringList& values, QString value) {
    value = value.trimmed();
    if (value.isEmpty()) {
        return;
    }
    if (!values.contains(value)) {
        values << value;
    }
}

static QString NormalizeLayerName(QString value) {
    value = QFileInfo(value).completeBaseName().trimmed();
    value.replace("_LineString", "", Qt::CaseInsensitive);
    value.replace("_Polygon", "", Qt::CaseInsensitive);
    value.replace("_Point", "", Qt::CaseInsensitive);
    value.replace("_标注", "");
    value.replace(" 代码标注", "");
    value.replace("_代码标注", "");
    value.replace("高速铁路及车站", "高铁");
    return value.trimmed();
}

static void AddLegendAliases(QStringList& values) {
    QStringList snapshot = values;
    for (const QString& value : snapshot) {
        if (value == "高速铁路及车站" || value == "高速铁路" || value == "高铁站") {
            AddUniqueFilterValue(values, "高铁");
        }
        if (value == "铁路及车站") {
            AddUniqueFilterValue(values, "铁路");
        }
        if (value == "高速公路及出入口") {
            AddUniqueFilterValue(values, "高速公路");
        }
        if (value == "干渠" || value == "总干渠" || value == "支渠") {
            AddUniqueFilterValue(values, "渠道");
        }
        if (value == "省级驻地" || value == "省政府驻地") {
            AddUniqueFilterValue(values, "省级政府驻地");
        }
        if (value == "市级驻地" || value == "地级市驻地" || value == "地级市州驻地" ||
            value == "市政府驻地" || value == "地级市政府驻地" || value == "地级市州政府驻地") {
            AddUniqueFilterValue(values, "地级市州政府驻地");
        }
        if (value == "县级驻地" || value == "县政府驻地" || value == "县(区、市)政府驻地" ||
            value == "县（区、市）政府驻地") {
            AddUniqueFilterValue(values, "县级政府驻地");
        }
        if (value == "乡镇级驻地" || value == "乡镇驻地" || value == "乡(镇)政府驻地" ||
            value == "乡（镇）政府驻地") {
            AddUniqueFilterValue(values, "乡镇政府驻地");
        }
    }
}

static bool NameMatchesFilter(const QString& elementName, const QString& symbolName, const QStringList& filters) {
    for (const QString& filter : filters) {
        if (filter.isEmpty()) {
            continue;
        }
        if (elementName.compare(filter, Qt::CaseInsensitive) == 0 ||
            symbolName.compare(filter, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

static QStringList SplitMultiValues(const QString& value) {
    QStringList parts = value.split(";", QString::SkipEmptyParts);
    for (QString& part : parts) {
        part = part.trimmed();
    }
    return parts;
}

static bool StringMatchesAny(const QString& actual, const QString& expected) {
    QString actualNorm = actual.trimmed();
    QStringList expectedParts = SplitMultiValues(expected);
    if (expectedParts.isEmpty()) {
        expectedParts << expected.trimmed();
    }
    for (const QString& item : expectedParts) {
        if (!item.isEmpty() && actualNorm.compare(item, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

static QString CanonicalDashValue(QString value) {
    value = value.trimmed();
    if (value.isEmpty()) {
        return value;
    }
    value.remove('[');
    value.remove(']');
    QStringList rawParts = value.split(",", QString::SkipEmptyParts);
    QStringList normalized;
    for (QString part : rawParts) {
        bool ok = false;
        double number = part.trimmed().toDouble(&ok);
        if (ok) {
            normalized << QString::number(number, 'f', 3);
        }
    }
    return normalized.join(",");
}

static bool DashMatchesAny(const QString& actual, const QString& expected) {
    QString actualCanon = CanonicalDashValue(actual);
    if (actualCanon.isEmpty()) {
        return expected.trimmed().isEmpty();
    }
    QStringList expectedParts = SplitMultiValues(expected);
    if (expectedParts.isEmpty()) {
        expectedParts << expected.trimmed();
    }
    for (const QString& item : expectedParts) {
        if (actualCanon == CanonicalDashValue(item)) {
            return true;
        }
    }
    return false;
}

static bool NumericMatches(const QString& actual, const QString& expected, double scale = 100.0) {
    bool okActual = false;
    bool okExpected = false;
    double actualValue = actual.trimmed().toDouble(&okActual);
    double expectedValue = expected.trimmed().toDouble(&okExpected);
    if (okActual && okExpected) {
        return qRound(actualValue * scale) == qRound(expectedValue * scale);
    }
    return actual.trimmed().compare(expected.trimmed(), Qt::CaseInsensitive) == 0;
}

static bool ParseRangeText(const QString& text, double& minValue, double& maxValue) {
    QString normalized = text.trimmed();
    normalized.replace("－", "-");
    normalized.replace("—", "-");
    normalized.replace("–", "-");
    QRegExp rx("([0-9]+(?:\\.[0-9]+)?)\\s*-\\s*([0-9]+(?:\\.[0-9]+)?)");
    if (rx.indexIn(normalized) >= 0) {
        bool ok1 = false;
        bool ok2 = false;
        minValue = rx.cap(1).toDouble(&ok1);
        maxValue = rx.cap(2).toDouble(&ok2);
        if (ok1 && ok2 && maxValue >= minValue) {
            return true;
        }
    }

    // 处理“总干渠0.3mm,支渠0.15mm”这类非连字符范围：取多个mm数值的最小/最大值。
    QRegExp mmRx("([0-9]+(?:\\.[0-9]+)?)\\s*(?:mm|毫米)");
    int pos = 0;
    bool hasValue = false;
    while ((pos = mmRx.indexIn(normalized, pos)) >= 0) {
        bool ok = false;
        double value = mmRx.cap(1).toDouble(&ok);
        if (ok) {
            if (!hasValue) {
                minValue = value;
                maxValue = value;
                hasValue = true;
            } else {
                if (value < minValue) minValue = value;
                if (value > maxValue) maxValue = value;
            }
        }
        pos += mmRx.matchedLength();
    }
    return hasValue && maxValue >= minValue && maxValue > minValue;
}

static bool StrokeWidthMatches(
    const QString& actual,
    const CsvStandardFeature& standard,
    QString& detail)
{
    bool okActual = false;
    double actualValue = actual.trimmed().toDouble(&okActual);
    if (!okActual) {
        bool matched = actual.trimmed().compare(standard.strokeWid.trimmed(), Qt::CaseInsensitive) == 0;
        if (!matched) {
            detail = QString::fromUtf8(u8"stroke_wid不匹配(标准:%1,实际:%2)").arg(standard.strokeWid).arg(actual);
        }
        return matched;
    }

    double minWidth = 0.0;
    double maxWidth = 0.0;
    if (ParseRangeText(standard.sizeOrWidth, minWidth, maxWidth)) {
        // CSV标准里的范围单位是mm；地图SHP里的stroke_wid常见是pt。
        double actualMm = actualValue * 0.3527778;
        const double toleranceMm = 0.03;
        if (actualMm + toleranceMm >= minWidth && actualMm - toleranceMm <= maxWidth) {
            return true;
        }
        detail = QString::fromUtf8(u8"stroke_wid超出范围(要求:%1,实际:%2pt≈%3mm)")
            .arg(standard.sizeOrWidth)
            .arg(actual)
            .arg(QString::number(actualMm, 'f', 3));
        return false;
    }

    bool okExpected = false;
    double expectedValue = standard.strokeWid.trimmed().toDouble(&okExpected);
    if (okExpected) {
        const double tolerance = 0.01;
        if (std::fabs(actualValue - expectedValue) <= tolerance) {
            return true;
        }
        detail = QString::fromUtf8(u8"stroke_wid不匹配(标准:%1,实际:%2,容差:%3)")
            .arg(standard.strokeWid)
            .arg(actual)
            .arg(tolerance);
        return false;
    }

    bool matched = actual.trimmed().compare(standard.strokeWid.trimmed(), Qt::CaseInsensitive) == 0;
    if (!matched) {
        detail = QString::fromUtf8(u8"stroke_wid不匹配(标准:%1,实际:%2)").arg(standard.strokeWid).arg(actual);
    }
    return matched;
}

static QString FindStandardCsvPath(const QString& standardLayerDir) {
    QFileInfo info(standardLayerDir);
    if (info.isFile() && info.fileName().compare("standard_legend_table.csv", Qt::CaseInsensitive) == 0) {
        return info.absoluteFilePath();
    }
    QDir dir(standardLayerDir);
    QString directPath = dir.absoluteFilePath("standard_legend_table.csv");
    if (QFileInfo::exists(directPath)) {
        return directPath;
    }
    QFileInfoList csvFiles = dir.entryInfoList(QStringList() << "*.csv", QDir::Files);
    for (const QFileInfo& csvInfo : csvFiles) {
        if (csvInfo.fileName().compare("standard_legend_table.csv", Qt::CaseInsensitive) == 0) {
            return csvInfo.absoluteFilePath();
        }
    }
    return QString();
}

static bool LoadCsvStandardFeatures(
    const QString& csvPath,
    const QStringList& elementNaList,
    QList<CsvStandardFeature>& standards,
    QString& errorMsg)
{
    QFile file(csvPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errorMsg = QString::fromUtf8(u8"无法打开标准图例CSV: ") + csvPath;
        return false;
    }

    QTextStream in(&file);
    in.setCodec("UTF-8");
    if (in.atEnd()) {
        errorMsg = QString::fromUtf8(u8"标准图例CSV为空: ") + csvPath;
        return false;
    }

    QString headerLine = in.readLine();
    QStringList headers = SplitCsvLine(headerLine);
    QMap<QString, int> idx;
    for (int i = 0; i < headers.size(); ++i) {
        idx[NormalizeCsvText(headers[i])] = i;
    }

    auto valueAt = [](const QStringList& cells, const QMap<QString, int>& index, const QString& field)->QString {
        int i = index.value(field, -1);
        if (i < 0 || i >= cells.size()) {
            return QString();
        }
        return NormalizeCsvText(cells[i]);
    };

    if (!idx.contains("element_name") && !idx.contains("symbol_nam")) {
        errorMsg = QString::fromUtf8(u8"标准图例CSV缺少 element_name/symbol_nam 字段");
        return false;
    }

    int lineNo = 1;
    while (!in.atEnd()) {
        QString line = in.readLine();
        ++lineNo;
        if (line.trimmed().isEmpty()) {
            continue;
        }
        QStringList cells = SplitCsvLine(line);
        QString elementName = valueAt(cells, idx, "element_name");
        QString symbolName = valueAt(cells, idx, "symbol_nam");
        if (symbolName.isEmpty()) {
            symbolName = elementName;
        }
        if (elementName.isEmpty()) {
            elementName = symbolName;
        }
        if (!NameMatchesFilter(elementName, symbolName, elementNaList)) {
            continue;
        }

        QString elementTy = valueAt(cells, idx, "element_ty");
        if (!elementTy.isEmpty() && elementTy.compare("path", Qt::CaseInsensitive) != 0) {
            continue;
        }

        CsvStandardFeature feature;
        feature.elementName = elementName;
        feature.symbolName = symbolName;
        feature.stroked = valueAt(cells, idx, "stroked");
        feature.strokeCol = valueAt(cells, idx, "stroke_col");
        feature.strokeWid = valueAt(cells, idx, "stroke_wid");
        feature.strokeDas = valueAt(cells, idx, "stroke_das");
        feature.isDashed = valueAt(cells, idx, "is_dashed");
        feature.fillColor = valueAt(cells, idx, "fill_color");
        feature.sizeOrWidth = valueAt(cells, idx, "size_or_width");
        feature.sourcePdf = valueAt(cells, idx, "source_pdf");
        feature.sourceRow = valueAt(cells, idx, "source_row");
        feature.splitPart = valueAt(cells, idx, "split_part");
        standards.append(feature);
    }

    if (standards.isEmpty()) {
        QStringList sampleNames;
        QFile sampleFile(csvPath);
        if (sampleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream sampleIn(&sampleFile);
            sampleIn.setCodec("UTF-8");
            if (!sampleIn.atEnd()) {
                QString sampleHeaderLine = sampleIn.readLine();
                QStringList sampleHeaders = SplitCsvLine(sampleHeaderLine);
                QMap<QString, int> sampleIdx;
                for (int i = 0; i < sampleHeaders.size(); ++i) {
                    sampleIdx[NormalizeCsvText(sampleHeaders[i])] = i;
                }
                int nameIdx = sampleIdx.value("element_name", -1);
                while (!sampleIn.atEnd() && sampleNames.size() < 20) {
                    QStringList cells = SplitCsvLine(sampleIn.readLine());
                    if (nameIdx >= 0 && nameIdx < cells.size()) {
                        QString sampleName = NormalizeCsvText(cells[nameIdx]);
                        if (!sampleName.isEmpty() && !sampleNames.contains(sampleName)) {
                            sampleNames << sampleName;
                        }
                    }
                }
            }
        }
        errorMsg = QString::fromUtf8(u8"标准图例CSV中未找到匹配的图例名称。筛选值=[")
            + elementNaList.join(",")
            + QString::fromUtf8(u8"]，CSV路径=")
            + csvPath
            + QString::fromUtf8(u8"，CSV名称样例=[")
            + sampleNames.join(",")
            + "]";
        return false;
    }
    return true;
}

static void WriteCheckJson(
    const QString& outputJsonPath,
    const QString& info,
    bool errExist,
    const QJsonObject& errorsObj = QJsonObject())
{
    QJsonObject checkItem;
    checkItem["id"] = "105";
    checkItem["name"] = QString::fromUtf8(u8"标准图例匹配检查");
    checkItem["err_exist"] = errExist ? "true" : "false";
    checkItem["status"] = errExist ? "质检完成" : "质检完成";
    checkItem["info"] = info;
    if (!errorsObj.isEmpty()) {
        checkItem["errors"] = errorsObj;
    }
    QJsonObject resultJson;
    resultJson["check_item"] = checkItem;
    QFile outFile(outputJsonPath);
    if (outFile.open(QIODevice::WriteOnly)) {
        QTextStream out(&outFile);
        out.setCodec("UTF-8");
        out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
        outFile.close();
    }
}

static void WriteFailureJson(const QString& outputJsonPath, const QString& info) {
    QJsonObject checkItem;
    checkItem["id"] = "105";
    checkItem["name"] = QString::fromUtf8(u8"标准图例匹配检查");
    checkItem["err_exist"] = "true";
    checkItem["status"] = QString::fromUtf8(u8"质检失败");
    checkItem["info"] = info;
    QJsonObject resultJson;
    resultJson["check_item"] = checkItem;
    QFile outFile(outputJsonPath);
    if (outFile.open(QIODevice::WriteOnly)) {
        QTextStream out(&outFile);
        out.setCodec("UTF-8");
        out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
        outFile.close();
    }
}

static const CsvStandardFeature* PickStandardForFeature(
    OGRFeature* feature,
    int symbolNameIdx,
    const QList<CsvStandardFeature>& standards)
{
    if (standards.isEmpty()) {
        return nullptr;
    }
    if (symbolNameIdx >= 0) {
        QString symbolName = QString::fromUtf8(feature->GetFieldAsString(symbolNameIdx)).trimmed();
        if (!symbolName.isEmpty()) {
            for (const CsvStandardFeature& standard : standards) {
                if (symbolName.compare(standard.symbolName, Qt::CaseInsensitive) == 0 ||
                    symbolName.compare(standard.elementName, Qt::CaseInsensitive) == 0) {
                    return &standard;
                }
            }
        }
    }
    return &standards.first();
}

static SE_Error LegendMatchCheckByCsv(
    const QString& csvPath,
    const char* szMapLayerPath,
    const QString& elementNaFilter,
    const char* szOutputShpPath,
    const QString& outputJsonPath,
    bool& bPassed)
{
    bPassed = false;
    QStringList elementNaList = SplitFilterValues(elementNaFilter);
    AddUniqueFilterValue(elementNaList, NormalizeLayerName(QString::fromUtf8(szMapLayerPath)));
    AddLegendAliases(elementNaList);
    QList<CsvStandardFeature> standards;
    QString errorMsg;
    if (!LoadCsvStandardFeatures(csvPath, elementNaList, standards, errorMsg)) {
        WriteFailureJson(outputJsonPath, errorMsg);
        return SE_ERROR_FAILURE;
    }

    SAFE_LOG_INFO("使用CSV标准图例表进行匹配: {}", csvPath.toStdString());
    SAFE_LOG_INFO("CSV标准记录数: {}", standards.size());

    GDALDataset* pMapDS = (GDALDataset*)GDALOpenEx(szMapLayerPath, GDAL_OF_VECTOR, NULL, NULL, NULL);
    if (pMapDS == nullptr) {
        QString msg = QString::fromUtf8(u8"无法打开地图图层: ") + QString::fromUtf8(szMapLayerPath);
        WriteFailureJson(outputJsonPath, msg);
        return SE_ERROR_FILE_NOT_FOUND;
    }
    OGRLayer* pMapLayer = pMapDS->GetLayer(0);
    if (pMapLayer == nullptr) {
        GDALClose(pMapDS);
        QString msg = QString::fromUtf8(u8"地图图层为空");
        WriteFailureJson(outputJsonPath, msg);
        return SE_ERROR_LAYER_INVALID;
    }

    OGRFeatureDefn* pMapLayerDefn = pMapLayer->GetLayerDefn();
    int mapElementTyIdx = pMapLayerDefn->GetFieldIndex("element_ty");
    int mapSymbolNamIdx = pMapLayerDefn->GetFieldIndex("symbol_nam");
    int mapStrokedIdx = pMapLayerDefn->GetFieldIndex("stroked");
    int mapStrokeColIdx = pMapLayerDefn->GetFieldIndex("stroke_col");
    int mapStrokeWidIdx = pMapLayerDefn->GetFieldIndex("stroke_wid");
    int mapFillColorIdx = pMapLayerDefn->GetFieldIndex("fill_color");
    int mapStrokeDasIdx = pMapLayerDefn->GetFieldIndex("stroke_das");
    int mapIsDashedIdx = pMapLayerDefn->GetFieldIndex("is_dashed");

    int totalPathFeatures = 0;
    int matchedFeatures = 0;
    QJsonObject errorsObj;
    QMap<GIntBig, QString> errorInfoMap;

    pMapLayer->ResetReading();
    OGRFeature* pFeature = nullptr;
    while ((pFeature = pMapLayer->GetNextFeature()) != nullptr) {
        if (mapElementTyIdx >= 0) {
            QString elementTy = QString::fromUtf8(pFeature->GetFieldAsString(mapElementTyIdx)).trimmed();
            if (elementTy.compare("path", Qt::CaseInsensitive) != 0) {
                OGRFeature::DestroyFeature(pFeature);
                continue;
            }
        }

        ++totalPathFeatures;
        const CsvStandardFeature* standard = PickStandardForFeature(pFeature, mapSymbolNamIdx, standards);
        bool featureMatched = true;
        QStringList errorFields;
        QStringList details;

        auto checkStringField = [&](int fieldIdx, const QString& fieldName, const QString& expected, bool allowAny)->void {
            if (fieldIdx < 0 || expected.trimmed().isEmpty()) {
                return;
            }
            QString actual = QString::fromUtf8(pFeature->GetFieldAsString(fieldIdx)).trimmed();
            if (actual.isEmpty()) {
                return;
            }
            bool ok = allowAny ? StringMatchesAny(actual, expected)
                               : (actual.compare(expected, Qt::CaseInsensitive) == 0);
            if (!ok) {
                featureMatched = false;
                errorFields.append(fieldName);
                    details.append(QString::fromUtf8(u8"%1不匹配(标准:%2,实际:%3)").arg(fieldName).arg(expected).arg(actual));
            }
        };

        if (standard != nullptr) {
            checkStringField(mapStrokedIdx, "stroked", standard->stroked, false);
            checkStringField(mapStrokeColIdx, "stroke_col", standard->strokeCol, true);
            checkStringField(mapFillColorIdx, "fill_color", standard->fillColor, true);
            checkStringField(mapIsDashedIdx, "is_dashed", standard->isDashed, false);

            if (mapStrokeWidIdx >= 0 && !standard->strokeWid.isEmpty()) {
                QString actual = QString::fromUtf8(pFeature->GetFieldAsString(mapStrokeWidIdx)).trimmed();
                QString widthDetail;
                if (!actual.isEmpty() && !StrokeWidthMatches(actual, *standard, widthDetail)) {
                    featureMatched = false;
                    errorFields.append("stroke_wid");
                    details.append(widthDetail);
                }
            }

            if (kCheckStrokeDas && mapStrokeDasIdx >= 0 && !standard->strokeDas.isEmpty()) {
                QString actual = QString::fromUtf8(pFeature->GetFieldAsString(mapStrokeDasIdx)).trimmed();
                if (!actual.isEmpty() && !DashMatchesAny(actual, standard->strokeDas)) {
                    featureMatched = false;
                    errorFields.append("stroke_das");
                    details.append(QString::fromUtf8(u8"stroke_das不匹配(标准:%1,实际:%2)").arg(standard->strokeDas).arg(actual));
                }
            }
        }

        if (featureMatched) {
            ++matchedFeatures;
        } else {
            QString errorKey = QString("error%1").arg(errorsObj.size() + 1);
            QString symbolName = mapSymbolNamIdx >= 0 ? QString::fromUtf8(pFeature->GetFieldAsString(mapSymbolNamIdx)).trimmed() : QString();
            QString desc = QString("FID=%1").arg(pFeature->GetFID());
            if (!symbolName.isEmpty()) {
                desc += QString("(symbol_nam=%1)").arg(symbolName);
            }
            desc += QString::fromUtf8(u8"的%1字段不匹配: %2").arg(errorFields.join(",")).arg(details.join("; "));
            errorsObj[errorKey] = desc;
            errorInfoMap[pFeature->GetFID()] = desc;
        }

        OGRFeature::DestroyFeature(pFeature);
    }

    if (totalPathFeatures == 0) {
        GDALClose(pMapDS);
        QString msg = QString::fromUtf8(u8"地图图层中未找到element_ty='path'的要素");
        WriteCheckJson(outputJsonPath, msg, true);
        return SE_ERROR_NONE;
    }

    bPassed = (matchedFeatures == totalPathFeatures);
    if (bPassed) {
        WriteCheckJson(outputJsonPath, QString::fromUtf8(u8"%1图层不存在图例匹配错误").arg(elementNaFilter), false);
    } else {
        WriteCheckJson(outputJsonPath, QString::fromUtf8(u8"%1图层存在图例匹配错误").arg(elementNaFilter), true, errorsObj);
    }

    if (!bPassed && szOutputShpPath != nullptr && strlen(szOutputShpPath) > 0 && !errorInfoMap.isEmpty()) {
        pMapLayer->ResetReading();
        GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
        if (poDriver != nullptr) {
            CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");
            char** options = nullptr;
            options = CSLSetNameValue(options, "ENCODING", "UTF-8");
            GDALDataset* poOutDS = poDriver->Create(szOutputShpPath, 0, 0, 0, GDT_Unknown, options);
            CSLDestroy(options);
            if (poOutDS != nullptr) {
                char** layerOptions = nullptr;
                layerOptions = CSLSetNameValue(layerOptions, "ENCODING", "UTF-8");
                OGRLayer* poOutLayer = poOutDS->CreateLayer("error_features", pMapLayer->GetSpatialRef(), pMapLayer->GetGeomType(), layerOptions);
                CSLDestroy(layerOptions);
                if (poOutLayer != nullptr) {
                    OGRFeatureDefn* mapDefn = pMapLayer->GetLayerDefn();
                    for (int i = 0; i < mapDefn->GetFieldCount(); i++) {
                        poOutLayer->CreateField(mapDefn->GetFieldDefn(i));
                    }
                    OGRFieldDefn hasErrField("HAS_ERR", OFTString);
                    hasErrField.SetWidth(10);
                    poOutLayer->CreateField(&hasErrField);
                    OGRFieldDefn errDetailField("ERR_DETAIL", OFTString);
                    errDetailField.SetWidth(254);
                    poOutLayer->CreateField(&errDetailField);

                    OGRFeature* pOutFeat = nullptr;
                    while ((pOutFeat = pMapLayer->GetNextFeature()) != nullptr) {
                        GIntBig fid = pOutFeat->GetFID();
                        if (errorInfoMap.contains(fid)) {
                            OGRFeature* poOutFeature = OGRFeature::CreateFeature(poOutLayer->GetLayerDefn());
                            poOutFeature->SetGeometry(pOutFeat->GetGeometryRef());
                            for (int i = 0; i < mapDefn->GetFieldCount(); i++) {
                                poOutFeature->SetField(i, pOutFeat->GetRawFieldRef(i));
                            }
                            poOutFeature->SetField("HAS_ERR", "true");
                            QByteArray detailBytes = errorInfoMap[fid].toUtf8();
                            poOutFeature->SetField("ERR_DETAIL", detailBytes.constData());
                            poOutLayer->CreateFeature(poOutFeature);
                            OGRFeature::DestroyFeature(poOutFeature);
                        }
                        OGRFeature::DestroyFeature(pOutFeat);
                    }
                }
                GDALClose(poOutDS);
            }
        }
    }

    SAFE_LOG_INFO("CSV标准匹配完成: 总path要素={}, 匹配={}", totalPathFeatures, matchedFeatures);
    GDALClose(pMapDS);
    return SE_ERROR_NONE;
}

/**
 * @brief 标准图例匹配检查主函数（输出JSON）
 */
SE_Error LegendMatchCheck(
    const char* szStandardLayerDir,
    const char* szMapLayerPath,
    const char* szElementNaFilter,
    const char* szOutputShpPath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    const char* szOutputLogPath,
    bool& bPassed)
{
    bPassed = false;
    GDALDataset* pStandardDS = nullptr;
    GDALDataset* pMapDS = nullptr;
    OGRLayer* pMapLayer = nullptr;

    // 初始化日志（使用安全方式，防止崩溃）
    try {
        CleanupLogger();
        if (szOutputLogPath != nullptr && strlen(szOutputLogPath) > 0)
        {
            // 将UTF-8路径转换为QString
            QString logPath = QString::fromUtf8(szOutputLogPath);
            
            // 先确保日志目录存在
            QFileInfo logFileInfo(logPath);
            QDir logDir = logFileInfo.absoluteDir();
            if (!logDir.exists()) {
                logDir.mkpath(".");
            }
            
            // 使用QFile预创建文件以支持中文路径
            QFile logFile(logPath);
            if (logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                logFile.close();
            }
            
            // 检查是否已存在同名日志器，存在则先删除
            if (spdlog::get("legend_match_check")) {
                spdlog::drop("legend_match_check");
            }
            
            // 将路径转换为本地8bit编码（Windows ANSI），spdlog可以正确处理
            // 注意：这只适用于路径中的中文字符在系统默认编码中可表示的情况
            std::string localPath = logPath.toLocal8Bit().constData();
            auto logger = spdlog::basic_logger_mt("legend_match_check", localPath);
            spdlog::set_default_logger(logger);
            spdlog::set_level(spdlog::level::info);
            logger->flush_on(spdlog::level::info);
            
            SAFE_LOG_INFO("=== 标准图例匹配检查开始 ===");
            QString standardDir = QString::fromUtf8(szStandardLayerDir);
            QString mapPath = QString::fromUtf8(szMapLayerPath);
            QString elementNa = QString::fromUtf8(szElementNaFilter);
            SAFE_LOG_INFO("标准图例文件夹: {}", standardDir.toStdString());
            SAFE_LOG_INFO("地图图层: {}", mapPath.toStdString());
            SAFE_LOG_INFO("筛选element_na: {}", elementNa.toStdString());
        }
    } catch (const spdlog::spdlog_ex& ex) {
        // spdlog异常，继续执行但不使用日志
    } catch (...) {
        // 其他异常，继续执行但不使用日志
    }

    // 初始化GDAL
    GDALAllRegister();
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");

    // 检查必需参数
    if (szElementNaFilter == NULL || strlen(szElementNaFilter) == 0) {
        QString errMsg = QString::fromUtf8("element_na筛选值不能为空");
        
        QJsonObject checkItem;
        checkItem["id"] = "105";
        checkItem["name"] = "标准图例匹配检查";
        checkItem["err_exist"] = "true";
        checkItem["status"] = "质检失败";
        checkItem["info"] = errMsg;
        
        QJsonObject resultJson;
        resultJson["check_item"] = checkItem;
        
        QFile outFile(QString::fromUtf8(szOutputJsonPath));
        if (outFile.open(QIODevice::WriteOnly)) {
            QTextStream out(&outFile);
            out.setCodec("UTF-8");
            out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
            outFile.close();
        }
        
        return SE_ERROR_FAILURE;
    }

    QString elementNaFilter = QString::fromUtf8(szElementNaFilter).trimmed();
    QString standardInput = QString::fromUtf8(szStandardLayerDir);
    QString standardCsvPath = FindStandardCsvPath(standardInput);
    if (!standardCsvPath.isEmpty()) {
        SAFE_LOG_INFO("检测到标准图例CSV，启用CSV属性表匹配模式: {}", standardCsvPath.toStdString());
        SE_Error csvRet = LegendMatchCheckByCsv(
            standardCsvPath,
            szMapLayerPath,
            elementNaFilter,
            szOutputShpPath,
            QString::fromUtf8(szOutputJsonPath),
            bPassed);
        CleanupLogger();
        return csvRet;
    }

    // 1. 遍历文件夹查找匹配的标准图例文件
    QString standardDir = QString::fromUtf8(szStandardLayerDir);
    QDir legendDir(standardDir);
    if (!legendDir.exists()) {
        QString errMsg = QString::fromUtf8("标准图例文件夹不存在: ") + standardDir;
        SAFE_LOG_ERROR("{}", errMsg.toStdString());
        
        QJsonObject checkItem;
        checkItem["id"] = "105";
        checkItem["name"] = "标准图例匹配检查";
        checkItem["err_exist"] = "true";
        checkItem["status"] = "质检失败";
        checkItem["info"] = errMsg;
        
        QJsonObject resultJson;
        resultJson["check_item"] = checkItem;
        
        QFile outFile(QString::fromUtf8(szOutputJsonPath));
        if (outFile.open(QIODevice::WriteOnly)) {
            QTextStream out(&outFile);
            out.setCodec("UTF-8");
            out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
            outFile.close();
        }
        
        CleanupLogger();
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 遍历文件夹查找SHP文件
    QStringList filters;
    filters << "*.shp";
    QFileInfoList shpFiles = legendDir.entryInfoList(filters, QDir::Files);
    
    try { 
        SAFE_LOG_INFO("扫描标准图例文件夹，共找到 {} 个SHP文件", shpFiles.size()); 
    } catch(...) {}

    QString matchedLegendPath;
    for (const QFileInfo& fileInfo : shpFiles) {
        QString shpPath = fileInfo.absoluteFilePath();
        try { 
            SAFE_LOG_INFO("检查文件: {}", shpPath.toStdString()); 
        } catch(...) {}

        GDALDataset* tempDS = (GDALDataset*)GDALOpenEx(shpPath.toUtf8().constData(), GDAL_OF_VECTOR, NULL, NULL, NULL);
        if (tempDS == nullptr) {
            continue;
        }

        OGRLayer* tempLayer = tempDS->GetLayer(0);
        if (tempLayer == nullptr) {
            GDALClose(tempDS);
            continue;
        }

        // 检查是否包含element_na字段
        OGRFeatureDefn* layerDefn = tempLayer->GetLayerDefn();
        int elementNaIdx = layerDefn->GetFieldIndex("element_na");
        if (elementNaIdx < 0) {
            GDALClose(tempDS);
            continue;
        }

        // 查找匹配element_na的要素
        tempLayer->ResetReading();
        OGRFeature* feat = nullptr;
        bool foundMatch = false;
        
        // 支持逗号分隔的多个值（如"公路桥，铁路桥"）
        QStringList filterList;
        QString filterStr = elementNaFilter;
        // 同时支持中文逗号和英文逗号
        filterStr.replace("，", ",");
        if (filterStr.contains(",")) {
            // 如果包含逗号，拆分为多个值
            filterList = filterStr.split(",", QString::SkipEmptyParts);
            for (QString& s : filterList) {
                s = s.trimmed();
            }
        } else {
            filterList << filterStr.trimmed();
        }
        
        while ((feat = tempLayer->GetNextFeature()) != nullptr) {
            const char* elemNa = feat->GetFieldAsString(elementNaIdx);
            if (elemNa != nullptr) {
                QString elemNaStr = QString::fromUtf8(elemNa).trimmed();
                // 检查是否匹配任意一个值
                if (filterList.contains(elemNaStr)) {
                    foundMatch = true;
                    OGRFeature::DestroyFeature(feat);
                    break;
                }
            }
            OGRFeature::DestroyFeature(feat);
        }

        GDALClose(tempDS);

        if (foundMatch) {
            matchedLegendPath = shpPath;
            try { 
                SAFE_LOG_INFO("✓ 找到匹配的标准图例文件: {}", shpPath.toStdString()); 
            } catch(...) {}
            break;
        }
    }

    if (matchedLegendPath.isEmpty()) {
        QString errMsg = QString::fromUtf8("未找到element_na为 [") + elementNaFilter + QString::fromUtf8("] 的标准图例文件");
        SAFE_LOG_ERROR("{}", errMsg.toStdString());
        
        QJsonObject checkItem;
        checkItem["id"] = "105";
        checkItem["name"] = "标准图例匹配检查";
        checkItem["err_exist"] = "true";
        checkItem["status"] = "质检失败";
        checkItem["info"] = errMsg;
        
        QJsonObject resultJson;
        resultJson["check_item"] = checkItem;
        
        QFile outFile(QString::fromUtf8(szOutputJsonPath));
        if (outFile.open(QIODevice::WriteOnly)) {
            QTextStream out(&outFile);
            out.setCodec("UTF-8");
            out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
            outFile.close();
        }
        
        CleanupLogger();
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 打开匹配的标准图例文件
    pStandardDS = (GDALDataset*)GDALOpenEx(matchedLegendPath.toUtf8().constData(), GDAL_OF_VECTOR, NULL, NULL, NULL);
    if (pStandardDS == NULL) {
        QString errMsg = QString::fromUtf8("无法打开标准图例图层: ") + matchedLegendPath;
        SAFE_LOG_ERROR("{}", errMsg.toStdString());
        
        QJsonObject checkItem;
        checkItem["id"] = "105";
        checkItem["name"] = "标准图例匹配检查";
        checkItem["err_exist"] = "true";
        checkItem["status"] = "质检失败";
        checkItem["info"] = errMsg;
        
        QJsonObject resultJson;
        resultJson["check_item"] = checkItem;
        
        QFile outFile(QString::fromUtf8(szOutputJsonPath));
        if (outFile.open(QIODevice::WriteOnly)) {
            QTextStream out(&outFile);
            out.setCodec("UTF-8");
            out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
            outFile.close();
        }
        
        CleanupLogger();
        return SE_ERROR_FILE_NOT_FOUND;
    }

    OGRLayer* pStandardLayer = pStandardDS->GetLayer(0);
    if (pStandardLayer == NULL) {
        GDALClose(pStandardDS);
        
        QString errMsg = QString::fromUtf8("标准图例图层为空");
        SAFE_LOG_ERROR("{}", errMsg.toStdString());
        
        QJsonObject checkItem;
        checkItem["id"] = "105";
        checkItem["name"] = "标准图例匹配检查";
        checkItem["err_exist"] = "true";
        checkItem["status"] = "质检失败";
        checkItem["info"] = errMsg;
        
        QJsonObject resultJson;
        resultJson["check_item"] = checkItem;
        
        QFile outFile(QString::fromUtf8(szOutputJsonPath));
        if (outFile.open(QIODevice::WriteOnly)) {
            QTextStream out(&outFile);
            out.setCodec("UTF-8");
            out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
            outFile.close();
        }
        
        CleanupLogger();
        return SE_ERROR_LAYER_INVALID;
    }

    // 2. 打开待检查的地图图层
    pMapDS = (GDALDataset*)GDALOpenEx(szMapLayerPath, GDAL_OF_VECTOR, NULL, NULL, NULL);
    if (pMapDS == NULL) {
        GDALClose(pStandardDS);
        
        QString errMsg = QString::fromUtf8("无法打开地图图层: ") + QString::fromUtf8(szMapLayerPath);
        SAFE_LOG_ERROR("{}", errMsg.toStdString());
        
        QJsonObject checkItem;
        checkItem["id"] = "105";
        checkItem["name"] = "标准图例匹配检查";
        checkItem["err_exist"] = "true";
        checkItem["status"] = "质检失败";
        checkItem["info"] = errMsg;
        
        QJsonObject resultJson;
        resultJson["check_item"] = checkItem;
        
        QFile outFile(QString::fromUtf8(szOutputJsonPath));
        if (outFile.open(QIODevice::WriteOnly)) {
            QTextStream out(&outFile);
            out.setCodec("UTF-8");
            out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
            outFile.close();
        }
        
        CleanupLogger();
        return SE_ERROR_FILE_NOT_FOUND;
    }

    pMapLayer = pMapDS->GetLayer(0);
    if (pMapLayer == NULL) {
        GDALClose(pStandardDS);
        GDALClose(pMapDS);
        
        QString errMsg = QString::fromUtf8("地图图层为空");
        SAFE_LOG_ERROR("{}", errMsg.toStdString());
        
        QJsonObject checkItem;
        checkItem["id"] = "105";
        checkItem["name"] = "标准图例匹配检查";
        checkItem["err_exist"] = "true";
        checkItem["status"] = "质检失败";
        checkItem["info"] = errMsg;
        
        QJsonObject resultJson;
        resultJson["check_item"] = checkItem;
        
        QFile outFile(QString::fromUtf8(szOutputJsonPath));
        if (outFile.open(QIODevice::WriteOnly)) {
            QTextStream out(&outFile);
            out.setCodec("UTF-8");
            out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
            outFile.close();
        }
        
        CleanupLogger();
        return SE_ERROR_LAYER_INVALID;
    }

    // 3. 从标准图例中提取标准值（只取element_ty="path"的要素）
    pStandardLayer->ResetReading();
    
    // 字段索引
    OGRFeatureDefn* pStdLayerDefn = pStandardLayer->GetLayerDefn();
    int stdElementTyIdx = pStdLayerDefn->GetFieldIndex("element_ty");
    int stdElementNaIdx = pStdLayerDefn->GetFieldIndex("element_na");
    int stdClosedIdx = pStdLayerDefn->GetFieldIndex("closed");
    int stdFilledIdx = pStdLayerDefn->GetFieldIndex("filled");
    int stdStrokedIdx = pStdLayerDefn->GetFieldIndex("stroked");
    int stdStylePrioIdx = pStdLayerDefn->GetFieldIndex("style_prio");
    int stdStrokeColIdx = pStdLayerDefn->GetFieldIndex("stroke_col");
    int stdStrokeWidIdx = pStdLayerDefn->GetFieldIndex("stroke_wid");
    int stdFillColorIdx = pStdLayerDefn->GetFieldIndex("fill_color");
    int stdStrokeDasIdx = pStdLayerDefn->GetFieldIndex("stroke_das");
    int stdIsDashedIdx = pStdLayerDefn->GetFieldIndex("is_dashed");

    // 3. 收集标准图例中的所有匹配要素（支持复合符号）
    struct StandardPathFeature {
        QString closed;
        QString filled;
        QString stroked;
        QString stylePrio;
        QString strokeCol;
        QString strokeWid;
        QString fillColor;
        QString strokeDas;
        QString isDashed;
    };  // 添加分号
    
    // 拆分element_na过滤值（支持逗号分隔）
    QString filterStr = elementNaFilter;
    filterStr.replace("，", ",");
    QStringList elementNaList;
    if (filterStr.contains(",")) {
        elementNaList = filterStr.split(",", QString::SkipEmptyParts);
        for (QString& s : elementNaList) {
            s = s.trimmed();
        }
    } else {
        elementNaList << filterStr.trimmed();
    }
    
    QList<StandardPathFeature> standardFeatures;  // 存储所有标准path要素（非桥梁用）
    QMap<QString, QList<StandardPathFeature>> elementNaToFeaturesMap;  // 桥梁等多值情况，按element_na分组

    OGRFeature* pStdFeature = NULL;
    while ((pStdFeature = pStandardLayer->GetNextFeature()) != NULL) {
        if (stdElementTyIdx >= 0 && stdElementNaIdx >= 0) {
            const char* elementTy = pStdFeature->GetFieldAsString(stdElementTyIdx);
            const char* elementNa = pStdFeature->GetFieldAsString(stdElementNaIdx);
            QString elementTyStr = QString::fromUtf8(elementTy).trimmed();
            QString elementNaStr = QString::fromUtf8(elementNa).trimmed();
            
            // 只处理 element_ty="path" 且 element_na 在筛选列表中的要素
            if (elementTyStr == "path" && elementNaList.contains(elementNaStr)) {
                StandardPathFeature stdPath;
                
                if (stdClosedIdx >= 0) stdPath.closed = QString::fromUtf8(pStdFeature->GetFieldAsString(stdClosedIdx)).trimmed();
                if (stdFilledIdx >= 0) stdPath.filled = QString::fromUtf8(pStdFeature->GetFieldAsString(stdFilledIdx)).trimmed();
                if (stdStrokedIdx >= 0) stdPath.stroked = QString::fromUtf8(pStdFeature->GetFieldAsString(stdStrokedIdx)).trimmed();
                if (stdStylePrioIdx >= 0) stdPath.stylePrio = QString::fromUtf8(pStdFeature->GetFieldAsString(stdStylePrioIdx)).trimmed();
                if (stdStrokeColIdx >= 0) stdPath.strokeCol = QString::fromUtf8(pStdFeature->GetFieldAsString(stdStrokeColIdx)).trimmed();
                if (stdStrokeWidIdx >= 0) stdPath.strokeWid = QString::fromUtf8(pStdFeature->GetFieldAsString(stdStrokeWidIdx)).trimmed();
                if (stdFillColorIdx >= 0) stdPath.fillColor = QString::fromUtf8(pStdFeature->GetFieldAsString(stdFillColorIdx)).trimmed();
                if (stdStrokeDasIdx >= 0) stdPath.strokeDas = QString::fromUtf8(pStdFeature->GetFieldAsString(stdStrokeDasIdx)).trimmed();
                if (stdIsDashedIdx >= 0) stdPath.isDashed = QString::fromUtf8(pStdFeature->GetFieldAsString(stdIsDashedIdx)).trimmed();
                
                // 如果是多值情况（如桥梁），按element_na分组存储
                if (elementNaList.size() > 1) {
                    elementNaToFeaturesMap[elementNaStr].append(stdPath);
                } else {
                    standardFeatures.append(stdPath);
                }
            }
        }
        OGRFeature::DestroyFeature(pStdFeature);
    }

    // 检查是否找到标准特征（单值或多值情况）
    bool hasStandardFeatures = !standardFeatures.isEmpty() || !elementNaToFeaturesMap.isEmpty();
    
    if (!hasStandardFeatures) {
        GDALClose(pStandardDS);
        GDALClose(pMapDS);
        
        QString errMsg = QString::fromUtf8("标准图例中未找到element_ty='path'且element_na='") + elementNaFilter + QString::fromUtf8("'的要素");
        SAFE_LOG_ERROR("{}", errMsg.toStdString());
        
        QJsonObject checkItem;
        checkItem["id"] = "105";
        checkItem["name"] = "标准图例匹配检查";
        checkItem["err_exist"] = "true";
        checkItem["status"] = "质检失败";
        checkItem["info"] = errMsg;
        checkItem["element_na_filter"] = elementNaFilter;
        
        QJsonObject resultJson;
        resultJson["check_item"] = checkItem;
        
        QFile outFile(QString::fromUtf8(szOutputJsonPath));
        if (outFile.open(QIODevice::WriteOnly)) {
            QTextStream out(&outFile);
            out.setCodec("UTF-8");
            out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
            outFile.close();
        }
        
        CleanupLogger();
        return SE_ERROR_FAILURE;
    }

    try {
        int totalFeaturesCount = standardFeatures.size();
        for (const auto& list : elementNaToFeaturesMap) {
            totalFeaturesCount += list.size();
        }
        SAFE_LOG_INFO("✓ 标准图例中找到 {} 个组成部分", totalFeaturesCount);
    } catch(...) {}

    // 判断是否是晕带图层，只对晕带做特殊处理
    bool isHaloBand = (QString::fromUtf8(szElementNaFilter).trimmed() == "晕带");
    
    // 判断是否是河流图层，河流的stroke_wid需要特殊处理（pt转mm，渐变范围0.1-0.6mm）
    bool isRiver = (QString::fromUtf8(szElementNaFilter).trimmed() == "河流");
    // 渠道/干渠/支渠线宽按0.15-0.3mm范围检查。
    QString elementFilterName = QString::fromUtf8(szElementNaFilter).trimmed();
    bool isCanal = (elementFilterName == "渠道" || elementFilterName == "干渠" ||
                    elementFilterName == "总干渠" || elementFilterName == "支渠");
    // 判断是否是自然村，用于添加额外诊断日志
    bool isNaturalVillage = (QString::fromUtf8(szElementNaFilter).trimmed() == "自然村");
    
    // 判断是否是多值情况（如桥梁：element_na包含逗号，需要根据symbol_nam字段区分）
    bool isMultiValue = (elementNaList.size() > 1);
    
    // 收集标准图层中所有的fill_color值（仅用于晕带等多颜色符号的特殊判断）
    QSet<QString> standardFillColors;
    if (isHaloBand) {
        for (const StandardPathFeature& stdPath : standardFeatures) {
            if (!stdPath.fillColor.isEmpty()) {
                // 归一化为小写以进行不区分大小写的匹配
                standardFillColors.insert(stdPath.fillColor.trimmed().toLower());
            }
        }
    }

    // 辅助：不区分大小写比较和归一化字符串
    auto eqCI = [](const QString& a, const QString& b)->bool {
        return a.compare(b, Qt::CaseInsensitive) == 0;
    };
    auto norm = [](const QString& s)->QString {
        return s.trimmed();
    };

    // 4. 检查地图图层中的path要素（支持复合符号组匹配）
    pMapLayer->ResetReading();
    
    OGRFeatureDefn* pMapLayerDefn = pMapLayer->GetLayerDefn();
    int mapElementTyIdx = pMapLayerDefn->GetFieldIndex("element_ty");
    int mapSymbolNamIdx = pMapLayerDefn->GetFieldIndex("symbol_nam");
    int mapClosedIdx = pMapLayerDefn->GetFieldIndex("closed");
    int mapFilledIdx = pMapLayerDefn->GetFieldIndex("filled");
    int mapStrokedIdx = pMapLayerDefn->GetFieldIndex("stroked");
    int mapStylePrioIdx = pMapLayerDefn->GetFieldIndex("style_prio");
    int mapStrokeColIdx = pMapLayerDefn->GetFieldIndex("stroke_col");
    int mapStrokeWidIdx = pMapLayerDefn->GetFieldIndex("stroke_wid");
    int mapFillColorIdx = pMapLayerDefn->GetFieldIndex("fill_color");
    int mapStrokeDasIdx = pMapLayerDefn->GetFieldIndex("stroke_das");
    int mapIsDashedIdx = pMapLayerDefn->GetFieldIndex("is_dashed");

    int standardPartCount = 0;  // 标准符号的组成部分数量（单值情况）
    if (!isMultiValue && !standardFeatures.isEmpty()) {
        standardPartCount = standardFeatures.size();
    }
    
    int totalSymbols = 0;      // 符号总数（组数）
    int matchedSymbols = 0;    // 匹配的符号数
    QJsonArray errorArray;
    
    // 收集所有地图中的path要素
    QList<OGRFeature*> mapPathFeatures;
    QMap<QString, QList<OGRFeature*>> bridgeMapFeaturesMap;  // 桥梁按symbol_nam分组
    
    OGRFeature* pMapFeature = NULL;
    while ((pMapFeature = pMapLayer->GetNextFeature()) != NULL) {
        if (mapElementTyIdx >= 0) {
            const char* elementTy = pMapFeature->GetFieldAsString(mapElementTyIdx);
            QString elementTyStr = QString::fromUtf8(elementTy).trimmed();
            
            if (elementTyStr == "path") {
                if (isMultiValue && mapSymbolNamIdx >= 0) {
                    // 多值特殊处理（如桥梁）：按symbol_nam分组
                    const char* symbolNam = pMapFeature->GetFieldAsString(mapSymbolNamIdx);
                    QString symbolNamStr = QString::fromUtf8(symbolNam).trimmed();
                    bridgeMapFeaturesMap[symbolNamStr].append(pMapFeature);
                } else {
                    mapPathFeatures.append(pMapFeature);
                }
            } else {
                OGRFeature::DestroyFeature(pMapFeature);
            }
        } else {
            OGRFeature::DestroyFeature(pMapFeature);
        }
    }
    
    int totalMapFeatures = mapPathFeatures.size();
    
    // 多值特殊处理（如桥梁）：分别检查每个symbol_nam组
    if (isMultiValue && !bridgeMapFeaturesMap.isEmpty()) {
        SAFE_LOG_INFO("多值模式：找到 {} 个不同的symbol_nam组", bridgeMapFeaturesMap.size());
        
        for (auto it = bridgeMapFeaturesMap.begin(); it != bridgeMapFeaturesMap.end(); ++it) {
            QString symbolNam = it.key();
            QList<OGRFeature*>& features = it.value();
            
            if (symbolNam.isEmpty()) {
                SAFE_LOG_WARN("跳过空symbol_nam的 {} 个要素", features.size());
                for (OGRFeature* f : features) {
                    OGRFeature::DestroyFeature(f);
                }
                continue;
            }
            
            SAFE_LOG_INFO("处理symbol_nam='{}': {} 个要素", symbolNam.toStdString(), features.size());
            
            // 查找对应的标准特征列表（从elementNaToFeaturesMap中获取）
            if (!elementNaToFeaturesMap.contains(symbolNam)) {
                // 没有对应的标准图例，记录错误
                QString errorDesc = QString("symbol_nam='%1'在标准图例中未找到对应的element_na特征").arg(symbolNam);
                SAFE_LOG_ERROR("{}", errorDesc.toStdString());
                
                QJsonObject singleError;
                singleError[QString("error%1").arg(errorArray.size() + 1)] = errorDesc;
                errorArray.append(singleError);
                
                // 清理要素
                for (OGRFeature* f : features) {
                    OGRFeature::DestroyFeature(f);
                }
                continue;
            }
            
            QList<StandardPathFeature> currentStdFeatures = elementNaToFeaturesMap[symbolNam];
            int currentStdPartCount = currentStdFeatures.size();
            
            if (currentStdPartCount == 0) {
                SAFE_LOG_ERROR("symbol_nam='{}' 的标准特征列表为空", symbolNam.toStdString());
                for (OGRFeature* f : features) {
                    OGRFeature::DestroyFeature(f);
                }
                continue;
            }
            
            int currentTotalSymbols = features.size() / currentStdPartCount;
            SAFE_LOG_INFO("标准部分数:{}, 符号组数:{}", currentStdPartCount, currentTotalSymbols);
            totalSymbols += currentTotalSymbols;
            
            // 检查当前symbol_nam的要素组（复用字段检查逻辑）
            for (int groupIdx = 0; groupIdx < currentTotalSymbols; groupIdx++) {
                bool groupMatched = true;
                QStringList groupErrorMessages;
                
                for (int partIdx = 0; partIdx < currentStdPartCount; partIdx++) {
                    int featureIdx = groupIdx * currentStdPartCount + partIdx;
                    if (featureIdx >= features.size()) break;
                    
                    OGRFeature* pCurrentFeature = features[featureIdx];
                    const StandardPathFeature& stdPath = currentStdFeatures[partIdx];
                    
                    bool partMatched = true;
                    QStringList errorFields;
                    QString errorMsg;

                    // 以下字段检查逻辑与主流程相同
                    // 注释掉closed字段检查
                    /*
                    if (mapClosedIdx >= 0 && !stdPath.closed.isEmpty()) {
                        QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapClosedIdx)).trimmed();
                        if (!mapValue.isEmpty() && mapValue != stdPath.closed) {
                            partMatched = false;
                            errorFields.append("closed");
                            errorMsg += QString("closed不匹配(标准:%1,实际:%2) ").arg(stdPath.closed).arg(mapValue);
                        }
                    }
                    */

                    // 注释掉filled字段检查
                    /*
                    if (mapFilledIdx >= 0 && !stdPath.filled.isEmpty()) {
                        QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapFilledIdx)).trimmed();
                        if (!mapValue.isEmpty() && mapValue != stdPath.filled) {
                            partMatched = false;
                            errorFields.append("filled");
                            errorMsg += QString("filled不匹配(标准:%1,实际:%2) ").arg(stdPath.filled).arg(mapValue);
                        }
                    }
                    */

                    if (mapStrokedIdx >= 0 && !stdPath.stroked.isEmpty()) {
                        QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapStrokedIdx)).trimmed();
                        if (!mapValue.isEmpty() && mapValue.compare(stdPath.stroked, Qt::CaseInsensitive) != 0) {
                            partMatched = false;
                            errorFields.append("stroked");
                            errorMsg += QString("stroked不匹配(标准:%1,实际:%2) ").arg(stdPath.stroked).arg(mapValue);
                        }
                    }

                    // 注释掉style_prio字段检查
                    /*
                    if (mapStylePrioIdx >= 0 && !stdPath.stylePrio.isEmpty()) {
                        QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapStylePrioIdx)).trimmed();
                        if (!mapValue.isEmpty() && mapValue != stdPath.stylePrio) {
                            partMatched = false;
                            errorFields.append("style_prio");
                            errorMsg += QString("style_prio不匹配(标准:%1,实际:%2) ").arg(stdPath.stylePrio).arg(mapValue);
                        }
                    }
                    */

                    if (mapStrokeColIdx >= 0 && !stdPath.strokeCol.isEmpty()) {
                        QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapStrokeColIdx)).trimmed();
                        if (!mapValue.isEmpty() && mapValue.compare(stdPath.strokeCol, Qt::CaseInsensitive) != 0) {
                            partMatched = false;
                            errorFields.append("stroke_col");
                            errorMsg += QString("stroke_col不匹配(标准:%1,实际:%2) ").arg(stdPath.strokeCol).arg(mapValue);
                        }
                    }

                    if (mapStrokeWidIdx >= 0 && !stdPath.strokeWid.isEmpty()) {
                        QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapStrokeWidIdx)).trimmed();
                        if (!mapValue.isEmpty()) {
                            bool ok1, ok2;
                            double stdValue = stdPath.strokeWid.toDouble(&ok1);
                            double mapValueNum = mapValue.toDouble(&ok2);
                            if (ok1 && ok2) {
                                if (std::fabs(stdValue - mapValueNum) > 0.01) {
                                    partMatched = false;
                                    errorFields.append("stroke_wid");
                                    errorMsg += QString("stroke_wid不匹配(标准:%1,实际:%2) ").arg(stdPath.strokeWid).arg(mapValue);
                                }
                            } else if (mapValue.compare(stdPath.strokeWid, Qt::CaseInsensitive) != 0) {
                                partMatched = false;
                                errorFields.append("stroke_wid");
                                errorMsg += QString("stroke_wid不匹配(标准:%1,实际:%2) ").arg(stdPath.strokeWid).arg(mapValue);
                            }
                        }
                    }

                    if (mapFillColorIdx >= 0 && !stdPath.fillColor.isEmpty()) {
                        QString mapFillColor = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapFillColorIdx)).trimmed();
                        if (!mapFillColor.isEmpty() && mapFillColor.compare(stdPath.fillColor, Qt::CaseInsensitive) != 0) {
                            partMatched = false;
                            errorFields.append("fill_color");
                            errorMsg += QString("fill_color不匹配(标准:%1,实际:%2) ").arg(stdPath.fillColor).arg(mapFillColor);
                        }
                    }

                    if (kCheckStrokeDas && mapStrokeDasIdx >= 0 && !stdPath.strokeDas.isEmpty()) {
                        QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapStrokeDasIdx)).trimmed();
                        if (!mapValue.isEmpty() && mapValue.compare(stdPath.strokeDas, Qt::CaseInsensitive) != 0) {
                            partMatched = false;
                            errorFields.append("stroke_das");
                            errorMsg += QString("stroke_das不匹配(标准:%1,实际:%2) ").arg(stdPath.strokeDas).arg(mapValue);
                        }
                    }

                    if (mapIsDashedIdx >= 0 && !stdPath.isDashed.isEmpty()) {
                        QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapIsDashedIdx)).trimmed();
                        if (!mapValue.isEmpty() && mapValue.compare(stdPath.isDashed, Qt::CaseInsensitive) != 0) {
                            partMatched = false;
                            errorFields.append("is_dashed");
                            errorMsg += QString("is_dashed不匹配(标准:%1,实际:%2) ").arg(stdPath.isDashed).arg(mapValue);
                        }
                    }

                    if (!partMatched) {
                        groupMatched = false;
                        QString partErrorMsg = QString("第%1部分(FID=%2,symbol_nam=%3)的%4字段不匹配: %5")
                            .arg(partIdx + 1)
                            .arg(pCurrentFeature->GetFID())
                            .arg(symbolNam)
                            .arg(errorFields.join(","))
                            .arg(errorMsg.trimmed());
                        groupErrorMessages.append(partErrorMsg);
                        if (isNaturalVillage) {
                            try { SAFE_LOG_INFO("自然村不匹配细节: {}", partErrorMsg.toStdString()); } catch(...) {}
                        }
                    }
                }
                
                if (groupMatched) {
                    matchedSymbols++;
                } else {
                    QString errorKey = QString("error%1").arg(errorArray.size() + 1);
                    QString errorDesc = QString("符号组%1(symbol_nam=%2)不匹配: %3")
                        .arg(groupIdx + 1)
                        .arg(symbolNam)
                        .arg(groupErrorMessages.join("; "));
                    QJsonObject singleError;
                    singleError[errorKey] = errorDesc;
                    errorArray.append(singleError);
                }
            }
            
            // 清理要素
            for (OGRFeature* f : features) {
                OGRFeature::DestroyFeature(f);
            }
        }
        
        totalMapFeatures = 0;  // 桥梁已经单独处理，避免重复
    }
    
    // 单值模式：计算符号组数（向下取整）
    if (!isMultiValue && standardPartCount > 0) {
        totalSymbols = totalMapFeatures / standardPartCount;
        
        // 如果不是整数倍，只处理完整的组，剩余的要素不检查
        int remainingFeatures = totalMapFeatures % standardPartCount;
        if (remainingFeatures > 0) {
            SAFE_LOG_WARN("地图要素数({})不是标准部分数({})的整数倍，剩余{}个要素将被忽略", 
                totalMapFeatures, standardPartCount, remainingFeatures);
        }
    } else if (!isMultiValue) {
        SAFE_LOG_ERROR("标准符号部分数为0，无法进行检查");
    }
    
    // 按组检查（每standardPartCount个要素为一组）
    for (int groupIdx = 0; groupIdx < totalSymbols; groupIdx++) {
        bool groupMatched = true;
        QStringList groupErrorMessages;
        
        // 检查当前组的每个部分
        for (int partIdx = 0; partIdx < standardPartCount; partIdx++) {
            int featureIdx = groupIdx * standardPartCount + partIdx;
            OGRFeature* pCurrentFeature = mapPathFeatures[featureIdx];
            const StandardPathFeature& stdPath = standardFeatures[partIdx];
            
            bool partMatched = true;
            QStringList errorFields;
            QString errorMsg;

            // 注释掉closed字段检查
            /*
            // 检查closed字段
            if (mapClosedIdx >= 0 && !stdPath.closed.isEmpty()) {
                QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapClosedIdx)).trimmed();
                if (!mapValue.isEmpty() && mapValue != stdPath.closed) {
                    partMatched = false;
                    errorFields.append("closed");
                    errorMsg += QString("closed不匹配(标准:%1,实际:%2) ").arg(stdPath.closed).arg(mapValue);
                }
            }
            */

            // 注释掉filled字段检查
            /*
            // 检查filled字段
            if (mapFilledIdx >= 0 && !stdPath.filled.isEmpty()) {
                QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapFilledIdx)).trimmed();
                if (!mapValue.isEmpty() && mapValue != stdPath.filled) {
                    partMatched = false;
                    errorFields.append("filled");
                    errorMsg += QString("filled不匹配(标准:%1,实际:%2) ").arg(stdPath.filled).arg(mapValue);
                }
            }
            */

            // 检查stroked字段
            if (mapStrokedIdx >= 0 && !stdPath.stroked.isEmpty()) {
                QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapStrokedIdx)).trimmed();
                if (!mapValue.isEmpty() && mapValue.compare(stdPath.stroked, Qt::CaseInsensitive) != 0) {
                    partMatched = false;
                    errorFields.append("stroked");
                    errorMsg += QString("stroked不匹配(标准:%1,实际:%2) ").arg(stdPath.stroked).arg(mapValue);
                }
            }

            // 注释掉style_prio字段检查
            /*
            // 检查style_prio字段
            if (mapStylePrioIdx >= 0 && !stdPath.stylePrio.isEmpty()) {
                QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapStylePrioIdx)).trimmed();
                if (!mapValue.isEmpty() && mapValue != stdPath.stylePrio) {
                    partMatched = false;
                    errorFields.append("style_prio");
                    errorMsg += QString("style_prio不匹配(标准:%1,实际:%2) ").arg(stdPath.stylePrio).arg(mapValue);
                }
            }
            */

            // 检查stroke_col字段
            if (mapStrokeColIdx >= 0 && !stdPath.strokeCol.isEmpty()) {
                QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapStrokeColIdx)).trimmed();
                if (!mapValue.isEmpty() && mapValue.compare(stdPath.strokeCol, Qt::CaseInsensitive) != 0) {
                    partMatched = false;
                    errorFields.append("stroke_col");
                    errorMsg += QString("stroke_col不匹配(标准:%1,实际:%2) ").arg(stdPath.strokeCol).arg(mapValue);
                }
            }

            // 检查stroke_wid字段（数值比较，河流/渠道使用图上mm范围）
            if (mapStrokeWidIdx >= 0 && !stdPath.strokeWid.isEmpty()) {
                QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapStrokeWidIdx)).trimmed();
                if (!mapValue.isEmpty()) {
                    bool ok1, ok2;
                    double stdValue = stdPath.strokeWid.toDouble(&ok1);
                    double mapValueNum = mapValue.toDouble(&ok2);
                    
                    if (ok1 && ok2) {
                        if (isRiver || isCanal) {
                            // 河流/渠道特殊处理：pt转mm（1pt ≈ 0.3527778mm），按图上mm范围检查
                            double mapValueMm = mapValueNum * 0.3527778;  // pt转mm
                            const double minMm = isRiver ? 0.1 : 0.15;
                            const double maxMm = isRiver ? 0.6 : 0.3;
                            const QString requiredRange = isRiver ? "0.1-0.6mm" : "0.15-0.3mm";
                            
                            if (mapValueMm < minMm || mapValueMm > maxMm) {
                                partMatched = false;
                                errorFields.append("stroke_wid");
                                errorMsg += QString("stroke_wid超出范围(要求:%1,实际:%2pt≈%3mm) ")
                                    .arg(requiredRange)
                                    .arg(mapValue)
                                    .arg(QString::number(mapValueMm, 'f', 2));
                            }
                        } else {
                            // 其他图层：四舍五入比较
                            if (std::fabs(stdValue - mapValueNum) > 0.01) {
                                partMatched = false;
                                errorFields.append("stroke_wid");
                                errorMsg += QString("stroke_wid不匹配(标准:%1,实际:%2) ").arg(stdPath.strokeWid).arg(mapValue);
                            }
                        }
                    } else if (mapValue.compare(stdPath.strokeWid, Qt::CaseInsensitive) != 0) {
                        partMatched = false;
                        errorFields.append("stroke_wid");
                        errorMsg += QString("stroke_wid不匹配(标准:%1,实际:%2) ").arg(stdPath.strokeWid).arg(mapValue);
                    }
                }
            }

            // 检查fill_color字段（晕带使用多值匹配，其他图层使用单值匹配）
            if (mapFillColorIdx >= 0 && !stdPath.fillColor.isEmpty()) {
                QString mapFillColor = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapFillColorIdx)).trimmed();
                if (!mapFillColor.isEmpty()) {
                    bool colorMatched = false;
                    QString mapFillLower = mapFillColor.toLower();
                    QString stdFillLower = stdPath.fillColor.trimmed().toLower();
                    
                    if (isHaloBand) {
                        // 晕带特殊处理：只要实际颜色在标准图层的任意fill_color值中就算匹配
                        colorMatched = standardFillColors.contains(mapFillLower);
                    } else {
                        // 其他图层：不区分大小写匹配
                        colorMatched = (mapFillLower == stdFillLower);
                    }
                    
                    if (!colorMatched) {
                        partMatched = false;
                        errorFields.append("fill_color");
                        errorMsg += QString("fill_color不匹配(标准:%1,实际:%2) ").arg(stdPath.fillColor).arg(mapFillColor);
                    }
                }
            }

            // 检查stroke_das字段
            if (kCheckStrokeDas && mapStrokeDasIdx >= 0 && !stdPath.strokeDas.isEmpty()) {
                QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapStrokeDasIdx)).trimmed();
                if (!mapValue.isEmpty() && mapValue.compare(stdPath.strokeDas, Qt::CaseInsensitive) != 0) {
                    partMatched = false;
                    errorFields.append("stroke_das");
                    errorMsg += QString("stroke_das不匹配(标准:%1,实际:%2) ").arg(stdPath.strokeDas).arg(mapValue);
                }
            }

            // 检查is_dashed字段
            if (mapIsDashedIdx >= 0 && !stdPath.isDashed.isEmpty()) {
                QString mapValue = QString::fromUtf8(pCurrentFeature->GetFieldAsString(mapIsDashedIdx)).trimmed();
                if (!mapValue.isEmpty() && mapValue.compare(stdPath.isDashed, Qt::CaseInsensitive) != 0) {
                    partMatched = false;
                    errorFields.append("is_dashed");
                    errorMsg += QString("is_dashed不匹配(标准:%1,实际:%2) ").arg(stdPath.isDashed).arg(mapValue);
                }
            }

            // 如果当前部分不匹配，记录到组错误信息
            if (!partMatched) {
                groupMatched = false;
                QString partErrorMsg = QString("第%1部分(FID=%2)的%3字段不匹配: %4")
                    .arg(partIdx + 1)
                    .arg(pCurrentFeature->GetFID())
                    .arg(errorFields.join(","))
                    .arg(errorMsg.trimmed());
                groupErrorMessages.append(partErrorMsg);
                if (isNaturalVillage) {
                    try { SAFE_LOG_INFO("自然村不匹配细节: {}", partErrorMsg.toStdString()); } catch(...) {}
                }
            }
        }
        
        // 判断当前组是否匹配
        if (groupMatched) {
            matchedSymbols++;
        } else {
            // 构建错误对象
            QString errorKey = QString("error%1").arg(errorArray.size() + 1);
            QString errorDesc = QString("符号组%1(FID范围:%2-%3)不匹配: %4")
                .arg(groupIdx + 1)
                .arg(mapPathFeatures[groupIdx * standardPartCount]->GetFID())
                .arg(mapPathFeatures[(groupIdx + 1) * standardPartCount - 1]->GetFID())
                .arg(groupErrorMessages.join("; "));
            
            QJsonObject singleError;
            singleError[errorKey] = errorDesc;
            errorArray.append(singleError);
        }
    }
    
    // 清理地图要素
    for (OGRFeature* feature : mapPathFeatures) {
        OGRFeature::DestroyFeature(feature);
    }
    
    // 5. 生成JSON结果
    QJsonObject checkItem;
    
    // 基本信息
    checkItem["id"] = "105";
    checkItem["name"] = "标准图例匹配检查";
    
    if (totalSymbols == 0) {
        checkItem["err_exist"] = "true";
        checkItem["status"] = "质检失败";
        checkItem["info"] = QString("地图图层中未找到element_ty='path'的要素");
        bPassed = false;
    } else if (matchedSymbols == totalSymbols) {
        checkItem["err_exist"] = "false";
        checkItem["status"] = "质检完成";
        checkItem["info"] = QString("%1图层不存在图例匹配错误").arg(elementNaFilter);
        bPassed = true;
    } else {
        checkItem["err_exist"] = "true";
        checkItem["status"] = "质检完成";
        checkItem["info"] = QString("%1图层存在图例匹配错误").arg(elementNaFilter);
        
        // 构建errors对象
        QJsonObject errorsObj;
        for (int i = 0; i < errorArray.size(); ++i) {
            QString errorKey = QString("error%1").arg(i + 1);
            QJsonObject errorItem = errorArray[i].toObject();
            if (errorItem.contains(errorKey)) {
                errorsObj[errorKey] = errorItem[errorKey].toString();
            }
        }
        checkItem["errors"] = errorsObj;
        
        bPassed = false;
    }

    // 构建最终JSON结构
    QJsonObject resultJson;
    resultJson["check_item"] = checkItem;

    // 6. 写入JSON文件
    QFile outFile(QString::fromUtf8(szOutputJsonPath));
    if (!outFile.open(QIODevice::WriteOnly)) {
        SAFE_LOG_ERROR("无法写入JSON文件: {}", QString::fromUtf8(szOutputJsonPath).toStdString());
        GDALClose(pStandardDS);
        GDALClose(pMapDS);
        CleanupLogger();
        return SE_ERROR_FAILURE;
    }

    QTextStream out(&outFile);
    out.setCodec("UTF-8");
    out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
    outFile.close();

    // 7. 记录检查结果到日志
    try {
        SAFE_LOG_INFO("=== 检查结果 ===");
        SAFE_LOG_INFO("标准符号部分数: {}", standardPartCount);
        SAFE_LOG_INFO("地图中符号组总数: {}", totalSymbols);
        SAFE_LOG_INFO("匹配的符号组数: {}", matchedSymbols);
        if (bPassed) {
            SAFE_LOG_INFO("✓ 检查通过：所有符号组均匹配标准图例");
        } else {
            SAFE_LOG_WARN("✗ 检查未通过：{} 个符号组不匹配", totalSymbols - matchedSymbols);
            for (int i = 0; i < errorArray.size(); ++i) {
                QJsonObject errorObj = errorArray[i].toObject();
                QString errorKey = QString("error%1").arg(i + 1);
                if (errorObj.contains(errorKey)) {
                    QString errorDesc = errorObj[errorKey].toString();
                    SAFE_LOG_WARN("  - {}", errorDesc.toStdString());
                }
            }
        }
    } catch (...) {}

    // 8. 输出错误要素Shapefile
    if (!bPassed && szOutputShpPath != nullptr && strlen(szOutputShpPath) > 0) {
        SAFE_LOG_INFO("=== 输出错误Shapefile ===");
        SAFE_LOG_INFO("输出路径: {}", QString::fromUtf8(szOutputShpPath).toStdString());

        // 重新读取地图图层以输出错误要素
        pMapLayer->ResetReading();
        QMap<GIntBig, QString> errorInfoMap;  // FID -> 错误详情

        // 重新收集map要素用于输出
        QList<OGRFeature*> mapPathFeaturesForOutput;
        OGRFeature* pMapFeat = NULL;
        while ((pMapFeat = pMapLayer->GetNextFeature()) != NULL) {
            if (mapElementTyIdx >= 0) {
                const char* elementTy = pMapFeat->GetFieldAsString(mapElementTyIdx);
                QString elementTyStr = QString::fromUtf8(elementTy).trimmed();
                
                if (elementTyStr == "path") {
                    mapPathFeaturesForOutput.append(pMapFeat);
                } else {
                    OGRFeature::DestroyFeature(pMapFeat);
                }
            } else {
                OGRFeature::DestroyFeature(pMapFeat);
            }
        }

        // 根据errorArray构建错误信息映射
        for (int i = 0; i < errorArray.size(); ++i) {
            QJsonObject errorObj = errorArray[i].toObject();
            QString errorKey = QString("error%1").arg(i + 1);
            if (errorObj.contains(errorKey)) {
                QString errorDesc = errorObj[errorKey].toString();
                
                // 解析错误描述，提取符号组索引和FID范围
                // 格式: "符号组X(FID范围:A-B)不匹配: ..." 或 "符号组X(symbol_nam=Y)不匹配: ..."
                QRegExp rx1("符号组(\\d+)\\(FID范围:(\\d+)-(\\d+)\\)不匹配: (.+)");
                QRegExp rx2("符号组(\\d+)\\(symbol_nam=(.+?)\\)不匹配: (.+)");
                
                if (rx1.indexIn(errorDesc) != -1) {
                    int groupIdx = rx1.cap(1).toInt() - 1;  // 转为0-based索引
                    GIntBig fidStart = rx1.cap(2).toLongLong();
                    GIntBig fidEnd = rx1.cap(3).toLongLong();
                    QString errorDetail = rx1.cap(4);

                    // 为该组的所有要素添加错误信息
                    for (int partIdx = 0; partIdx < standardPartCount; partIdx++) {
                        int featureIdx = groupIdx * standardPartCount + partIdx;
                        if (featureIdx < mapPathFeaturesForOutput.size()) {
                            GIntBig fid = mapPathFeaturesForOutput[featureIdx]->GetFID();
                            errorInfoMap[fid] = QString("符号组%1 第%2部分: %3")
                                .arg(groupIdx + 1)
                                .arg(partIdx + 1)
                                .arg(errorDetail);
                        }
                    }
                } else if (rx2.indexIn(errorDesc) != -1) {
                    // 桥梁格式：包含symbol_nam
                    int groupIdx = rx2.cap(1).toInt() - 1;
                    QString symbolNam = rx2.cap(2);
                    QString errorDetail = rx2.cap(3);
                    
                    // 由于无法确定FID范围，记录警告
                    SAFE_LOG_WARN("无法为symbol_nam='{}' 的错误映射到具体FID", symbolNam.toStdString());
                }
            }
        }

        // 创建输出Shapefile
        GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
        if (poDriver != nullptr) {
            CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");
            
            char** options = nullptr;
            options = CSLSetNameValue(options, "ENCODING", "UTF-8");
            
            GDALDataset* poOutDS = poDriver->Create(szOutputShpPath, 0, 0, 0, GDT_Unknown, options);
            CSLDestroy(options);
            
            if (poOutDS != nullptr) {
                char** layerOptions = nullptr;
                layerOptions = CSLSetNameValue(layerOptions, "ENCODING", "UTF-8");

                OGRLayer* poOutLayer = poOutDS->CreateLayer(
                    "error_features",
                    pMapLayer->GetSpatialRef(),
                    pMapLayer->GetGeomType(),
                    layerOptions
                );
                CSLDestroy(layerOptions);

                if (poOutLayer != nullptr) {
                    // 复制原始字段
                    OGRFeatureDefn* mapDefn = pMapLayer->GetLayerDefn();
                    for (int i = 0; i < mapDefn->GetFieldCount(); i++) {
                        OGRFieldDefn* fieldDefn = mapDefn->GetFieldDefn(i);
                        poOutLayer->CreateField(fieldDefn);
                    }

                    // 添加错误字段
                    OGRFieldDefn hasErrField("HAS_ERR", OFTString);
                    hasErrField.SetWidth(10);
                    poOutLayer->CreateField(&hasErrField);

                    OGRFieldDefn errDetailField("ERR_DETAIL", OFTString);
                    errDetailField.SetWidth(254);
                    poOutLayer->CreateField(&errDetailField);

                    // 写入错误要素
                    int outputCount = 0;
                    for (OGRFeature* pFeat : mapPathFeaturesForOutput) {
                        GIntBig fid = pFeat->GetFID();
                        if (errorInfoMap.contains(fid)) {
                            OGRFeature* poOutFeature = OGRFeature::CreateFeature(poOutLayer->GetLayerDefn());
                            
                            // 复制几何
                            poOutFeature->SetGeometry(pFeat->GetGeometryRef());
                            
                            // 复制原始字段
                            for (int i = 0; i < mapDefn->GetFieldCount(); i++) {
                                poOutFeature->SetField(i, pFeat->GetRawFieldRef(i));
                            }
                            
                            // 设置错误字段
                            poOutFeature->SetField("HAS_ERR", "true");
                            QString errDetail = errorInfoMap[fid];
                            QByteArray detailBytes = errDetail.toUtf8();
                            poOutFeature->SetField("ERR_DETAIL", detailBytes.constData());
                            
                            if (poOutLayer->CreateFeature(poOutFeature) == OGRERR_NONE) {
                                outputCount++;
                            }
                            
                            OGRFeature::DestroyFeature(poOutFeature);
                        }
                    }

                    SAFE_LOG_INFO("✓ 成功输出 {} 个错误要素到Shapefile", outputCount);
                }
                
                GDALClose(poOutDS);
            }
        }

        // 清理输出用的要素列表
        for (OGRFeature* feature : mapPathFeaturesForOutput) {
            OGRFeature::DestroyFeature(feature);
        }
    }

    // 9. 清理资源
    SAFE_LOG_INFO("=== 检查完成 ===");

    CleanupLogger();
    GDALClose(pStandardDS);
    GDALClose(pMapDS);

    return SE_ERROR_NONE;
}
