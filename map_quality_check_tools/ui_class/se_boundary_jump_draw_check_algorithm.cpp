#include "se_boundary_jump_draw_check_algorithm.h"
#include "ui_se_boundary_jump_draw_check_algorithm.h"

#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#include <QApplication>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QSet>
#include <QStringList>
#include <QTextEdit>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "cpl_string.h"

namespace
{
    const int kMaxProblemLogCount = 20;

    // 比例尺分母直接写在代码里，1:145000 就填 145000。
    // XML 中的 0.3mm、3-5mm 都是图上距离；OGR 的 Distance/Buffer 使用图层坐标单位。
    // 当前默认图层坐标单位为米，因此 0.3mm 在 1:145000 下换算为 43.5m。
    const int kDefaultMapScaleDenominator = 145000;

    enum class IssueGeometryBucket
    {
        Point,
        Line,
        Polygon,
        Other
    };

    JumpGdalDatasetPtr openVectorDataset(const QString& path, int flags = GDAL_OF_VECTOR | GDAL_OF_READONLY)
    {
        return JumpGdalDatasetPtr(static_cast<GDALDataset*>(
            GDALOpenEx(path.toUtf8().constData(), flags, nullptr, nullptr, nullptr)));
    }

    QString selectShpPath(QWidget* parent, const QString& title)
    {
        const QString path = QFileDialog::getOpenFileName(
            parent,
            title,
            QString(),
            "Shapefile (*.shp);;所有文件 (*.*)");

        if (path.isEmpty())
            return QString();

        if (QFileInfo(path).suffix().compare("shp", Qt::CaseInsensitive) != 0) {
            QMessageBox::warning(parent, "提示", "请选择 .shp 格式的矢量数据文件。");
            return QString();
        }

        return path;
    }

    QStringList selectShpPaths(QWidget* parent, const QString& title)
    {
        const QStringList paths = QFileDialog::getOpenFileNames(
            parent,
            title,
            QString(),
            "Shapefile (*.shp);;所有文件 (*.*)");

        for (const QString& path : paths) {
            if (QFileInfo(path).suffix().compare("shp", Qt::CaseInsensitive) != 0) {
                QMessageBox::warning(parent, "提示", "请选择 .shp 格式的矢量数据文件。");
                return QStringList();
            }
        }

        return paths;
    }

    QStringList splitDatasetPaths(const QString& pathText)
    {
        QStringList paths = pathText.split(";", QString::SkipEmptyParts);
        for (QString& path : paths)
            path = path.trimmed();

        paths.removeAll(QString());
        return paths;
    }

    QVector<double> parseRuleNumbers(const QString& text)
    {
        QVector<double> numbers;
        QString normalizedText = text;
        normalizedText.replace(',', ';');
        normalizedText.replace(QChar(0xFF0C), ';');
        const QStringList parts = normalizedText.split(";", QString::SkipEmptyParts);

        // XML 中兼容英文/中文逗号和分号分隔的多个数值。
        for (const QString& part : parts) {
            bool ok = false;
            const double number = part.trimmed().toDouble(&ok);
            if (ok)
                numbers.push_back(number);
        }

        std::sort(numbers.begin(), numbers.end());
        return numbers;
    }

    double mapMmToLayerUnit(double mm, double scaleDenominator)
    {
        // 图上 mm -> 实地 m；调用方再换算为成果图层的线性坐标单位。
        return mm / 1000.0 * scaleDenominator;
    }

    bool toleranceToLayerUnit(
        double value,
        const QString& unit,
        double scaleDenominator,
        double coordinateUnitsPerMetre,
        double& result)
    {
        const QString normalizedUnit = unit.trimmed().toLower();
        if (normalizedUnit.isEmpty() || normalizedUnit == "mm" || normalizedUnit == "毫米") {
            result = mapMmToLayerUnit(value, scaleDenominator) * coordinateUnitsPerMetre;
            return true;
        }
        if (normalizedUnit == "cm" || normalizedUnit == "厘米") {
            result = mapMmToLayerUnit(value * 10.0, scaleDenominator) * coordinateUnitsPerMetre;
            return true;
        }
        if (normalizedUnit == "m" || normalizedUnit == "米") {
            result = value * coordinateUnitsPerMetre;
            return true;
        }

        return false;
    }

    double layerUnitsPerMetre(const QString& datasetPath, QString& error)
    {
        JumpGdalDatasetPtr dataset = openVectorDataset(datasetPath);
        if (!dataset || dataset->GetLayerCount() <= 0) {
            error = "无法读取成果境界线数据的空间参考：" + datasetPath;
            return 0.0;
        }

        OGRLayer* layer = dataset->GetLayer(0);
        OGRSpatialReference* spatialReference = layer == nullptr ? nullptr : layer->GetSpatialRef();
        if (spatialReference == nullptr || !spatialReference->IsProjected()) {
            error = "跳绘检查要求成果境界线数据使用投影坐标系，不能直接对经纬度坐标进行距离缓冲。";
            return 0.0;
        }

        const double metresPerUnit = spatialReference->GetLinearUnits();
        if (metresPerUnit <= 0.0) {
            error = "无法识别成果境界线数据的线性坐标单位。";
            return 0.0;
        }

        return 1.0 / metresPerUnit;
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

    bool isPolygonGeometry(const OGRGeometry* geometry)
    {
        if (geometry == nullptr)
            return false;

        const OGRwkbGeometryType flatType = wkbFlatten(geometry->getGeometryType());
        if (flatType == wkbPolygon || flatType == wkbMultiPolygon)
            return true;

        if (!isCollectionType(geometry->getGeometryType()))
            return false;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return false;

        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            if (isPolygonGeometry(collection->getGeometryRef(i)))
                return true;
        }

        return false;
    }

    double lineStringLength(const OGRLineString* line)
    {
        if (line == nullptr || line->getNumPoints() < 2)
            return 0.0;

        double length = 0.0;
        for (int i = 1; i < line->getNumPoints(); ++i) {
            OGRPoint startPoint;
            OGRPoint endPoint;
            line->getPoint(i - 1, &startPoint);
            line->getPoint(i, &endPoint);

            const double dx = endPoint.getX() - startPoint.getX();
            const double dy = endPoint.getY() - startPoint.getY();
            length += std::sqrt(dx * dx + dy * dy);
        }

        return length;
    }

    double geometryLineLength(const OGRGeometry* geometry)
    {
        if (geometry == nullptr || geometry->IsEmpty())
            return 0.0;

        const OGRwkbGeometryType flatType = wkbFlatten(geometry->getGeometryType());
        if (flatType == wkbLineString) {
            const auto* line = dynamic_cast<const OGRLineString*>(geometry);
            return lineStringLength(line);
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return 0.0;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return 0.0;

        double length = 0.0;
        for (int i = 0; i < collection->getNumGeometries(); ++i)
            length += geometryLineLength(collection->getGeometryRef(i));

        return length;
    }

    void updateNearestReferenceDirection(
        const OGRLineString* candidateSegment,
        const OGRGeometry* referenceGeometry,
        double& nearestDistance,
        double& nearestDirectionCosine,
        bool& foundReferenceSegment)
    {
        if (candidateSegment == nullptr || referenceGeometry == nullptr
            || referenceGeometry->IsEmpty()) {
            return;
        }

        const OGRwkbGeometryType flatType =
            wkbFlatten(referenceGeometry->getGeometryType());
        if (flatType == wkbLineString) {
            const auto* referenceLine =
                dynamic_cast<const OGRLineString*>(referenceGeometry);
            if (referenceLine == nullptr)
                return;

            OGRPoint candidateStart;
            OGRPoint candidateEnd;
            candidateSegment->getPoint(0, &candidateStart);
            candidateSegment->getPoint(candidateSegment->getNumPoints() - 1, &candidateEnd);
            const double candidateDx = candidateEnd.getX() - candidateStart.getX();
            const double candidateDy = candidateEnd.getY() - candidateStart.getY();
            const double candidateLength = std::sqrt(
                candidateDx * candidateDx + candidateDy * candidateDy);
            if (candidateLength <= 1e-8)
                return;

            for (int i = 1; i < referenceLine->getNumPoints(); ++i) {
                OGRPoint referenceStart;
                OGRPoint referenceEnd;
                referenceLine->getPoint(i - 1, &referenceStart);
                referenceLine->getPoint(i, &referenceEnd);

                const double referenceDx = referenceEnd.getX() - referenceStart.getX();
                const double referenceDy = referenceEnd.getY() - referenceStart.getY();
                const double referenceLength = std::sqrt(
                    referenceDx * referenceDx + referenceDy * referenceDy);
                if (referenceLength <= 1e-8)
                    continue;

                OGRLineString referenceSegment;
                referenceSegment.addPoint(referenceStart.getX(), referenceStart.getY());
                referenceSegment.addPoint(referenceEnd.getX(), referenceEnd.getY());
                const double distance = candidateSegment->Distance(&referenceSegment);
                if (distance < 0.0 || (foundReferenceSegment && distance >= nearestDistance))
                    continue;

                nearestDistance = distance;
                nearestDirectionCosine = std::fabs(
                    (candidateDx * referenceDx + candidateDy * referenceDy)
                    / (candidateLength * referenceLength));
                foundReferenceSegment = true;
            }
            return;
        }

        if (!isCollectionType(referenceGeometry->getGeometryType()))
            return;

        const auto* collection =
            dynamic_cast<const OGRGeometryCollection*>(referenceGeometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            updateNearestReferenceDirection(
                candidateSegment,
                collection->getGeometryRef(i),
                nearestDistance,
                nearestDirectionCosine,
                foundReferenceSegment);
        }
    }

    bool isDirectionCompatibleWithReference(
        const OGRLineString* candidateSegment,
        const OGRGeometry* referenceGeometry,
        double minimumDirectionCosine)
    {
        if (candidateSegment == nullptr || candidateSegment->getNumPoints() < 2
            || referenceGeometry == nullptr || referenceGeometry->IsEmpty()) {
            return true;
        }

        double nearestDistance = 0.0;
        double nearestDirectionCosine = 0.0;
        bool foundReferenceSegment = false;
        updateNearestReferenceDirection(
            candidateSegment,
            referenceGeometry,
            nearestDistance,
            nearestDirectionCosine,
            foundReferenceSegment);

        return !foundReferenceSegment
            || nearestDirectionCosine >= minimumDirectionCosine;
    }

    void addDirectionCompatibleCandidateSegments(
        const OGRGeometry* geometry,
        const OGRGeometry* referenceGeometry,
        double minimumDirectionCosine,
        OGRMultiLineString* retainedLines)
    {
        if (geometry == nullptr || geometry->IsEmpty() || retainedLines == nullptr)
            return;

        const OGRwkbGeometryType flatType =
            wkbFlatten(geometry->getGeometryType());
        if (flatType == wkbLineString) {
            const auto* line = dynamic_cast<const OGRLineString*>(geometry);
            if (line == nullptr)
                return;

            for (int i = 1; i < line->getNumPoints(); ++i) {
                OGRPoint startPoint;
                OGRPoint endPoint;
                line->getPoint(i - 1, &startPoint);
                line->getPoint(i, &endPoint);

                OGRLineString segment;
                segment.addPoint(startPoint.getX(), startPoint.getY());
                segment.addPoint(endPoint.getX(), endPoint.getY());
                if (lineStringLength(&segment) > 1e-8
                    && isDirectionCompatibleWithReference(
                        &segment, referenceGeometry, minimumDirectionCosine)) {
                    retainedLines->addGeometry(&segment);
                }
            }
            return;
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            addDirectionCompatibleCandidateSegments(
                collection->getGeometryRef(i),
                referenceGeometry,
                minimumDirectionCosine,
                retainedLines);
        }
    }

    JumpOgrGeometryPtr retainDirectionCompatibleCandidateSegments(
        const OGRGeometry* candidateGeometry,
        const OGRGeometry* referenceGeometry,
        double minimumDirectionCosine)
    {
        if (candidateGeometry == nullptr || candidateGeometry->IsEmpty())
            return nullptr;

        JumpOgrGeometryPtr retainedGeometry(
            OGRGeometryFactory::createGeometry(wkbMultiLineString));
        auto* retainedLines =
            dynamic_cast<OGRMultiLineString*>(retainedGeometry.get());
        if (retainedLines == nullptr)
            return nullptr;

        addDirectionCompatibleCandidateSegments(
            candidateGeometry,
            referenceGeometry,
            minimumDirectionCosine,
            retainedLines);
        if (retainedLines->getNumGeometries() <= 0)
            return nullptr;

        return retainedGeometry;
    }

    // 原始关联线段与综合水系面必须存在实际线长度的重叠；仅端点接触不建立匹配。
    bool hasLineOverlapWithArea(
        const OGRGeometry* lineGeometry,
        const OGRGeometry* areaGeometry)
    {
        if (lineGeometry == nullptr || areaGeometry == nullptr
            || lineGeometry->IsEmpty() || areaGeometry->IsEmpty()) {
            return false;
        }

        JumpOgrGeometryPtr overlapGeometry(
            lineGeometry->Intersection(areaGeometry));
        return geometryLineLength(overlapGeometry.get()) > 1e-8;
    }

    // 只有水系面覆盖整条理论关联链，才允许其替代线状表达参与跳绘段数检查。
    // 线仅局部进入水面时，水面只能约束该局部的绘制距离和连通，不能把整条
    // 连续链（包括远离水面的部分）误判为水系面跳绘区。
    bool doesAreaCoverWholeLine(
        const OGRGeometry* lineGeometry,
        const OGRGeometry* areaGeometry,
        double topologyTolerance)
    {
        if (lineGeometry == nullptr || areaGeometry == nullptr
            || lineGeometry->IsEmpty() || areaGeometry->IsEmpty()) {
            return false;
        }

        const double tolerance = std::max(topologyTolerance, 1e-8);
        JumpOgrGeometryPtr tolerantArea(areaGeometry->Buffer(tolerance));
        const OGRGeometry* checkingArea = tolerantArea && !tolerantArea->IsEmpty()
            ? tolerantArea.get() : areaGeometry;
        JumpOgrGeometryPtr outsideGeometry(
            lineGeometry->Difference(checkingArea));
        return outsideGeometry
            && (outsideGeometry->IsEmpty()
            || geometryLineLength(outsideGeometry.get()) <= tolerance);
    }

    bool isPointOnGeometryWithTolerance(
        const OGRPoint& point,
        const OGRGeometry* geometry,
        double tolerance)
    {
        return geometry != nullptr
            && !geometry->IsEmpty()
            && point.Distance(geometry) <= tolerance;
    }

    // 水系面外的候选境界线局部段若仅在一个端点接到水面边界，属于从水面接出
    // 的普通境界线，不作为“应完全落入水系面”的错误输出。
    void addNonBranchExteriorLineParts(
        const OGRGeometry* geometry,
        const OGRGeometry* areaBoundary,
        double topologyTolerance,
        OGRMultiLineString* retainedLines)
    {
        if (geometry == nullptr || geometry->IsEmpty() || retainedLines == nullptr)
            return;

        const OGRwkbGeometryType flatType =
            wkbFlatten(geometry->getGeometryType());
        if (flatType == wkbLineString) {
            const auto* line = dynamic_cast<const OGRLineString*>(geometry);
            if (line == nullptr || line->getNumPoints() < 2)
                return;

            OGRPoint startPoint;
            OGRPoint endPoint;
            line->getPoint(0, &startPoint);
            line->getPoint(line->getNumPoints() - 1, &endPoint);

            const bool startOnAreaBoundary = isPointOnGeometryWithTolerance(
                startPoint, areaBoundary, topologyTolerance);
            const bool endOnAreaBoundary = isPointOnGeometryWithTolerance(
                endPoint, areaBoundary, topologyTolerance);
            if (startOnAreaBoundary != endOnAreaBoundary)
                return;

            retainedLines->addGeometry(line);
            return;
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            addNonBranchExteriorLineParts(
                collection->getGeometryRef(i),
                areaBoundary,
                topologyTolerance,
                retainedLines);
        }
    }

    JumpOgrGeometryPtr removeOneEndedExteriorAreaBranches(
        const OGRGeometry* exteriorGeometry,
        const OGRGeometry* areaGeometry,
        double topologyTolerance)
    {
        if (exteriorGeometry == nullptr || exteriorGeometry->IsEmpty()
            || areaGeometry == nullptr || areaGeometry->IsEmpty()) {
            return nullptr;
        }

        JumpOgrGeometryPtr areaBoundary(areaGeometry->Boundary());
        if (!areaBoundary || areaBoundary->IsEmpty())
            return JumpOgrGeometryPtr(exteriorGeometry->clone());

        JumpOgrGeometryPtr retainedGeometry(
            OGRGeometryFactory::createGeometry(wkbMultiLineString));
        auto* retainedLines =
            dynamic_cast<OGRMultiLineString*>(retainedGeometry.get());
        if (retainedLines == nullptr)
            return nullptr;

        addNonBranchExteriorLineParts(
            exteriorGeometry,
            areaBoundary.get(),
            topologyTolerance,
            retainedLines);
        if (retainedLines->getNumGeometries() <= 0)
            return nullptr;

        return retainedGeometry;
    }

    // 成果境界线被搜索缓冲区截出的局部段若首尾均落在缓冲区外边界，说明它只是
    // 穿过较大搜索范围的无关线，不作为该原始关联段的水系面候选。
    void addLinePartsNotCrossingSearchBuffer(
        const OGRGeometry* geometry,
        const OGRGeometry* searchBufferBoundary,
        OGRMultiLineString* retainedLines)
    {
        if (geometry == nullptr || geometry->IsEmpty()
            || searchBufferBoundary == nullptr || searchBufferBoundary->IsEmpty()
            || retainedLines == nullptr) {
            return;
        }

        const OGRwkbGeometryType flatType = wkbFlatten(geometry->getGeometryType());
        if (flatType == wkbLineString) {
            const auto* line = dynamic_cast<const OGRLineString*>(geometry);
            if (line == nullptr || line->getNumPoints() < 2)
                return;

            OGRPoint startPoint;
            OGRPoint endPoint;
            line->getPoint(0, &startPoint);
            line->getPoint(line->getNumPoints() - 1, &endPoint);
            const bool startOnSearchBoundary =
                startPoint.Distance(searchBufferBoundary) <= 1e-8;
            const bool endOnSearchBoundary =
                endPoint.Distance(searchBufferBoundary) <= 1e-8;
            if (startOnSearchBoundary && endOnSearchBoundary)
                return;

            retainedLines->addGeometry(line);
            return;
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            addLinePartsNotCrossingSearchBuffer(
                collection->getGeometryRef(i),
                searchBufferBoundary,
                retainedLines);
        }
    }

    JumpOgrGeometryPtr removeLinePartsCrossingSearchBuffer(
        const OGRGeometry* candidateGeometry,
        const OGRGeometry* searchBufferGeometry)
    {
        if (candidateGeometry == nullptr || candidateGeometry->IsEmpty()
            || searchBufferGeometry == nullptr || searchBufferGeometry->IsEmpty()) {
            return nullptr;
        }

        JumpOgrGeometryPtr searchBufferBoundary(searchBufferGeometry->Boundary());
        if (!searchBufferBoundary || searchBufferBoundary->IsEmpty())
            return JumpOgrGeometryPtr(candidateGeometry->clone());

        JumpOgrGeometryPtr retainedGeometry(
            OGRGeometryFactory::createGeometry(wkbMultiLineString));
        auto* retainedLines = dynamic_cast<OGRMultiLineString*>(retainedGeometry.get());
        if (retainedLines == nullptr)
            return nullptr;

        addLinePartsNotCrossingSearchBuffer(
            candidateGeometry,
            searchBufferBoundary.get(),
            retainedLines);
        if (retainedLines->getNumGeometries() <= 0)
            return nullptr;

        return retainedGeometry;
    }

    void addPointParts(
        const OGRGeometry* geometry,
        std::vector<OGRPoint>& points)
    {
        if (geometry == nullptr || geometry->IsEmpty())
            return;

        const OGRwkbGeometryType flatType = wkbFlatten(geometry->getGeometryType());
        if (flatType == wkbPoint) {
            const auto* point = dynamic_cast<const OGRPoint*>(geometry);
            if (point != nullptr)
                points.push_back(*point);
            return;
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i)
            addPointParts(collection->getGeometryRef(i), points);
    }

    void addUniqueTopologyPoint(
        std::vector<OGRPoint>& points,
        const OGRPoint& point)
    {
        for (const OGRPoint& existingPoint : points) {
            if (existingPoint.Distance(&point) <= 1e-8)
                return;
        }

        points.push_back(point);
    }

    bool hasLineLengthAtTopologyNode(
        const OGRGeometry* lineGeometry,
        const OGRGeometry* clippingGeometry,
        const OGRPoint& node)
    {
        if (lineGeometry == nullptr || clippingGeometry == nullptr
            || lineGeometry->IsEmpty() || clippingGeometry->IsEmpty()) {
            return false;
        }

        JumpOgrGeometryPtr clippedLine(lineGeometry->Intersection(clippingGeometry));
        if (!clippedLine || clippedLine->IsEmpty()
            || geometryLineLength(clippedLine.get()) <= 1e-8) {
            return false;
        }

        return node.Distance(clippedLine.get()) <= 1e-8;
    }

    // 水系面边界处按严格拓扑节点检查：同一交点必须同时连接面内、面外的
    // 成果境界线局部段。这里不使用 XML 容差，避免把有缝隙的线误判为连通。
    bool hasContinuousBoundaryAcrossWaterArea(
        const std::vector<JumpOgrGeometryPtr>& nearbyBoundaryGeometries,
        const OGRGeometry* waterArea,
        const OGRPoint& node)
    {
        if (waterArea == nullptr || waterArea->IsEmpty())
            return true;

        bool hasInsideLine = false;
        bool hasOutsideLine = false;
        for (const JumpOgrGeometryPtr& geometry : nearbyBoundaryGeometries) {
            if (!geometry || geometry->IsEmpty())
                continue;

            hasInsideLine = hasInsideLine || hasLineLengthAtTopologyNode(
                geometry.get(), waterArea, node);
            JumpOgrGeometryPtr outsideGeometry(geometry->Difference(waterArea));
            hasOutsideLine = hasOutsideLine || (
                outsideGeometry && !outsideGeometry->IsEmpty()
                && hasLineLengthAtTopologyNode(
                    outsideGeometry.get(), outsideGeometry.get(), node));

            if (hasInsideLine && hasOutsideLine)
                return true;
        }

        return false;
    }

    void addLinePartsToMultiLine(
        const OGRGeometry* geometry,
        OGRMultiLineString* multiLine)
    {
        if (geometry == nullptr || geometry->IsEmpty() || multiLine == nullptr)
            return;

        const OGRwkbGeometryType flatType = wkbFlatten(geometry->getGeometryType());
        if (flatType == wkbLineString) {
            if (geometryLineLength(geometry) <= 0.0)
                return;

            multiLine->addGeometry(geometry);
            return;
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i)
            addLinePartsToMultiLine(collection->getGeometryRef(i), multiLine);
    }

    void addRingToMultiLine(
        const OGRLinearRing* ring,
        OGRMultiLineString* multiLine)
    {
        if (ring == nullptr || multiLine == nullptr || ring->getNumPoints() < 2)
            return;

        OGRLineString line;
        for (int i = 0; i < ring->getNumPoints(); ++i) {
            OGRPoint point;
            ring->getPoint(i, &point);
            line.addPoint(point.getX(), point.getY());
        }

        if (lineStringLength(&line) > 0.0)
            multiLine->addGeometry(&line);
    }

    void addAdminBoundaryPartsToMultiLine(
        const OGRGeometry* geometry,
        OGRMultiLineString* multiLine)
    {
        if (geometry == nullptr || geometry->IsEmpty() || multiLine == nullptr)
            return;

        const OGRwkbGeometryType flatType = wkbFlatten(geometry->getGeometryType());
        if (flatType == wkbLineString || flatType == wkbMultiLineString) {
            addLinePartsToMultiLine(geometry, multiLine);
            return;
        }

        if (flatType == wkbPolygon) {
            const auto* polygon = dynamic_cast<const OGRPolygon*>(geometry);
            if (polygon == nullptr)
                return;

            addRingToMultiLine(polygon->getExteriorRing(), multiLine);
            for (int i = 0; i < polygon->getNumInteriorRings(); ++i)
                addRingToMultiLine(polygon->getInteriorRing(i), multiLine);

            return;
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i)
            addAdminBoundaryPartsToMultiLine(collection->getGeometryRef(i), multiLine);
    }

    // 综合后水系既可能是线，也可能以面表达。面要素以其边界线参与匹配和距离判定。
    JumpOgrGeometryPtr extractLinearGeometry(const OGRGeometry* geometry)
    {
        if (geometry == nullptr || geometry->IsEmpty())
            return nullptr;

        JumpOgrGeometryPtr linearGeometry(
            OGRGeometryFactory::createGeometry(wkbMultiLineString));
        auto* multiLine = dynamic_cast<OGRMultiLineString*>(linearGeometry.get());
        if (multiLine == nullptr)
            return nullptr;

        addAdminBoundaryPartsToMultiLine(geometry, multiLine);
        if (multiLine->getNumGeometries() <= 0)
            return nullptr;

        return linearGeometry;
    }

    enum class JumpDrawType
    {
        Unknown,
        SameSide,
        OppositeSide
    };

    struct JumpDrawExpectation
    {
        JumpDrawType type = JumpDrawType::Unknown;
        double zoneLength = 0.0;
        int groupCount = 0;
        int expectedSegmentCount = 0;
        OGRPoint startPoint; // 原始有效重叠段端点 P1
        OGRPoint endPoint;   // 原始有效重叠段端点 P2
    };

    void findLongestLineString(
        const OGRGeometry* geometry,
        const OGRLineString*& longestLine,
        double& longestLength)
    {
        if (geometry == nullptr || geometry->IsEmpty())
            return;

        const OGRwkbGeometryType flatType = wkbFlatten(geometry->getGeometryType());
        if (flatType == wkbLineString) {
            const auto* line = dynamic_cast<const OGRLineString*>(geometry);
            const double length = lineStringLength(line);
            if (line != nullptr && length > longestLength) {
                longestLine = line;
                longestLength = length;
            }
            return;
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            findLongestLineString(
                collection->getGeometryRef(i), longestLine, longestLength);
        }
    }

    bool locatePointAlongLineString(
        const OGRPoint& point,
        const OGRLineString* line,
        double& measure)
    {
        if (line == nullptr || line->getNumPoints() < 2)
            return false;

        double nearestDistanceSquared = std::numeric_limits<double>::max();
        double accumulatedLength = 0.0;
        measure = 0.0;
        for (int i = 1; i < line->getNumPoints(); ++i) {
            OGRPoint firstPoint;
            OGRPoint secondPoint;
            line->getPoint(i - 1, &firstPoint);
            line->getPoint(i, &secondPoint);
            const double dx = secondPoint.getX() - firstPoint.getX();
            const double dy = secondPoint.getY() - firstPoint.getY();
            const double lengthSquared = dx * dx + dy * dy;
            if (lengthSquared <= 1e-16)
                continue;

            const double projection = std::max(0.0, std::min(1.0,
                ((point.getX() - firstPoint.getX()) * dx
                    + (point.getY() - firstPoint.getY()) * dy) / lengthSquared));
            const double projectedX = firstPoint.getX() + projection * dx;
            const double projectedY = firstPoint.getY() + projection * dy;
            const double distanceX = point.getX() - projectedX;
            const double distanceY = point.getY() - projectedY;
            const double distanceSquared = distanceX * distanceX + distanceY * distanceY;
            const double segmentLength = std::sqrt(lengthSquared);
            if (distanceSquared < nearestDistanceSquared) {
                nearestDistanceSquared = distanceSquared;
                measure = accumulatedLength + projection * segmentLength;
            }
            accumulatedLength += segmentLength;
        }
        return nearestDistanceSquared < std::numeric_limits<double>::max();
    }

    bool extractJumpZoneEndpoints(
        const OGRGeometry* intersectionGeometry,
        const OGRGeometry* sourceLineGeometry,
        OGRPoint& startPoint,
        OGRPoint& endPoint,
        double& jumpZoneLength)
    {
        const OGRLineString* jumpZoneLine = nullptr;
        jumpZoneLength = 0.0;
        findLongestLineString(intersectionGeometry, jumpZoneLine, jumpZoneLength);
        if (jumpZoneLine != nullptr && jumpZoneLength > 1e-8) {
            jumpZoneLine->getPoint(0, &startPoint);
            jumpZoneLine->getPoint(jumpZoneLine->getNumPoints() - 1, &endPoint);
            return true;
        }

        std::vector<OGRPoint> intersectionPoints;
        addPointParts(intersectionGeometry, intersectionPoints);
        if (intersectionPoints.size() < 2)
            return false;

        const OGRLineString* sourceLine = nullptr;
        double sourceLength = 0.0;
        findLongestLineString(sourceLineGeometry, sourceLine, sourceLength);
        if (sourceLine == nullptr || sourceLength <= 1e-8)
            return false;

        bool found = false;
        double minimumMeasure = 0.0;
        double maximumMeasure = 0.0;
        OGRPoint minimumPoint;
        OGRPoint maximumPoint;
        for (const OGRPoint& intersectionPoint : intersectionPoints) {
            double measure = 0.0;
            if (!locatePointAlongLineString(intersectionPoint, sourceLine, measure))
                continue;
            if (!found || measure < minimumMeasure) {
                minimumMeasure = measure;
                minimumPoint = intersectionPoint;
            }
            if (!found || measure > maximumMeasure) {
                maximumMeasure = measure;
                maximumPoint = intersectionPoint;
            }
            found = true;
        }
        if (!found || maximumMeasure - minimumMeasure <= 1e-8)
            return false;

        startPoint = minimumPoint;
        endPoint = maximumPoint;
        jumpZoneLength = maximumMeasure - minimumMeasure;
        return true;
    }

    void addDetectedJumpEndpoint(
        std::vector<DetectedJumpEndpoint>& endpoints,
        const DetectedJumpEndpoint& endpoint,
        double topologyTolerance)
    {
        const double tolerance = std::max(topologyTolerance, 1e-8);
        for (const DetectedJumpEndpoint& existing : endpoints) {
            if (existing.endpointName != endpoint.endpointName
                || existing.endpointSource != endpoint.endpointSource
                || existing.lineLayerName != endpoint.lineLayerName
                || existing.lineFid != endpoint.lineFid) {
                continue;
            }

            if (existing.point.Distance(&endpoint.point) <= tolerance)
                return;
        }

        endpoints.push_back(endpoint);
    }

    int countLineComponents(const OGRGeometry* geometry)
    {
        if (geometry == nullptr || geometry->IsEmpty())
            return 0;

        const OGRwkbGeometryType flatType = wkbFlatten(geometry->getGeometryType());
        if (flatType == wkbLineString)
            return lineStringLength(dynamic_cast<const OGRLineString*>(geometry)) > 1e-8 ? 1 : 0;

        if (!isCollectionType(geometry->getGeometryType()))
            return 0;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return 0;

        int count = 0;
        for (int i = 0; i < collection->getNumGeometries(); ++i)
            count += countLineComponents(collection->getGeometryRef(i));
        return count;
    }

    struct LinePartEndpoints
    {
        OGRPoint startPoint;
        OGRPoint endPoint;
    };

    void collectLinePartEndpoints(
        const OGRGeometry* geometry,
        std::vector<LinePartEndpoints>& parts)
    {
        if (geometry == nullptr || geometry->IsEmpty())
            return;

        const OGRwkbGeometryType flatType = wkbFlatten(geometry->getGeometryType());
        if (flatType == wkbLineString) {
            const auto* line = dynamic_cast<const OGRLineString*>(geometry);
            if (line == nullptr || line->getNumPoints() < 2
                || lineStringLength(line) <= 1e-8) {
                return;
            }

            LinePartEndpoints part;
            line->getPoint(0, &part.startPoint);
            line->getPoint(line->getNumPoints() - 1, &part.endPoint);
            parts.push_back(part);
            return;
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i)
            collectLinePartEndpoints(collection->getGeometryRef(i), parts);
    }

    bool extractAssociatedLineChainEndpoints(
        const OGRGeometry* lineGeometry,
        double topologyTolerance,
        OGRPoint& startPoint,
        OGRPoint& endPoint,
        double& lineLength)
    {
        std::vector<LinePartEndpoints> parts;
        collectLinePartEndpoints(lineGeometry, parts);
        if (parts.empty())
            return false;

        std::vector<OGRPoint> endpoints;
        for (const LinePartEndpoints& part : parts) {
            endpoints.push_back(part.startPoint);
            endpoints.push_back(part.endPoint);
        }

        const double tolerance = std::max(topologyTolerance, 1e-8);
        std::vector<OGRPoint> outerEndpoints;
        for (size_t i = 0; i < endpoints.size(); ++i) {
            int sameLocationCount = 0;
            for (size_t j = 0; j < endpoints.size(); ++j) {
                if (endpoints[i].Distance(&endpoints[j]) <= tolerance)
                    ++sameLocationCount;
            }
            if (sameLocationCount == 1)
                outerEndpoints.push_back(endpoints[i]);
        }
        if (outerEndpoints.size() < 2)
            return false;

        double maximumDistance = -1.0;
        for (size_t i = 0; i < outerEndpoints.size(); ++i) {
            for (size_t j = i + 1; j < outerEndpoints.size(); ++j) {
                const double distance = outerEndpoints[i].Distance(&outerEndpoints[j]);
                if (distance > maximumDistance) {
                    maximumDistance = distance;
                    startPoint = outerEndpoints[i];
                    endPoint = outerEndpoints[j];
                }
            }
        }
        lineLength = geometryLineLength(lineGeometry);
        return maximumDistance > 1e-8 && lineLength > 1e-8;
    }

    int countConnectedLineComponents(
        const OGRGeometry* geometry,
        double connectionTolerance)
    {
        std::vector<LinePartEndpoints> parts;
        collectLinePartEndpoints(geometry, parts);
        if (parts.empty())
            return 0;

        const double tolerance = std::max(connectionTolerance, 1e-8);
        std::vector<bool> visited(parts.size(), false);
        int componentCount = 0;
        for (size_t startIndex = 0; startIndex < parts.size(); ++startIndex) {
            if (visited[startIndex])
                continue;

            ++componentCount;
            std::vector<size_t> pending;
            pending.push_back(startIndex);
            visited[startIndex] = true;
            while (!pending.empty()) {
                const size_t currentIndex = pending.back();
                pending.pop_back();
                const LinePartEndpoints& current = parts[currentIndex];
                for (size_t candidateIndex = 0;
                    candidateIndex < parts.size(); ++candidateIndex) {
                    if (visited[candidateIndex])
                        continue;

                    const LinePartEndpoints& candidate = parts[candidateIndex];
                    const bool connected =
                        current.startPoint.Distance(&candidate.startPoint) <= tolerance
                        || current.startPoint.Distance(&candidate.endPoint) <= tolerance
                        || current.endPoint.Distance(&candidate.startPoint) <= tolerance
                        || current.endPoint.Distance(&candidate.endPoint) <= tolerance;
                    if (!connected)
                        continue;

                    visited[candidateIndex] = true;
                    pending.push_back(candidateIndex);
                }
            }
        }

        return componentCount;
    }

    // 不先 Union，直接按各成果线裁剪段的真实端点连通关系计数。Union 或将
    // 端点投影到弯曲参考线后再合并，都会把彼此未接上的两条成果线误并成一段。
    int countConnectedLineComponents(
        const std::vector<JumpOgrGeometryPtr>& geometries,
        double connectionTolerance)
    {
        std::vector<LinePartEndpoints> parts;
        for (const JumpOgrGeometryPtr& geometry : geometries)
            collectLinePartEndpoints(geometry.get(), parts);
        if (parts.empty())
            return 0;

        const double tolerance = std::max(connectionTolerance, 1e-8);
        std::vector<bool> visited(parts.size(), false);
        int componentCount = 0;
        for (size_t startIndex = 0; startIndex < parts.size(); ++startIndex) {
            if (visited[startIndex])
                continue;

            ++componentCount;
            std::vector<size_t> pending;
            pending.push_back(startIndex);
            visited[startIndex] = true;
            while (!pending.empty()) {
                const size_t currentIndex = pending.back();
                pending.pop_back();
                const LinePartEndpoints& current = parts[currentIndex];
                for (size_t candidateIndex = 0;
                    candidateIndex < parts.size(); ++candidateIndex) {
                    if (visited[candidateIndex])
                        continue;

                    const LinePartEndpoints& candidate = parts[candidateIndex];
                    const bool connected =
                        current.startPoint.Distance(&candidate.startPoint) <= tolerance
                        || current.startPoint.Distance(&candidate.endPoint) <= tolerance
                        || current.endPoint.Distance(&candidate.startPoint) <= tolerance
                        || current.endPoint.Distance(&candidate.endPoint) <= tolerance;
                    if (!connected)
                        continue;

                    visited[candidateIndex] = true;
                    pending.push_back(candidateIndex);
                }
            }
        }
        return componentCount;
    }

    void updateReferenceSideFromGeometry(
        const OGRGeometry* geometry,
        const OGRPoint& point,
        double& nearestDistance,
        double& nearestCross,
        bool& foundSegment)
    {
        if (geometry == nullptr || geometry->IsEmpty())
            return;

        const OGRwkbGeometryType flatType = wkbFlatten(geometry->getGeometryType());
        if (flatType == wkbLineString) {
            const auto* line = dynamic_cast<const OGRLineString*>(geometry);
            if (line == nullptr)
                return;

            for (int i = 1; i < line->getNumPoints(); ++i) {
                OGRPoint startPoint;
                OGRPoint endPoint;
                line->getPoint(i - 1, &startPoint);
                line->getPoint(i, &endPoint);
                const double dx = endPoint.getX() - startPoint.getX();
                const double dy = endPoint.getY() - startPoint.getY();
                if (std::sqrt(dx * dx + dy * dy) <= 1e-8)
                    continue;

                OGRLineString segment;
                segment.addPoint(startPoint.getX(), startPoint.getY());
                segment.addPoint(endPoint.getX(), endPoint.getY());
                const double distance = point.Distance(&segment);
                if (distance < 0.0 || (foundSegment && distance >= nearestDistance))
                    continue;

                nearestDistance = distance;
                nearestCross = dx * (point.getY() - startPoint.getY())
                    - dy * (point.getX() - startPoint.getX());
                foundSegment = true;
            }
            return;
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            updateReferenceSideFromGeometry(
                collection->getGeometryRef(i), point, nearestDistance,
                nearestCross, foundSegment);
        }
    }

    int sideOfPointRelativeToReference(
        const OGRPoint& point,
        const OGRGeometry* referenceGeometry)
    {
        if (referenceGeometry == nullptr || referenceGeometry->IsEmpty())
            return 0;

        double nearestDistance = 0.0;
        double nearestCross = 0.0;
        bool foundSegment = false;
        updateReferenceSideFromGeometry(
            referenceGeometry, point, nearestDistance, nearestCross, foundSegment);
        if (!foundSegment || std::fabs(nearestCross) <= 1e-8)
            return 0;
        return nearestCross > 0.0 ? 1 : -1;
    }

    void updateSideNearEndpoint(
        const OGRGeometry* geometry,
        const OGRPoint& endpoint,
        const OGRGeometry* referenceGeometry,
        double& nearestDistance,
        int& side,
        bool& found)
    {
        if (geometry == nullptr || geometry->IsEmpty())
            return;

        const OGRwkbGeometryType flatType = wkbFlatten(geometry->getGeometryType());
        if (flatType == wkbLineString) {
            const auto* line = dynamic_cast<const OGRLineString*>(geometry);
            if (line == nullptr || line->getNumPoints() < 2)
                return;

            for (int i = 1; i < line->getNumPoints(); ++i) {
                OGRPoint startPoint;
                OGRPoint endPoint;
                line->getPoint(i - 1, &startPoint);
                line->getPoint(i, &endPoint);

                OGRLineString segment;
                segment.addPoint(startPoint.getX(), startPoint.getY());
                segment.addPoint(endPoint.getX(), endPoint.getY());
                const double distance = endpoint.Distance(&segment);
                if (distance < 0.0 || (found && distance >= nearestDistance))
                    continue;

                OGRPoint middlePoint(
                    (startPoint.getX() + endPoint.getX()) / 2.0,
                    (startPoint.getY() + endPoint.getY()) / 2.0);
                const int candidateSide = sideOfPointRelativeToReference(
                    middlePoint, referenceGeometry);
                if (candidateSide == 0)
                    continue;

                nearestDistance = distance;
                side = candidateSide;
                found = true;
            }
            return;
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            updateSideNearEndpoint(
                collection->getGeometryRef(i), endpoint, referenceGeometry,
                nearestDistance, side, found);
        }
    }

    bool determineJumpDrawExpectation(
        const OGRGeometry* adminBoundaryGeometry,
        const BoundaryLineRelation& relation,
        const BoundaryJumpRule& rule,
        JumpDrawExpectation& expectation)
    {
        if (adminBoundaryGeometry == nullptr || relation.sourceLineGeometry == nullptr
            || relation.sourceLineGeometry->IsEmpty() || rule.boundarySymbolLength <= 0.0
            || rule.maxJumpStep <= 0.0) {
            return false;
        }

        // 理论跳绘区端点取原始有效关联线局部链的外侧端点。该局部链已由
        // 原始红绿要素的距离、重叠长度和连通关系筛选，首尾即图上跳绘区两端。
        double jumpZoneLength = 0.0;
        OGRPoint startPoint;
        OGRPoint endPoint;
        if (!extractAssociatedLineChainEndpoints(
            relation.lineGeometry.get(), rule.topologyTolerance,
            startPoint, endPoint, jumpZoneLength)) {
            return false;
        }
        expectation.startPoint = startPoint;
        expectation.endPoint = endPoint;

        JumpOgrGeometryPtr nonJumpGeometry(
            adminBoundaryGeometry->Difference(relation.overlapGeometry.get()));
        if (!nonJumpGeometry || nonJumpGeometry->IsEmpty())
            return false;

        double startNearestDistance = 0.0;
        double endNearestDistance = 0.0;
        int startSide = 0;
        int endSide = 0;
        bool foundStartSide = false;
        bool foundEndSide = false;
        updateSideNearEndpoint(
            nonJumpGeometry.get(), startPoint, relation.lineGeometry.get(),
            startNearestDistance, startSide, foundStartSide);
        updateSideNearEndpoint(
            nonJumpGeometry.get(), endPoint, relation.lineGeometry.get(),
            endNearestDistance, endSide, foundEndSide);
        if (!foundStartSide || !foundEndSide)
            return false;

        expectation.type = startSide == endSide
            ? JumpDrawType::SameSide : JumpDrawType::OppositeSide;
        expectation.zoneLength = jumpZoneLength;

        const double d = rule.boundarySymbolLength;
        if (expectation.type == JumpDrawType::SameSide) {
            if (jumpZoneLength < 2.0 * d) {
                expectation.expectedSegmentCount = 1;
                return true;
            }
            if (jumpZoneLength < 6.0 * d) {
                expectation.expectedSegmentCount = 3;
                return true;
            }

            const int m = static_cast<int>(std::ceil(jumpZoneLength / rule.maxJumpStep));
            expectation.groupCount = std::max(1, static_cast<int>(std::ceil((m - 1) / 4.0)));
            expectation.expectedSegmentCount = 4 * expectation.groupCount + 1;
            return true;
        }

        if (jumpZoneLength < 2.0 * d) {
            expectation.expectedSegmentCount = 1;
            return true;
        }
        if (jumpZoneLength < 4.0 * d) {
            expectation.expectedSegmentCount = 3;
            return true;
        }

        const int m = static_cast<int>(std::ceil(jumpZoneLength / rule.maxJumpStep));
        expectation.groupCount = std::max(1, static_cast<int>(std::ceil((m + 1) / 4.0)));
        expectation.expectedSegmentCount = 4 * expectation.groupCount - 1;
        return true;
    }

    struct SourceGdbBoundaryFeature
    {
        QString layerName;
        GIntBig fid = -1;
        JumpOgrGeometryPtr geometry;

        SourceGdbBoundaryFeature() = default;
        SourceGdbBoundaryFeature(SourceGdbBoundaryFeature&&) noexcept = default;
        SourceGdbBoundaryFeature& operator=(SourceGdbBoundaryFeature&&) noexcept = default;

        SourceGdbBoundaryFeature(const SourceGdbBoundaryFeature&) = delete;
        SourceGdbBoundaryFeature& operator=(const SourceGdbBoundaryFeature&) = delete;
    };

    bool collectSourceGdbBoundaryFeatures(
        GDALDataset* adminDataset,
        const BoundaryJumpRule& rule,
        std::vector<SourceGdbBoundaryFeature>& features,
        QString& error)
    {
        if (adminDataset == nullptr) {
            error = "原始 GDB 境界线数据为空。";
            return false;
        }

        bool foundAttribute = false;
        for (int layerIndex = 0; layerIndex < adminDataset->GetLayerCount(); ++layerIndex) {
            OGRLayer* layer = adminDataset->GetLayer(layerIndex);
            if (layer == nullptr)
                continue;

            const int fieldIndex = layer->GetLayerDefn()->GetFieldIndex(
                rule.missingBoundaryAttributeName.toUtf8().constData());
            if (fieldIndex < 0)
                continue;

            foundAttribute = true;
            const QString layerName = QString::fromUtf8(layer->GetName());
            layer->ResetReading();
            OGRFeature* rawFeature = nullptr;
            while ((rawFeature = layer->GetNextFeature()) != nullptr) {
                std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                    feature(rawFeature, OGRFeature::DestroyFeature);
                const QString attributeValue = QString::fromUtf8(
                    feature->GetFieldAsString(fieldIndex)).trimmed();
                if (attributeValue != rule.missingBoundaryAttributeValue)
                    continue;

                JumpOgrGeometryPtr linearGeometry(
                    extractLinearGeometry(feature->GetGeometryRef()));
                if (!linearGeometry || linearGeometry->IsEmpty()
                    || geometryLineLength(linearGeometry.get()) <= 1e-8) {
                    continue;
                }

                SourceGdbBoundaryFeature sourceFeature;
                sourceFeature.layerName = layerName;
                sourceFeature.fid = feature->GetFID();
                sourceFeature.geometry = std::move(linearGeometry);
                features.push_back(std::move(sourceFeature));
            }
        }

        if (!foundAttribute) {
            error = QString("原始 GDB 境界线数据中未找到属性项“%1”。")
                .arg(rule.missingBoundaryAttributeName);
            return false;
        }
        if (features.empty()) {
            error = QString("原始 GDB 境界线数据中未找到属性“%1=%2”的线状境界要素。")
                .arg(rule.missingBoundaryAttributeName)
                .arg(rule.missingBoundaryAttributeValue);
            return false;
        }

        return true;
    }

    class AdminBoundaryAnalyzer
    {
    public:
        // 从行政界线 SHP 提取原始行政界线。
        // 以线状要素为主；同时兼容 Polygon/MultiPolygon 的外环和内环，便于历史数据复用。
        // 该结果只用于判断水系、道路等线状要素是否作为行政界线，不代表最终绘制成果。
        JumpOgrGeometryPtr extractBoundary(
            GDALDataset* adminDataset,
            const BoundaryJumpRule& rule)
        {
            if (adminDataset == nullptr)
                return nullptr;

            JumpOgrGeometryPtr boundaryGeometry(OGRGeometryFactory::createGeometry(wkbMultiLineString));
            auto* multiLine = dynamic_cast<OGRMultiLineString*>(boundaryGeometry.get());
            if (multiLine == nullptr)
                return nullptr;

            for (int layerIndex = 0; layerIndex < adminDataset->GetLayerCount(); ++layerIndex) {
                OGRLayer* layer = adminDataset->GetLayer(layerIndex);
                if (layer == nullptr)
                    continue;

                const int attributeIndex = layer->GetLayerDefn()->GetFieldIndex(
                    rule.missingBoundaryAttributeName.toUtf8().constData());
                if (attributeIndex < 0)
                    continue;

                layer->ResetReading();
                OGRFeature* rawFeature = nullptr;
                while ((rawFeature = layer->GetNextFeature()) != nullptr) {
                    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                        feature(rawFeature, OGRFeature::DestroyFeature);

                    const QString attributeValue = QString::fromUtf8(
                        feature->GetFieldAsString(attributeIndex)).trimmed();
                    if (attributeValue != rule.missingBoundaryAttributeValue)
                        continue;

                    OGRGeometry* geometry = feature->GetGeometryRef();
                    if (geometry == nullptr || geometry->IsEmpty()) {
                        continue;
                    }

                    if (!isPolygonGeometry(geometry) && !isLineGeometry(geometry)) {
                        continue;
                    }

                    addAdminBoundaryPartsToMultiLine(geometry, multiLine);
                }
            }

            if (multiLine->getNumGeometries() <= 0)
                return nullptr;

            return boundaryGeometry;
        }
    };

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
            break;
        }

        return IssueGeometryBucket::Other;
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

        // Shapefile 字段名最长约 10 个字符，字段名保持短英文，中文说明放在 msg。
        createField(layer, "ptype", OFTString, 32);
        createField(layer, "bd_layer", OFTString, 64);
        createField(layer, "bd_fid", OFTInteger64);
        createField(layer, "oth_layer", OFTString, 64);
        createField(layer, "oth_fid", OFTInteger64);
        createField(layer, "ovlp_len", OFTReal, 18, 8);
        // 保留旧字段供既有结果读取器使用；新规则下其值为闭区间上限。
        createField(layer, "draw_dist", OFTReal, 18, 8);
        createField(layer, "draw_min", OFTReal, 18, 8);
        createField(layer, "draw_max", OFTReal, 18, 8);
        createField(layer, "offset_dis", OFTReal, 18, 8);
        createField(layer, "msg", OFTString, 254);

        return true;
    }

    QString firstSentenceForOutput(const QString& message)
    {
        const int semicolonIndex = message.indexOf(QChar(0xFF1B));
        return (semicolonIndex < 0 ? message : message.left(semicolonIndex)).trimmed();
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
            return "jump_point";

        case IssueGeometryBucket::Line:
            return "jump_line";

        case IssueGeometryBucket::Polygon:
            return "jump_polygon";

        default:
            return "jump_other";
        }
    }

    OGRwkbGeometryType geometryTypeForBucket(IssueGeometryBucket bucket)
    {
        switch (bucket)
        {
        case IssueGeometryBucket::Point:
            // 同一成果要素的同一种点错误可能包含多个定位点，统一作为
            // 一条 MultiPoint 记录输出。
            return wkbMultiPoint;

        case IssueGeometryBucket::Line:
            // Shapefile 的 PolyLine 支持多部件线。同一成果要素的同一种
            // 线错误统一作为一条 MultiLineString 记录输出。
            return wkbMultiLineString;

        case IssueGeometryBucket::Polygon:
            return wkbMultiPolygon;

        default:
            return wkbUnknown;
        }
    }

    class ResultWriter
    {
    public:
        explicit ResultWriter(std::vector<BoundaryJumpIssue>& issues)
            : m_issues(issues)
        {
        }

        // 同一条成果境界线可能分别命中多条关联线状要素。先按成果要素汇总并集，
        // 消除重复和重叠的错误段；真正不连续的部分在写出时才分别输出。
        void addIssue(
            const QString& type,
            const QString& boundaryLayerName,
            GIntBig boundaryFid,
            const QString& otherLayerName,
            GIntBig otherFid,
            const QString& message,
            const OGRGeometry* geometry,
            const QString& mergeKey = QString())
        {
            if (geometry == nullptr || geometry->IsEmpty())
                return;

            for (BoundaryJumpIssue& issue : m_issues) {
                if (issue.type != type
                    || issue.boundaryLayerName != boundaryLayerName
                    || issue.boundaryFid != boundaryFid
                    || issue.mergeKey != mergeKey
                    || !issue.geometry) {
                    continue;
                }

                // 不以关联线要素 FID 作为去重条件：同一成果境界线可对应多条关联线。
                JumpOgrGeometryPtr mergedGeometry(issue.geometry->Union(geometry));
                if (mergedGeometry && !mergedGeometry->IsEmpty()) {
                    issue.geometry = std::move(mergedGeometry);
                    // 同一跳绘区可同时关联综合线的多个分段。合并后不能继续
                    // 假称只对应首条分段 FID，-1 表示多个综合表达要素。
                    if (issue.type == "JUMP_BREAK_ERR"
                        && (issue.otherLayerName != otherLayerName
                            || issue.otherFid != otherFid)) {
                        if (issue.otherLayerName != otherLayerName)
                            issue.otherLayerName = "综合表达";
                        issue.otherFid = -1;
                    }
                    return;
                }

                // 极少数无效几何无法做并集时，保留当前错误段，避免漏检。
                break;
            }

            BoundaryJumpIssue issue;
            issue.type = type;
            issue.boundaryLayerName = boundaryLayerName;
            issue.boundaryFid = boundaryFid;
            issue.otherLayerName = otherLayerName;
            issue.otherFid = otherFid;
            issue.mergeKey = mergeKey;
            issue.message = message.left(254);
            issue.geometry.reset(geometry->clone());

            m_issues.push_back(std::move(issue));
        }

        static bool hasGeometryPart(
            const OGRGeometry* geometry,
            IssueGeometryBucket bucket)
        {
            if (geometry == nullptr || geometry->IsEmpty())
                return false;

            if (isCollectionType(geometry->getGeometryType())) {
                const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
                if (collection == nullptr)
                    return false;

                for (int i = 0; i < collection->getNumGeometries(); ++i) {
                    if (hasGeometryPart(collection->getGeometryRef(i), bucket))
                        return true;
                }
                return false;
            }

            return bucketOf(geometry) == bucket;
        }

        static void appendGeometryParts(
            const OGRGeometry* geometry,
            IssueGeometryBucket bucket,
            OGRGeometryCollection* outputGeometry)
        {
            if (geometry == nullptr || geometry->IsEmpty()
                || outputGeometry == nullptr) {
                return;
            }

            if (isCollectionType(geometry->getGeometryType())) {
                const auto* collection =
                    dynamic_cast<const OGRGeometryCollection*>(geometry);
                if (collection == nullptr)
                    return;

                for (int i = 0; i < collection->getNumGeometries(); ++i) {
                    appendGeometryParts(
                        collection->getGeometryRef(i), bucket, outputGeometry);
                }
                return;
            }

            if (bucketOf(geometry) == bucket)
                outputGeometry->addGeometry(geometry);
        }

        static JumpOgrGeometryPtr collectGeometryParts(
            const OGRGeometry* geometry,
            IssueGeometryBucket bucket)
        {
            OGRwkbGeometryType outputType = wkbUnknown;
            switch (bucket) {
            case IssueGeometryBucket::Point:
                outputType = wkbMultiPoint;
                break;
            case IssueGeometryBucket::Line:
                outputType = wkbMultiLineString;
                break;
            case IssueGeometryBucket::Polygon:
                outputType = wkbMultiPolygon;
                break;
            default:
                return nullptr;
            }

            JumpOgrGeometryPtr collectedGeometry(
                OGRGeometryFactory::createGeometry(outputType));
            auto* collection = dynamic_cast<OGRGeometryCollection*>(
                collectedGeometry.get());
            if (collection == nullptr)
                return nullptr;

            appendGeometryParts(geometry, bucket, collection);
            if (collection->getNumGeometries() <= 0)
                return nullptr;

            return collectedGeometry;
        }

        static bool writeGeometryParts(
            OGRLayer* outputLayer,
            const BoundaryJumpIssue& issue,
            const BoundaryJumpRule& rule,
            IssueGeometryBucket bucket,
            const OGRGeometry* geometry,
            QString& error)
        {
            if (geometry == nullptr || geometry->IsEmpty())
                return true;

            // 同一 BoundaryJumpIssue 已经按“成果图层 + 成果 FID + 错误类型”
            // 汇总。这里把其中同一几何类型的所有部件收集为一个多部件几何，
            // 只写一条属性记录，避免按顶点小段生成大量重复行。
            JumpOgrGeometryPtr outputGeometry =
                collectGeometryParts(geometry, bucket);
            if (!outputGeometry || outputGeometry->IsEmpty())
                return true;

            OGRFeatureDefn* definition = outputLayer->GetLayerDefn();
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                outputFeature(OGRFeature::CreateFeature(definition), OGRFeature::DestroyFeature);

            if (!outputFeature) {
                error = "创建问题要素失败。";
                return false;
            }

            const QByteArray typeBytes = issue.type.toUtf8();
            const QByteArray boundaryLayerBytes = issue.boundaryLayerName.toUtf8();
            const QByteArray otherLayerBytes = issue.otherLayerName.toUtf8();
            const QByteArray messageBytes =
                firstSentenceForOutput(issue.message).toUtf8();

            outputFeature->SetField("ptype", typeBytes.constData());
            outputFeature->SetField("bd_layer", boundaryLayerBytes.constData());
            outputFeature->SetField("bd_fid", issue.boundaryFid);
            outputFeature->SetField("oth_layer", otherLayerBytes.constData());
            outputFeature->SetField("oth_fid", issue.otherFid);
            outputFeature->SetField("ovlp_len", rule.overlapLength);
            outputFeature->SetField("draw_dist", rule.drawDistanceMax);
            outputFeature->SetField("draw_min", rule.drawDistanceMin);
            outputFeature->SetField("draw_max", rule.drawDistanceMax);
            outputFeature->SetField("offset_dis", rule.nonJumpOffsetDistance);
            outputFeature->SetField("msg", messageBytes.constData());
            outputFeature->SetGeometry(outputGeometry.get());

            if (outputLayer->CreateFeature(outputFeature.get()) != OGRERR_NONE) {
                error = "写入问题要素失败。";
                return false;
            }

            return true;
        }

        // 统一输出跳绘距离错误 Shapefile，按几何类型输出 point/line/polygon 三份文件。
        static bool writeProblemShapefiles(
            const QString& outputBaseShpPath,
            const std::vector<BoundaryJumpIssue>& issues,
            const BoundaryJumpRule& rule,
            const QString& boundaryPath,
            QStringList& outputPaths,
            QStringList& logs,
            QString& error)
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

            std::unique_ptr<OGRSpatialReference> spatialReference =
                readSpatialReference(boundaryPath);

            const IssueGeometryBucket buckets[] =
            {
                IssueGeometryBucket::Point,
                IssueGeometryBucket::Line,
                IssueGeometryBucket::Polygon
            };

            for (IssueGeometryBucket bucket : buckets) {
                int bucketCount = 0;

                // 每个已汇总问题在当前几何类型的结果图层中最多输出一条记录。
                for (const BoundaryJumpIssue& issue : issues) {
                    if (hasGeometryPart(issue.geometry.get(), bucket))
                        ++bucketCount;
                }

                if (bucketCount == 0)
                    continue;

                const QString shpPath = outputPathForBucket(outputBaseShpPath, bucket);
                const QByteArray shpPathBytes = shpPath.toUtf8();

                if (QFileInfo(shpPath).exists())
                    driver->Delete(shpPathBytes.constData());

                JumpGdalDatasetPtr outputDataset(
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

                for (const BoundaryJumpIssue& issue : issues) {
                    if (!writeGeometryParts(
                        outputLayer,
                        issue,
                        rule,
                        bucket,
                        issue.geometry.get(),
                        error)) {
                        error += "：" + shpPath;
                        return false;
                    }
                }

                outputLayer->SyncToDisk();
                outputPaths << shpPath;

                logs << QString("输出问题 SHP：%1，数量=%2。")
                    .arg(shpPath)
                    .arg(bucketCount);
            }

            return true;
        }

    private:
        static std::unique_ptr<OGRSpatialReference> readSpatialReference(
            const QString& shpPath)
        {
            JumpGdalDatasetPtr dataset = openVectorDataset(shpPath);
            if (!dataset || dataset->GetLayerCount() <= 0)
                return nullptr;

            OGRLayer* layer = dataset->GetLayer(0);
            if (layer == nullptr || layer->GetSpatialRef() == nullptr)
                return nullptr;

            return std::unique_ptr<OGRSpatialReference>(layer->GetSpatialRef()->Clone());
        }

        std::vector<BoundaryJumpIssue>& m_issues;
    };

    // 跳绘区端点是辅助排查成果，单独输出，避免与错误图层混在一起。
    bool writeDetectedJumpEndpointShapefile(
        const QString& outputBaseShpPath,
        const std::vector<DetectedJumpEndpoint>& endpoints,
        const BoundaryJumpRule& rule,
        const QString& boundaryPath,
        QStringList& outputPaths,
        QStringList& logs,
        QString& error)
    {
        QString normalizedPath = outputBaseShpPath;
        if (QFileInfo(normalizedPath).suffix().compare("shp", Qt::CaseInsensitive) != 0)
            normalizedPath += ".shp";

        const QFileInfo outputInfo(normalizedPath);
        QString endpointBaseName = outputInfo.completeBaseName();
        if (endpointBaseName.endsWith("_errors", Qt::CaseInsensitive))
            endpointBaseName.chop(7);
        const QString endpointPath = QDir(outputInfo.absolutePath()).filePath(
            endpointBaseName + "_jump_endpoints.shp");
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
        if (driver == nullptr) {
            error = "当前 GDAL 环境不支持 ESRI Shapefile 驱动。";
            return false;
        }

        JumpGdalDatasetPtr boundaryDataset = openVectorDataset(boundaryPath);
        std::unique_ptr<OGRSpatialReference> spatialReference;
        if (boundaryDataset && boundaryDataset->GetLayerCount() > 0) {
            OGRLayer* boundaryLayer = boundaryDataset->GetLayer(0);
            if (boundaryLayer != nullptr && boundaryLayer->GetSpatialRef() != nullptr)
                spatialReference.reset(boundaryLayer->GetSpatialRef()->Clone());
        }

        const QByteArray endpointPathBytes = endpointPath.toUtf8();
        if (QFileInfo(endpointPath).exists())
            driver->Delete(endpointPathBytes.constData());

        JumpGdalDatasetPtr outputDataset(
            driver->Create(endpointPathBytes.constData(), 0, 0, 0, GDT_Unknown, nullptr));
        if (!outputDataset) {
            error = "跳绘区端点 SHP 创建失败：" + endpointPath;
            return false;
        }

        char** layerOptions = nullptr;
        layerOptions = CSLSetNameValue(layerOptions, "ENCODING", "UTF-8");
        OGRLayer* outputLayer = outputDataset->CreateLayer(
            "jump_endpoint", spatialReference.get(), wkbPoint, layerOptions);
        CSLDestroy(layerOptions);
        if (outputLayer == nullptr) {
            error = "跳绘区端点图层创建失败：" + endpointPath;
            return false;
        }

        createField(outputLayer, "pt_name", OFTString, 4);
        createField(outputLayer, "pt_src", OFTString, 24);
        createField(outputLayer, "raw_layer", OFTString, 64);
        createField(outputLayer, "raw_fid", OFTInteger64);
        createField(outputLayer, "ovlp_len", OFTReal, 18, 3);
        createField(outputLayer, "msg", OFTString, 128);

        // 相邻原始线要素分段在同一位置首尾相接时，该位置是重叠链的内部
        // 节点，不是跳绘区 P1、P2。仅保留每条连续重叠链的外侧端点。
        std::vector<bool> internalEndpoint(endpoints.size(), false);
        const double endpointTolerance = std::max(rule.topologyTolerance, 1e-8);
        for (size_t i = 0; i < endpoints.size(); ++i) {
            for (size_t j = i + 1; j < endpoints.size(); ++j) {
                if (endpoints[i].lineLayerName == endpoints[j].lineLayerName
                    && endpoints[i].lineFid == endpoints[j].lineFid
                    && endpoints[i].endpointSource == endpoints[j].endpointSource) {
                    continue;
                }

                if (endpoints[i].endpointSource != endpoints[j].endpointSource)
                    continue;

                if (endpoints[i].point.Distance(&endpoints[j].point) <= endpointTolerance) {
                    internalEndpoint[i] = true;
                    internalEndpoint[j] = true;
                }
            }
        }

        int outputEndpointCount = 0;
        for (size_t endpointIndex = 0; endpointIndex < endpoints.size(); ++endpointIndex) {
            if (internalEndpoint[endpointIndex])
                continue;

            const DetectedJumpEndpoint& endpoint = endpoints[endpointIndex];
            OGRFeatureDefn* definition = outputLayer->GetLayerDefn();
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                outputFeature(OGRFeature::CreateFeature(definition), OGRFeature::DestroyFeature);
            if (!outputFeature) {
                error = "跳绘区端点要素创建失败。";
                return false;
            }

            outputFeature->SetField("pt_name", endpoint.endpointName.toUtf8().constData());
            outputFeature->SetField("pt_src", endpoint.endpointSource.toUtf8().constData());
            outputFeature->SetField("raw_layer", endpoint.lineLayerName.toUtf8().constData());
            outputFeature->SetField("raw_fid", endpoint.lineFid);
            outputFeature->SetField("ovlp_len", endpoint.overlapLength);
            const QString message = QString("%1：%2。")
                .arg(endpoint.endpointSource)
                .arg(endpoint.endpointName);
            outputFeature->SetField("msg", message.toUtf8().constData());
            outputFeature->SetGeometry(&endpoint.point);

            if (outputLayer->CreateFeature(outputFeature.get()) != OGRERR_NONE) {
                error = "跳绘区端点写入失败：" + endpointPath;
                return false;
            }
            ++outputEndpointCount;
        }

        outputLayer->SyncToDisk();
        outputPaths << endpointPath;
        logs << QString("输出跳绘区端点 SHP：%1，端点数量=%2。")
            .arg(endpointPath)
            .arg(outputEndpointCount);
        return true;
    }

    // 对未参与跳绘表达的原始 GDB 境界线做缺失和偏移检查：以该境界线建立
    // 搜索缓冲区，收集附近成果境界线形成覆盖带。搜索缓冲区内有成果但未落入
    // “偏移距离”覆盖带的部分记为偏移错误；搜索缓冲区内也没有成果的部分记为缺失。
    bool collectBoundaryMissingIssues(
        GDALDataset* boundaryDataset,
        GDALDataset* adminDataset,
        const std::vector<BoundaryLineRelation>& jumpRelations,
        const BoundaryJumpRule& rule,
        std::vector<BoundaryJumpIssue>& issues,
        QStringList& logs,
        QString& error)
    {
        if (boundaryDataset == nullptr || adminDataset == nullptr) {
            error = "境界缺失检查输入数据为空。";
            return false;
        }

        std::vector<SourceGdbBoundaryFeature> sourceFeatures;
        if (!collectSourceGdbBoundaryFeatures(
            adminDataset, rule, sourceFeatures, error)) {
            return false;
        }

        // 已参与跳绘检查的行政境界重叠段不再进行常规境界缺失检查。
        JumpOgrGeometryPtr jumpGeometry;
        for (const BoundaryLineRelation& relation : jumpRelations) {
            if (!relation.overlapGeometry || relation.overlapGeometry->IsEmpty())
                continue;

            if (!jumpGeometry) {
                jumpGeometry.reset(relation.overlapGeometry->clone());
                continue;
            }

            JumpOgrGeometryPtr mergedGeometry(
                jumpGeometry->Union(relation.overlapGeometry.get()));
            if (mergedGeometry && !mergedGeometry->IsEmpty())
                jumpGeometry = std::move(mergedGeometry);
        }

        ResultWriter resultWriter(issues);
        int checkedFeatureCount = 0;
        int missingFeatureCount = 0;
        int offsetFeatureCount = 0;
        for (const SourceGdbBoundaryFeature& sourceFeature : sourceFeatures) {
            JumpOgrGeometryPtr unmatchedGdbGeometry(sourceFeature.geometry->clone());
            if (jumpGeometry && !jumpGeometry->IsEmpty()) {
                JumpOgrGeometryPtr differenceGeometry(
                    sourceFeature.geometry->Difference(jumpGeometry.get()));
                if (!differenceGeometry || differenceGeometry->IsEmpty()
                    || geometryLineLength(differenceGeometry.get()) <= 1e-8) {
                    continue;
                }
                unmatchedGdbGeometry = std::move(differenceGeometry);
            }

            JumpOgrGeometryPtr gdbSearchBuffer(
                unmatchedGdbGeometry->Buffer(rule.missingBoundaryBufferRadius));
            if (!gdbSearchBuffer || gdbSearchBuffer->IsEmpty())
                continue;

            ++checkedFeatureCount;
            JumpOgrGeometryPtr resultCoverageGeometry;
            JumpOgrGeometryPtr offsetCoverageGeometry;
            for (int layerIndex = 0;
                layerIndex < boundaryDataset->GetLayerCount();
                ++layerIndex) {
                OGRLayer* boundaryLayer = boundaryDataset->GetLayer(layerIndex);
                if (boundaryLayer == nullptr)
                    continue;

                boundaryLayer->SetSpatialFilter(gdbSearchBuffer.get());
                boundaryLayer->ResetReading();
                OGRFeature* rawBoundaryFeature = nullptr;
                while ((rawBoundaryFeature = boundaryLayer->GetNextFeature()) != nullptr) {
                    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                        boundaryFeature(rawBoundaryFeature, OGRFeature::DestroyFeature);
                    OGRGeometry* boundaryGeometry = boundaryFeature->GetGeometryRef();
                    if (!isLineGeometry(boundaryGeometry))
                        continue;

                    JumpOgrGeometryPtr localBoundaryGeometry(
                        boundaryGeometry->Intersection(gdbSearchBuffer.get()));
                    if (!localBoundaryGeometry || localBoundaryGeometry->IsEmpty()
                        || geometryLineLength(localBoundaryGeometry.get()) <= 1e-8) {
                        continue;
                    }

                    JumpOgrGeometryPtr offsetCoveragePart(
                        localBoundaryGeometry->Buffer(rule.nonJumpOffsetDistance));
                    if (offsetCoveragePart && !offsetCoveragePart->IsEmpty()) {
                        if (!offsetCoverageGeometry) {
                            offsetCoverageGeometry = std::move(offsetCoveragePart);
                        }
                        else {
                            JumpOgrGeometryPtr mergedOffsetCoverageGeometry(
                                offsetCoverageGeometry->Union(offsetCoveragePart.get()));
                            if (mergedOffsetCoverageGeometry
                                && !mergedOffsetCoverageGeometry->IsEmpty()) {
                                offsetCoverageGeometry =
                                    std::move(mergedOffsetCoverageGeometry);
                            }
                        }
                    }

                    JumpOgrGeometryPtr coveragePart(
                        localBoundaryGeometry->Buffer(rule.missingBoundaryBufferRadius));
                    if (!coveragePart || coveragePart->IsEmpty())
                        continue;

                    if (!resultCoverageGeometry) {
                        resultCoverageGeometry = std::move(coveragePart);
                        continue;
                    }

                    JumpOgrGeometryPtr mergedCoverageGeometry(
                        resultCoverageGeometry->Union(coveragePart.get()));
                    if (mergedCoverageGeometry && !mergedCoverageGeometry->IsEmpty())
                        resultCoverageGeometry = std::move(mergedCoverageGeometry);
                }
                boundaryLayer->SetSpatialFilter(nullptr);
            }

            JumpOgrGeometryPtr missingGeometry;
            if (resultCoverageGeometry && !resultCoverageGeometry->IsEmpty()) {
                missingGeometry.reset(
                    unmatchedGdbGeometry->Difference(resultCoverageGeometry.get()));
            }
            else {
                missingGeometry.reset(unmatchedGdbGeometry->clone());
            }

            if (!missingGeometry || missingGeometry->IsEmpty()
                || geometryLineLength(missingGeometry.get()) <= 1e-8) {
                missingGeometry.reset();
            }

            // 只在“缺失检查搜索范围内确实存在成果境界线”的库体区段上判断
            // 偏移，避免把完全缺失的同一段同时报成缺失和偏移。
            if (resultCoverageGeometry && !resultCoverageGeometry->IsEmpty()) {
                JumpOgrGeometryPtr presentGdbGeometry(
                    unmatchedGdbGeometry->Intersection(resultCoverageGeometry.get()));
                JumpOgrGeometryPtr offsetGeometry;
                if (presentGdbGeometry && !presentGdbGeometry->IsEmpty()) {
                    if (offsetCoverageGeometry && !offsetCoverageGeometry->IsEmpty()) {
                        offsetGeometry.reset(presentGdbGeometry->Difference(
                            offsetCoverageGeometry.get()));
                    }
                    else {
                        offsetGeometry.reset(presentGdbGeometry->clone());
                    }
                }

                if (offsetGeometry && !offsetGeometry->IsEmpty()
                    && geometryLineLength(offsetGeometry.get()) > 1e-8) {
                    ++offsetFeatureCount;
                    const QString offsetMessage = QString(
                        "非跳绘区成果境界线相对库体数据的偏移距离超过%1mm（图层单位阈值=%2）；属性%3=%4。")
                        .arg(rule.nonJumpOffsetDistanceMm, 0, 'f', 3)
                        .arg(rule.nonJumpOffsetDistance, 0, 'f', 3)
                        .arg(rule.missingBoundaryAttributeName)
                        .arg(rule.missingBoundaryAttributeValue);
                    resultWriter.addIssue(
                        "NON_JUMP_OFFSET_ERR",
                        sourceFeature.layerName,
                        sourceFeature.fid,
                        "成果境界线",
                        -1,
                        offsetMessage,
                        offsetGeometry.get());
                }
            }

            if (missingGeometry) {
                ++missingFeatureCount;
                const QString issueMessage = QString(
                    "原始 GDB 境界线未参与跳绘且未被成果境界线覆盖；属性%1=%2，缓冲区半径=%3。")
                    .arg(rule.missingBoundaryAttributeName)
                    .arg(rule.missingBoundaryAttributeValue)
                    .arg(rule.missingBoundaryBufferRadius, 0, 'f', 3);
                resultWriter.addIssue(
                    "BOUNDARY_MISSING_ERR",
                    sourceFeature.layerName,
                    sourceFeature.fid,
                    "成果境界线",
                    -1,
                    issueMessage,
                    missingGeometry.get());
            }
        }

        logs << QString("未参与跳绘的 GDB 境界缺失及偏移检查完成：属性%1=%2，检查要素=%3，缺失要素=%4，偏移超限要素=%5，缺失缓冲区半径=%6，偏移距离阈值=%7mm（图层单位=%8）。")
            .arg(rule.missingBoundaryAttributeName)
            .arg(rule.missingBoundaryAttributeValue)
            .arg(checkedFeatureCount)
            .arg(missingFeatureCount)
            .arg(offsetFeatureCount)
            .arg(rule.missingBoundaryBufferRadius, 0, 'f', 3)
            .arg(rule.nonJumpOffsetDistanceMm, 0, 'f', 3)
            .arg(rule.nonJumpOffsetDistance, 0, 'f', 3);
        return true;
    }

    void mergeGeometryInto(
        JumpOgrGeometryPtr& target,
        const OGRGeometry* geometry)
    {
        if (geometry == nullptr || geometry->IsEmpty())
            return;
        if (!target) {
            target.reset(geometry->clone());
            return;
        }
        JumpOgrGeometryPtr mergedGeometry(target->Union(geometry));
        if (mergedGeometry && !mergedGeometry->IsEmpty())
            target = std::move(mergedGeometry);
    }

    void mergeConnectedOriginalRelations(
        std::vector<BoundaryLineRelation>& relations,
        double topologyTolerance)
    {
        if (relations.size() < 2)
            return;

        std::vector<int> parent(relations.size());
        for (size_t i = 0; i < parent.size(); ++i)
            parent[i] = static_cast<int>(i);
        const auto findRoot = [&parent](int index) {
            int root = index;
            while (parent[static_cast<size_t>(root)] != root)
                root = parent[static_cast<size_t>(root)];
            while (parent[static_cast<size_t>(index)] != index) {
                const int next = parent[static_cast<size_t>(index)];
                parent[static_cast<size_t>(index)] = root;
                index = next;
            }
            return root;
        };

        const double tolerance = std::max(topologyTolerance, 1e-8);
        for (size_t i = 0; i < relations.size(); ++i) {
            if (!relations[i].lineGeometry)
                continue;
            for (size_t j = i + 1; j < relations.size(); ++j) {
                if (!relations[j].lineGeometry
                    || relations[i].lineLayerName != relations[j].lineLayerName
                    || relations[i].lineGeometry->Distance(relations[j].lineGeometry.get())
            > tolerance) {
                    continue;
                }
                const int leftRoot = findRoot(static_cast<int>(i));
                const int rightRoot = findRoot(static_cast<int>(j));
                if (leftRoot != rightRoot)
                    parent[static_cast<size_t>(rightRoot)] = leftRoot;
            }
        }

        std::vector<BoundaryLineRelation> chains;
        std::vector<int> groupSizes(relations.size(), 0);
        std::vector<int> rootToChain(relations.size(), -1);
        for (size_t i = 0; i < relations.size(); ++i)
            ++groupSizes[static_cast<size_t>(findRoot(static_cast<int>(i)))];

        for (size_t i = 0; i < relations.size(); ++i) {
            const int root = findRoot(static_cast<int>(i));
            int chainIndex = rootToChain[static_cast<size_t>(root)];
            if (chainIndex < 0) {
                BoundaryLineRelation chain;
                chain.lineLayerName = relations[i].lineLayerName;
                chain.lineFid = groupSizes[static_cast<size_t>(root)] == 1
                    ? relations[i].lineFid : -1;
                rootToChain[static_cast<size_t>(root)] =
                    static_cast<int>(chains.size());
                chains.push_back(std::move(chain));
                chainIndex = static_cast<int>(chains.size()) - 1;
            }

            BoundaryLineRelation& chain = chains[static_cast<size_t>(chainIndex)];
            chain.overlapLength += relations[i].overlapLength;
            mergeGeometryInto(chain.overlapGeometry, relations[i].overlapGeometry.get());
            mergeGeometryInto(chain.sourceLineGeometry, relations[i].sourceLineGeometry.get());
            mergeGeometryInto(chain.lineGeometry, relations[i].lineGeometry.get());
        }

        relations = std::move(chains);
    }

    // 同一原始连续关联链在同一水系面上可能因数据分块或重复要素命中而形成
    // 多条局部匹配记录。它们不能各自生成一组 P1/P2 或独立统计跳绘段数。
    void mergeDuplicateWaterAreaRelations(
        std::vector<BoundaryLineRelation>& relations)
    {
        if (relations.size() < 2)
            return;

        std::vector<BoundaryLineRelation> mergedRelations;
        for (BoundaryLineRelation& relation : relations) {
            if (!relation.generalizedIsArea || relation.originalRelationIndex < 0) {
                mergedRelations.push_back(std::move(relation));
                continue;
            }

            BoundaryLineRelation* mergedRelation = nullptr;
            for (BoundaryLineRelation& existing : mergedRelations) {
                if (existing.generalizedIsArea
                    && existing.originalRelationIndex == relation.originalRelationIndex
                    && existing.generalizedLineLayerName
                        == relation.generalizedLineLayerName
                    && existing.generalizedLineFid == relation.generalizedLineFid
                    && existing.waterAreaCoversJumpZone
                        == relation.waterAreaCoversJumpZone) {
                    mergedRelation = &existing;
                    break;
                }
            }

            if (mergedRelation == nullptr) {
                mergedRelations.push_back(std::move(relation));
                continue;
            }

            // 这些记录来自同一原始连续链，overlapLength 不应因重复命中累加。
            mergedRelation->overlapLength = std::max(
                mergedRelation->overlapLength, relation.overlapLength);
            mergeGeometryInto(
                mergedRelation->overlapGeometry, relation.overlapGeometry.get());
            mergeGeometryInto(
                mergedRelation->sourceLineGeometry, relation.sourceLineGeometry.get());
            mergeGeometryInto(
                mergedRelation->lineGeometry, relation.lineGeometry.get());
            mergeGeometryInto(
                mergedRelation->generalizedLineGeometry,
                relation.generalizedLineGeometry.get());
        }
        relations = std::move(mergedRelations);
    }

    // 综合后的河流、道路等线状要素通常会经过抽稀、平滑或延伸，不能要求它
    // 与原始跳绘区的局部缓冲区相交。下面这一组函数以完整原始线和完整综合线
    // 的路径相似性建立兜底关联；局部缓冲只保留给前面的强匹配和空间索引使用。
    void collectLineSamplePoints(
        const OGRGeometry* geometry,
        std::vector<OGRPoint>& points)
    {
        if (geometry == nullptr || geometry->IsEmpty())
            return;

        if (wkbFlatten(geometry->getGeometryType()) == wkbLineString) {
            const auto* line = dynamic_cast<const OGRLineString*>(geometry);
            if (line == nullptr)
                return;

            for (int i = 0; i < line->getNumPoints(); ++i) {
                OGRPoint point;
                line->getPoint(i, &point);
                points.push_back(point);
            }
            return;
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i)
            collectLineSamplePoints(collection->getGeometryRef(i), points);
    }

    double percentile(std::vector<double> values, double ratio)
    {
        if (values.empty())
            return std::numeric_limits<double>::max();

        std::sort(values.begin(), values.end());
        const size_t index = static_cast<size_t>(std::floor(
            std::max(0.0, std::min(1.0, ratio)) * (values.size() - 1)));
        return values[index];
    }

    double sampledLineCoverageRatio(
        const OGRGeometry* sourceGeometry,
        const OGRGeometry* targetGeometry,
        double distanceLimit,
        double& distanceP90)
    {
        std::vector<OGRPoint> points;
        collectLineSamplePoints(sourceGeometry, points);
        if (points.empty() || targetGeometry == nullptr || targetGeometry->IsEmpty()) {
            distanceP90 = std::numeric_limits<double>::max();
            return 0.0;
        }

        // 顶点非常密集的现状线不需要逐点参与评分；均匀抽样可避免波浪线因
        // 顶点数更多而过度影响结果。
        const size_t maxSampleCount = 256;
        const size_t step = std::max<size_t>(1, points.size() / maxSampleCount);
        int coveredCount = 0;
        int sampleCount = 0;
        std::vector<double> distances;
        for (size_t i = 0; i < points.size(); i += step) {
            const double distance = points[i].Distance(targetGeometry);
            if (distance < 0.0)
                continue;

            distances.push_back(distance);
            ++sampleCount;
            if (distance <= distanceLimit)
                ++coveredCount;
        }
        if (sampleCount <= 0) {
            distanceP90 = std::numeric_limits<double>::max();
            return 0.0;
        }

        distanceP90 = percentile(distances, 0.90);
        return static_cast<double>(coveredCount) / sampleCount;
    }

    double directionCompatibleLengthRatio(
        const OGRGeometry* sourceGeometry,
        const OGRGeometry* targetGeometry,
        double minimumDirectionCosine)
    {
        const OGRLineString* sourceLine = nullptr;
        double sourceLength = 0.0;
        findLongestLineString(sourceGeometry, sourceLine, sourceLength);
        if (sourceLine == nullptr || sourceLength <= 1e-8)
            return 0.0;

        double compatibleLength = 0.0;
        for (int i = 1; i < sourceLine->getNumPoints(); ++i) {
            OGRPoint startPoint;
            OGRPoint endPoint;
            sourceLine->getPoint(i - 1, &startPoint);
            sourceLine->getPoint(i, &endPoint);
            OGRLineString segment;
            segment.addPoint(startPoint.getX(), startPoint.getY());
            segment.addPoint(endPoint.getX(), endPoint.getY());
            const double segmentLength = lineStringLength(&segment);
            if (segmentLength > 1e-8 && isDirectionCompatibleWithReference(
                &segment, targetGeometry, minimumDirectionCosine)) {
                compatibleLength += segmentLength;
            }
        }

        return compatibleLength / sourceLength;
    }

    bool pointAtLineStringMeasure(
        const OGRLineString* line,
        double measure,
        OGRPoint& point)
    {
        if (line == nullptr || line->getNumPoints() < 2)
            return false;

        const double totalLength = lineStringLength(line);
        if (totalLength <= 1e-8)
            return false;

        const double targetMeasure = std::max(0.0, std::min(totalLength, measure));
        double accumulatedLength = 0.0;
        for (int i = 1; i < line->getNumPoints(); ++i) {
            OGRPoint startPoint;
            OGRPoint endPoint;
            line->getPoint(i - 1, &startPoint);
            line->getPoint(i, &endPoint);
            const double dx = endPoint.getX() - startPoint.getX();
            const double dy = endPoint.getY() - startPoint.getY();
            const double segmentLength = std::sqrt(dx * dx + dy * dy);
            if (segmentLength <= 1e-8)
                continue;

            if (targetMeasure <= accumulatedLength + segmentLength || i == line->getNumPoints() - 1) {
                const double ratio = std::max(0.0, std::min(1.0,
                    (targetMeasure - accumulatedLength) / segmentLength));
                point.setX(startPoint.getX() + ratio * dx);
                point.setY(startPoint.getY() + ratio * dy);
                return true;
            }
            accumulatedLength += segmentLength;
        }
        return false;
    }

    JumpOgrGeometryPtr extractLineStringInterval(
        const OGRLineString* line,
        double firstMeasure,
        double secondMeasure)
    {
        if (line == nullptr || line->getNumPoints() < 2)
            return nullptr;

        const double totalLength = lineStringLength(line);
        const double startMeasure = std::max(0.0, std::min(totalLength,
            std::min(firstMeasure, secondMeasure)));
        const double endMeasure = std::max(0.0, std::min(totalLength,
            std::max(firstMeasure, secondMeasure)));
        if (endMeasure - startMeasure <= 1e-8)
            return nullptr;

        JumpOgrGeometryPtr intervalGeometry(
            OGRGeometryFactory::createGeometry(wkbLineString));
        auto* interval = dynamic_cast<OGRLineString*>(intervalGeometry.get());
        if (interval == nullptr)
            return nullptr;

        OGRPoint startPoint;
        OGRPoint endPoint;
        if (!pointAtLineStringMeasure(line, startMeasure, startPoint)
            || !pointAtLineStringMeasure(line, endMeasure, endPoint)) {
            return nullptr;
        }
        interval->addPoint(startPoint.getX(), startPoint.getY());

        double accumulatedLength = 0.0;
        for (int i = 1; i < line->getNumPoints(); ++i) {
            OGRPoint vertex;
            line->getPoint(i, &vertex);
            OGRPoint previousVertex;
            line->getPoint(i - 1, &previousVertex);
            const double dx = vertex.getX() - previousVertex.getX();
            const double dy = vertex.getY() - previousVertex.getY();
            accumulatedLength += std::sqrt(dx * dx + dy * dy);
            if (accumulatedLength > startMeasure + 1e-8
                && accumulatedLength < endMeasure - 1e-8) {
                interval->addPoint(vertex.getX(), vertex.getY());
            }
        }
        interval->addPoint(endPoint.getX(), endPoint.getY());
        return intervalGeometry;
    }

    JumpOgrGeometryPtr mapOriginalJumpZoneToGeneralizedLine(
        const OGRGeometry* sourceGeometry,
        const OGRGeometry* jumpZoneGeometry,
        const OGRGeometry* generalizedGeometry,
        double topologyTolerance)
    {
        const OGRLineString* sourceLine = nullptr;
        const OGRLineString* generalizedLine = nullptr;
        double sourceLength = 0.0;
        double generalizedLength = 0.0;
        findLongestLineString(sourceGeometry, sourceLine, sourceLength);
        findLongestLineString(generalizedGeometry, generalizedLine, generalizedLength);
        if (sourceLine == nullptr || generalizedLine == nullptr
            || sourceLength <= 1e-8 || generalizedLength <= 1e-8) {
            return nullptr;
        }

        OGRPoint zoneStartPoint;
        OGRPoint zoneEndPoint;
        double zoneLength = 0.0;
        if (!extractAssociatedLineChainEndpoints(
            jumpZoneGeometry, topologyTolerance,
            zoneStartPoint, zoneEndPoint, zoneLength)) {
            return nullptr;
        }

        double sourceStartMeasure = 0.0;
        double sourceEndMeasure = 0.0;
        if (!locatePointAlongLineString(zoneStartPoint, sourceLine, sourceStartMeasure)
            || !locatePointAlongLineString(zoneEndPoint, sourceLine, sourceEndMeasure)) {
            return nullptr;
        }

        OGRPoint generalizedStartPoint;
        OGRPoint generalizedEndPoint;
        generalizedLine->getPoint(0, &generalizedStartPoint);
        generalizedLine->getPoint(
            generalizedLine->getNumPoints() - 1, &generalizedEndPoint);
        OGRPoint sourceStartPoint;
        OGRPoint sourceEndPoint;
        sourceLine->getPoint(0, &sourceStartPoint);
        sourceLine->getPoint(sourceLine->getNumPoints() - 1, &sourceEndPoint);
        const double directEndpointDistance =
            sourceStartPoint.Distance(&generalizedStartPoint)
            + sourceEndPoint.Distance(&generalizedEndPoint);
        const double reverseEndpointDistance =
            sourceStartPoint.Distance(&generalizedEndPoint)
            + sourceEndPoint.Distance(&generalizedStartPoint);
        const bool reversed = reverseEndpointDistance < directEndpointDistance;

        double generalizedStartMeasure = sourceStartMeasure / sourceLength * generalizedLength;
        double generalizedEndMeasure = sourceEndMeasure / sourceLength * generalizedLength;
        if (reversed) {
            generalizedStartMeasure = generalizedLength - generalizedStartMeasure;
            generalizedEndMeasure = generalizedLength - generalizedEndMeasure;
        }
        return extractLineStringInterval(
            generalizedLine, generalizedStartMeasure, generalizedEndMeasure);
    }

    struct GlobalGeneralizedLineMatch
    {
        QString layerName;
        GIntBig fid = -1;
        JumpOgrGeometryPtr geometry;
        double coverageRatio = 0.0;
        double directionRatio = 0.0;
        double distanceP90 = 0.0;

        GlobalGeneralizedLineMatch() = default;
        GlobalGeneralizedLineMatch(GlobalGeneralizedLineMatch&&) noexcept = default;
        GlobalGeneralizedLineMatch& operator=(GlobalGeneralizedLineMatch&&) noexcept = default;
        GlobalGeneralizedLineMatch(const GlobalGeneralizedLineMatch&) = delete;
        GlobalGeneralizedLineMatch& operator=(const GlobalGeneralizedLineMatch&) = delete;
    };

    bool findGlobalGeneralizedLineMatch(
        const BoundaryLineRelation& relation,
        const std::vector<JumpGdalDatasetPtr>& generalizedLineDatasets,
        const BoundaryJumpRule& rule,
        GlobalGeneralizedLineMatch& match)
    {
        if (!relation.sourceLineGeometry || relation.sourceLineGeometry->IsEmpty())
            return false;

        // 该距离仅用于路径相似度评分，绝不再要求原始跳绘区缓冲与综合线相交。
        // 取现有三项图上容差的较大值再放宽三倍，容纳综合平滑后的峰谷位移。
        const double associationDistance = 3.0 * std::max(
            rule.generalizedLineMatchDistance,
            std::max(rule.boundaryCandidateSearchDistance,
                rule.drawDistanceMax));
        const double minimumDirectionCosine = std::cos(
            rule.directionAngleThresholdDeg * 3.14159265358979323846 / 180.0);

        double bestScore = -1.0;
        for (const JumpGdalDatasetPtr& dataset : generalizedLineDatasets) {
            if (!dataset)
                continue;

            for (int layerIndex = 0; layerIndex < dataset->GetLayerCount(); ++layerIndex) {
                OGRLayer* layer = dataset->GetLayer(layerIndex);
                if (layer == nullptr)
                    continue;

                layer->ResetReading();
                OGRFeature* rawFeature = nullptr;
                while ((rawFeature = layer->GetNextFeature()) != nullptr) {
                    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                        feature(rawFeature, OGRFeature::DestroyFeature);
                    JumpOgrGeometryPtr candidateGeometry(
                        extractLinearGeometry(feature->GetGeometryRef()));
                    if (!candidateGeometry || candidateGeometry->IsEmpty())
                        continue;

                    double distanceP90 = 0.0;
                    const double coverageRatio = sampledLineCoverageRatio(
                        relation.sourceLineGeometry.get(), candidateGeometry.get(),
                        associationDistance, distanceP90);
                    if (coverageRatio < 0.70 || distanceP90 > associationDistance)
                        continue;

                    const double directionRatio = directionCompatibleLengthRatio(
                        relation.sourceLineGeometry.get(), candidateGeometry.get(),
                        minimumDirectionCosine);
                    if (directionRatio < 0.60)
                        continue;

                    const double score = 0.70 * coverageRatio
                        + 0.30 * directionRatio
                        - 0.10 * distanceP90 / associationDistance;
                    if (score <= bestScore)
                        continue;

                    bestScore = score;
                    match.layerName = QString::fromUtf8(layer->GetName());
                    match.fid = feature->GetFID();
                    match.geometry = std::move(candidateGeometry);
                    match.coverageRatio = coverageRatio;
                    match.directionRatio = directionRatio;
                    match.distanceP90 = distanceP90;
                }
            }
        }

        return bestScore >= 0.0 && match.geometry && !match.geometry->IsEmpty();
    }

    struct LineMeasureInterval
    {
        double start = 0.0;
        double end = 0.0;
    };

    // 原始跳绘区端点在综合后路径上的对应区间。startPoint/endPoint 是先吸附到
    // 最近成果境界线后，再投影回综合路径得到的 P1'/P2'；geometry 是该路径
    // 上真正参与跳绘统计的局部段。
    struct ProjectedJumpZone
    {
        const OGRLineString* referenceLine = nullptr;
        double startMeasure = 0.0;
        double endMeasure = 0.0;
        OGRPoint startPoint;
        OGRPoint endPoint;
        JumpOgrGeometryPtr geometry;
    };

    void updateNearestPointOnLineGeometry(
        const OGRPoint& point,
        const OGRGeometry* geometry,
        double& nearestDistanceSquared,
        OGRPoint& nearestPoint,
        bool& found)
    {
        if (geometry == nullptr || geometry->IsEmpty())
            return;

        if (wkbFlatten(geometry->getGeometryType()) == wkbLineString) {
            const auto* line = dynamic_cast<const OGRLineString*>(geometry);
            if (line == nullptr || line->getNumPoints() < 2)
                return;

            for (int i = 1; i < line->getNumPoints(); ++i) {
                OGRPoint firstPoint;
                OGRPoint secondPoint;
                line->getPoint(i - 1, &firstPoint);
                line->getPoint(i, &secondPoint);
                const double dx = secondPoint.getX() - firstPoint.getX();
                const double dy = secondPoint.getY() - firstPoint.getY();
                const double lengthSquared = dx * dx + dy * dy;
                if (lengthSquared <= 1e-16)
                    continue;

                const double ratio = std::max(0.0, std::min(1.0,
                    ((point.getX() - firstPoint.getX()) * dx
                        + (point.getY() - firstPoint.getY()) * dy) / lengthSquared));
                const double x = firstPoint.getX() + ratio * dx;
                const double y = firstPoint.getY() + ratio * dy;
                const double distanceX = point.getX() - x;
                const double distanceY = point.getY() - y;
                const double distanceSquared = distanceX * distanceX
                    + distanceY * distanceY;
                if (found && distanceSquared >= nearestDistanceSquared)
                    continue;

                nearestDistanceSquared = distanceSquared;
                nearestPoint.setX(x);
                nearestPoint.setY(y);
                found = true;
            }
            return;
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;
        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            updateNearestPointOnLineGeometry(
                point, collection->getGeometryRef(i), nearestDistanceSquared,
                nearestPoint, found);
        }
    }

    bool projectPointToLineGeometry(
        const OGRPoint& point,
        const OGRGeometry* geometry,
        OGRPoint& projectedPoint)
    {
        double nearestDistanceSquared = std::numeric_limits<double>::max();
        bool found = false;
        updateNearestPointOnLineGeometry(
            point, geometry, nearestDistanceSquared, projectedPoint, found);
        return found;
    }

    bool buildProjectedJumpZone(
        const BoundaryLineRelation& relation,
        const OGRPoint& startAnchor,
        const OGRPoint& endAnchor,
        ProjectedJumpZone& zone)
    {
        if (!relation.generalizedLineGeometry
            || relation.generalizedLineGeometry->IsEmpty()) {
            return false;
        }

        double referenceLength = 0.0;
        findLongestLineString(
            relation.generalizedLineGeometry.get(), zone.referenceLine,
            referenceLength);
        if (zone.referenceLine == nullptr || referenceLength <= 1e-8)
            return false;

        double firstMeasure = 0.0;
        double secondMeasure = 0.0;
        if (!locatePointAlongLineString(startAnchor, zone.referenceLine, firstMeasure)
            || !locatePointAlongLineString(endAnchor, zone.referenceLine, secondMeasure)) {
            return false;
        }

        zone.startMeasure = std::min(firstMeasure, secondMeasure);
        zone.endMeasure = std::max(firstMeasure, secondMeasure);
        if (zone.endMeasure - zone.startMeasure <= 1e-8)
            return false;

        if (!pointAtLineStringMeasure(
            zone.referenceLine, zone.startMeasure, zone.startPoint)
            || !pointAtLineStringMeasure(
                zone.referenceLine, zone.endMeasure, zone.endPoint)) {
            return false;
        }
        zone.geometry = extractLineStringInterval(
            zone.referenceLine, zone.startMeasure, zone.endMeasure);
        return zone.geometry && !zone.geometry->IsEmpty();
    }

    bool tangentAtLineStringMeasure(
        const OGRLineString* line,
        double measure,
        double& tangentX,
        double& tangentY)
    {
        if (line == nullptr || line->getNumPoints() < 2)
            return false;

        const double targetMeasure = std::max(0.0, std::min(
            lineStringLength(line), measure));
        double accumulatedLength = 0.0;
        for (int i = 1; i < line->getNumPoints(); ++i) {
            OGRPoint firstPoint;
            OGRPoint secondPoint;
            line->getPoint(i - 1, &firstPoint);
            line->getPoint(i, &secondPoint);
            const double dx = secondPoint.getX() - firstPoint.getX();
            const double dy = secondPoint.getY() - firstPoint.getY();
            const double segmentLength = std::sqrt(dx * dx + dy * dy);
            if (segmentLength <= 1e-8)
                continue;
            if (targetMeasure <= accumulatedLength + segmentLength + 1e-8
                || i == line->getNumPoints() - 1) {
                tangentX = dx / segmentLength;
                tangentY = dy / segmentLength;
                return true;
            }
            accumulatedLength += segmentLength;
        }
        return false;
    }

    JumpOgrGeometryPtr createEndpointDisplayHalfPlane(
        const OGRPoint& endpoint,
        double tangentX,
        double tangentY,
        bool keepForward,
        const OGRGeometry* supportGeometry)
    {
        if (supportGeometry == nullptr || supportGeometry->IsEmpty())
            return nullptr;

        OGREnvelope envelope;
        supportGeometry->getEnvelope(&envelope);
        const double maximumDelta = std::max(
            std::max(std::fabs(envelope.MinX - endpoint.getX()),
                std::fabs(envelope.MaxX - endpoint.getX())),
            std::max(std::fabs(envelope.MinY - endpoint.getY()),
                std::fabs(envelope.MaxY - endpoint.getY())));
        const double extent = std::max(1.0, maximumDelta * 4.0 + 1.0);
        if (!keepForward) {
            tangentX = -tangentX;
            tangentY = -tangentY;
        }
        const double normalX = -tangentY;
        const double normalY = tangentX;
        const double forwardX = tangentX * extent * 2.0;
        const double forwardY = tangentY * extent * 2.0;

        JumpOgrGeometryPtr halfPlane(
            OGRGeometryFactory::createGeometry(wkbPolygon));
        auto* polygon = dynamic_cast<OGRPolygon*>(halfPlane.get());
        if (polygon == nullptr)
            return nullptr;
        OGRLinearRing ring;
        ring.addPoint(endpoint.getX() - normalX * extent,
            endpoint.getY() - normalY * extent);
        ring.addPoint(endpoint.getX() + normalX * extent,
            endpoint.getY() + normalY * extent);
        ring.addPoint(endpoint.getX() + normalX * extent + forwardX,
            endpoint.getY() + normalY * extent + forwardY);
        ring.addPoint(endpoint.getX() - normalX * extent + forwardX,
            endpoint.getY() - normalY * extent + forwardY);
        ring.closeRings();
        polygon->addRing(&ring);
        return halfPlane;
    }

    // 仅用于错误图层显示，把区端点外的尾线裁掉。统计仍使用已关联候选，
    // 避免曲折综合线的切线方向影响实际弧段数量。
    JumpOgrGeometryPtr clipGeometryForJumpDisplay(
        const OGRGeometry* geometry,
        const ProjectedJumpZone& zone)
    {
        if (geometry == nullptr || geometry->IsEmpty()
            || zone.referenceLine == nullptr) {
            return nullptr;
        }
        double startTangentX = 0.0;
        double startTangentY = 0.0;
        double endTangentX = 0.0;
        double endTangentY = 0.0;
        if (!tangentAtLineStringMeasure(zone.referenceLine, zone.startMeasure,
            startTangentX, startTangentY)
            || !tangentAtLineStringMeasure(zone.referenceLine, zone.endMeasure,
                endTangentX, endTangentY)) {
            return nullptr;
        }
        JumpOgrGeometryPtr startHalfPlane(createEndpointDisplayHalfPlane(
            zone.startPoint, startTangentX, startTangentY, true, geometry));
        JumpOgrGeometryPtr endHalfPlane(createEndpointDisplayHalfPlane(
            zone.endPoint, endTangentX, endTangentY, false, geometry));
        if (!startHalfPlane || !endHalfPlane)
            return nullptr;
        JumpOgrGeometryPtr afterStart(
            geometry->Intersection(startHalfPlane.get()));
        if (!afterStart || afterStart->IsEmpty())
            return nullptr;
        return JumpOgrGeometryPtr(afterStart->Intersection(endHalfPlane.get()));
    }

    void collectProjectedLineIntervals(
        const OGRGeometry* geometry,
        const OGRLineString* referenceLine,
        double zoneStartMeasure,
        double zoneEndMeasure,
        double coverageRadius,
        std::vector<LineMeasureInterval>& intervals)
    {
        if (geometry == nullptr || geometry->IsEmpty() || referenceLine == nullptr)
            return;

        if (wkbFlatten(geometry->getGeometryType()) == wkbLineString) {
            const auto* line = dynamic_cast<const OGRLineString*>(geometry);
            if (line == nullptr || line->getNumPoints() < 2)
                return;

            bool foundMeasure = false;
            double minimumMeasure = 0.0;
            double maximumMeasure = 0.0;
            for (int i = 0; i < line->getNumPoints(); ++i) {
                OGRPoint point;
                line->getPoint(i, &point);
                double measure = 0.0;
                if (!locatePointAlongLineString(point, referenceLine, measure))
                    continue;

                if (!foundMeasure) {
                    minimumMeasure = measure;
                    maximumMeasure = measure;
                    foundMeasure = true;
                }
                else {
                    minimumMeasure = std::min(minimumMeasure, measure);
                    maximumMeasure = std::max(maximumMeasure, measure);
                }
            }
            if (!foundMeasure)
                return;

            LineMeasureInterval interval;
            interval.start = std::max(zoneStartMeasure,
                minimumMeasure - coverageRadius);
            interval.end = std::min(zoneEndMeasure,
                maximumMeasure + coverageRadius);
            if (interval.end - interval.start > 1e-8)
                intervals.push_back(interval);
            return;
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            collectProjectedLineIntervals(
                collection->getGeometryRef(i), referenceLine,
                zoneStartMeasure, zoneEndMeasure, coverageRadius, intervals);
        }
    }

    // 按成果线每个组件的首尾端点建立里程区间。端点同时落在 P1/P2 外侧
    // 缓冲帽中的组件不会形成有效区间，因此不会被当成跳绘弧段。
    void collectEndpointProjectedLineIntervals(
        const OGRGeometry* geometry,
        const OGRLineString* referenceLine,
        double zoneStartMeasure,
        double zoneEndMeasure,
        std::vector<LineMeasureInterval>& intervals)
    {
        if (geometry == nullptr || geometry->IsEmpty() || referenceLine == nullptr)
            return;

        if (wkbFlatten(geometry->getGeometryType()) == wkbLineString) {
            const auto* line = dynamic_cast<const OGRLineString*>(geometry);
            if (line == nullptr || line->getNumPoints() < 2)
                return;

            OGRPoint startPoint;
            OGRPoint endPoint;
            line->getPoint(0, &startPoint);
            line->getPoint(line->getNumPoints() - 1, &endPoint);
            double startMeasure = 0.0;
            double endMeasure = 0.0;
            if (!locatePointAlongLineString(startPoint, referenceLine, startMeasure)
                || !locatePointAlongLineString(endPoint, referenceLine, endMeasure)) {
                return;
            }

            LineMeasureInterval interval;
            interval.start = std::max(zoneStartMeasure,
                std::min(startMeasure, endMeasure));
            interval.end = std::min(zoneEndMeasure,
                std::max(startMeasure, endMeasure));
            if (interval.end - interval.start > 1e-8)
                intervals.push_back(interval);
            return;
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;

        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            collectEndpointProjectedLineIntervals(
                collection->getGeometryRef(i), referenceLine,
                zoneStartMeasure, zoneEndMeasure, intervals);
        }
    }

    // 以 P1'、P2' 裁剪后的成果境界线端点实际是否相接为准；不再以投影里程
    // 是否重叠判断连通，避免弯曲综合线把两条独立成果线误并为一段。
    bool countJumpDrawSegmentsByEndpoints(
        const ProjectedJumpZone& zone,
        const std::vector<JumpOgrGeometryPtr>& jumpDrawParts,
        double topologyTolerance,
        int& drawSegmentCount)
    {
        drawSegmentCount = 0;
        if (zone.referenceLine == nullptr || !zone.geometry
            || zone.endMeasure - zone.startMeasure <= 1e-8) {
            return false;
        }
        drawSegmentCount = countConnectedLineComponents(
            jumpDrawParts, topologyTolerance);
        return true;
    }

    // 在单一路径的成果跳绘区内，空白段只存在于不相连的两段成果弧段之间。
    // 因此实际空白数 = 实际成果弧段数 - 1；没有成果弧段时整个区间为空白 1 段。
    bool countLogicalJumpBlankSegments(
        const ProjectedJumpZone& zone,
        const std::vector<JumpOgrGeometryPtr>& jumpDrawParts,
        double topologyTolerance,
        int& blankSegmentCount)
    {
        blankSegmentCount = 0;
        if (zone.referenceLine == nullptr || !zone.geometry
            || zone.endMeasure - zone.startMeasure <= 1e-8) {
            return false;
        }

        const int drawSegmentCount = countConnectedLineComponents(
            jumpDrawParts, topologyTolerance);
        if (drawSegmentCount == 0) {
            blankSegmentCount = 1;
            return true;
        }
        blankSegmentCount = drawSegmentCount - 1;
        return true;
    }

    // 在跳绘区端点附近，成果境界线进入/离开综合水系面的交点比原始关联线
    // 与水面边界的交点更可靠。将该交点投影回原始关联线并作为分段点，可让
    // 入水段走水系面优先、面外段继续按综合线状要素检查。
    void collectLineStringGeometryRefs(
        const OGRGeometry* geometry,
        std::vector<const OGRLineString*>& lines)
    {
        if (geometry == nullptr || geometry->IsEmpty())
            return;

        if (wkbFlatten(geometry->getGeometryType()) == wkbLineString) {
            const auto* line = dynamic_cast<const OGRLineString*>(geometry);
            if (line != nullptr && lineStringLength(line) > 1e-8)
                lines.push_back(line);
            return;
        }

        if (!isCollectionType(geometry->getGeometryType()))
            return;

        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (collection == nullptr)
            return;
        for (int index = 0; index < collection->getNumGeometries(); ++index)
            collectLineStringGeometryRefs(collection->getGeometryRef(index), lines);
    }

    bool splitRelationAtNearbyWaterTransitions(
        const BoundaryLineRelation& relation,
        GDALDataset* boundaryDataset,
        const std::vector<JumpGdalDatasetPtr>& generalizedLineDatasets,
        const BoundaryJumpRule& rule,
        std::vector<BoundaryLineRelation>& splitRelations,
        int& transitionCount)
    {
        if (boundaryDataset == nullptr || !relation.lineGeometry
            || relation.lineGeometry->IsEmpty()) {
            return false;
        }

        std::vector<const OGRLineString*> relationLines;
        collectLineStringGeometryRefs(relation.lineGeometry.get(), relationLines);
        if (relationLines.empty()) {
            return false;
        }

        OGRPoint startEndpoint;
        OGRPoint endEndpoint;
        double jumpZoneLength = 0.0;
        if (!extractAssociatedLineChainEndpoints(
            relation.lineGeometry.get(), rule.topologyTolerance,
            startEndpoint, endEndpoint, jumpZoneLength)) {
            return false;
        }

        const double searchDistance = std::max(
            rule.boundaryCandidateSearchDistance,
            rule.generalizedLineMatchDistance);
        if (searchDistance <= 1e-8)
            return false;

        JumpOgrGeometryPtr endpointSearchArea(startEndpoint.Buffer(searchDistance));
        JumpOgrGeometryPtr endEndpointSearchArea(endEndpoint.Buffer(searchDistance));
        if (!endpointSearchArea || endpointSearchArea->IsEmpty()
            || !endEndpointSearchArea || endEndpointSearchArea->IsEmpty()) {
            return false;
        }
        JumpOgrGeometryPtr endpointSearchUnion(
            endpointSearchArea->Union(endEndpointSearchArea.get()));
        if (!endpointSearchUnion || endpointSearchUnion->IsEmpty())
            return false;

        bool foundStartTransition = false;
        bool foundEndTransition = false;
        double nearestStartDistance = std::numeric_limits<double>::max();
        double nearestEndDistance = std::numeric_limits<double>::max();
        OGRPoint nearestStartTransition;
        OGRPoint nearestEndTransition;

        for (const JumpGdalDatasetPtr& generalizedDataset : generalizedLineDatasets) {
            if (!generalizedDataset)
                continue;

            for (int generalizedLayerIndex = 0;
                generalizedLayerIndex < generalizedDataset->GetLayerCount();
                ++generalizedLayerIndex) {
                OGRLayer* generalizedLayer =
                    generalizedDataset->GetLayer(generalizedLayerIndex);
                if (generalizedLayer == nullptr)
                    continue;

                generalizedLayer->SetSpatialFilter(endpointSearchUnion.get());
                generalizedLayer->ResetReading();
                OGRFeature* rawGeneralizedFeature = nullptr;
                while ((rawGeneralizedFeature = generalizedLayer->GetNextFeature()) != nullptr) {
                    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                        generalizedFeature(rawGeneralizedFeature, OGRFeature::DestroyFeature);
                    OGRGeometry* generalizedGeometry =
                        generalizedFeature->GetGeometryRef();
                    if (!isPolygonGeometry(generalizedGeometry))
                        continue;

                    JumpOgrGeometryPtr validWaterArea;
                    if (generalizedGeometry->IsValid())
                        validWaterArea.reset(generalizedGeometry->clone());
                    else
                        validWaterArea.reset(generalizedGeometry->Buffer(0.0));
                    if (!validWaterArea || validWaterArea->IsEmpty()
                        || !isPolygonGeometry(validWaterArea.get())
                        || !hasLineOverlapWithArea(
                            relation.lineGeometry.get(), validWaterArea.get())) {
                        continue;
                    }

                    JumpOgrGeometryPtr waterBoundary(validWaterArea->Boundary());
                    if (!waterBoundary || waterBoundary->IsEmpty())
                        continue;
                    JumpOgrGeometryPtr boundarySearchArea(
                        waterBoundary->Buffer(searchDistance));
                    if (!boundarySearchArea || boundarySearchArea->IsEmpty())
                        continue;

                    for (int boundaryLayerIndex = 0;
                        boundaryLayerIndex < boundaryDataset->GetLayerCount();
                        ++boundaryLayerIndex) {
                        OGRLayer* boundaryLayer =
                            boundaryDataset->GetLayer(boundaryLayerIndex);
                        if (boundaryLayer == nullptr)
                            continue;

                        boundaryLayer->SetSpatialFilter(boundarySearchArea.get());
                        boundaryLayer->ResetReading();
                        OGRFeature* rawBoundaryFeature = nullptr;
                        while ((rawBoundaryFeature = boundaryLayer->GetNextFeature()) != nullptr) {
                            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                                boundaryFeature(rawBoundaryFeature, OGRFeature::DestroyFeature);
                            OGRGeometry* boundaryGeometry =
                                boundaryFeature->GetGeometryRef();
                            if (!isLineGeometry(boundaryGeometry))
                                continue;

                            JumpOgrGeometryPtr intersectionGeometry(
                                boundaryGeometry->Intersection(waterBoundary.get()));
                            std::vector<OGRPoint> intersectionPoints;
                            addPointParts(intersectionGeometry.get(), intersectionPoints);
                            for (const OGRPoint& intersectionPoint : intersectionPoints) {
                                const double startDistance =
                                    intersectionPoint.Distance(&startEndpoint);
                                if (startDistance <= searchDistance
                                    && startDistance < nearestStartDistance) {
                                    nearestStartDistance = startDistance;
                                    nearestStartTransition = intersectionPoint;
                                    foundStartTransition = true;
                                }

                                const double endDistance =
                                    intersectionPoint.Distance(&endEndpoint);
                                if (endDistance <= searchDistance
                                    && endDistance < nearestEndDistance) {
                                    nearestEndDistance = endDistance;
                                    nearestEndTransition = intersectionPoint;
                                    foundEndTransition = true;
                                }
                            }
                        }
                        boundaryLayer->SetSpatialFilter(nullptr);
                    }
                }
                generalizedLayer->SetSpatialFilter(nullptr);
            }
        }

        // 交点分别投影到距离最近的线组件。连续关联链合并后可能是
        // MultiLineString，不能再只按最长组件切分，否则面外组件会被遗漏。
        std::vector<std::vector<double>> transitionMeasuresByLine(
            relationLines.size());
        std::vector<std::vector<OGRPoint>> transitionPointsByLine(
            relationLines.size());
        const auto addTransitionMeasure = [&](const OGRPoint& transitionPoint) {
            int nearestLineIndex = -1;
            double nearestDistance = std::numeric_limits<double>::max();
            double nearestMeasure = 0.0;
            for (size_t lineIndex = 0; lineIndex < relationLines.size(); ++lineIndex) {
                OGRPoint projectedPoint;
                if (!projectPointToLineGeometry(
                    transitionPoint, relationLines[lineIndex], projectedPoint)) {
                    continue;
                }
                double measure = 0.0;
                if (!locatePointAlongLineString(
                    projectedPoint, relationLines[lineIndex], measure)) {
                    continue;
                }
                const double distance = transitionPoint.Distance(&projectedPoint);
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    nearestLineIndex = static_cast<int>(lineIndex);
                    nearestMeasure = measure;
                }
            }
            if (nearestLineIndex < 0)
                return;

            const double lineLength = lineStringLength(
                relationLines[static_cast<size_t>(nearestLineIndex)]);
            if (nearestMeasure <= rule.topologyTolerance
                || lineLength - nearestMeasure <= rule.topologyTolerance) {
                return;
            }
            std::vector<double>& measures = transitionMeasuresByLine[
                static_cast<size_t>(nearestLineIndex)];
            for (double existingMeasure : measures) {
                if (std::fabs(existingMeasure - nearestMeasure)
                    <= rule.topologyTolerance) {
                    return;
            }
            }
            measures.push_back(nearestMeasure);
            transitionPointsByLine[static_cast<size_t>(nearestLineIndex)].push_back(
                transitionPoint);
        };
        if (foundStartTransition)
            addTransitionMeasure(nearestStartTransition);
        if (foundEndTransition)
            addTransitionMeasure(nearestEndTransition);
        int validTransitionCount = 0;
        for (const std::vector<double>& measures : transitionMeasuresByLine)
            validTransitionCount += static_cast<int>(measures.size());
        if (validTransitionCount <= 0)
            return false;

        std::vector<BoundaryLineRelation> candidateRelations;
        for (size_t lineIndex = 0; lineIndex < relationLines.size(); ++lineIndex) {
            std::vector<double>& transitionMeasures =
                transitionMeasuresByLine[lineIndex];
            std::sort(transitionMeasures.begin(), transitionMeasures.end());
            std::vector<double> measures;
            measures.reserve(transitionMeasures.size() + 2);
            measures.push_back(0.0);
            measures.insert(measures.end(), transitionMeasures.begin(),
                transitionMeasures.end());
            measures.push_back(lineStringLength(relationLines[lineIndex]));

            for (size_t measureIndex = 1; measureIndex < measures.size(); ++measureIndex) {
                JumpOgrGeometryPtr segmentGeometry(extractLineStringInterval(
                    relationLines[lineIndex], measures[measureIndex - 1],
                    measures[measureIndex]));
                if (!segmentGeometry || segmentGeometry->IsEmpty()
                    || geometryLineLength(segmentGeometry.get()) <= 1e-8) {
                    continue;
                }

                BoundaryLineRelation segment;
                segment.lineLayerName = relation.lineLayerName;
                segment.lineFid = relation.lineFid;
                segment.hasOriginalJumpEndpoints = true;
                segment.originalJumpStartPoint = startEndpoint;
                segment.originalJumpEndPoint = endEndpoint;
                segment.originalJumpZoneLength = jumpZoneLength;
                segment.nearbyWaterTransitionPoints = transitionPointsByLine[lineIndex];
                if (relation.sourceLineGeometry) {
                    segment.sourceLineGeometry.reset(
                        relation.sourceLineGeometry->clone());
                }
                segment.lineGeometry = std::move(segmentGeometry);
                JumpOgrGeometryPtr segmentTopologyBuffer(
                    segment.lineGeometry->Buffer(
                        std::max(rule.topologyTolerance, 1e-8)));
                if (!segmentTopologyBuffer || segmentTopologyBuffer->IsEmpty())
                    continue;
                if (relation.overlapGeometry) {
                    segment.overlapGeometry.reset(
                        relation.overlapGeometry->Intersection(
                            segmentTopologyBuffer.get()));
                }
                segment.overlapLength = geometryLineLength(
                    segment.overlapGeometry.get());
                if (!segment.overlapGeometry || segment.overlapGeometry->IsEmpty()
                    || segment.overlapLength <= 1e-8) {
                    continue;
                }
                candidateRelations.push_back(std::move(segment));
            }
        }

        if (candidateRelations.size() < 2)
            return false;

        transitionCount += validTransitionCount;
        for (BoundaryLineRelation& segment : candidateRelations)
            splitRelations.push_back(std::move(segment));
        return true;
    }

    // 跳绘检查：先建立行政界线—线状要素关联关系，再检查成果境界线距离。
    bool collectJumpDistanceIssues(
        GDALDataset* boundaryDataset,
        GDALDataset* adminDataset,
        const std::vector<JumpGdalDatasetPtr>& lineDatasets,
        const std::vector<JumpGdalDatasetPtr>& generalizedLineDatasets,
        const BoundaryJumpRule& rule,
        std::vector<BoundaryJumpIssue>& issues,
        std::vector<DetectedJumpEndpoint>& detectedEndpoints,
        QStringList& logs,
        QString& error)
    {
        if (boundaryDataset == nullptr || adminDataset == nullptr) {
            error = "跳绘检查输入数据为空。";
            return false;
        }

        AdminBoundaryAnalyzer adminBoundaryAnalyzer;
        JumpOgrGeometryPtr adminBoundaryGeometry =
            adminBoundaryAnalyzer.extractBoundary(adminDataset, rule);
        if (!adminBoundaryGeometry || adminBoundaryGeometry->IsEmpty()) {
            error = "行政界线 SHP 中未提取到可用行政界线。";
            return false;
        }

        const double topologyTolerance = std::max(rule.topologyTolerance, 1e-8);
        const double minimumDirectionCosine = std::cos(
            rule.directionAngleThresholdDeg * 3.14159265358979323846 / 180.0);
        std::vector<BoundaryLineRelation> relations;
        int lineFeatureCount = 0;

        // ① 行政界线与线状要素叠加，建立有效关联关系。
        for (const JumpGdalDatasetPtr& dataset : lineDatasets) {
            if (!dataset)
                continue;

            for (int layerIndex = 0; layerIndex < dataset->GetLayerCount(); ++layerIndex) {
                OGRLayer* layer = dataset->GetLayer(layerIndex);
                if (layer == nullptr)
                    continue;

                const QString layerName = QString::fromUtf8(layer->GetName());
                layer->ResetReading();
                OGRFeature* rawFeature = nullptr;
                while ((rawFeature = layer->GetNextFeature()) != nullptr) {
                    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                        feature(rawFeature, OGRFeature::DestroyFeature);
                    OGRGeometry* lineGeometry = feature->GetGeometryRef();
                    if (!isLineGeometry(lineGeometry))
                        continue;

                    ++lineFeatureCount;
                    // 使用独立的小拓扑容差容忍坐标误差，再对行政界线做 Intersection。
                    JumpOgrGeometryPtr lineTopologyBuffer(
                        lineGeometry->Buffer(topologyTolerance));
                    if (!lineTopologyBuffer || lineTopologyBuffer->IsEmpty())
                        continue;

                    JumpOgrGeometryPtr overlapGeometry(
                        adminBoundaryGeometry->Intersection(lineTopologyBuffer.get()));
                    const double overlapLength = geometryLineLength(overlapGeometry.get());
                    if (!overlapGeometry || overlapGeometry->IsEmpty()
                        || overlapLength <= rule.overlapLength) {
                        continue;
                    }

                    JumpOgrGeometryPtr adminTopologyBuffer(
                        adminBoundaryGeometry->Buffer(topologyTolerance));
                    if (!adminTopologyBuffer || adminTopologyBuffer->IsEmpty())
                        continue;

                    JumpOgrGeometryPtr relatedLineGeometry(
                        lineGeometry->Intersection(adminTopologyBuffer.get()));
                    if (!relatedLineGeometry || relatedLineGeometry->IsEmpty()
                        || !isLineGeometry(relatedLineGeometry.get())) {
                        continue;
                    }

                    BoundaryLineRelation relation;
                    relation.lineLayerName = layerName;
                    relation.lineFid = feature->GetFID();
                    relation.overlapLength = overlapLength;
                    relation.overlapGeometry = std::move(overlapGeometry);
                    relation.sourceLineGeometry.reset(lineGeometry->clone());
                    relation.lineGeometry = std::move(relatedLineGeometry);
                    relations.push_back(std::move(relation));
                }
            }
        }

        const int validRelationCount = static_cast<int>(relations.size());
        mergeConnectedOriginalRelations(relations, topologyTolerance);

        // 若跳绘端点附近存在成果境界线进出综合水系面的交点，按该交点把原始
        // 关联线切成子段。这样水面优先只作用于真正入水的子段，面外成果
        // 境界线（例如 FID5）仍会进入综合线状要素质检。
        std::vector<BoundaryLineRelation> transitionSplitRelations;
        transitionSplitRelations.reserve(relations.size());
        int waterTransitionCount = 0;
        int waterTransitionSplitChainCount = 0;
        for (const BoundaryLineRelation& relation : relations) {
            if (splitRelationAtNearbyWaterTransitions(
                relation, boundaryDataset, generalizedLineDatasets, rule,
                transitionSplitRelations, waterTransitionCount)) {
                ++waterTransitionSplitChainCount;
                continue;
            }

            BoundaryLineRelation retainedRelation;
            retainedRelation.lineLayerName = relation.lineLayerName;
            retainedRelation.lineFid = relation.lineFid;
            retainedRelation.overlapLength = relation.overlapLength;
            retainedRelation.hasOriginalJumpEndpoints = relation.hasOriginalJumpEndpoints;
            retainedRelation.originalJumpStartPoint = relation.originalJumpStartPoint;
            retainedRelation.originalJumpEndPoint = relation.originalJumpEndPoint;
            retainedRelation.originalJumpZoneLength = relation.originalJumpZoneLength;
            retainedRelation.nearbyWaterTransitionPoints =
                relation.nearbyWaterTransitionPoints;
            if (relation.overlapGeometry)
                retainedRelation.overlapGeometry.reset(relation.overlapGeometry->clone());
            if (relation.sourceLineGeometry)
                retainedRelation.sourceLineGeometry.reset(
                    relation.sourceLineGeometry->clone());
            if (relation.lineGeometry)
                retainedRelation.lineGeometry.reset(relation.lineGeometry->clone());
            transitionSplitRelations.push_back(std::move(retainedRelation));
        }
        relations = std::move(transitionSplitRelations);

        logs << QString("行政界线—线状要素关联完成：线状要素=%1，有效关联段=%2，连续关联链=%3，最小重叠长度=%4，拓扑容差=%5。")
            .arg(lineFeatureCount)
            .arg(validRelationCount)
            .arg(relations.size())
            .arg(rule.overlapLength, 0, 'f', 3)
            .arg(topologyTolerance, 0, 'f', 3);
        if (waterTransitionSplitChainCount > 0) {
            logs << QString("水系面进出分段：命中跳绘端点附近成果境界线入/出面交点=%1，拆分原始连续关联链=%2，拆分后关联段=%3。")
                .arg(waterTransitionCount)
                .arg(waterTransitionSplitChainCount)
                .arg(relations.size());
        }

        // ② 原始连续关联链只确定“候选跳绘区”。只有该原始线在综合后仍能
        //    找到对应表达时，候选区才会升级为正式跳绘区并输出 P1、P2。
        //    未匹配候选区不是跳绘区，但仍保留给后续常规境界缺失检查。
        std::vector<bool> originalJumpZoneFlags(relations.size(), false);
        int originalJumpZoneCount = 0;
        for (size_t relationIndex = 0; relationIndex < relations.size(); ++relationIndex) {
            const BoundaryLineRelation& relation = relations[relationIndex];
            OGRPoint startPoint;
            OGRPoint endPoint;
            double jumpZoneLength = 0.0;
            if (!extractAssociatedLineChainEndpoints(
                relation.lineGeometry.get(), rule.topologyTolerance,
                startPoint, endPoint, jumpZoneLength)) {
                continue;
            }

            originalJumpZoneFlags[relationIndex] = true;
            ++originalJumpZoneCount;
        }
        logs << QString("原始跳绘候选区确定完成：连续关联链=%1，候选区=%2；待综合后表达匹配成功后再确认跳绘区和输出 P1、P2。")
            .arg(relations.size())
            .arg(originalJumpZoneCount);

        // ③ 在原始有效关联段的匹配缓冲区内查找解析后线状要素或水系面。匹配
        //    成功才将候选区确认成跳绘区；局部失败时还会尝试完整路径兜底匹配。
        std::vector<BoundaryLineRelation> matchedRelations;
        int matchedGeneralizedFeatureCount = 0;
        QSet<QString> matchedGeneralizedFeatureKeys;
        QSet<int> matchedOriginalRelationIndexes;
        for (size_t relationIndex = 0; relationIndex < relations.size(); ++relationIndex) {
            const BoundaryLineRelation& relation = relations[relationIndex];
            if (!relation.lineGeometry || relation.lineGeometry->IsEmpty())
                continue;

            JumpOgrGeometryPtr matchBuffer(
                relation.lineGeometry->Buffer(rule.generalizedLineMatchDistance));
            if (!matchBuffer || matchBuffer->IsEmpty())
                continue;

            // 只要原始关联线与任一综合水系面存在有效线长度交集，即由水系面
            // 负责本关联段的成果境界线检查。此时不能再混入综合 _LineString
            // 的绘制距离结果，也不能在后续完整路径兜底时重新关联到线要素。
            bool hasMatchedWaterArea = false;
            for (const JumpGdalDatasetPtr& generalizedDataset : generalizedLineDatasets) {
                if (!generalizedDataset || hasMatchedWaterArea)
                    continue;

                for (int layerIndex = 0;
                    layerIndex < generalizedDataset->GetLayerCount()
                        && !hasMatchedWaterArea;
                    ++layerIndex) {
                    OGRLayer* generalizedLayer = generalizedDataset->GetLayer(layerIndex);
                    if (generalizedLayer == nullptr)
                        continue;

                    generalizedLayer->SetSpatialFilter(matchBuffer.get());
                    generalizedLayer->ResetReading();
                    OGRFeature* rawGeneralizedFeature = nullptr;
                    while ((rawGeneralizedFeature = generalizedLayer->GetNextFeature()) != nullptr) {
                        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                            generalizedFeature(rawGeneralizedFeature, OGRFeature::DestroyFeature);
                        OGRGeometry* generalizedGeometry = generalizedFeature->GetGeometryRef();
                        if (!isPolygonGeometry(generalizedGeometry))
                            continue;

                        JumpOgrGeometryPtr validWaterArea;
                        if (generalizedGeometry->IsValid())
                            validWaterArea.reset(generalizedGeometry->clone());
                        else
                            validWaterArea.reset(generalizedGeometry->Buffer(0.0));

                        if (validWaterArea
                            && !validWaterArea->IsEmpty()
                            && isPolygonGeometry(validWaterArea.get())
                            && hasLineOverlapWithArea(
                                relation.lineGeometry.get(), validWaterArea.get())) {
                            hasMatchedWaterArea = true;
                            break;
                        }
                    }
                    generalizedLayer->SetSpatialFilter(nullptr);
                }
            }
            if (hasMatchedWaterArea) {
                logs << QString("综合水系面优先：原始关联线图层=%1，FID=%2 与综合水系面存在有效线长度交集；跳过综合 _LineString 的绘制距离检查，并禁止后续完整路径兜底匹配。")
                    .arg(relation.lineLayerName)
                    .arg(relation.lineFid);
            }

            for (const JumpGdalDatasetPtr& generalizedDataset : generalizedLineDatasets) {
                if (!generalizedDataset)
                    continue;

                for (int layerIndex = 0;
                    layerIndex < generalizedDataset->GetLayerCount();
                    ++layerIndex) {
                    OGRLayer* generalizedLayer = generalizedDataset->GetLayer(layerIndex);
                    if (generalizedLayer == nullptr)
                        continue;

                    const QString generalizedLayerName =
                        QString::fromUtf8(generalizedLayer->GetName());
                    generalizedLayer->SetSpatialFilter(matchBuffer.get());
                    generalizedLayer->ResetReading();
                        OGRFeature* rawGeneralizedFeature = nullptr;
                        while ((rawGeneralizedFeature = generalizedLayer->GetNextFeature()) != nullptr) {
                        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                            generalizedFeature(rawGeneralizedFeature, OGRFeature::DestroyFeature);
                        OGRGeometry* generalizedGeometry = generalizedFeature->GetGeometryRef();
                        const bool generalizedIsArea = isPolygonGeometry(generalizedGeometry);
                        if (hasMatchedWaterArea && !generalizedIsArea) {
                            continue;
                        }
                        JumpOgrGeometryPtr matchedGeneralizedGeometry;
                        JumpOgrGeometryPtr generalizedLinearGeometry;
                        JumpOgrGeometryPtr validGeneralizedAreaGeometry;
                        JumpOgrGeometryPtr matchedWaterAreaLineGeometry;
                        bool waterAreaCoversJumpZone = false;
                        if (generalizedIsArea) {
                            // 综合后水系以面表达时，只要成果境界线落入该水系面即可；
                            // 面本身不参与 0.2mm 的线状绘制距离环计算。
                            // 有效面直接使用原始几何，保证判定面与图上显示面一致；只有
                            // 无效面才用 Buffer(0) 修复自相交、断裂等问题。
                            if (generalizedGeometry != nullptr) {
                                if (generalizedGeometry->IsValid()) {
                                    validGeneralizedAreaGeometry.reset(
                                        generalizedGeometry->clone());
                                }
                                else {
                                    validGeneralizedAreaGeometry.reset(
                                        generalizedGeometry->Buffer(0.0));
                                }
                            }
                            if (!validGeneralizedAreaGeometry
                                || validGeneralizedAreaGeometry->IsEmpty()
                                || !isPolygonGeometry(validGeneralizedAreaGeometry.get())
                                || !hasLineOverlapWithArea(
                                    relation.lineGeometry.get(),
                                    validGeneralizedAreaGeometry.get())) {
                                continue;
                            }

                            // 面内相交长度用于建立水面局部检查范围；只有后续确认
                            // 覆盖整条理论跳绘区时，才会启用水系面跳绘段数检查。
                            matchedWaterAreaLineGeometry.reset(
                                relation.lineGeometry->Intersection(
                                    validGeneralizedAreaGeometry.get()));
                            if (!matchedWaterAreaLineGeometry
                                || geometryLineLength(
                                    matchedWaterAreaLineGeometry.get()) <= 1e-8) {
                                continue;
                            }
                            waterAreaCoversJumpZone = doesAreaCoverWholeLine(
                                relation.lineGeometry.get(),
                                validGeneralizedAreaGeometry.get(), topologyTolerance);
                        }
                        else {
                            generalizedLinearGeometry =
                                extractLinearGeometry(generalizedGeometry);
                            if (!generalizedLinearGeometry
                                || generalizedLinearGeometry->IsEmpty()) {
                                continue;
                            }

                            // 仅保留原始关联段匹配范围内的综合线局部，避免其他分支
                            // 扩大最终距离允许区。
                            matchedGeneralizedGeometry.reset(
                                generalizedLinearGeometry->Intersection(matchBuffer.get()));
                            if (!matchedGeneralizedGeometry || matchedGeneralizedGeometry->IsEmpty()
                                || !isLineGeometry(matchedGeneralizedGeometry.get())) {
                                continue;
                            }

                            // 局部缓冲相交仅用于确认原始关联线与综合线同源。原始
                            // 线较曲折时，综合平滑线可能在其他位置离开局部缓冲区；
                            // 若仍将这个局部相交段用于候选搜索，成果境界线会在进入
                            // 跳绘段数和绘制距离检查前被漏掉。将原始跳绘区按里程
                            // 映射到完整综合线，后续候选搜索、P1/P2 和距离允许环
                            // 均基于这条对应的平滑参考路径。
                            JumpOgrGeometryPtr mappedGeneralizedGeometry(
                                mapOriginalJumpZoneToGeneralizedLine(
                                    relation.sourceLineGeometry.get(),
                                    relation.lineGeometry.get(),
                                    generalizedLinearGeometry.get(),
                                    rule.topologyTolerance));
                            if (mappedGeneralizedGeometry
                                && !mappedGeneralizedGeometry->IsEmpty()
                                && geometryLineLength(
                                    mappedGeneralizedGeometry.get()) > 1e-8) {
                                matchedGeneralizedGeometry = std::move(
                                    mappedGeneralizedGeometry);
                            }

                        }

                        BoundaryLineRelation matchedRelation;
                        matchedRelation.originalRelationIndex =
                            static_cast<int>(relationIndex);
                        matchedRelation.lineLayerName = relation.lineLayerName;
                        matchedRelation.lineFid = relation.lineFid;
                        matchedRelation.overlapLength = relation.overlapLength;
                        matchedRelation.hasOriginalJumpEndpoints =
                            relation.hasOriginalJumpEndpoints;
                        matchedRelation.originalJumpStartPoint =
                            relation.originalJumpStartPoint;
                        matchedRelation.originalJumpEndPoint =
                            relation.originalJumpEndPoint;
                        matchedRelation.originalJumpZoneLength =
                            relation.originalJumpZoneLength;
                        matchedRelation.nearbyWaterTransitionPoints =
                            relation.nearbyWaterTransitionPoints;
                        if (relation.overlapGeometry)
                            matchedRelation.overlapGeometry.reset(
                                relation.overlapGeometry->clone());
                        if (relation.sourceLineGeometry)
                            matchedRelation.sourceLineGeometry.reset(
                                relation.sourceLineGeometry->clone());
                        // 水系面局部相交时只保留实际面内段，避免其候选搜索范围
                        // 扩散到远处无水面的原始连续链。整段均被水面覆盖时，该
                        // 几何与原始关联链等价，可用于水系面跳绘段数检查。
                        matchedRelation.lineGeometry.reset(
                            (generalizedIsArea
                                ? matchedWaterAreaLineGeometry.get()
                                : relation.lineGeometry.get())->clone());
                        matchedRelation.generalizedLineLayerName = generalizedLayerName;
                        matchedRelation.generalizedLineFid = generalizedFeature->GetFID();
                        matchedRelation.generalizedIsArea = generalizedIsArea;
                        matchedRelation.waterAreaCoversJumpZone =
                            generalizedIsArea && waterAreaCoversJumpZone;
                        if (generalizedIsArea) {
                            matchedRelation.generalizedAreaGeometry.reset(
                                validGeneralizedAreaGeometry->clone());
                            // 水系面按实际面内关联段搜索候选。仅当该面覆盖完整
                            // 理论跳绘区时，后续才会启用水系面跳绘段数检查。
                            matchedRelation.generalizedLineGeometry.reset(
                                matchedWaterAreaLineGeometry->clone());
                        }
                        else {
                            matchedRelation.generalizedLineGeometry =
                                std::move(matchedGeneralizedGeometry);
                        }
                        matchedRelations.push_back(std::move(matchedRelation));
                        // 水系面即使只覆盖原始关联线的一部分，也已经接管该关联段，
                        // 不允许后续完整路径兜底再匹配到综合线状要素。
                        matchedOriginalRelationIndexes.insert(
                            static_cast<int>(relationIndex));

                        const QString generalizedFeatureKey = generalizedLayerName
                            + QChar(0x1f)
                            + QString::number(generalizedFeature->GetFID());
                        if (!matchedGeneralizedFeatureKeys.contains(generalizedFeatureKey)) {
                            matchedGeneralizedFeatureKeys.insert(generalizedFeatureKey);
                            ++matchedGeneralizedFeatureCount;
                        }
                    }
                    generalizedLayer->SetSpatialFilter(nullptr);
                }
            }
        }

        // 局部匹配失败不等同于该原始线已经消失。综合平滑会使综合线整体仍与
        // 原始线同源，却在当前跳绘区附近离开小缓冲区。对此按完整路径做一次
        // 兜底关联，并将原始跳绘区按里程比例映射到综合线的对应局部段。
        int globalPathMatchedCount = 0;
        for (size_t relationIndex = 0; relationIndex < relations.size(); ++relationIndex) {
            if (matchedOriginalRelationIndexes.contains(static_cast<int>(relationIndex)))
                continue;

            const BoundaryLineRelation& relation = relations[relationIndex];
            GlobalGeneralizedLineMatch globalMatch;
            if (!findGlobalGeneralizedLineMatch(
                relation, generalizedLineDatasets, rule, globalMatch)) {
                continue;
            }

            JumpOgrGeometryPtr mappedGeneralizedGeometry(
                mapOriginalJumpZoneToGeneralizedLine(
                    relation.sourceLineGeometry.get(), relation.lineGeometry.get(),
                    globalMatch.geometry.get(), rule.topologyTolerance));
            if (!mappedGeneralizedGeometry || mappedGeneralizedGeometry->IsEmpty()
                || geometryLineLength(mappedGeneralizedGeometry.get()) <= 1e-8) {
                // 连续链或多部件线无法稳定投影时，宁可使用完整综合线，也不能
                // 回退到原始跳绘区的局部缓冲相交判定。
                mappedGeneralizedGeometry.reset(globalMatch.geometry->clone());
            }

            BoundaryLineRelation matchedRelation;
            matchedRelation.originalRelationIndex = static_cast<int>(relationIndex);
            matchedRelation.lineLayerName = relation.lineLayerName;
            matchedRelation.lineFid = relation.lineFid;
            matchedRelation.overlapLength = relation.overlapLength;
            matchedRelation.hasOriginalJumpEndpoints = relation.hasOriginalJumpEndpoints;
            matchedRelation.originalJumpStartPoint = relation.originalJumpStartPoint;
            matchedRelation.originalJumpEndPoint = relation.originalJumpEndPoint;
            matchedRelation.originalJumpZoneLength = relation.originalJumpZoneLength;
            matchedRelation.nearbyWaterTransitionPoints =
                relation.nearbyWaterTransitionPoints;
            if (relation.overlapGeometry)
                matchedRelation.overlapGeometry.reset(relation.overlapGeometry->clone());
            if (relation.sourceLineGeometry)
                matchedRelation.sourceLineGeometry.reset(
                    relation.sourceLineGeometry->clone());
            if (relation.lineGeometry)
                matchedRelation.lineGeometry.reset(relation.lineGeometry->clone());
            matchedRelation.generalizedLineLayerName = globalMatch.layerName;
            matchedRelation.generalizedLineFid = globalMatch.fid;
            matchedRelation.generalizedLineGeometry = std::move(mappedGeneralizedGeometry);
            matchedRelations.push_back(std::move(matchedRelation));
            matchedOriginalRelationIndexes.insert(static_cast<int>(relationIndex));
            ++globalPathMatchedCount;

            const QString generalizedFeatureKey = globalMatch.layerName
                + QChar(0x1f) + QString::number(globalMatch.fid);
            if (!matchedGeneralizedFeatureKeys.contains(generalizedFeatureKey)) {
                matchedGeneralizedFeatureKeys.insert(generalizedFeatureKey);
                ++matchedGeneralizedFeatureCount;
            }
            logs << QString("综合后路径兜底匹配：原始图层=%1，FID=%2，综合图层=%3，FID=%4，覆盖率=%5，走向一致率=%6，90%距离=%7。")
                .arg(relation.lineLayerName)
                .arg(relation.lineFid)
                .arg(globalMatch.layerName)
                .arg(globalMatch.fid)
                .arg(globalMatch.coverageRatio, 0, 'f', 3)
                .arg(globalMatch.directionRatio, 0, 'f', 3)
                .arg(globalMatch.distanceP90, 0, 'f', 3);
        }

        const int waterRelationCountBeforeMerge = static_cast<int>(std::count_if(
            matchedRelations.cbegin(), matchedRelations.cend(),
            [](const BoundaryLineRelation& relation) {
                return relation.generalizedIsArea;
            }));
        mergeDuplicateWaterAreaRelations(matchedRelations);
        const int waterRelationCountAfterMerge = static_cast<int>(std::count_if(
            matchedRelations.cbegin(), matchedRelations.cend(),
            [](const BoundaryLineRelation& relation) {
                return relation.generalizedIsArea;
            }));
        if (waterRelationCountAfterMerge != waterRelationCountBeforeMerge) {
            logs << QString("水系面跳绘关联已按原始连续链和水系面要素合并：合并前=%1，合并后=%2。")
                .arg(waterRelationCountBeforeMerge)
                .arg(waterRelationCountAfterMerge);
        }

        // 只有候选区已匹配综合后表达，才是正式跳绘区。未匹配的原始绿线可能
        // 已被综合消隐，因此不再输出为 P1/P2，也不会参与跳绘距离和断开检查；
        // 它并未从 relations/matchedRelations 的差集剔除，仍会进入常规缺失检查。
        int confirmedJumpZoneCount = 0;
        int confirmedTheoreticalSegmentCount = 0;
        for (int relationIndex : matchedOriginalRelationIndexes) {
            if (relationIndex < 0
                || relationIndex >= static_cast<int>(relations.size())
                || !originalJumpZoneFlags[static_cast<size_t>(relationIndex)]) {
                continue;
            }

            const BoundaryLineRelation& relation =
                relations[static_cast<size_t>(relationIndex)];
            OGRPoint startPoint;
            OGRPoint endPoint;
            double jumpZoneLength = 0.0;
            if (!extractAssociatedLineChainEndpoints(
                relation.lineGeometry.get(), rule.topologyTolerance,
                startPoint, endPoint, jumpZoneLength)) {
                continue;
            }

            ++confirmedJumpZoneCount;
            JumpDrawExpectation expectation;
            if (determineJumpDrawExpectation(
                adminBoundaryGeometry.get(), relation, rule, expectation)) {
                ++confirmedTheoreticalSegmentCount;
            }
            else {
                logs << QString("正式跳绘区理论类型未识别：图层=%1，FID=%2，仍输出 P1、P2 和长度 L=%3。")
                    .arg(relation.lineLayerName)
                    .arg(relation.lineFid)
                    .arg(jumpZoneLength, 0, 'f', 3);
            }

            const OGRPoint& outputStartPoint = relation.hasOriginalJumpEndpoints
                ? relation.originalJumpStartPoint : startPoint;
            const OGRPoint& outputEndPoint = relation.hasOriginalJumpEndpoints
                ? relation.originalJumpEndPoint : endPoint;
            const double outputJumpZoneLength = relation.hasOriginalJumpEndpoints
                ? relation.originalJumpZoneLength : jumpZoneLength;

            DetectedJumpEndpoint startEndpoint;
            startEndpoint.endpointName = "P1";
            startEndpoint.endpointSource = "已匹配综合后表达的理论端点";
            startEndpoint.lineLayerName = relation.lineLayerName;
            startEndpoint.lineFid = relation.lineFid;
            startEndpoint.overlapLength = outputJumpZoneLength;
            startEndpoint.point = outputStartPoint;
            addDetectedJumpEndpoint(
                detectedEndpoints, startEndpoint, topologyTolerance);

            DetectedJumpEndpoint endEndpoint;
            endEndpoint.endpointName = "P2";
            endEndpoint.endpointSource = "已匹配综合后表达的理论端点";
            endEndpoint.lineLayerName = relation.lineLayerName;
            endEndpoint.lineFid = relation.lineFid;
            endEndpoint.overlapLength = outputJumpZoneLength;
            endEndpoint.point = outputEndPoint;
            addDetectedJumpEndpoint(
                detectedEndpoints, endEndpoint, topologyTolerance);

            for (const OGRPoint& transitionPoint : relation.nearbyWaterTransitionPoints) {
                DetectedJumpEndpoint transitionEndpoint;
                transitionEndpoint.endpointName = "水面进出点";
                transitionEndpoint.endpointSource =
                    "成果境界线与综合水系面边界交点";
                transitionEndpoint.lineLayerName = relation.lineLayerName;
                transitionEndpoint.lineFid = relation.lineFid;
                transitionEndpoint.overlapLength = outputJumpZoneLength;
                transitionEndpoint.point = transitionPoint;
                addDetectedJumpEndpoint(
                    detectedEndpoints, transitionEndpoint, topologyTolerance);
            }
        }

        int matchedOriginalJumpZoneCount = 0;
        for (int relationIndex : matchedOriginalRelationIndexes) {
            if (relationIndex >= 0
                && relationIndex < static_cast<int>(originalJumpZoneFlags.size())
                && originalJumpZoneFlags[relationIndex]) {
                ++matchedOriginalJumpZoneCount;
            }
        }
        logs << QString("跳绘区确认完成：原始候选区=%1，已匹配综合后表达并确认=%2，未确认=%3；其中完整路径兜底匹配=%4，已完成理论分段=%5。未确认候选区不输出 P1、P2，也不开展跳绘检查，但仍参与常规境界缺失检查；局部强匹配范围为原始关联段缓冲区，半径=%6。")
            .arg(originalJumpZoneCount)
            .arg(confirmedJumpZoneCount)
            .arg(std::max(0, originalJumpZoneCount - matchedOriginalJumpZoneCount))
            .arg(globalPathMatchedCount)
            .arg(confirmedTheoreticalSegmentCount)
            .arg(rule.generalizedLineMatchDistance, 0, 'f', 3);

        // ④ 匹配后的跳绘区开展成果境界线搜索、距离和断开检查。
        ResultWriter resultWriter(issues);
        int candidateBoundaryCount = 0;
        int checkedBoundaryCount = 0;
        int waterTopologyNodeCount = 0;
        int waterTopologyErrorCount = 0;
        QSet<QString> checkedBoundaryKeys;
        for (const BoundaryLineRelation& relation : matchedRelations) {
            if (!relation.lineGeometry || relation.lineGeometry->IsEmpty()) {
                continue;
            }
            if (relation.generalizedIsArea) {
                if (!relation.generalizedAreaGeometry
                    || relation.generalizedAreaGeometry->IsEmpty()) {
                    continue;
                }
            }
            else if (!relation.generalizedLineGeometry
                || relation.generalizedLineGeometry->IsEmpty()) {
                continue;
            }

            // 候选查询范围按原始 GDB 的有效关联段建立。原始关联段会拆成折线
            // 单段，每段只选择搜索缓冲区内距离最近的成果境界线局部段。
            const OGRGeometry* searchLineGeometry = relation.lineGeometry.get();
            const double outerDistance = rule.drawDistanceMax;
            const double innerDistance = rule.drawDistanceMin;

            JumpOgrGeometryPtr allowedDistanceZone;
            if (relation.generalizedIsArea) {
                allowedDistanceZone.reset(relation.generalizedAreaGeometry->clone());
            }
            else {
                // 最终距离环严格按匹配后的综合线局部建立。
                const OGRGeometry* checkingLineGeometry =
                    relation.generalizedLineGeometry.get();
                JumpOgrGeometryPtr outerDistanceBuffer(
                    checkingLineGeometry->Buffer(outerDistance));
                if (!outerDistanceBuffer || outerDistanceBuffer->IsEmpty())
                    continue;

                if (innerDistance > 0.0) {
                    // “绘制距离”是闭区间。内侧排除缓冲区略小于下限，确保
                    // 距离恰好等于下限的成果线仍处于允许环带中。
                    const double exclusiveInnerDistance =
                        std::nextafter(innerDistance, 0.0);
                    JumpOgrGeometryPtr innerDistanceBuffer(
                        checkingLineGeometry->Buffer(exclusiveInnerDistance));
                    if (!innerDistanceBuffer || innerDistanceBuffer->IsEmpty())
                        continue;

                    allowedDistanceZone.reset(
                        outerDistanceBuffer->Difference(innerDistanceBuffer.get()));
                }
                else {
                    allowedDistanceZone = std::move(outerDistanceBuffer);
                }
            }

            if (!allowedDistanceZone || allowedDistanceZone->IsEmpty())
                continue;

            JumpOgrGeometryPtr candidateSearchBuffer(
                searchLineGeometry->Buffer(rule.boundaryCandidateSearchDistance));
            if (!candidateSearchBuffer || candidateSearchBuffer->IsEmpty())
                continue;

            // 成果境界线的检索范围同时覆盖原始关联段和已匹配综合线。综合线平滑
            // 位移后，不能再只用原始绿线的小缓冲区漏掉应参与跳绘统计的成果线。
            if (!relation.generalizedIsArea) {
                JumpOgrGeometryPtr generalizedSearchBuffer(
                    relation.generalizedLineGeometry->Buffer(
                        rule.boundaryCandidateSearchDistance));
                if (generalizedSearchBuffer && !generalizedSearchBuffer->IsEmpty()) {
                    JumpOgrGeometryPtr mergedSearchBuffer(
                        candidateSearchBuffer->Union(generalizedSearchBuffer.get()));
                    if (mergedSearchBuffer && !mergedSearchBuffer->IsEmpty())
                        candidateSearchBuffer = std::move(mergedSearchBuffer);
                }
            }

            // 命中综合水系面时，综合线状表达不再参与本关联段；但水系面内的
            // 成果境界线仍须按原始跳绘区进行跳绘断开统计。
            JumpDrawExpectation jumpDrawExpectation;
            // 仅作为“已成功得到 P1'—P2' 成果跳绘区”的标记；实际裁剪由
            // 两个端点的垂线半平面完成，不使用横向缓冲区。
            JumpOgrGeometryPtr jumpZoneMarker;
            JumpOgrGeometryPtr jumpDrawGeometry;
            ProjectedJumpZone projectedJumpZone;
            // 分段统计先保留每条通过走向判定的成果境界线；待 P1/P2 投影到
            // 成果线和综合路径后，再裁剪其对应的成果跳绘区。
            std::vector<JumpOgrGeometryPtr> jumpDrawCandidates;
            std::vector<JumpOgrGeometryPtr> jumpDrawParts;
            // 已确认跳绘区的端点已在综合后表达匹配完成后输出；此处仅取理论
            // 结果，供已匹配线状要素和水系面的断开检查使用。
            const bool hasOriginalJumpZone = determineJumpDrawExpectation(
                adminBoundaryGeometry.get(), relation, rule,
                jumpDrawExpectation);
            const bool canCheckJumpBreak = hasOriginalJumpZone;
            if (!canCheckJumpBreak) {
                const QString skipReason =
                    "原始行政界线与原始线状要素未形成单一实际重叠段，或无法确定两端的同侧/异侧类型";
                logs << QString("跳绘断开检查跳过：%1，关联线要素FID=%2。")
                    .arg(skipReason)
                    .arg(relation.lineFid);
            }

            // 水系面仍以原始 GDB 关联段建立搜索范围。收集缓冲区内全部成果
            // 境界线，供后续面内与线—面边界连通检查使用。
            std::vector<JumpOgrGeometryPtr> nearbyWaterBoundaryGeometries;
            if (relation.generalizedIsArea) {
                for (int layerIndex = 0;
                    layerIndex < boundaryDataset->GetLayerCount();
                    ++layerIndex) {
                    OGRLayer* boundaryLayer = boundaryDataset->GetLayer(layerIndex);
                    if (boundaryLayer == nullptr)
                        continue;

                    const QString boundaryLayerName =
                        QString::fromUtf8(boundaryLayer->GetName());
                    boundaryLayer->SetSpatialFilter(candidateSearchBuffer.get());
                    boundaryLayer->ResetReading();
                    OGRFeature* rawBoundaryFeature = nullptr;
                    while ((rawBoundaryFeature = boundaryLayer->GetNextFeature()) != nullptr) {
                        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                            boundaryFeature(rawBoundaryFeature, OGRFeature::DestroyFeature);
                        OGRGeometry* boundaryGeometry = boundaryFeature->GetGeometryRef();
                        if (!isLineGeometry(boundaryGeometry))
                            continue;

                        JumpOgrGeometryPtr candidateGeometry(
                            boundaryGeometry->Intersection(candidateSearchBuffer.get()));
                        if (!candidateGeometry || candidateGeometry->IsEmpty()
                            || !isLineGeometry(candidateGeometry.get())) {
                            continue;
                        }

                        JumpOgrGeometryPtr filteredCandidateGeometry(
                            removeLinePartsCrossingSearchBuffer(
                                candidateGeometry.get(),
                                candidateSearchBuffer.get()));
                        if (!filteredCandidateGeometry
                            || filteredCandidateGeometry->IsEmpty()
                            || geometryLineLength(filteredCandidateGeometry.get()) <= 1e-8) {
                            continue;
                        }

                        nearbyWaterBoundaryGeometries.push_back(
                            std::move(filteredCandidateGeometry));
                    }
                    boundaryLayer->SetSpatialFilter(nullptr);
                }
            }

            for (int layerIndex = 0; layerIndex < boundaryDataset->GetLayerCount(); ++layerIndex) {
                OGRLayer* boundaryLayer = boundaryDataset->GetLayer(layerIndex);
                if (boundaryLayer == nullptr)
                    continue;

                const QString boundaryLayerName = QString::fromUtf8(boundaryLayer->GetName());
                boundaryLayer->SetSpatialFilter(candidateSearchBuffer.get());
                boundaryLayer->ResetReading();
                OGRFeature* rawBoundaryFeature = nullptr;
                while ((rawBoundaryFeature = boundaryLayer->GetNextFeature()) != nullptr) {
                    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                        boundaryFeature(rawBoundaryFeature, OGRFeature::DestroyFeature);
                    OGRGeometry* boundaryGeometry = boundaryFeature->GetGeometryRef();
                    if (!isLineGeometry(boundaryGeometry))
                        continue;

                    JumpOgrGeometryPtr candidateBoundaryGeometry(
                        boundaryGeometry->Intersection(candidateSearchBuffer.get()));
                    if (!candidateBoundaryGeometry || candidateBoundaryGeometry->IsEmpty()
                        || !isLineGeometry(candidateBoundaryGeometry.get())) {
                        continue;
                    }

                    // 水系面的绘制距离检查仍沿用搜索缓冲区穿越线过滤；但跳绘
                    // 统计由 P1/P2 确定最终范围，不能因候选线恰好横穿搜索缓冲区
                    // 而丢失一个实际跳绘弧段，因此单独保留过滤前的局部段。
                    JumpOgrGeometryPtr waterJumpCandidateGeometry;
                    bool skipWaterDistanceCandidate = false;
                    if (relation.generalizedIsArea) {
                        waterJumpCandidateGeometry.reset(
                            candidateBoundaryGeometry->clone());
                    }

                    if (relation.generalizedIsArea) {
                        JumpOgrGeometryPtr filteredCandidateGeometry(
                            removeLinePartsCrossingSearchBuffer(
                                candidateBoundaryGeometry.get(),
                                candidateSearchBuffer.get()));
                        if (!filteredCandidateGeometry
                            || filteredCandidateGeometry->IsEmpty()
                            || geometryLineLength(filteredCandidateGeometry.get()) <= 1e-8) {
                            // 该候选不参与水系面绘制距离和边界连通检查，但仍需
                            // 用过滤前几何参加下方跳绘分段统计。
                            skipWaterDistanceCandidate = true;
                        }
                        else {
                            candidateBoundaryGeometry = std::move(filteredCandidateGeometry);
                        }

                    }

                    // 断开分段统计不受绘制距离走向过滤影响。水系面候选必须
                    // 与水系面有实际线长度交集；其面外局部段单独按
                    // DRAW_DIST_ERR 输出，但不把同一条连续线切成跳绘断开。
                    if (canCheckJumpBreak) {
                        JumpOgrGeometryPtr jumpDrawCandidateGeometry;
                        if (relation.generalizedIsArea) {
                            // 先确认该候选在线面内确实存在有效长度；统计时保留
                            // 原线的连续性，不能因少量面外 DRAW_DIST_ERR 段把一条
                            // 连续境界线人为拆成两个跳绘弧段。
                            JumpOgrGeometryPtr waterInteriorGeometry(
                                waterJumpCandidateGeometry->Intersection(
                                    relation.generalizedAreaGeometry.get()));
                            if (waterInteriorGeometry
                                && !waterInteriorGeometry->IsEmpty()
                                && geometryLineLength(waterInteriorGeometry.get()) > 1e-8) {
                                jumpDrawCandidateGeometry.reset(
                                    waterJumpCandidateGeometry->clone());
                            }
                        }
                        else {
                            jumpDrawCandidateGeometry.reset(
                                candidateBoundaryGeometry->clone());
                        }
                        if (jumpDrawCandidateGeometry
                            && !jumpDrawCandidateGeometry->IsEmpty()
                            && geometryLineLength(jumpDrawCandidateGeometry.get()) > 1e-8) {
                            jumpDrawCandidates.emplace_back(
                                std::move(jumpDrawCandidateGeometry));
                            logs << QString("参与跳绘段数候选：图层=%1，FID=%2，关联线要素FID=%3，原始连续链=%4，表达=%5。")
                                .arg(boundaryLayerName)
                                .arg(boundaryFeature->GetFID())
                                .arg(relation.lineFid)
                                .arg(relation.originalRelationIndex)
                                .arg(relation.generalizedIsArea ? "水系面" : "线状");
                        }
                    }

                    if (skipWaterDistanceCandidate)
                        continue;

                    if (!relation.generalizedIsArea) {
                        // 走向过滤按顶点小段执行，适合距离判定；但它会把一条
                        // 弯曲的连续成果境界线拆成若干小段。跳绘弧段统计应保留
                        // 该成果线在跳绘区内的完整连续局部几何。更不能以走向
                        // 过滤作为“是否存在跳绘弧段”的前提：只要成果线落入
                        // P1'—P2' 跳绘区，就必须参与实际弧段/空白段统计。
                        JumpOgrGeometryPtr checkingBoundaryGeometry(
                            retainDirectionCompatibleCandidateSegments(
                                candidateBoundaryGeometry.get(),
                                relation.generalizedLineGeometry.get(),
                                minimumDirectionCosine));
                        if (!checkingBoundaryGeometry
                            || checkingBoundaryGeometry->IsEmpty()
                            || geometryLineLength(checkingBoundaryGeometry.get()) <= 1e-8) {
                            continue;
                        }
                        candidateBoundaryGeometry = std::move(checkingBoundaryGeometry);
                    }

                    ++candidateBoundaryCount;
                    ++checkedBoundaryCount;
                    const QString checkedBoundaryKey = boundaryLayerName
                        + QChar(0x1f)
                        + QString::number(boundaryFeature->GetFID());
                    if (!checkedBoundaryKeys.contains(checkedBoundaryKey)) {
                        checkedBoundaryKeys.insert(checkedBoundaryKey);
                        logs << QString("参与跳绘距离检查的成果境界线：图层=%1，FID=%2。")
                            .arg(boundaryLayerName)
                            .arg(boundaryFeature->GetFID());
                    }
                    // 水系面只判断包含关系：在线面内或完整贴在面边界上即合格。
                    // 不以 Difference 的结果决定“是否合格”，避免复杂面叠加产生误报。
                    if (relation.generalizedIsArea) {
                        const OGRGeometry* waterArea =
                            relation.generalizedAreaGeometry.get();
                        JumpOgrGeometryPtr waterAreaBoundary(waterArea->Boundary());
                        if (waterAreaBoundary && !waterAreaBoundary->IsEmpty()) {
                            std::vector<OGRPoint> topologyNodes;
                            JumpOgrGeometryPtr intersectionGeometry(
                                candidateBoundaryGeometry->Intersection(
                                    waterAreaBoundary.get()));
                            std::vector<OGRPoint> intersectionPoints;
                            addPointParts(intersectionGeometry.get(), intersectionPoints);
                            for (const OGRPoint& point : intersectionPoints)
                                addUniqueTopologyPoint(topologyNodes, point);

                            for (const OGRPoint& node : topologyNodes) {
                                ++waterTopologyNodeCount;
                                if (hasContinuousBoundaryAcrossWaterArea(
                                    nearbyWaterBoundaryGeometries,
                                    waterArea,
                                    node)) {
                                    continue;
                                }

                                const QString issueMessage = QString(
                                    "成果境界线在综合水系面边界处未形成面外—面内连续连接；"
                                    "综合水系面要素FID=%1。")
                                    .arg(relation.generalizedLineFid);
                                ++waterTopologyErrorCount;
                                resultWriter.addIssue(
                                    "WATER_TOPO_ERR",
                                    boundaryLayerName,
                                    boundaryFeature->GetFID(),
                                    relation.generalizedLineLayerName,
                                    relation.generalizedLineFid,
                                    issueMessage,
                                    &node);
                            }
                        }
                        if (candidateBoundaryGeometry->Within(waterArea)
                            || candidateBoundaryGeometry->Touches(waterArea)) {
                            continue;
                        }
                    }

                    JumpOgrGeometryPtr outsideAllowedZone(
                        candidateBoundaryGeometry->Difference(allowedDistanceZone.get()));
                    if (!outsideAllowedZone || outsideAllowedZone->IsEmpty())
                        continue;

                    // 面内覆盖判断的差集偶尔只会留下端点或接触点；它们没有实际的
                    // 面外线段，不应被写成 DRAW_DIST_ERR。
                    if (relation.generalizedIsArea
                        && geometryLineLength(outsideAllowedZone.get()) <= 1e-8) {
                        continue;
                    }

                    if (relation.generalizedIsArea) {
                        // 单端/双端属性必须按完整成果境界线判定，不能按搜索缓冲区内
                        // 的裁剪段判定。否则一段本来两端都回接水系面边界的面外线，
                        // 会因缓冲区截掉其中一端而被误当作单端段过滤。
                        const OGRGeometry* waterArea =
                            relation.generalizedAreaGeometry.get();
                        JumpOgrGeometryPtr fullExteriorGeometry(
                            boundaryGeometry->Difference(waterArea));
                        if (!fullExteriorGeometry || fullExteriorGeometry->IsEmpty()
                            || geometryLineLength(fullExteriorGeometry.get()) <= 1e-8) {
                            continue;
                        }

                        JumpOgrGeometryPtr retainedFullExteriorGeometry(
                            removeOneEndedExteriorAreaBranches(
                                fullExteriorGeometry.get(),
                                waterArea,
                                topologyTolerance));
                        if (!retainedFullExteriorGeometry
                            || retainedFullExteriorGeometry->IsEmpty()
                            || geometryLineLength(retainedFullExteriorGeometry.get()) <= 1e-8) {
                            continue;
                        }

                        // 最终只输出当前原始关联段搜索缓冲区内的错误位置。
                        JumpOgrGeometryPtr retainedExteriorGeometry(
                            retainedFullExteriorGeometry->Intersection(
                                candidateSearchBuffer.get()));
                        if (!retainedExteriorGeometry
                            || retainedExteriorGeometry->IsEmpty()
                            || geometryLineLength(retainedExteriorGeometry.get()) <= 1e-8) {
                            continue;
                        }

                        outsideAllowedZone = std::move(retainedExteriorGeometry);
                    }

                    const QString issueMessage = relation.generalizedIsArea
                        ? QString("成果境界线未完全落入匹配后的综合水系面；原始关联重叠长度=%1，综合水系面要素FID=%2。")
                        .arg(relation.overlapLength, 0, 'f', 3)
                        .arg(relation.generalizedLineFid)
                        : QString("成果境界线未完全落入 XML 绘制距离闭区间；允许距离范围=%1-%2（图上%3-%4mm，含上下限），原始关联重叠长度=%5，匹配后的综合线状要素FID=%6。")
                        .arg(innerDistance, 0, 'f', 3)
                        .arg(outerDistance, 0, 'f', 3)
                        .arg(rule.drawDistanceMinMm, 0, 'f', 3)
                        .arg(rule.drawDistanceMaxMm, 0, 'f', 3)
                        .arg(relation.overlapLength, 0, 'f', 3)
                        .arg(relation.generalizedLineFid);

                    resultWriter.addIssue(
                        relation.generalizedIsArea
                            ? "WATER_AREA_OUTSIDE_ERR" : "DRAW_DIST_ERR",
                        boundaryLayerName,
                        boundaryFeature->GetFID(),
                        relation.generalizedLineLayerName,
                        relation.generalizedLineFid,
                        issueMessage,
                        outsideAllowedZone.get());
                }
                boundaryLayer->SetSpatialFilter(nullptr);
            }

            if (canCheckJumpBreak) {
                // 线状要素的 P1/P2 优先吸附到最近的成果境界线，再投影到同一条
                // 已匹配综合路径；水系面则直接使用理论 P1/P2。若区内没有成果
                // 境界线，均保留原始端点投影的兜底，才能正确判为“0 条成果线、
                // 1 段空白”。
                OGRPoint startAnchor = jumpDrawExpectation.startPoint;
                OGRPoint endAnchor = jumpDrawExpectation.endPoint;
                bool snappedStart = false;
                bool snappedEnd = false;
                // 线状综合要素可能相对原始线发生平滑位移，仍需按最近成果
                // 境界线吸附 P1/P2。水系面直接使用理论 P1/P2，避免缩短完整
                // 原始连续关联链对应的跳绘区。
                if (!relation.generalizedIsArea) {
                    for (const JumpOgrGeometryPtr& candidate : jumpDrawCandidates) {
                        OGRPoint projectedPoint;
                        if (projectPointToLineGeometry(
                            jumpDrawExpectation.startPoint, candidate.get(),
                            projectedPoint)) {
                            if (!snappedStart
                                || projectedPoint.Distance(&jumpDrawExpectation.startPoint)
                                < startAnchor.Distance(&jumpDrawExpectation.startPoint)) {
                                startAnchor = projectedPoint;
                                snappedStart = true;
                            }
                        }
                        if (projectPointToLineGeometry(
                            jumpDrawExpectation.endPoint, candidate.get(),
                            projectedPoint)) {
                            if (!snappedEnd
                                || projectedPoint.Distance(&jumpDrawExpectation.endPoint)
                                < endAnchor.Distance(&jumpDrawExpectation.endPoint)) {
                                endAnchor = projectedPoint;
                                snappedEnd = true;
                            }
                        }
                    }
                }

                if (buildProjectedJumpZone(
                    relation, startAnchor, endAnchor, projectedJumpZone)) {
                    jumpZoneMarker.reset(projectedJumpZone.geometry->clone());
                    if (jumpZoneMarker && !jumpZoneMarker->IsEmpty()) {
                        for (const JumpOgrGeometryPtr& candidate : jumpDrawCandidates) {
                            if (relation.generalizedIsArea) {
                                // 水系面：以 P1'—P2' 内的完整成果局部段保持
                                // 连续性，避免少量面外 DRAW_DIST_ERR 段被误当成
                                // 新的跳绘断开。JUMP_BREAK_ERR 是整个跳绘区级
                                // 错误，输出也必须保留该区内完整境界线，不能再
                                // 按水系面边界切成若干不连续小段。
                                JumpOgrGeometryPtr jumpZoneBoundaryPart(
                                    clipGeometryForJumpDisplay(
                                        candidate.get(), projectedJumpZone));
                                if (!jumpZoneBoundaryPart
                                    || jumpZoneBoundaryPart->IsEmpty()
                                    || geometryLineLength(
                                        jumpZoneBoundaryPart.get()) <= 1e-8) {
                                    continue;
                                }
                                JumpOgrGeometryPtr displayBoundaryPart(
                                    jumpZoneBoundaryPart->clone());
                                if (!displayBoundaryPart
                                    || displayBoundaryPart->IsEmpty()
                                    || geometryLineLength(
                                        displayBoundaryPart.get()) <= 1e-8) {
                                    continue;
                                }
                                jumpDrawParts.emplace_back(
                                    std::move(jumpZoneBoundaryPart));
                                if (!jumpDrawGeometry) {
                                    jumpDrawGeometry = std::move(displayBoundaryPart);
                                }
                                else {
                                    JumpOgrGeometryPtr mergedJumpDrawGeometry(
                                        jumpDrawGeometry->Union(
                                            displayBoundaryPart.get()));
                                    if (mergedJumpDrawGeometry
                                        && !mergedJumpDrawGeometry->IsEmpty()) {
                                        jumpDrawGeometry = std::move(mergedJumpDrawGeometry);
                                    }
                                }
                                continue;
                            }

                            // 线状要素保持原有统计范围：候选局部线直接参与弧段
                            // 统计，P1'—P2' 裁剪仅用于错误图层显示。
                            JumpOgrGeometryPtr jumpZoneBoundaryPart(candidate->clone());
                            if (!jumpZoneBoundaryPart || jumpZoneBoundaryPart->IsEmpty()
                                || geometryLineLength(jumpZoneBoundaryPart.get()) <= 1e-8) {
                                continue;
                            }
                            jumpDrawParts.emplace_back(
                                std::move(jumpZoneBoundaryPart));
                            JumpOgrGeometryPtr displayBoundaryPart(
                                clipGeometryForJumpDisplay(
                                    jumpDrawParts.back().get(), projectedJumpZone));
                            if (!displayBoundaryPart || displayBoundaryPart->IsEmpty()
                                || geometryLineLength(displayBoundaryPart.get()) <= 1e-8) {
                                continue;
                            }
                            if (!jumpDrawGeometry) {
                                jumpDrawGeometry = std::move(displayBoundaryPart);
                            }
                            else {
                                JumpOgrGeometryPtr mergedJumpDrawGeometry(
                                    jumpDrawGeometry->Union(displayBoundaryPart.get()));
                                if (mergedJumpDrawGeometry
                                    && !mergedJumpDrawGeometry->IsEmpty()) {
                                    jumpDrawGeometry = std::move(mergedJumpDrawGeometry);
                                }
                            }
                        }
                        logs << QString("跳绘区已映射到成果路径：P1=%1，P2=%2，成果端点吸附=%3/%4，候选成果线=%5，弧段范围=已关联成果候选。")
                            .arg(projectedJumpZone.startMeasure, 0, 'f', 3)
                            .arg(projectedJumpZone.endMeasure, 0, 'f', 3)
                            .arg(snappedStart ? "是" : "否")
                            .arg(snappedEnd ? "是" : "否")
                            .arg(jumpDrawCandidates.size());
                        logs << QString("跳绘弧段统计输入：成果候选线组件=%1，P1'—P2'有效连通组件=%2，跳绘区内要素数=%3；关联线要素FID=%4。")
                            .arg(countConnectedLineComponents(
                                jumpDrawCandidates, topologyTolerance))
                            .arg(countConnectedLineComponents(
                                jumpDrawParts, topologyTolerance))
                            .arg(jumpDrawParts.size())
                            .arg(relation.lineFid);
                    }
                    else {
                        jumpZoneMarker.reset();
                    }
                }
                else {
                    logs << QString("跳绘断开检查跳过：P1/P2 无法投影到匹配后的综合路径，关联线要素FID=%1。")
                        .arg(relation.lineFid);
                }
            }

            if (jumpZoneMarker) {
                int actualDrawArcCount = 0;
                int actualBlankSegmentCount = 0;
                const bool drawCountedByEndpoints = countJumpDrawSegmentsByEndpoints(
                    projectedJumpZone, jumpDrawParts, topologyTolerance,
                    actualDrawArcCount);
                const bool blankCountedByMeasure = countLogicalJumpBlankSegments(
                    projectedJumpZone, jumpDrawParts, topologyTolerance,
                    actualBlankSegmentCount);
                if (!drawCountedByEndpoints) {
                    actualDrawArcCount = countConnectedLineComponents(
                        jumpDrawGeometry.get(), topologyTolerance);
                    logs << QString("跳绘弧段统计回退为几何连通块：关联线要素FID=%1。")
                        .arg(relation.lineFid);
                }
                if (!blankCountedByMeasure) {
                    // 极少数多支路、非单链几何无法投影到同一里程基线时，保留
                    // 旧的几何 Difference 空白段统计作为兜底，并在日志中明确标识。
                    if (relation.lineGeometry && !relation.lineGeometry->IsEmpty()) {
                        if (jumpDrawGeometry && !jumpDrawGeometry->IsEmpty()) {
                            JumpOgrGeometryPtr drawCoverage(
                                jumpDrawGeometry->Buffer(topologyTolerance));
                            JumpOgrGeometryPtr blankGeometry;
                            if (drawCoverage && !drawCoverage->IsEmpty()) {
                                blankGeometry.reset(relation.lineGeometry->Difference(
                                    drawCoverage.get()));
                            }
                            actualBlankSegmentCount = countConnectedLineComponents(
                                blankGeometry.get(), topologyTolerance);
                        }
                        else {
                            actualBlankSegmentCount = countConnectedLineComponents(
                                relation.lineGeometry.get(), topologyTolerance);
                        }
                    }
                    logs << QString("跳绘空白段统计回退为几何 Difference：关联线要素FID=%1。")
                        .arg(relation.lineFid);
                }

                const int actualSegmentCount = actualDrawArcCount
                    + actualBlankSegmentCount;
                const bool isBreakCompliant = jumpDrawExpectation.expectedSegmentCount == 1
                    ? actualDrawArcCount == 0 && actualBlankSegmentCount == 1
                    : actualSegmentCount == jumpDrawExpectation.expectedSegmentCount;
                if (!isBreakCompliant) {
                    const QString typeName = jumpDrawExpectation.type == JumpDrawType::SameSide
                        ? "同侧" : "异侧";
                    const QString issueMessage = QString(
                        "跳绘断开分段数不符合规则：类型=%1，跳绘区长度=%2，理论分段数N=%3，实际分段数=%4，实际跳绘弧段数=%5，实际空白断开段数=%6，统计方式=%7。")
                        .arg(typeName)
                        .arg(jumpDrawExpectation.zoneLength, 0, 'f', 3)
                        .arg(jumpDrawExpectation.expectedSegmentCount)
                        .arg(actualSegmentCount)
                        .arg(actualDrawArcCount)
                        .arg(actualBlankSegmentCount)
                        .arg(drawCountedByEndpoints && blankCountedByMeasure
                            ? "成果端点投影+成果里程空白" : "含回退统计");
                    // 断开错误只输出 P1—P2 跳绘区内实际参与分段统计的成果
                    // 境界线，不输出同一 FID 的完整行政区界，避免带出无关区段。
                    const OGRGeometry* issueGeometry = jumpDrawGeometry
                        ? jumpDrawGeometry.get() : relation.lineGeometry.get();
                    // 原始连续链合并后 FID 可能为 -1，不能再仅凭该 FID 汇总。
                    // 同一跳绘区以“原始连续链 + 理论 P1/P2 + 跳绘距离”识别，
                    // 不以综合线 FID 区分。这样同一跳绘区关联到 FID 18、19、20
                    // 时会合并；其他位置即使同属一条原始连续链也不会混在一起。
                    const QString jumpZoneMergeKey = QString("JUMP_GROUP:%1")
                        .arg(relation.originalRelationIndex);
                    resultWriter.addIssue(
                        "JUMP_BREAK_ERR",
                        relation.lineLayerName,
                        relation.lineFid,
                        relation.generalizedLineLayerName,
                        relation.generalizedLineFid,
                        issueMessage,
                        issueGeometry,
                        jumpZoneMergeKey);
                }

                logs << QString("跳绘断开检查：类型=%1，跳绘区长度=%2，理论N=%3，实际N=%4，跳绘弧段=%5，空白断开段=%6，统计方式=%7。")
                    .arg(jumpDrawExpectation.type == JumpDrawType::SameSide ? "同侧" : "异侧")
                    .arg(jumpDrawExpectation.zoneLength, 0, 'f', 3)
                    .arg(jumpDrawExpectation.expectedSegmentCount)
                    .arg(actualSegmentCount)
                    .arg(actualDrawArcCount)
                    .arg(actualBlankSegmentCount)
                    .arg(drawCountedByEndpoints && blankCountedByMeasure
                        ? "成果端点投影+成果里程空白" : "含回退统计");
            }
        }

        if (!collectBoundaryMissingIssues(
            boundaryDataset,
            adminDataset,
            matchedRelations,
            rule,
            issues,
            logs,
            error)) {
            return false;
        }

        logs << QString("境界线缺失和跳绘检查完成：候选境界线=%1，实际检查=%2，参与检查的唯一成果境界线=%3，问题数=%4，候选搜索半径=%5，绘制距离闭区间=%6-%7（图层单位）。")
            .arg(candidateBoundaryCount)
            .arg(checkedBoundaryCount)
            .arg(checkedBoundaryKeys.size())
            .arg(issues.size())
            .arg(rule.boundaryCandidateSearchDistance, 0, 'f', 3)
            .arg(rule.drawDistanceMin, 0, 'f', 3)
            .arg(rule.drawDistanceMax, 0, 'f', 3);
        if (waterTopologyNodeCount <= 0) {
            logs << "水系面边界连通性检查：未发现可检查的境界线入水交点。";
        }
        else if (waterTopologyErrorCount <= 0) {
            logs << QString("水系面边界连通性检查完成：检查入水交点=%1，未检查到问题。")
                .arg(waterTopologyNodeCount);
        }
        else {
            logs << QString("水系面边界连通性检查完成：检查入水交点=%1，发现不连通节点=%2。")
                .arg(waterTopologyNodeCount)
                .arg(waterTopologyErrorCount);
        }
        return true;
    }

}


se_boundary_jump_draw_check_algorithm::se_boundary_jump_draw_check_algorithm(
    const QString& boundaryShpPath,
    const QString& otherShpPath,
    QWidget* parent,
    Qt::WindowFlags fl)
    : QDialog(parent, fl)
    , ui(new Ui::se_boundary_jump_draw_check_algorithm)
    , m_boundaryPath(boundaryShpPath)
    , m_otherPath(otherShpPath)
{
    ui->setupUi(this);
    GDALAllRegister();

    if (!m_boundaryPath.isEmpty())
        ui->lineEdit_jj->setText(m_boundaryPath);

    if (!m_otherPath.isEmpty())
        ui->lineEdit_hldl->setText(m_otherPath);

    appendLog("境界线缺失和跳绘检查模块已初始化。");
}

se_boundary_jump_draw_check_algorithm::~se_boundary_jump_draw_check_algorithm()
{
    delete ui;
}

void se_boundary_jump_draw_check_algorithm::on_pushButton_jj_clicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        "选择解析后的境界线数据",
        QString(),
        "矢量数据 (*.shp);;所有文件 (*.*)");

    if (path.isEmpty())
        return;

    m_boundaryPath = path;
    ui->lineEdit_jj->setText(path);
    resetLogAfterReload("已重新加载解析后的境界线数据");
}

void se_boundary_jump_draw_check_algorithm::on_pushButton_xzq_clicked()
{
    const QString path = selectShpPath(this, "选择原始行政界线 SHP 数据");

    if (path.isEmpty())
        return;

    m_adminPath = path;
    ui->lineEdit_xzq->setText(path);
    resetLogAfterReload("已重新加载原始行政界线 SHP 数据");
}

void se_boundary_jump_draw_check_algorithm::on_pushButton_hldl_clicked()
{
    const QString path = selectShpPath(this, "选择水系、道路等线状要素 SHP 数据");

    if (path.isEmpty())
        return;

    m_otherPath = path;
    ui->lineEdit_hldl->setText(path);
    resetLogAfterReload("已重新加载水系、道路等线状要素 SHP 数据");
}

void se_boundary_jump_draw_check_algorithm::on_pushButton_hldlshp_clicked()
{
    const QStringList paths = selectShpPaths(
        this,
        "选择综合后水系、道路等线状要素 SHP 数据（可多选，含水系面）");

    if (paths.isEmpty())
        return;

    m_generalizedLinePath = paths.join(";");
    ui->lineEdit_hldlshp->setText(m_generalizedLinePath);
    resetLogAfterReload(QString("已重新加载综合后水系、道路等线状要素 SHP 数据：%1 个文件")
        .arg(paths.size()));
}

void se_boundary_jump_draw_check_algorithm::on_pushButton_xml_clicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        "选择 XML 规则文件",
        QString(),
        "XML 文件 (*.xml)");

    if (path.isEmpty())
        return;

    m_xmlPath = path;
    ui->lineEdit_xml->setText(path);
    resetLogAfterReload("已重新加载 XML 规则文件");
}

void se_boundary_jump_draw_check_algorithm::on_pushButton_start_clicked()
{
    m_boundaryPath = ui->lineEdit_jj->text().trimmed();
    m_adminPath = ui->lineEdit_xzq->text().trimmed();
    m_otherPath = ui->lineEdit_hldl->text().trimmed();
    m_generalizedLinePath = ui->lineEdit_hldlshp->text().trimmed();
    m_xmlPath = ui->lineEdit_xml->text().trimmed();

    if (m_boundaryPath.isEmpty() || m_adminPath.isEmpty() || m_otherPath.isEmpty()
        || m_generalizedLinePath.isEmpty() || m_xmlPath.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先选择解析后的境界线数据、行政界线 SHP、原始线状要素 SHP、综合后线状要素 SHP 和 XML 规则文件。");
        return;
    }

    appendLog("开始执行境界线缺失和跳绘检查。");

    int problemCount = 0;
    QString message;
    if (!runCheck(QString(), false, problemCount, message)) {
        m_lastCheckFinished = false;
        appendLog("检查失败：" + message);
        QMessageBox::critical(this, "检查失败", message);
        return;
    }

    m_lastCheckedBoundaryPath = m_boundaryPath;
    m_lastCheckedAdminPath = m_adminPath;
    m_lastCheckedOtherPath = m_otherPath;
    m_lastCheckedGeneralizedLinePath = m_generalizedLinePath;
    m_lastCheckedXmlPath = m_xmlPath;
    m_lastCheckFinished = true;
    m_lastProblemCount = problemCount;

    appendLog(message);
    QMessageBox::information(this, "检查完成", message);
}

void se_boundary_jump_draw_check_algorithm::on_pushButton_shuchu_clicked()
{
    m_boundaryPath = ui->lineEdit_jj->text().trimmed();
    m_adminPath = ui->lineEdit_xzq->text().trimmed();
    m_otherPath = ui->lineEdit_hldl->text().trimmed();
    m_generalizedLinePath = ui->lineEdit_hldlshp->text().trimmed();
    m_xmlPath = ui->lineEdit_xml->text().trimmed();

    if (m_boundaryPath.isEmpty() || m_adminPath.isEmpty() || m_otherPath.isEmpty()
        || m_generalizedLinePath.isEmpty() || m_xmlPath.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先选择解析后的境界线数据、行政界线 SHP、原始线状要素 SHP、综合后线状要素 SHP 和 XML 规则文件。");
        return;
    }

    const bool needFreshCheck =
        !m_lastCheckFinished ||
        m_lastCheckedBoundaryPath != m_boundaryPath ||
        m_lastCheckedAdminPath != m_adminPath ||
        m_lastCheckedOtherPath != m_otherPath ||
        m_lastCheckedGeneralizedLinePath != m_generalizedLinePath ||
        m_lastCheckedXmlPath != m_xmlPath;

    if (needFreshCheck) {
        appendLog("输出前未找到当前数据的检查结果，先执行一次检查。");

        int checkProblemCount = 0;
        QString checkMessage;
        if (!runCheck(QString(), false, checkProblemCount, checkMessage)) {
            m_lastCheckFinished = false;
            appendLog("检查失败：" + checkMessage);
            QMessageBox::critical(this, "检查失败", checkMessage);
            return;
        }

        m_lastCheckedBoundaryPath = m_boundaryPath;
        m_lastCheckedAdminPath = m_adminPath;
        m_lastCheckedOtherPath = m_otherPath;
        m_lastCheckedGeneralizedLinePath = m_generalizedLinePath;
        m_lastCheckedXmlPath = m_xmlPath;
        m_lastCheckFinished = true;
        m_lastProblemCount = checkProblemCount;

        appendLog(checkMessage);
    }

    if (m_lastProblemCount == 0) {
        const QString message = "未发现境界线跳绘问题，无需输出错误图层。";
        appendLog(message);
        QMessageBox::information(this, "提示", message);
        return;
    }

    const QString defaultPath =
        QFileInfo(m_boundaryPath).absolutePath() + "/boundary_jump_result.shp";

    const QString outputBaseShpPath = QFileDialog::getSaveFileName(
        this,
        "保存境界线缺失和跳绘检查结果",
        defaultPath,
        "Shapefile (*.shp)");

    if (outputBaseShpPath.isEmpty())
        return;

    appendLog("开始输出境界线缺失和跳绘错误图层。");

    int outputProblemCount = 0;
    QString message;
    if (!runCheck(outputBaseShpPath, true, outputProblemCount, message)) {
        appendLog("检查失败：" + message);
        QMessageBox::critical(this, "检查失败", message);
        return;
    }

    m_lastProblemCount = outputProblemCount;
    appendLog(message);
    QMessageBox::information(this, "检查完成", message);
}

void se_boundary_jump_draw_check_algorithm::appendLog(const QString& message)
{
    const QString timeText =
        QDateTime::currentDateTime().toString("[yyyy-MM-dd HH:mm:ss] ");
    QTextEdit* logEdit = findChild<QTextEdit*>("textEdit_log");
    if (logEdit != nullptr)
        logEdit->append(timeText + message);

    QApplication::processEvents();
}

void se_boundary_jump_draw_check_algorithm::resetLogAfterReload(
    const QString& reloadMessage)
{
    QTextEdit* logEdit = findChild<QTextEdit*>("textEdit_log");
    if (logEdit != nullptr)
        logEdit->clear();

    m_lastCheckFinished = false;
    m_lastProblemCount = 0;
    appendLog(reloadMessage);
}

bool se_boundary_jump_draw_check_algorithm::runCheck(
    const QString& outputBaseShpPath,
    bool writeOutput,
    int& problemCount,
    QString& message)
{
    BoundaryJumpCheckResult result;
    const bool ok = RunBoundaryJumpDrawCheck(
        m_boundaryPath, m_adminPath, m_otherPath, m_generalizedLinePath, m_xmlPath,
        kDefaultMapScaleDenominator, writeOutput ? outputBaseShpPath : QString(), result,
        [this](const QString& text) { appendLog(text); });
    problemCount = result.problemCount;
    message = result.message;
    return ok;
}

bool se_boundary_jump_draw_check_algorithm::RunBoundaryJumpDrawCheck(
    const QString& boundaryShpPath,
    const QString& adminShpPath,
    const QString& otherShpPaths,
    const QString& generalizedShpPaths,
    const QString& xmlPath,
    int scale,
    const QString& outputBaseShpPath,
    BoundaryJumpCheckResult& result,
    const std::function<void(const QString&)>& log)
{
    GDALAllRegister();
    return runCore(boundaryShpPath, adminShpPath, otherShpPaths, generalizedShpPaths,
        xmlPath, scale, outputBaseShpPath, !outputBaseShpPath.trimmed().isEmpty(), result, log);
}

bool se_boundary_jump_draw_check_algorithm::runCore(
    const QString& boundaryShpPath,
    const QString& adminShpPath,
    const QString& otherShpPaths,
    const QString& generalizedShpPaths,
    const QString& xmlPath,
    int scale,
    const QString& outputBaseShpPath,
    bool writeOutput,
    BoundaryJumpCheckResult& result,
    const std::function<void(const QString&)>& log)
{
    result = BoundaryJumpCheckResult();
    const auto writeLog = [&log](const QString& text) { if (log) log(text); };

    if (boundaryShpPath.trimmed().isEmpty() || adminShpPath.trimmed().isEmpty()
        || otherShpPaths.trimmed().isEmpty() || generalizedShpPaths.trimmed().isEmpty()
        || xmlPath.trimmed().isEmpty()) {
        result.message = "境界线缺失和跳绘检查需要成果境界线、GDB 行政界线、关联 GDB 线要素、关联解析后图面要素和 XML 规则。";
        return false;
    }
    if (scale <= 0) {
        result.message = "未读取到有效比例尺，无法执行境界线缺失和跳绘检查。";
        return false;
    }

    BoundaryJumpRule rule;
    QString error;
    if (!readRuleFromXml(xmlPath, boundaryShpPath, scale, rule, error)) {
        result.message = error;
        return false;
    }

    writeLog(QString("规则读取完成：重叠长度=%1cm，重叠拓扑容差=%2mm，匹配缓冲区=%3mm，搜索缓冲区=%4mm，绘制距离闭区间=%5-%6mm，走向夹角阈值=%7°，符号长度d=%8mm，最大跳绘步长系数=%9，比例尺=1:%10。")
        .arg(rule.overlapLengthCm).arg(rule.topologyToleranceMm)
        .arg(rule.generalizedLineMatchDistanceMm).arg(rule.boundaryCandidateSearchDistanceMm)
        .arg(rule.drawDistanceMinMm).arg(rule.drawDistanceMaxMm)
        .arg(rule.directionAngleThresholdDeg).arg(rule.boundarySymbolLengthMm)
        .arg(rule.maxJumpStepCoefficient).arg(rule.scaleDenominator, 0, 'f', 0));
    writeLog(QString("非跳绘区检查规则：属性项=%1，属性值=%2，缺失缓冲区半径=%3mm，偏移距离=%4mm。")
        .arg(rule.missingBoundaryAttributeName).arg(rule.missingBoundaryAttributeValue)
        .arg(rule.missingBoundaryBufferRadiusMm)
        .arg(rule.nonJumpOffsetDistanceMm));

    std::vector<BoundaryJumpIssue> issues;
    std::vector<DetectedJumpEndpoint> detectedEndpoints;
    if (!collectJumpIssues(boundaryShpPath, adminShpPath, otherShpPaths, generalizedShpPaths,
            rule, issues, detectedEndpoints, error, log)) {
        result.message = error;
        return false;
    }

    result.problemCount = static_cast<int>(issues.size());
    for (const BoundaryJumpIssue& issue : issues) {
        QJsonObject detail;
        detail["type"] = issue.type;
        detail["boundary_layer"] = issue.boundaryLayerName;
        detail["boundary_fid"] = QString::number(issue.boundaryFid);
        detail["other_layer"] = issue.otherLayerName;
        detail["other_fid"] = QString::number(issue.otherFid);
        detail["message"] = issue.message;
        result.problemDetails.append(detail);
    }

    const int logCount = std::min(result.problemCount, kMaxProblemLogCount);
    for (int i = 0; i < logCount; ++i) {
        const BoundaryJumpIssue& issue = issues[static_cast<size_t>(i)];
        writeLog(QString("问题%1：%2，境界线FID=%3，图面要素FID=%4，%5")
            .arg(i + 1).arg(issue.type).arg(issue.boundaryFid).arg(issue.otherFid).arg(issue.message));
    }
    if (result.problemCount > logCount)
        writeLog(QString("还有 %1 个问题未在日志中逐条列出。").arg(result.problemCount - logCount));

    if (writeOutput) {
        if (result.problemCount > 0 && !writeProblemShapefiles(
                outputBaseShpPath, issues, rule, boundaryShpPath, result.outputPaths, error, log)) {
            result.message = error;
            return false;
        }
        QStringList endpointLogs;
        if (!writeDetectedJumpEndpointShapefile(outputBaseShpPath, detectedEndpoints, rule,
                boundaryShpPath, result.outputPaths, endpointLogs, error)) {
            result.message = error;
            return false;
        }
        for (const QString& endpointLog : endpointLogs)
            writeLog(endpointLog);
    }

    result.message = result.problemCount == 0
        ? QStringLiteral("境界线缺失和跳绘检查完成，未发现问题。")
        : (writeOutput
            ? QString("境界线缺失和跳绘检查完成，发现问题 %1 个，已输出错误图层：\n%2")
                .arg(result.problemCount).arg(result.outputPaths.join("\n"))
            : QString("境界线缺失和跳绘检查完成，发现问题 %1 个。")
                .arg(result.problemCount));
    return true;
}

bool se_boundary_jump_draw_check_algorithm::readRuleFromXml(
    const QString& xmlPath,
    const QString& boundaryShpPath,
    int scale,
    BoundaryJumpRule& rule,
    QString& error)
{
    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = "XML 规则文件打开失败：" + xmlPath;
        return false;
    }

    QDomDocument doc;
    if (!doc.setContent(&file)) {
        error = "XML 规则文件解析失败：" + xmlPath;
        return false;
    }

    // 只读取 ID=116 的“境界线缺失和跳绘检查”规则，避免其他质检项干扰。
    bool foundRule = false;
    bool foundOverlapLength = false;
    bool foundTopologyTolerance = false;
    bool foundGeneralizedLineMatchDistance = false;
    bool foundBoundaryCandidateSearchDistance = false;
    bool foundDrawDistance = false;
    bool foundNonJumpOffsetDistance = false;
    bool foundDirectionAngleThreshold = false;
    bool foundMissingBoundaryAttributeName = false;
    bool foundMissingBoundaryAttributeValue = false;
    bool foundMissingBoundaryBufferRadius = false;
    bool foundBoundarySymbolLength = false;
    bool foundMaxJumpStepCoefficient = false;

    const QDomNodeList items = doc.elementsByTagName("check_item");
    for (int i = 0; i < items.count(); ++i) {
        const QDomElement item = items.at(i).toElement();
        const QString id = item.firstChildElement("id").text().trimmed();

        if (id != "116")
            continue;

        foundRule = true;
        const QDomNodeList ruleNodes = item.elementsByTagName("rule");
        for (int j = 0; j < ruleNodes.count(); ++j) {
            const QDomElement ruleElement = ruleNodes.at(j).toElement();
            const QString name = ruleElement.firstChildElement("name").text().trimmed();
            const QString value = ruleElement.firstChildElement("value").text().trimmed();
            const QString unit = ruleElement.firstChildElement("unit").text().trimmed();

            if (name == "属性项") {
                rule.missingBoundaryAttributeName = value;
                foundMissingBoundaryAttributeName = !value.isEmpty();
                continue;
            }
            if (name == "属性值") {
                rule.missingBoundaryAttributeValue = value;
                foundMissingBoundaryAttributeValue = !value.isEmpty();
                continue;
            }

            const QVector<double> numbers = parseRuleNumbers(value);

            if (numbers.isEmpty())
                continue;

            if (name == "重叠长度") {
                rule.overlapLengthCm = numbers.first();
                rule.overlapLengthUnit = unit;
                foundOverlapLength = true;
            }
            else if (name == "重叠拓扑容差") {
                rule.topologyToleranceMm = numbers.first();
                rule.topologyToleranceUnit = unit;
                foundTopologyTolerance = true;
            }
            else if (name == "匹配缓冲区半径") {
                rule.generalizedLineMatchDistanceMm = numbers.first();
                rule.generalizedLineMatchDistanceUnit = unit;
                foundGeneralizedLineMatchDistance = true;
            }
            else if (name == "搜索缓冲区半径") {
                rule.boundaryCandidateSearchDistanceMm = numbers.first();
                rule.boundaryCandidateSearchDistanceUnit = unit;
                foundBoundaryCandidateSearchDistance = true;
            }
            else if (name == "绘制距离") {
                if (numbers.size() < 2)
                    continue;
                rule.drawDistanceMinMm = numbers.at(0);
                rule.drawDistanceMaxMm = numbers.at(1);
                rule.drawDistanceUnit = unit;
                foundDrawDistance = true;
            }
            else if (name == "偏移距离") {
                rule.nonJumpOffsetDistanceMm = numbers.first();
                rule.nonJumpOffsetDistanceUnit = unit;
                foundNonJumpOffsetDistance = true;
            }
            else if (name == "走向夹角阈值") {
                rule.directionAngleThresholdDeg = numbers.first();
                rule.directionAngleThresholdUnit = unit;
                foundDirectionAngleThreshold = true;
            }
            else if (name == "缓冲区半径") {
                rule.missingBoundaryBufferRadiusMm = numbers.first();
                rule.missingBoundaryBufferRadiusUnit = unit;
                foundMissingBoundaryBufferRadius = true;
            }
            else if (name == "境界线符号长度") {
                rule.boundarySymbolLengthMm = numbers.first();
                rule.boundarySymbolLengthUnit = unit;
                foundBoundarySymbolLength = true;
            }
            else if (name == "最大跳绘步长系数") {
                rule.maxJumpStepCoefficient = numbers.first();
                rule.maxJumpStepCoefficientUnit = unit;
                foundMaxJumpStepCoefficient = true;
            }
        }

        break;
    }

    if (!foundRule) {
        error = "未找到 ID=116 的境界线缺失和跳绘检查规则。";
        return false;
    }

    if (!foundOverlapLength || !foundTopologyTolerance || !foundGeneralizedLineMatchDistance
        || !foundBoundaryCandidateSearchDistance
        || !foundDrawDistance || !foundNonJumpOffsetDistance
        || !foundDirectionAngleThreshold
        || !foundMissingBoundaryAttributeName || !foundMissingBoundaryAttributeValue
        || !foundMissingBoundaryBufferRadius || !foundBoundarySymbolLength
        || !foundMaxJumpStepCoefficient) {
        error = "ID=116 的境界线缺失和跳绘检查规则不完整，必须包含“重叠长度”、“重叠拓扑容差”、“匹配缓冲区半径”、“搜索缓冲区半径”、“绘制距离”（两个闭区间端点）、“偏移距离”、“走向夹角阈值”、“属性项”、“属性值”、“缓冲区半径”、“境界线符号长度”和“最大跳绘步长系数”。";
        return false;
    }

    if (!rule.overlapLengthUnit.isEmpty()
        && rule.overlapLengthUnit.compare("cm", Qt::CaseInsensitive) != 0) {
        error = "“重叠长度”规则单位必须为 cm。";
        return false;
    }

    if (!rule.drawDistanceUnit.isEmpty()
        && rule.drawDistanceUnit.compare("mm", Qt::CaseInsensitive) != 0) {
        error = "“绘制距离”规则单位必须为 mm。";
        return false;
    }

    if (!rule.nonJumpOffsetDistanceUnit.isEmpty()
        && rule.nonJumpOffsetDistanceUnit.compare("mm", Qt::CaseInsensitive) != 0) {
        error = "“偏移距离”规则单位必须为 mm。";
        return false;
    }

    if (rule.drawDistanceMinMm <= 0.0
        || rule.drawDistanceMaxMm < rule.drawDistanceMinMm) {
        error = "“绘制距离”必须提供两个大于 0 且由小到大排列的闭区间端点。";
        return false;
    }

    if (!rule.boundarySymbolLengthUnit.isEmpty()
        && rule.boundarySymbolLengthUnit.compare("mm", Qt::CaseInsensitive) != 0) {
        error = "“境界线符号长度”规则单位必须为 mm。";
        return false;
    }

    const QString normalizedMaxStepUnit = rule.maxJumpStepCoefficientUnit.trimmed().toLower();
    if (!normalizedMaxStepUnit.isEmpty() && normalizedMaxStepUnit != "times"
        && normalizedMaxStepUnit != "multiple" && normalizedMaxStepUnit != "倍") {
        error = "“最大跳绘步长系数”规则单位必须为“倍”或 times。";
        return false;
    }

    const QString directionUnit = rule.directionAngleThresholdUnit.trimmed().toLower();
    if (!directionUnit.isEmpty() && directionUnit != "degree" && directionUnit != "deg"
        && directionUnit != "°" && directionUnit != "度") {
        error = "“走向夹角阈值”规则单位必须为 degree。";
        return false;
    }
    if (rule.directionAngleThresholdDeg <= 0.0
        || rule.directionAngleThresholdDeg > 90.0) {
        error = "“走向夹角阈值”必须大于 0 且不大于 90 度。";
        return false;
    }

    QString unitError;
    const double coordinateUnitsPerMetre = layerUnitsPerMetre(boundaryShpPath, unitError);
    if (coordinateUnitsPerMetre <= 0.0) {
        error = unitError;
        return false;
    }

    rule.scaleDenominator = scale;
    rule.overlapLength = mapMmToLayerUnit(
        rule.overlapLengthCm * 10.0,
        rule.scaleDenominator) * coordinateUnitsPerMetre;
    rule.drawDistanceMin =
        mapMmToLayerUnit(rule.drawDistanceMinMm, rule.scaleDenominator)
        * coordinateUnitsPerMetre;
    rule.drawDistanceMax =
        mapMmToLayerUnit(rule.drawDistanceMaxMm, rule.scaleDenominator)
        * coordinateUnitsPerMetre;
    rule.nonJumpOffsetDistance =
        mapMmToLayerUnit(rule.nonJumpOffsetDistanceMm, rule.scaleDenominator)
        * coordinateUnitsPerMetre;
    if (!toleranceToLayerUnit(
        rule.topologyToleranceMm,
        rule.topologyToleranceUnit,
        rule.scaleDenominator,
        coordinateUnitsPerMetre,
        rule.topologyTolerance)) {
        error = "“重叠拓扑容差”规则单位仅支持 mm、cm 或 m。";
        return false;
    }
    if (!toleranceToLayerUnit(
        rule.generalizedLineMatchDistanceMm,
        rule.generalizedLineMatchDistanceUnit,
        rule.scaleDenominator,
        coordinateUnitsPerMetre,
        rule.generalizedLineMatchDistance)) {
        error = "“匹配缓冲区半径”规则单位仅支持 mm、cm 或 m。";
        return false;
    }
    if (!toleranceToLayerUnit(
        rule.boundaryCandidateSearchDistanceMm,
        rule.boundaryCandidateSearchDistanceUnit,
        rule.scaleDenominator,
        coordinateUnitsPerMetre,
        rule.boundaryCandidateSearchDistance)) {
        error = "“搜索缓冲区半径”规则单位仅支持 mm、cm 或 m。";
        return false;
    }
    if (!toleranceToLayerUnit(
        rule.missingBoundaryBufferRadiusMm,
        rule.missingBoundaryBufferRadiusUnit,
        rule.scaleDenominator,
        coordinateUnitsPerMetre,
        rule.missingBoundaryBufferRadius)) {
        error = "“缓冲区半径”规则单位仅支持 mm、cm 或 m。";
        return false;
    }
    if (!toleranceToLayerUnit(
        rule.boundarySymbolLengthMm,
        rule.boundarySymbolLengthUnit,
        rule.scaleDenominator,
        coordinateUnitsPerMetre,
        rule.boundarySymbolLength)) {
        error = "“境界线符号长度”规则单位仅支持 mm、cm 或 m。";
        return false;
    }
    rule.maxJumpStep = rule.boundarySymbolLength * rule.maxJumpStepCoefficient;
    if (rule.overlapLength <= 0.0 || rule.topologyTolerance <= 0.0
        || rule.generalizedLineMatchDistance <= 0.0
        || rule.boundaryCandidateSearchDistance <= 0.0
        || rule.missingBoundaryBufferRadius <= 0.0
        || rule.boundarySymbolLength <= 0.0 || rule.maxJumpStepCoefficient <= 0.0
        || rule.maxJumpStep <= 0.0
        || rule.drawDistanceMin <= 0.0
        || rule.drawDistanceMax < rule.drawDistanceMin
        || rule.nonJumpOffsetDistance <= 0.0) {
        error = "ID=116 的距离或长度阈值必须大于 0，绘制距离闭区间必须由小到大排列。";
        return false;
    }
    return true;
}


bool se_boundary_jump_draw_check_algorithm::collectJumpIssues(
    const QString& boundaryShpPath,
    const QString& adminShpPath,
    const QString& otherShpPaths,
    const QString& generalizedShpPaths,
    const BoundaryJumpRule& rule,
    std::vector<BoundaryJumpIssue>& issues,
    std::vector<DetectedJumpEndpoint>& detectedEndpoints,
    QString& error,
    const std::function<void(const QString&)>& log)
{
    JumpGdalDatasetPtr boundaryDataset = openVectorDataset(boundaryShpPath);
    if (!boundaryDataset) {
        error = "解析后的境界线数据打开失败：" + boundaryShpPath;
        return false;
    }

    if (boundaryDataset->GetLayerCount() <= 0) {
        error = "解析后的境界线数据中没有可检查的图层：" + boundaryShpPath;
        return false;
    }

    JumpGdalDatasetPtr adminDataset = openVectorDataset(adminShpPath);
    if (!adminDataset) {
        error = "行政界线 SHP 数据打开失败：" + adminShpPath;
        return false;
    }

    if (adminDataset->GetLayerCount() <= 0) {
        error = "行政界线 SHP 中没有可检查的图层：" + adminShpPath;
        return false;
    }

    const QStringList otherPaths = splitDatasetPaths(otherShpPaths);
    if (otherPaths.isEmpty()) {
        error = "水系、道路等线状要素 SHP 路径为空。";
        return false;
    }

    std::vector<JumpGdalDatasetPtr> otherDatasets;
    for (const QString& otherPath : otherPaths) {
        JumpGdalDatasetPtr otherDataset = openVectorDataset(otherPath);
        if (!otherDataset) {
            error = "水系、道路等线状要素 SHP 数据打开失败：" + otherPath;
            return false;
        }

        if (otherDataset->GetLayerCount() <= 0) {
            error = "水系、道路等线状要素 SHP 中没有可检查的图层：" + otherPath;
            return false;
        }

        otherDatasets.push_back(std::move(otherDataset));
    }

    const QStringList generalizedLinePaths = splitDatasetPaths(generalizedShpPaths);
    if (generalizedLinePaths.isEmpty()) {
        error = "综合后线状要素 SHP 路径为空。";
        return false;
    }

    std::vector<JumpGdalDatasetPtr> generalizedLineDatasets;
    for (const QString& generalizedLinePath : generalizedLinePaths) {
        JumpGdalDatasetPtr generalizedLineDataset = openVectorDataset(generalizedLinePath);
        if (!generalizedLineDataset) {
            error = "综合后线状要素 SHP 数据打开失败：" + generalizedLinePath;
            return false;
        }

        if (generalizedLineDataset->GetLayerCount() <= 0) {
            error = "综合后线状要素 SHP 中没有可检查的图层：" + generalizedLinePath;
            return false;
        }

        generalizedLineDatasets.push_back(std::move(generalizedLineDataset));
    }

    // 原始关联段负责候选搜索；综合后线状要素负责保留判定和最终绘制距离检查。
    QStringList checkLogs;
    const bool distanceCheckOk = collectJumpDistanceIssues(
        boundaryDataset.get(),
        adminDataset.get(),
        otherDatasets,
        generalizedLineDatasets,
        rule,
        issues,
        detectedEndpoints,
        checkLogs,
        error);
    for (const QString& logMessage : checkLogs) {
        if (log)
            log(logMessage);
    }
    return distanceCheckOk;

}

bool se_boundary_jump_draw_check_algorithm::writeProblemShapefiles(
    const QString& outputBaseShpPath,
    const std::vector<BoundaryJumpIssue>& issues,
    const BoundaryJumpRule& rule,
    const QString& boundaryShpPath,
    QStringList& outputPaths,
    QString& error,
    const std::function<void(const QString&)>& log)
{
    QStringList logs;
    const bool ok = ResultWriter::writeProblemShapefiles(
        outputBaseShpPath,
        issues,
        rule,
        boundaryShpPath,
        outputPaths,
        logs,
        error);

    for (const QString& logMessage : logs) {
        if (log)
            log(logMessage);
    }

    return ok;
}
