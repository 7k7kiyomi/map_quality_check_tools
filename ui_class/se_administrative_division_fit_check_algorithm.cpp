#include "se_administrative_division_fit_check_algorithm.h"

// 让 VS/MSVC 按 UTF-8 解释本文件中的中文字符串，避免任务日志文字乱码。
#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <utility>

#include "cpl_string.h"

namespace
{
    const int kMaxProblemLogCount = 20;

    enum class IssueGeometryBucket
    {
        Point,
        Line,
        Polygon,
        Other
    };

    FitGdalDatasetPtr openVectorDataset(const QString& path, int flags = GDAL_OF_VECTOR | GDAL_OF_READONLY)
    {
        return FitGdalDatasetPtr(static_cast<GDALDataset*>(
            GDALOpenEx(path.toUtf8().constData(), flags, nullptr, nullptr, nullptr)));
    }


    bool isCollectionType(OGRwkbGeometryType geometryType)
    {
        const OGRwkbGeometryType flatType = wkbFlatten(geometryType);

        return flatType == wkbGeometryCollection
            || flatType == wkbMultiPoint
            || flatType == wkbMultiLineString
            || flatType == wkbMultiPolygon;
    }

    bool isLineGeometry(const OGRGeometry* geometry)
    {
        if (geometry == nullptr)
            return false;

        const OGRwkbGeometryType flatType = wkbFlatten(geometry->getGeometryType());
        if (flatType == wkbLineString || flatType == wkbMultiLineString)
            return true;

        if (!isCollectionType(geometry->getGeometryType()))
            return false;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return false;

        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            if (isLineGeometry(collection->getGeometryRef(i)))
                return true;
        }

        return false;
    }

    double segmentLength(const OGRPoint& startPoint, const OGRPoint& endPoint)
    {
        const double dx = endPoint.getX() - startPoint.getX();
        const double dy = endPoint.getY() - startPoint.getY();
        return std::sqrt(dx * dx + dy * dy);
    }

    bool lineStringEndpoints(
        const OGRLineString* line,
        OGRPoint& startPoint,
        OGRPoint& endPoint)
    {
        if (line == nullptr || line->getNumPoints() <= 0)
            return false;

        line->getPoint(0, &startPoint);
        line->getPoint(line->getNumPoints() - 1, &endPoint);
        return true;
    }

    // 过滤搜索范围边缘被截出的短线段：
    // 一端落在候选搜索区外边界，另一端落在规则容差边界，说明它更像是 validLine/Difference 的截断结果，
    // 不是境界线与行政区面边界之间真实连续的超限段。
    bool isSearchBoundaryCutoffLine(
        const OGRGeometry* lineGeometry,
        const OGRGeometry* toleranceBoundary,
        const OGRGeometry* boundarySearchOuterBoundary,
        double tolerance)
    {
        if (lineGeometry == nullptr
            || toleranceBoundary == nullptr
            || boundarySearchOuterBoundary == nullptr) {
            return false;
        }

        if (wkbFlatten(lineGeometry->getGeometryType()) != wkbLineString)
            return false;

        const auto* line = dynamic_cast<const OGRLineString*>(lineGeometry);
        if (line == nullptr)
            return false;

        OGRPoint startPoint;
        OGRPoint endPoint;
        if (!lineStringEndpoints(line, startPoint, endPoint))
            return false;

        const double endpointTolerance =
            std::max(0.5, std::min(tolerance * 0.1, 5.0));

        const bool startOnOuterBoundary =
            startPoint.Distance(boundarySearchOuterBoundary) <= endpointTolerance;
        const bool endOnOuterBoundary =
            endPoint.Distance(boundarySearchOuterBoundary) <= endpointTolerance;
        const bool startOnToleranceBoundary =
            startPoint.Distance(toleranceBoundary) <= endpointTolerance;
        const bool endOnToleranceBoundary =
            endPoint.Distance(toleranceBoundary) <= endpointTolerance;

        const bool oneEndOnOuterAndOtherOnTolerance =
            (startOnOuterBoundary && endOnToleranceBoundary)
            || (endOnOuterBoundary && startOnToleranceBoundary);
        return oneEndOnOuterAndOtherOnTolerance;
    }

    double maximumLineDistanceToGeometry(
        const OGRGeometry* lineGeometry,
        const OGRGeometry* referenceGeometry,
        double sampleStep)
    {
        if (lineGeometry == nullptr || referenceGeometry == nullptr)
            return 0.0;

        const OGRwkbGeometryType flatType = wkbFlatten(lineGeometry->getGeometryType());
        if (flatType != wkbLineString)
            return lineGeometry->Distance(referenceGeometry);

        const auto* line = dynamic_cast<const OGRLineString*>(lineGeometry);
        if (line == nullptr || line->getNumPoints() <= 0)
            return 0.0;

        if (sampleStep <= 0.0)
            sampleStep = 1.0;

        double maxDistance = 0.0;
        for (int i = 0; i < line->getNumPoints(); ++i) {
            OGRPoint point;
            line->getPoint(i, &point);

            const double pointDistance = point.Distance(referenceGeometry);
            if (pointDistance > maxDistance)
                maxDistance = pointDistance;
        }

        for (int i = 1; i < line->getNumPoints(); ++i) {
            OGRPoint startPoint;
            OGRPoint endPoint;
            line->getPoint(i - 1, &startPoint);
            line->getPoint(i, &endPoint);

            const double currentSegmentLength = segmentLength(startPoint, endPoint);
            if (currentSegmentLength <= 0.0)
                continue;

            int sampleCount = static_cast<int>(std::ceil(currentSegmentLength / sampleStep));
            if (sampleCount < 2)
                sampleCount = 2;

            for (int sampleIndex = 1; sampleIndex < sampleCount; ++sampleIndex) {
                const double ratio = static_cast<double>(sampleIndex) / sampleCount;
                OGRPoint samplePoint(
                    startPoint.getX() + (endPoint.getX() - startPoint.getX()) * ratio,
                    startPoint.getY() + (endPoint.getY() - startPoint.getY()) * ratio);

                const double sampleDistance = samplePoint.Distance(referenceGeometry);
                if (sampleDistance > maxDistance)
                    maxDistance = sampleDistance;
            }
        }

        return maxDistance;
    }

    IssueGeometryBucket bucketOf(const OGRGeometry* geometry)
    {
        if (geometry == nullptr)
            return IssueGeometryBucket::Other;

        switch (wkbFlatten(geometry->getGeometryType()))
        {
        case wkbPoint:
        case wkbMultiPoint:
            return IssueGeometryBucket::Point;

        case wkbLineString:
        case wkbMultiLineString:
            return IssueGeometryBucket::Line;

        case wkbPolygon:
        case wkbMultiPolygon:
            return IssueGeometryBucket::Polygon;

        default:
            return IssueGeometryBucket::Other;
        }
    }

    double geometryArea(const OGRGeometry* geometry)
    {
        if (geometry == nullptr)
            return 0.0;

        const OGRwkbGeometryType flatType = wkbFlatten(geometry->getGeometryType());

        if (flatType == wkbPolygon)
            return static_cast<const OGRPolygon*>(geometry)->get_Area();

        if (isCollectionType(geometry->getGeometryType())) {
            const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
            if (collection == nullptr)
                return 0.0;

            double area = 0.0;
            for (int i = 0; i < collection->getNumGeometries(); ++i)
                area += geometryArea(collection->getGeometryRef(i));

            return area;
        }

        return 0.0;
    }

    void createField(OGRLayer* layer, const char* name, OGRFieldType type, int width = 0, int precision = 0)
    {
        OGRFieldDefn field(name, type);

        if (width > 0)
            field.SetWidth(width);

        if (precision > 0)
            field.SetPrecision(precision);

        layer->CreateField(&field);
    }

    bool createOutputFields(OGRLayer* layer)
    {
        if (layer == nullptr)
            return false;

        createField(layer, "ptype", OFTString, 32);
        createField(layer, "src_layer", OFTString, 64);
        createField(layer, "src_fid", OFTInteger64);
        createField(layer, "dist", OFTReal, 18, 8);
        createField(layer, "area", OFTReal, 18, 8);
        createField(layer, "tol", OFTReal, 18, 8);
        createField(layer, "msg", OFTString, 254);

        return true;
    }

    QString outputPathForBucket(const QString& outputBaseShpPath, IssueGeometryBucket bucket)
    {
        QString normalizedPath = outputBaseShpPath;
        if (QFileInfo(normalizedPath).suffix().compare("shp", Qt::CaseInsensitive) != 0)
            normalizedPath += ".shp";

        const QFileInfo info(normalizedPath);
        const QString baseName = info.completeBaseName();
        const QDir dir(info.absolutePath());

        switch (bucket)
        {
        case IssueGeometryBucket::Point:
            return dir.filePath(baseName + "_point.shp");

        case IssueGeometryBucket::Line:
            return dir.filePath(baseName + "_line.shp");

        case IssueGeometryBucket::Polygon:
            return dir.filePath(baseName + "_polygon.shp");

        default:
            return dir.filePath(baseName + "_other.shp");
        }
    }

    const char* layerNameForBucket(IssueGeometryBucket bucket)
    {
        switch (bucket)
        {
        case IssueGeometryBucket::Point:
            return "fit_point";

        case IssueGeometryBucket::Line:
            return "fit_line";

        case IssueGeometryBucket::Polygon:
            return "fit_poly";

        default:
            return "fit_other";
        }
    }

    OGRwkbGeometryType geometryTypeForBucket(IssueGeometryBucket bucket)
    {
        switch (bucket)
        {
        case IssueGeometryBucket::Point:
            return wkbPoint;

        case IssueGeometryBucket::Line:
            return wkbLineString;

        case IssueGeometryBucket::Polygon:
            return wkbPolygon;

        default:
            return wkbUnknown;
        }
    }

    bool mapToleranceToGroundMeters(
        double mapTolerance,
        const QString& unit,
        double scaleDenominator,
        double& groundToleranceMeters)
    {
        if (mapTolerance <= 0.0 || scaleDenominator <= 0.0)
            return false;

        const QString normalizedUnit = unit.trimmed().toLower();
        double mapToleranceMillimeters = 0.0;

        if (normalizedUnit.isEmpty() || normalizedUnit == "mm" || normalizedUnit == "毫米") {
            mapToleranceMillimeters = mapTolerance;
        }
        else if (normalizedUnit == "cm" || normalizedUnit == "厘米") {
            mapToleranceMillimeters = mapTolerance * 10.0;
        }
        else if (normalizedUnit == "m" || normalizedUnit == "米") {
            mapToleranceMillimeters = mapTolerance * 1000.0;
        }
        else {
            return false;
        }

        // 图上距离(mm) * 比例尺分母 = 实地距离(mm)，再 /1000 转为米。
        groundToleranceMeters = mapToleranceMillimeters * scaleDenominator / 1000.0;
        return groundToleranceMeters > 0.0;
    }

}


bool se_administrative_division_fit_check_algorithm::RunAdministrativeFitCheck(
    const QString& polygonShpPath,
    const QString& lineShpPaths,
    const QString& xmlPath,
    int scale,
    const QString& outputBaseShpPath,
    AdministrativeFitCheckResult& result,
    const std::function<void(const QString&)>& log)
{
    GDALAllRegister();
    return runCore(
        polygonShpPath,
        lineShpPaths,
        xmlPath,
        scale,
        outputBaseShpPath,
        !outputBaseShpPath.trimmed().isEmpty(),
        result,
        log);
}

bool se_administrative_division_fit_check_algorithm::runCore(
    const QString& polygonShpPath,
    const QString& lineShpPaths,
    const QString& xmlPath,
    int scale,
    const QString& outputBaseShpPath,
    bool writeOutput,
    AdministrativeFitCheckResult& result,
    const std::function<void(const QString&)>& log)
{
    result.problemCount = 0;
    result.message.clear();
    result.problemDetails = QJsonArray();
    result.outputPaths.clear();

    GDALAllRegister();
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    CPLSetConfigOption("SHAPE_ENCODING", "");

    const auto writeLog = [&log](const QString& text) {
        if (log)
            log(text);
    };

    if (polygonShpPath.trimmed().isEmpty() || lineShpPaths.trimmed().isEmpty()) {
        result.message = "行政区划面套合检查需要同时提供行政区划面和行政界线 SHP。";
        return false;
    }

    if (scale <= 0) {
        result.message = "未读取到有效比例尺，无法执行行政区划面套合检查。";
        writeLog(result.message);
        return false;
    }

    AdministrativeFitRule rule;
    QString error;

    writeLog("开始执行行政区划面套合检查。");
    writeLog("正在读取 XML 规则。");
    if (!readRuleFromXml(xmlPath, scale, rule, error)) {
        result.message = error;
        return false;
    }

    writeLog(QString("规则读取完成：图上容差=%1%2，比例尺=1:%3，计算容差=%4米，候选搜索区半径=%5%6，计算搜索半径=%7米。")
        .arg(rule.mapTolerance, 0, 'f', 8)
        .arg(rule.unit)
        .arg(rule.scaleDenominator, 0, 'f', 0)
        .arg(rule.tolerance, 0, 'f', 8)
        .arg(rule.candidateSearchMapTolerance, 0, 'f', 8)
        .arg(rule.candidateSearchUnit)
        .arg(rule.candidateSearchTolerance, 0, 'f', 8));

    std::vector<FitTopologyIssue> issues;
    writeLog("正在执行线面套合拓扑计算。");
    if (!collectTopologyIssues(polygonShpPath, lineShpPaths, rule, issues, error, writeLog)) {
        result.message = error;
        return false;
    }

    result.problemCount = static_cast<int>(issues.size());
    for (const FitTopologyIssue& issue : issues) {
        QJsonObject detail;
        detail["type"] = issue.type;
        detail["source_layer"] = issue.sourceLayerName;
        detail["source_fid"] = QString::number(issue.sourceFid);
        detail["distance"] = issue.distance;
        detail["area"] = issue.area;
        detail["message"] = issue.message;
        result.problemDetails.append(detail);
    }

    writeLog(QString("问题统计：线段超限=%1。")
        .arg(result.problemCount));
    for (int i = 0; i < result.problemCount && i < kMaxProblemLogCount; ++i) {
        const FitTopologyIssue& issue = issues.at(i);
        writeLog(QString("发现问题：类型=%1，图层=%2，FID=%3，距离=%4，面积=%5，说明=%6")
            .arg(issue.type)
            .arg(issue.sourceLayerName)
            .arg(issue.sourceFid)
            .arg(issue.distance, 0, 'f', 8)
            .arg(issue.area, 0, 'f', 8)
            .arg(issue.message));
    }

    if (result.problemCount > kMaxProblemLogCount) {
        writeLog(QString("问题明细较多，仅在日志中展开前 %1 条，其余 %2 条可通过输出 SHP 查看。")
            .arg(kMaxProblemLogCount)
            .arg(result.problemCount - kMaxProblemLogCount));
    }

    if (result.problemCount == 0) {
        result.message = "检查完成。未发现行政区划线面不套合问题，未生成结果 SHP。";
        writeLog(result.message);
        return true;
    }

    if (!writeOutput) {
        result.message = QString("检查完成。行政区划线面不套合问题数量：%1。未指定输出路径，未生成问题 SHP。")
            .arg(result.problemCount);
        return true;
    }

    if (!writeProblemShapefiles(
            polygonShpPath,
            outputBaseShpPath,
            issues,
            rule.tolerance,
            result.outputPaths,
            error,
            writeLog)) {
        result.message = error;
        return false;
    }

    result.message = QString("检查完成。行政区划线面不套合问题数量：%1。结果文件：%2")
        .arg(result.problemCount)
        .arg(result.outputPaths.join("; "));
    writeLog(result.message);
    return true;
}

bool se_administrative_division_fit_check_algorithm::readRuleFromXml(
    const QString& xmlPath,
    int scale,
    AdministrativeFitRule& rule,
    QString& error)
{
    // 本功能读取：
    // <check_item><name>行政区划面套合检查</name><id>115</id> 下的
    // <rule><name>容差阈值</name><value>...</value><unit>...</unit></rule>
    // <rule><name>候选搜索区半径</name><value>...</value><unit>...</unit></rule>
    // 其中“容差阈值”和“候选搜索区半径”都是图上距离，都需要按比例尺换算成实地米值。
    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = "XML 文件打开失败：" + xmlPath;
        return false;
    }

    QDomDocument document;
    QString parseError;
    int errorLine = 0;
    int errorColumn = 0;

    if (!document.setContent(&file, &parseError, &errorLine, &errorColumn)) {
        error = QString("XML 解析失败：%1，第 %2 行，第 %3 列")
            .arg(parseError)
            .arg(errorLine)
            .arg(errorColumn);
        return false;
    }

    const QDomElement root = document.documentElement();
    const QDomNodeList itemNodes = root.elementsByTagName("check_item");

    for (int i = 0; i < itemNodes.count(); ++i) {
        const QDomElement itemElement = itemNodes.at(i).toElement();
        const QString itemName = itemElement.firstChildElement("name").text().trimmed();
        const QString idText = itemElement.firstChildElement("id").text().trimmed();

        if (itemName != "行政区划面套合检查" || idText != "115")
            continue;

        bool hasToleranceRule = false;
        bool hasCandidateSearchRule = false;
        const QDomNodeList ruleNodes = itemElement.elementsByTagName("rule");
        for (int j = 0; j < ruleNodes.count(); ++j) {
            const QDomElement ruleElement = ruleNodes.at(j).toElement();
            const QString ruleName = ruleElement.firstChildElement("name").text().trimmed();

            if (ruleName == "容差阈值") {
                bool ok = false;
                const double tolerance =
                    ruleElement.firstChildElement("value").text().trimmed().toDouble(&ok);

                if (!ok || tolerance <= 0.0) {
                    error = "XML 中 ID=115 的“容差阈值”必须是大于 0 的数字。";
                    return false;
                }

                rule.mapTolerance = tolerance;
                rule.unit = ruleElement.firstChildElement("unit").text().trimmed();

                if (rule.unit.isEmpty())
                    rule.unit = "mm";

                rule.scaleDenominator = scale;

                // XML 中的容差是图上距离，必须先按比例尺换算成实地米，再参与 OGR 计算。
                if (!mapToleranceToGroundMeters(
                    rule.mapTolerance,
                    rule.unit,
                    rule.scaleDenominator,
                    rule.tolerance)) {
                    error = "XML 中 ID=115 的“容差阈值”单位暂只支持 mm/cm/m。";
                    return false;
                }

                hasToleranceRule = true;
            }
            else if (ruleName == "候选搜索区半径") {
                bool ok = false;
                const double searchRadius =
                    ruleElement.firstChildElement("value").text().trimmed().toDouble(&ok);

                if (!ok || searchRadius <= 0.0) {
                    error = "XML 中 ID=115 的“候选搜索区半径”必须是大于 0 的数字。";
                    return false;
                }

                rule.candidateSearchMapTolerance = searchRadius;
                rule.candidateSearchUnit = ruleElement.firstChildElement("unit").text().trimmed();
                if (rule.candidateSearchUnit.isEmpty())
                    rule.candidateSearchUnit = "mm";

                // XML 中的候选搜索区半径也是图上距离，必须先按比例尺换算成实地米，再参与 OGR 计算。
                if (!mapToleranceToGroundMeters(
                    rule.candidateSearchMapTolerance,
                    rule.candidateSearchUnit,
                    rule.scaleDenominator,
                    rule.candidateSearchTolerance)) {
                    error = "XML 中 ID=115 的“候选搜索区半径”单位暂只支持 mm/cm/m。";
                    return false;
                }

                hasCandidateSearchRule = true;
            }
        }

        if (!hasToleranceRule) {
            error = "XML 中 ID=115 缺少“容差阈值”规则。";
            return false;
        }

        if (!hasCandidateSearchRule) {
            error = "XML 中 ID=115 缺少“候选搜索区半径”规则。";
            return false;
        }

        return true;
    }

    error = "XML 中没有找到 ID=115 的行政区划面套合检查规则。";
    return false;
}

bool se_administrative_division_fit_check_algorithm::collectTopologyIssues(
    const QString& polygonShpPath,
    const QString& lineShpPaths,
    const AdministrativeFitRule& rule,
    std::vector<FitTopologyIssue>& issues,
    QString& error,
    const std::function<void(const QString&)>& log)
{
    const auto writeLog = [&log](const QString& text) {
        if (log)
            log(text);
    };

    writeLog("正在提取全图行政区面边界几何。");
    FitOgrGeometryPtr polygonBoundary = loadBoundaryGeometry(polygonShpPath, error);
    if (!polygonBoundary || polygonBoundary->IsEmpty())
        return false;

    writeLog("正在缓存全图行政区面覆盖几何。");
    FitOgrGeometryPtr polygonCoverage(OGRGeometryFactory::createGeometry(wkbGeometryCollection));
    auto* polygonCoverageRaw =
        dynamic_cast<OGRGeometryCollection*>(polygonCoverage.get());

    if (polygonCoverageRaw == nullptr) {
        error = "行政区面覆盖集合创建失败。";
        return false;
    }

    FitGdalDatasetPtr polygonDataset = openVectorDataset(polygonShpPath);
    if (!polygonDataset) {
        error = "面 SHP 数据打开失败：" + polygonShpPath;
        return false;
    }

    for (int layerIndex = 0; layerIndex < polygonDataset->GetLayerCount(); ++layerIndex) {
        OGRLayer* polygonLayer = polygonDataset->GetLayer(layerIndex);
        if (polygonLayer == nullptr)
            continue;

        polygonLayer->ResetReading();

        OGRFeature* rawPolygonFeature = nullptr;
        while ((rawPolygonFeature = polygonLayer->GetNextFeature()) != nullptr) {
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                polygonFeature(rawPolygonFeature, OGRFeature::DestroyFeature);

            OGRGeometry* polygonGeometry = polygonFeature->GetGeometryRef();
            if (polygonGeometry == nullptr || polygonGeometry->IsEmpty())
                continue;

            const OGRwkbGeometryType flatType = wkbFlatten(polygonGeometry->getGeometryType());
            if (flatType != wkbPolygon && flatType != wkbMultiPolygon)
                continue;

            polygonCoverageRaw->addGeometry(polygonGeometry);
        }
    }

    if (polygonCoverageRaw->getNumGeometries() <= 0) {
        error = "面 SHP 中没有可用于判断覆盖范围的有效面几何：" + polygonShpPath;
        return false;
    }

    // 行政区面边界和面覆盖集合只在这里生成一次。
    // 后续所有行政界线 SHP 和 Layer 都复用这两份全图几何。
    // 候选搜索距离来自 XML 的“候选搜索区半径”，按图上距离换算成米后，
    // 只用于面覆盖筛选、生成 validLine 和查找附近面边界。
    const double candidateSearchTolerance = rule.candidateSearchTolerance;
    // 真正判错仍使用 rule.tolerance；validLine 不再按面边界折线小段做走向/投影/覆盖率筛选。

    writeLog(QString("候选搜索距离=%1米，判错容差=%2米。")
        .arg(candidateSearchTolerance, 0, 'f', 8)
        .arg(rule.tolerance, 0, 'f', 8));

    FitOgrGeometryPtr polygonCoverageSearchArea(polygonCoverage->Buffer(candidateSearchTolerance));
    if (!polygonCoverageSearchArea || polygonCoverageSearchArea->IsEmpty()) {
        error = "行政区面覆盖搜索范围 Buffer 运算失败。";
        return false;
    }

    const QStringList lineDatasetPaths =
        lineShpPaths.split(";", QString::SkipEmptyParts);

    if (lineDatasetPaths.isEmpty()) {
        error = "请至少选择一个行政界线 SHP 文件。";
        return false;
    }

    int totalDatasetCount = 0;
    int totalLayerCount = 0;
    int totalLineCount = 0;
    int totalCandidateCount = 0;
    int totalNoNearbyBoundaryCount = 0;
    int totalEmptyGeometryCount = 0;
    int totalNonLineGeometryCount = 0;

    // 行政界线只支持一个或多个 SHP 文件；不再按目录数据集加载。
    // 不按行政等级分类，所有 SHP 中的线要素都与同一份 BoundaryCollection 检查。
    for (const QString& rawDatasetPath : lineDatasetPaths) {
        const QString lineDatasetPath = rawDatasetPath.trimmed();
        if (lineDatasetPath.isEmpty())
            continue;

        if (QFileInfo(lineDatasetPath).suffix().compare("shp", Qt::CaseInsensitive) != 0) {
            error = "行政界线数据只支持选择一个或多个 SHP 文件：" + lineDatasetPath;
            return false;
        }

        FitGdalDatasetPtr lineDataset = openVectorDataset(lineDatasetPath);
        if (!lineDataset) {
            error = "行政界线 SHP 打开失败：" + lineDatasetPath;
            return false;
        }

        if (lineDataset->GetLayerCount() <= 0) {
            writeLog("行政界线 SHP 中没有可检查的图层：" + lineDatasetPath);
            continue;
        }

        ++totalDatasetCount;
        const QString lineDatasetName = QFileInfo(lineDatasetPath).fileName();

        for (int layerIndex = 0; layerIndex < lineDataset->GetLayerCount(); ++layerIndex) {
            OGRLayer* lineLayer = lineDataset->GetLayer(layerIndex);
            if (lineLayer == nullptr)
                continue;

            ++totalLayerCount;

            const QString layerName = QString("%1/%2")
                .arg(lineDatasetName)
                .arg(QString::fromUtf8(lineLayer->GetName()));
            writeLog(QString("检查行政界线图层：%1").arg(layerName));

            int checkedCount = 0;
            int candidateCount = 0;
            int noNearbyBoundaryCount = 0;
            int emptyGeometryCount = 0;
            int nonLineGeometryCount = 0;
            QStringList selectedCheckFids;
            QStringList skippedCheckFids;
            const int problemCountBeforeLayer = static_cast<int>(issues.size());

            lineLayer->ResetReading();

            OGRFeature* rawFeature = nullptr;
            while ((rawFeature = lineLayer->GetNextFeature()) != nullptr) {
                std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                    feature(rawFeature, OGRFeature::DestroyFeature);

                OGRGeometry* lineGeometry = feature->GetGeometryRef();
                if (lineGeometry == nullptr || lineGeometry->IsEmpty()) {
                    ++emptyGeometryCount;
                    continue;
                }

                if (!isLineGeometry(lineGeometry)) {
                    ++nonLineGeometryCount;
                    continue;
                }

                ++checkedCount;
                ++totalLineCount;

                // 如果当前线整体距离行政区面覆盖范围超过候选搜索半径，说明它附近没有可对应的面数据，
                // 直接跳过，不参与后续套合检查。
                const double distanceToPolygonCoverage = lineGeometry->Distance(polygonCoverage.get());
                if (distanceToPolygonCoverage > candidateSearchTolerance) {
                    ++noNearbyBoundaryCount;
                    skippedCheckFids << QString("FID=%1：无邻近面覆盖，距面=%2米")
                        .arg(feature->GetFID())
                        .arg(distanceToPolygonCoverage, 0, 'f', 3);
                    continue;
                }

                // validLine 表示原始境界线中仍处在行政区面覆盖搜索范围内的部分；
                // 超出面覆盖搜索范围的线段不参与后续距离判错。
                FitOgrGeometryPtr validLine(lineGeometry->Intersection(polygonCoverageSearchArea.get()));
                if (!validLine
                    || validLine->IsEmpty()
                    || !isLineGeometry(validLine.get())) {
                    ++noNearbyBoundaryCount;
                    skippedCheckFids << QString("FID=%1：无法生成有效检查线段")
                        .arg(feature->GetFID());
                    continue;
                }

                // 判错基准始终来自行政区面边界：只截取 validLine 附近的面边界参与后续容差判断。
                FitOgrGeometryPtr validLineBoundarySearchArea(validLine->Buffer(candidateSearchTolerance));
                if (!validLineBoundarySearchArea || validLineBoundarySearchArea->IsEmpty()) {
                    error = QString("有效检查线段搜索 Buffer 运算失败：%1，FID=%2。")
                        .arg(layerName)
                        .arg(feature->GetFID());
                    return false;
                }

                FitOgrGeometryPtr nearbyBoundary(
                    polygonBoundary->Intersection(validLineBoundarySearchArea.get()));
                if (!nearbyBoundary || nearbyBoundary->IsEmpty()) {
                    ++noNearbyBoundaryCount;
                    skippedCheckFids << QString("FID=%1：无邻近面边界，距面边界=%2米")
                        .arg(feature->GetFID())
                        .arg(validLine->Distance(polygonBoundary.get()), 0, 'f', 3);
                    continue;
                }

                ++candidateCount;
                selectedCheckFids << QString("FID=%1").arg(feature->GetFID());

                FitOgrGeometryPtr nearbyToleranceCorridor(nearbyBoundary->Buffer(rule.tolerance));
                if (!nearbyToleranceCorridor || nearbyToleranceCorridor->IsEmpty()) {
                    error = QString("邻近面边界容差 Buffer 运算失败：%1，FID=%2。")
                        .arg(layerName)
                        .arg(feature->GetFID());
                    return false;
                }

                FitOgrGeometryPtr nearbyBoundarySearchCorridor(
                    nearbyBoundary->Buffer(candidateSearchTolerance));
                if (!nearbyBoundarySearchCorridor || nearbyBoundarySearchCorridor->IsEmpty()) {
                    error = QString("邻近面边界候选搜索 Buffer 运算失败：%1，FID=%2。")
                        .arg(layerName)
                        .arg(feature->GetFID());
                    return false;
                }

                FitOgrGeometryPtr toleranceBoundary(nearbyToleranceCorridor->Boundary());
                if (!toleranceBoundary || toleranceBoundary->IsEmpty()) {
                    error = QString("邻近面边界规则容差边界提取失败：%1，FID=%2。")
                        .arg(layerName)
                        .arg(feature->GetFID());
                    return false;
                }

                FitOgrGeometryPtr boundarySearchOuterBoundary(
                    nearbyBoundarySearchCorridor->Boundary());
                if (!boundarySearchOuterBoundary || boundarySearchOuterBoundary->IsEmpty()) {
                    error = QString("邻近面边界候选搜索外边界提取失败：%1，FID=%2。")
                        .arg(layerName)
                        .arg(feature->GetFID());
                    return false;
                }

                // 线段级检查：有效线段减去附近面边界的规则容差缓冲区，剩余部分就是超限线段。
                // 输出前再过滤一端落在搜索外边界、一端落在容差边界的截断段，避免覆盖范围边缘误报。
                FitOgrGeometryPtr lineOutsideTolerance(validLine->Difference(nearbyToleranceCorridor.get()));
                if (lineOutsideTolerance && !lineOutsideTolerance->IsEmpty()) {
                    addLineOutToleranceIssues(
                        layerName,
                        feature->GetFID(),
                        lineOutsideTolerance.get(),
                        nearbyBoundary.get(),
                        toleranceBoundary.get(),
                        boundarySearchOuterBoundary.get(),
                        rule.tolerance,
                        issues);
                }

            }

            totalCandidateCount += candidateCount;
            totalNoNearbyBoundaryCount += noNearbyBoundaryCount;
            totalEmptyGeometryCount += emptyGeometryCount;
            totalNonLineGeometryCount += nonLineGeometryCount;

            const int layerProblemCount = static_cast<int>(issues.size()) - problemCountBeforeLayer;
            if (!selectedCheckFids.isEmpty()) {
                writeLog(QString("筛选进入后续检查的线要素：%1。%2")
                    .arg(layerName)
                    .arg(selectedCheckFids.join("，")));
            }
            else {
                writeLog(QString("筛选进入后续检查的线要素：%1。无。")
                    .arg(layerName));
            }

            if (!skippedCheckFids.isEmpty()) {
                writeLog(QString("未进入后续检查的线要素：%1。%2")
                    .arg(layerName)
                    .arg(skippedCheckFids.join("；")));
            }

            writeLog(QString("行政界线图层检查完成：%1。检查线要素=%2，进入后续检查=%3，无邻近边界或面覆盖跳过=%4，问题几何=%5，空几何跳过=%6，非线几何跳过=%7。")
                .arg(layerName)
                .arg(checkedCount)
                .arg(candidateCount)
                .arg(noNearbyBoundaryCount)
                .arg(layerProblemCount)
                .arg(emptyGeometryCount)
                .arg(nonLineGeometryCount));
        }
    }

    if (totalLineCount <= 0) {
        error = "行政界线 SHP 中没有可用的线几何。";
        return false;
    }

    writeLog(QString("行政界线 SHP 读取完成：SHP文件=%1，图层=%2，有效线要素=%3，进入后续检查=%4，无邻近边界或面覆盖跳过=%5，空几何跳过=%6，非线几何跳过=%7。")
        .arg(totalDatasetCount)
        .arg(totalLayerCount)
        .arg(totalLineCount)
        .arg(totalCandidateCount)
        .arg(totalNoNearbyBoundaryCount)
        .arg(totalEmptyGeometryCount)
        .arg(totalNonLineGeometryCount));

    return true;
}

FitOgrGeometryPtr se_administrative_division_fit_check_algorithm::loadBoundaryGeometry(
    const QString& shpPath,
    QString& error)
{
    FitGdalDatasetPtr dataset = openVectorDataset(shpPath);
    if (!dataset) {
        error = "SHP 数据打开失败：" + shpPath;
        return nullptr;
    }

    if (dataset->GetLayerCount() <= 0) {
        error = "SHP 中没有可检查的图层：" + shpPath;
        return nullptr;
    }

    FitOgrGeometryPtr boundaryCollection(OGRGeometryFactory::createGeometry(wkbGeometryCollection));
    auto* boundaryCollectionRaw =
        dynamic_cast<OGRGeometryCollection*>(boundaryCollection.get());

    if (boundaryCollectionRaw == nullptr) {
        error = "面边界集合创建失败。";
        return nullptr;
    }

    for (int layerIndex = 0; layerIndex < dataset->GetLayerCount(); ++layerIndex) {
        OGRLayer* layer = dataset->GetLayer(layerIndex);
        if (layer == nullptr)
            continue;

        layer->ResetReading();

        OGRFeature* rawFeature = nullptr;
        while ((rawFeature = layer->GetNextFeature()) != nullptr) {
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                feature(rawFeature, OGRFeature::DestroyFeature);

            OGRGeometry* geometry = feature->GetGeometryRef();
            if (geometry == nullptr || geometry->IsEmpty())
                continue;

            const OGRwkbGeometryType flatType = wkbFlatten(geometry->getGeometryType());
            if (flatType != wkbPolygon && flatType != wkbMultiPolygon)
                continue;
            

            // 这里不要做全局 Union。对每个行政区面直接取边界，避免局部无效面导致整体 Union 失败。
            FitOgrGeometryPtr boundary(geometry->Boundary());
            if (!boundary || boundary->IsEmpty())
                continue;

            boundaryCollectionRaw->addGeometry(boundary.get());
        }
    }

    if (boundaryCollectionRaw->getNumGeometries() <= 0) {
        error = "面 SHP 中没有可提取的有效边界：" + shpPath;
        return nullptr;
    }

    return boundaryCollection;
}

std::unique_ptr<OGRSpatialReference> se_administrative_division_fit_check_algorithm::readSpatialReference(
    const QString& shpPath)
{
    FitGdalDatasetPtr dataset = openVectorDataset(shpPath);
    if (!dataset || dataset->GetLayerCount() <= 0)
        return nullptr;

    OGRLayer* layer = dataset->GetLayer(0);
    if (layer == nullptr)
        return nullptr;

    OGRSpatialReference* spatialReference = layer->GetSpatialRef();
    if (spatialReference == nullptr)
        return nullptr;

    return std::unique_ptr<OGRSpatialReference>(spatialReference->Clone());
}

void se_administrative_division_fit_check_algorithm::addIssue(
    const QString& type,
    const QString& sourceLayerName,
    GIntBig sourceFid,
    double distance,
    double area,
    const QString& message,
    const OGRGeometry* geometry,
    std::vector<FitTopologyIssue>& issues)
{
    if (geometry == nullptr || geometry->IsEmpty())
        return;

    // Shapefile 单图层通常使用单一几何类型，先把 Multi* / GeometryCollection 拆成单几何。
    if (isCollectionType(geometry->getGeometryType())) {
        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            addIssue(
                type,
                sourceLayerName,
                sourceFid,
                distance,
                area,
                message,
                collection->getGeometryRef(i),
                issues);
        }

        return;
    }

    FitTopologyIssue issue;
    issue.type = type;
    issue.sourceLayerName = sourceLayerName;
    issue.sourceFid = sourceFid;
    issue.distance = distance;
    issue.area = area > 0.0 ? area : geometryArea(geometry);
    issue.message = message;
    issue.geometry.reset(geometry->clone());

    issues.push_back(std::move(issue));
}

void se_administrative_division_fit_check_algorithm::addLineOutToleranceIssues(
    const QString& sourceLayerName,
    GIntBig sourceFid,
    const OGRGeometry* geometry,
    const OGRGeometry* nearbyBoundary,
    const OGRGeometry* toleranceBoundary,
    const OGRGeometry* boundarySearchOuterBoundary,
    double tolerance,
    std::vector<FitTopologyIssue>& issues)
{
    if (geometry == nullptr || geometry->IsEmpty() || nearbyBoundary == nullptr)
        return;

    if (isCollectionType(geometry->getGeometryType())) {
        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            addLineOutToleranceIssues(
                sourceLayerName,
                sourceFid,
                collection->getGeometryRef(i),
                nearbyBoundary,
                toleranceBoundary,
                boundarySearchOuterBoundary,
                tolerance,
                issues);
        }

        return;
    }

    if (!isLineGeometry(geometry))
        return;

    if (isSearchBoundaryCutoffLine(
            geometry,
            toleranceBoundary,
            boundarySearchOuterBoundary,
            tolerance)) {
        return;
    }

    const double sampleStep = tolerance > 0.0 ? tolerance / 2.0 : 1.0;
    const double lineOutDistance = maximumLineDistanceToGeometry(
        geometry,
        nearbyBoundary,
        sampleStep);

    addIssue(
        "LINE_OUT_TOL",
        sourceLayerName,
        sourceFid,
        lineOutDistance,
        0.0,
        "境界线存在超过容差范围的非套合线段。",
        geometry,
        issues);
}

bool se_administrative_division_fit_check_algorithm::writeProblemShapefiles(
    const QString& polygonShpPath,
    const QString& outputBaseShpPath,
    const std::vector<FitTopologyIssue>& issues,
    double tolerance,
    QStringList& outputPaths,
    QString& error,
    const std::function<void(const QString&)>& log)
{
    if (outputBaseShpPath.trimmed().isEmpty()) {
        error = "输出路径为空，无法生成问题 SHP。";
        return false;
    }

    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (driver == nullptr) {
        error = "当前 GDAL 环境不支持 ESRI Shapefile 驱动。";
        return false;
    }

    std::unique_ptr<OGRSpatialReference> spatialReference = readSpatialReference(polygonShpPath);

    const IssueGeometryBucket buckets[] =
    {
        IssueGeometryBucket::Point,
        IssueGeometryBucket::Line,
        IssueGeometryBucket::Polygon
    };

    for (IssueGeometryBucket bucket : buckets) {
        int bucketCount = 0;

        for (const FitTopologyIssue& issue : issues) {
            if (bucketOf(issue.geometry.get()) == bucket)
                ++bucketCount;
        }

        if (bucketCount == 0)
            continue;

        const QString shpPath = outputPathForBucket(outputBaseShpPath, bucket);
        const QByteArray shpPathBytes = toGdalPath(shpPath);

        // 如果结果文件已存在，让 GDAL 删除 shp/dbf/shx/prj 等整组文件。
        if (QFileInfo(shpPath).exists())
            driver->Delete(shpPathBytes.constData());

        FitGdalDatasetPtr outputDataset(
            driver->Create(shpPathBytes.constData(), 0, 0, 0, GDT_Unknown, nullptr));

        if (!outputDataset) {
            error = "结果 SHP 创建失败：" + shpPath;
            return false;
        }

        char** layerOptions = nullptr;
        layerOptions = CSLSetNameValue(layerOptions, "ENCODING", "UTF-8");

        OGRLayer* outputLayer = outputDataset->CreateLayer(
            layerNameForBucket(bucket),
            spatialReference.get(),
            geometryTypeForBucket(bucket),
            layerOptions);

        CSLDestroy(layerOptions);

        if (outputLayer == nullptr) {
            error = "结果图层创建失败：" + shpPath;
            return false;
        }

        if (!createOutputFields(outputLayer)) {
            error = "结果字段创建失败：" + shpPath;
            return false;
        }

        for (const FitTopologyIssue& issue : issues) {
            if (bucketOf(issue.geometry.get()) != bucket)
                continue;

            OGRFeatureDefn* definition = outputLayer->GetLayerDefn();
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                outputFeature(OGRFeature::CreateFeature(definition), OGRFeature::DestroyFeature);

            const QByteArray typeBytes = issue.type.toUtf8();
            const QByteArray layerNameBytes = issue.sourceLayerName.toUtf8();
            const QByteArray messageBytes = issue.message.toUtf8();

            outputFeature->SetField("ptype", typeBytes.constData());
            outputFeature->SetField("src_layer", layerNameBytes.constData());
            outputFeature->SetField("src_fid", issue.sourceFid);
            outputFeature->SetField("dist", issue.distance);
            outputFeature->SetField("area", issue.area);
            outputFeature->SetField("tol", tolerance);
            outputFeature->SetField("msg", messageBytes.constData());
            outputFeature->SetGeometry(issue.geometry.get());

            if (outputLayer->CreateFeature(outputFeature.get()) != OGRERR_NONE) {
                error = "写入问题要素失败：" + shpPath;
                return false;
            }
        }

        outputLayer->SyncToDisk();
        outputPaths << shpPath;

        if (log) {
            log(QString("输出问题 SHP：%1，数量=%2。")
                .arg(shpPath)
                .arg(bucketCount));
        }
    }

    return true;
}

QByteArray se_administrative_division_fit_check_algorithm::toGdalPath(
    const QString& path)
{
    // GDALOpenEx/Create 使用 char* 路径，这里统一按 UTF-8 传入。
    return path.toUtf8();
}

