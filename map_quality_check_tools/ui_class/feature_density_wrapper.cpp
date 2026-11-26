#include "map_check_algorithms.h"
#include "map_check_task_manager.h"
#include "json_result_parser.h"
#include "../algorithm/se_feature_density_check.h"

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <ogrsf_frmts.h>

namespace {

QString resolveDensityBoundaryPath(const QString& shpDirPath)
{
    const QDir shpDir(shpDirPath);
    const QStringList candidateNames = {
        QStringLiteral("内图廓.shp"),
        QStringLiteral("图廓.shp"),
        QStringLiteral("图廓_Polygon.shp")
    };

    for (const QString& candidateName : candidateNames) {
        const QString candidatePath = shpDir.filePath(candidateName);
        if (QFileInfo::exists(candidatePath)) return candidatePath;
    }

    // 保留旧路径作为缺失时的默认值，便于输出明确的诊断路径。
    return shpDir.filePath(candidateNames.first());
}

void failDensity(const QString& name, const QString& jsonPath,
                 const QString& reason, CheckItemResult& result)
{
    result.id = QStringLiteral("121");
    result.name = name;
    result.status = QStringLiteral("质检失败");
    result.err_exist = true;
    result.info = QJsonObject{{QStringLiteral("description"), reason}};
    result.errors = QJsonObject{{QStringLiteral("error1"), reason}};
    const QJsonObject item{{QStringLiteral("id"), result.id},
                           {QStringLiteral("name"), result.name},
                           {QStringLiteral("status"), result.status},
                           {QStringLiteral("err_exist"), true},
                           {QStringLiteral("info"), reason},
                           {QStringLiteral("errors"), result.errors}};
    QFile file(jsonPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(QJsonObject{{QStringLiteral("check_item"), item}})
                       .toJson(QJsonDocument::Indented));
}

struct DensityInputs {
    QString type;
    bool source = false;
    bool mainArea = false;
};

bool readDensityRules(const QString& xmlPath, int scale, DensityInputs& inputs, QString& error)
{
    QFile file(xmlPath);
    QDomDocument doc;
    if (xmlPath.isEmpty() || !file.open(QIODevice::ReadOnly) || !doc.setContent(&file)) {
        error = QStringLiteral("无法读取121规则 XML：%1").arg(xmlPath);
        return false;
    }
    if (doc.documentElement().tagName() != QStringLiteral("check_list")) {
        error = QStringLiteral("121规则 XML 根节点不是 check_list：%1").arg(xmlPath);
        return false;
    }
    QList<QDomElement> exact, generic;
    const QDomNodeList scales = doc.documentElement().elementsByTagName(QStringLiteral("scale"));
    for (int i = 0; i < scales.size(); ++i) {
        const QDomElement scaleElement = scales.at(i).toElement();
        const QString value = scaleElement.firstChildElement(QStringLiteral("value")).text().trimmed();
        bool ok = false;
        const int configured = value.toInt(&ok);
        const bool isExact = ok && configured == scale;
        const bool isGeneric = value.isEmpty() || (ok && configured == 0);
        if (!isExact && !isGeneric) continue;
        const QDomNodeList items = scaleElement.elementsByTagName(QStringLiteral("check_item"));
        for (int j = 0; j < items.size(); ++j) {
            const QDomElement item = items.at(j).toElement();
            if (item.firstChildElement(QStringLiteral("id")).text().trimmed() != QStringLiteral("121") &&
                item.firstChildElement(QStringLiteral("name")).text().trimmed() !=
                    QStringLiteral("要素密度检查")) continue;
            (isExact ? exact : generic).append(item);
        }
    }
    const QList<QDomElement>& selected = exact.isEmpty() ? generic : exact;
    if (selected.isEmpty()) {
        error = QStringLiteral("121规则 XML 没有匹配比例尺 1:%1 的检查项：%2").arg(scale).arg(xmlPath);
        return false;
    }
    const QRegularExpression filterName(QStringLiteral("^属性筛选字段(?:[0-9]+)?$"));
    for (const QDomElement& item : selected) {
        QString type;
        const QDomNodeList rules = item.elementsByTagName(QStringLiteral("rule"));
        for (int i = 0; i < rules.size(); ++i) {
            const QDomElement rule = rules.at(i).toElement();
            const QString name = rule.firstChildElement(QStringLiteral("name")).text().trimmed();
            const QString value = rule.firstChildElement(QStringLiteral("value")).text().trimmed();
            if (name == QStringLiteral("检查对象类型")) type = value;
            if (name == QStringLiteral("检查范围") && !value.isEmpty())
                inputs.mainArea = true;
            if (filterName.match(name).hasMatch() && !value.isEmpty()) inputs.source = true;
        }
        if (type != QStringLiteral("符号") && type != QStringLiteral("注记")) {
            error = QStringLiteral("121规则缺少有效的检查对象类型：%1").arg(xmlPath);
            return false;
        }
        if (!inputs.type.isEmpty() && inputs.type != type) {
            error = QStringLiteral("同一121规则文件不能同时检查符号和注记：%1").arg(xmlPath);
            return false;
        }
        inputs.type = type;
    }
    return true;
}

enum class DensityLayerKind { ParsedFeature, ParsedAnnotation, SourceData };

QStringList mappedFiles(const CsvLayerInfo& info, const QString& root,
                        DensityLayerKind kind, QString& error)
{
    QStringList paths;
    const bool source = kind == DensityLayerKind::SourceData;
    const bool annotation = kind == DensityLayerKind::ParsedAnnotation;
    if (root.trimmed().isEmpty()) {
        error = source ? QStringLiteral("缺少 GDB_SHP 目录") : QStringLiteral("缺少解析图层目录");
        return paths;
    }
    const QList<QString> names = source ? info.gdbLayerNames :
        (annotation ? info.actualAnnotLayerNames : info.actualLayerNames);
    if (names.isEmpty()) {
        error = source ? QStringLiteral("CSV 缺少源数据图层映射") :
            (annotation ? QStringLiteral("CSV 缺少实际注记图层映射") : QStringLiteral("CSV 缺少实际图层映射"));
        return paths;
    }
    for (const QString& name : names) {
        QStringList found;
        const QStringList suffixes = source ? QStringList{QString()} :
            QStringList{QString(), QStringLiteral("_Point"), QStringLiteral("_LineString"),
                        QStringLiteral("_Polygon")};
        for (const QString& suffix : suffixes) {
            const QString path = QDir(root).filePath(name + suffix + QStringLiteral(".shp"));
            if (QFileInfo::exists(path) && !found.contains(path, Qt::CaseInsensitive)) found << path;
        }
        if (found.isEmpty()) {
            error = QStringLiteral("找不到%1图层文件：%2").arg(
                source ? QStringLiteral("源数据") :
                    (annotation ? QStringLiteral("实际注记") : QStringLiteral("实际")), name);
            return QStringList();
        }
        for (const QString& path : found)
            if (!paths.contains(path, Qt::CaseInsensitive)) paths << path;
    }
    return paths;
}

QString canonicalPath(const QString& path)
{
    const QFileInfo info(path.trimmed());
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

QStringList splitPaths(const QString& text)
{
    QStringList paths;
    for (const QString& path : text.split(';', Qt::SkipEmptyParts)) paths << canonicalPath(path);
    return paths;
}

bool validateAssociation(const QString& path, QString& error)
{
    const QFileInfo file(path);
    const QString stem = QDir(file.absolutePath()).filePath(file.completeBaseName());
    for (const QString& extension : {QStringLiteral(".shp"), QStringLiteral(".shx"), QStringLiteral(".dbf")}) {
        if (!QFileInfo::exists(stem + extension)) {
            error = QStringLiteral("缺少第200检查项完整注记匹配中间图层配套文件：%1")
                        .arg(stem + extension);
            return false;
        }
    }
    GDALAllRegister();
    const QByteArray bytes = path.toUtf8();
    GDALDataset* dataset = static_cast<GDALDataset*>(
        GDALOpenEx(bytes.constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    if (!dataset || dataset->GetLayerCount() < 1) {
        if (dataset) GDALClose(dataset);
        error = QStringLiteral("无法读取第200检查项完整注记匹配中间图层：%1").arg(path);
        return false;
    }
    OGRFeatureDefn* fields = dataset->GetLayer(0)->GetLayerDefn();
    QStringList missing;
    for (const char* field : {"text_conte", "assoc_name", "group_id"})
        if (fields->GetFieldIndex(field) < 0) missing << QString::fromLatin1(field);
    GDALClose(dataset);
    if (!missing.isEmpty()) {
        error = QStringLiteral("第200检查项完整注记匹配中间图层缺少字段 %1：%2")
                    .arg(missing.join(QStringLiteral("、")), path);
        return false;
    }
    return true;
}

} // namespace

void FeatureDensityCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths,
                                int scale, const QString& outputDir, CheckItemResult& result)
{
    const QString layerName = filePaths.standardLayer.trimmed();
    const QDir output(outputDir);
    const QString jsonDir = output.filePath(QStringLiteral("质检json文件"));
    const QString logDir = output.filePath(QStringLiteral("质检日志文件"));
    const QString errorDir = output.filePath(QStringLiteral("质检错误结果"));
    QDir().mkpath(jsonDir);
    QDir().mkpath(logDir);
    QDir().mkpath(errorDir);
    const QString base = QStringLiteral("要素密度检查_") + layerName;
    const QString jsonPath = QDir(jsonDir).filePath(base + QStringLiteral(".json"));
    const QString logPath = QDir(logDir).filePath(base + QStringLiteral(".log"));
    const QString gridPath = QDir(errorDir).filePath(
        QStringLiteral("要素密度检查_error_超限格网_") + layerName + QStringLiteral(".shp"));
    const QString featurePath = QDir(errorDir).filePath(
        QStringLiteral("要素密度检查_error_错误_") + layerName + QStringLiteral(".shp"));
    if (QFileInfo::exists(jsonPath) && !QFile::remove(jsonPath)) {
        failDensity(checkItemName, jsonPath,
                    QStringLiteral("无法清除上次运行的 JSON：%1").arg(jsonPath), result);
        return;
    }
    if (scale <= 0 || layerName.isEmpty()) {
        failDensity(checkItemName, jsonPath, QStringLiteral("缺少有效比例尺或待检标准图层名称"), result);
        return;
    }
    DensityInputs inputs;
    QString error;
    if (!readDensityRules(filePaths.densityXmlPath, scale, inputs, error)) {
        failDensity(checkItemName, jsonPath, error, result);
        return;
    }
    const QMap<QString, CsvLayerInfo> layerMap =
        map_check_task_manager::parseCsvToLayerMap(filePaths.csvPath);
    if (!layerMap.contains(layerName)) {
        failDensity(checkItemName, jsonPath,
                    QStringLiteral("图面配置 CSV 没有待检标准图层：%1").arg(layerName), result);
        return;
    }
    const bool annotation = inputs.type == QStringLiteral("注记");
    QStringList parsed = mappedFiles(layerMap.value(layerName), filePaths.shpPath,
        annotation ? DensityLayerKind::ParsedAnnotation : DensityLayerKind::ParsedFeature, error);
    if (!error.isEmpty()) {
        failDensity(checkItemName, jsonPath, error, result);
        return;
    }
    QStringList sources;
    if (inputs.source) {
        sources = mappedFiles(layerMap.value(layerName), filePaths.gdbShpPath,
                              DensityLayerKind::SourceData, error);
        if (!error.isEmpty()) {
            failDensity(checkItemName, jsonPath, error, result);
            return;
        }
    }
    QString mainArea;
    if (inputs.mainArea) {
        const QString mainLayer = QStringLiteral("行政区划面普色");
        if (!layerMap.contains(mainLayer)) {
            failDensity(checkItemName, jsonPath,
                        QStringLiteral("CSV 没有主区/外区所需标准图层：%1").arg(mainLayer), result);
            return;
        }
        const QStringList areas = mappedFiles(layerMap.value(mainLayer), filePaths.shpPath,
                                              DensityLayerKind::ParsedFeature, error);
        QStringList polygonAreas;
        for (const QString& area : areas)
            if (area.endsWith(QStringLiteral("_Polygon.shp"), Qt::CaseInsensitive)) polygonAreas << area;
        const QStringList selected = polygonAreas.isEmpty() ? areas : polygonAreas;
        if (!error.isEmpty() || selected.size() != 1) {
            failDensity(checkItemName, jsonPath, error.isEmpty() ?
                        QStringLiteral("行政区划面普色必须对应唯一解析面 SHP") : error, result);
            return;
        }
        mainArea = selected.first();
    }
    QString association;
    if (annotation) {
        const QJsonDocument doc = QJsonDocument::fromJson(filePaths.densityMatchesJson.toUtf8());
        const QJsonArray matches = doc.isArray() ? doc.array() : QJsonArray();
        int matchCount = 0;
        QStringList selectedAnnotations;
        for (const QJsonValue& value : matches) {
            const QJsonObject item = value.toObject();
            if (item.value(QStringLiteral("status")).toString() != QStringLiteral("质检成功")) continue;
            const QString path = item.value(QStringLiteral("output_layer")).toString();
            if (path.isEmpty() || !QFileInfo::exists(path)) continue;
            const QStringList matchedAnnotations = splitPaths(item.value(QStringLiteral("anno_layer")).toString());
            if (matchedAnnotations.isEmpty()) continue;
            bool sameTarget = true;
            QStringList sameBatchPaths;
            for (const QString& matched : matchedAnnotations) {
                bool found = false;
                for (const QString& candidate : parsed)
                    if (canonicalPath(candidate).compare(matched, Qt::CaseInsensitive) == 0) {
                        found = true;
                        if (!sameBatchPaths.contains(candidate, Qt::CaseInsensitive))
                            sameBatchPaths << candidate;
                    }
                if (!found) sameTarget = false;
            }
            if (!sameTarget) continue;
            ++matchCount;
            association = path;
            // 200 仅验证批次和筛选路径；121 仍使用 CSV“实际注记图层”列定位到的文件。
            selectedAnnotations = sameBatchPaths;
        }
        if (matchCount != 1) {
            failDensity(checkItemName, jsonPath,
                        QStringLiteral("缺少本幅图本次运行的第200检查项完整注记匹配中间图层"
                                       "（待检图层：%1；匹配结果数：%2）")
                            .arg(layerName).arg(matchCount), result);
            return;
        }
        if (!validateAssociation(association, error)) {
            failDensity(checkItemName, jsonPath, error, result);
            return;
        }
        parsed = selectedAnnotations;
    }
    const QString boundary = resolveDensityBoundaryPath(filePaths.shpPath);
    if (!QFileInfo::exists(boundary)) {
        failDensity(checkItemName, jsonPath,
                    QStringLiteral("缺少图廓（支持内图廓.shp、图廓.shp、图廓_Polygon.shp）：%1")
                        .arg(boundary),
                    result);
        return;
    }
    const QByteArray boundaryBytes = boundary.toUtf8();
    const QByteArray parsedBytes = parsed.join(';').toUtf8();
    const QByteArray sourceBytes = sources.join(';').toUtf8();
    const QByteArray mainBytes = mainArea.toUtf8();
    const QByteArray associationBytes = association.toUtf8();
    const QByteArray xmlBytes = filePaths.densityXmlPath.toUtf8();
    const QByteArray severityBytes = filePaths.densitySeverity.toUtf8();
    const QByteArray gridBytes = gridPath.toUtf8();
    const QByteArray featureBytes = featurePath.toUtf8();
    const QByteArray jsonBytes = jsonPath.toUtf8();
    const QByteArray logBytes = logPath.toUtf8();
    const int code = FeatureDensityCheck(
        boundaryBytes.constData(), parsedBytes.constData(), sourceBytes.constData(), "",
        mainBytes.constData(), associationBytes.constData(), xmlBytes.constData(), scale,
        severityBytes.constData(), gridBytes.constData(), featureBytes.constData(),
        jsonBytes.constData(), logBytes.constData(), filePaths.densitySourceMatchToleranceMm);
    if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
        failDensity(checkItemName, jsonPath,
                    QStringLiteral("121未生成可解析的本次 JSON，错误码：%1").arg(code), result);
        return;
    }
    if (code != 0 || result.status != QStringLiteral("质检成功")) {
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        result.info[QStringLiteral("return_code")] = code;
    } else if (QFileInfo::exists(gridPath)) {
        result.info[QStringLiteral("grid_layer")] = gridPath;
    }
    result.info[QStringLiteral("parsed_layer_mapping")] = annotation ?
        QStringLiteral("实际注记图层") : QStringLiteral("实际图层");
    result.info[QStringLiteral("parsed_layer_paths")] = QJsonArray::fromStringList(parsed);
}
