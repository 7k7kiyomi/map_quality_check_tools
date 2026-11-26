#include "algorithm_ScaleChecker.h"
#include <gdal_priv.h>
#include <ogr_api.h>
#include "ogrsf_frmts.h"
#include "ogr_feature.h"
#include "ogr_geometry.h"
#include <QFileInfo>
#include <QRegExp>
#include <QMessageBox>
#include <QDir>
#include <QJsonObject> // 新增头文件
#include <cmath>

// 执行比例尺检查核心逻辑
void ScaleCheckerAlgorithm::execute(
    const QString& checkItemName,
    const AlgorithmFilePaths& filePaths,
    int /*scale*/,
    const QString& outputDir,
    CheckItemResult& result
) {
    // 1. 初始化结果基础信息
    result.id = "402";
    result.name = checkItemName;
    result.status = "未质检";
    result.err_exist = false; // 初始默认为 false，如果发现任何错误则置为 true
    result.info = QJsonObject();
    result.errors = QJsonObject();

    // 2. 图例SHP路径处理
    QString legendPolygonPath;
    foreach(const QString & path, filePaths.annotShpList) {
        QFileInfo info(path);
        if (info.baseName().contains("_Polygon", Qt::CaseInsensitive)) {
            legendPolygonPath = path;
            break;
        }
    }
    if (legendPolygonPath.isEmpty()) {
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("未找到注记图层中的面要素（需包含_Polygon），路径列表：%1")
            .arg(filePaths.annotShpList.join(";"));
        result.errors["error1"] = errorDetail;
        return;
    }

    // 3. 内图廓SHP路径处理
    QString mapFrameShpPath = filePaths.actualShpSemicolon;
    if (mapFrameShpPath.isEmpty() || !QFile::exists(mapFrameShpPath)) {
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("内图廓SHP文件不存在：%1").arg(mapFrameShpPath);
        result.errors["error1"] = errorDetail;
        return;
    }

    // 4. 内图廓解析与比例尺计算
    QList<QgsPointXY> mapFramePoints;
    double attrPixelWidth = 0.0, attrPixelHeight = 0.0, mapFrameWidth = 0.0;
    if (!extractMapFramePointsFromShp(mapFrameShpPath, mapFramePoints, attrPixelWidth, attrPixelHeight, mapFrameWidth)) {
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = "内图廓SHP解析失败，无法计算比例尺";
        result.errors["error1"] = errorDetail;
        return;
    }
    QString calculatedScale = calculateActualMapScale(mapFrameWidth, attrPixelWidth);
    if (calculatedScale.isEmpty()) {
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = "内图廓比例尺计算失败";
        result.errors["error1"] = errorDetail;
        return;
    }
    // 即使后续无法读取图例比例尺，预检查也可以复用内图廓计算出的比例尺，
    // 将纸面距离（如 2 mm）换算为地面缓冲距离。
    result.info["calculated_scale"] = calculatedScale;

    // 5. 图例SHP提取比例尺
    QString legendScale;
    int scaleFeatureId = -1;
    if (!extractScaleFromLegendShp(legendPolygonPath, legendScale, scaleFeatureId)) {
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = "从图例SHP中未找到有效的比例尺信息（需包含text_conte字段）";
        result.errors["error1"] = errorDetail;
        return;
    }

    // 6. 【核心修改】比例尺对比，并根据结果设置最终状态
    bool isScaleMatch = (legendScale == calculatedScale);

    if (isScaleMatch) {
        // 比例尺一致，判定为成功
        result.status = "质检成功";
        result.err_exist = false; // 确保没有错误
        result.info["description"] = "比例尺一致";
        result.info["legend_scale"] = legendScale;
    }
    else {
        // 比例尺不一致，判定为失败，并记录错误信息
        result.status = "质检成功";
        result.err_exist = true; // 标记存在错误

        QJsonObject errorDetail;
        errorDetail["description"] = "图例比例尺与内图廓计算比例尺不一致";
        errorDetail["legend_scale"] = legendScale;
        errorDetail["calculated_scale"] = calculatedScale;

        result.errors["error1"] = errorDetail; // 使用一个更具描述性的键名
    }
}

// 以下所有辅助函数逻辑保持不变（提取、计算等功能正常执行即可）
// 从内图廓SHP提取顶点信息
bool ScaleCheckerAlgorithm::extractMapFramePointsFromShp(const QString& shpPath, QList<QgsPointXY>& points, double& attrPixelWidth, double& attrPixelHeight, double& mapFrameWidth) {
    GDALAllRegister();
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        shpPath.toStdString().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (!dataset) return false;

    OGRLayer* layer = dataset->GetLayer(0);
    if (!layer) {
        GDALClose(dataset);
        return false;
    }

    OGRwkbGeometryType geomType = layer->GetGeomType();
    if (!(wkbFlatten(geomType) == wkbPolygon || wkbFlatten(geomType) == wkbLineString)) {
        GDALClose(dataset);
        return false;
    }

    int widthFieldIdx = layer->GetLayerDefn()->GetFieldIndex("width");
    int heightFieldIdx = layer->GetLayerDefn()->GetFieldIndex("height");
    if (widthFieldIdx == -1 || heightFieldIdx == -1) {
        GDALClose(dataset);
        return false;
    }

    OGRFeature* feature = layer->GetNextFeature();
    if (!feature) {
        GDALClose(dataset);
        return false;
    }

    attrPixelWidth = feature->GetFieldAsDouble(widthFieldIdx);
    attrPixelHeight = feature->GetFieldAsDouble(heightFieldIdx);
    if (attrPixelWidth <= 0 || attrPixelHeight <= 0) {
        OGRFeature::DestroyFeature(feature);
        GDALClose(dataset);
        return false;
    }

    OGRGeometry* geom = feature->GetGeometryRef();
    if (!geom) {
        OGRFeature::DestroyFeature(feature);
        GDALClose(dataset);
        return false;
    }

    QList<QgsPointXY> rawPoints;
    OGRPoint point;
    if (wkbFlatten(geom->getGeometryType()) == wkbPolygon) {
        OGRPolygon* polygon = dynamic_cast<OGRPolygon*>(geom);
        if (polygon && polygon->getExteriorRing()) {
            OGRLinearRing* ring = polygon->getExteriorRing();
            for (int i = 0; i < ring->getNumPoints() - 1; ++i) {
                ring->getPoint(i, &point);
                rawPoints.append(QgsPointXY(point.getX(), point.getY()));
            }
        }
    }
    else if (wkbFlatten(geom->getGeometryType()) == wkbLineString) {
        OGRLineString* line = dynamic_cast<OGRLineString*>(geom);
        if (line) {
            for (int i = 0; i < line->getNumPoints(); ++i) {
                line->getPoint(i, &point);
                rawPoints.append(QgsPointXY(point.getX(), point.getY()));
            }
        }
    }

    if (rawPoints.size() < 4) {
        OGRFeature::DestroyFeature(feature);
        GDALClose(dataset);
        return false;
    }

    double meanX = 0.0, meanY = 0.0;
    for (const auto& p : rawPoints) {
        meanX += p.x();
        meanY += p.y();
    }
    meanX /= rawPoints.size();
    meanY /= rawPoints.size();

    double sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (const auto& p : rawPoints) {
        double dx = p.x() - meanX;
        double dy = p.y() - meanY;
        sxx += dx * dx;
        sxy += dx * dy;
        syy += dy * dy;
    }
    double a = sxx / rawPoints.size();
    double b = sxy / rawPoints.size();
    double c = syy / rawPoints.size();
    double theta = 0.5 * std::atan2(2.0 * b, a - c);
    double cosT = std::cos(theta);
    double sinT = std::sin(theta);

    QList<QgsPointXY> rotatedPoints;
    rotatedPoints.reserve(rawPoints.size());
    for (const auto& p : rawPoints) {
        double dx = p.x() - meanX;
        double dy = p.y() - meanY;
        double xp = cosT * dx + sinT * dy;
        double yp = -sinT * dx + cosT * dy;
        rotatedPoints.append(QgsPointXY(xp, yp));
    }

    double minXp = rotatedPoints[0].x(), maxXp = rotatedPoints[0].x();
    double minYp = rotatedPoints[0].y(), maxYp = rotatedPoints[0].y();
    for (const auto& rp : rotatedPoints) {
        minXp = qMin(minXp, rp.x());
        maxXp = qMax(maxXp, rp.x());
        minYp = qMin(minYp, rp.y());
        maxYp = qMax(maxYp, rp.y());
    }

    QList<QgsPointXY> targetsRot;
    targetsRot.append(QgsPointXY(minXp, minYp));
    targetsRot.append(QgsPointXY(minXp, maxYp));
    targetsRot.append(QgsPointXY(maxXp, minYp));
    targetsRot.append(QgsPointXY(maxXp, maxYp));

    QList<QgsPointXY> targetsOrig;
    for (const auto& t : targetsRot) {
        double X = meanX + cosT * t.x() - sinT * t.y();
        double Y = meanY + sinT * t.x() + cosT * t.y();
        targetsOrig.append(QgsPointXY(X, Y));
    }

    QList<QgsPointXY> cornerPoints;
    QVector<bool> used(rawPoints.size(), false);
    for (const auto& t : targetsOrig) {
        double bestDist = 1e100;
        int bestIdx = -1;
        for (int i = 0; i < rawPoints.size(); ++i) {
            if (used[i]) continue;
            const auto& p = rawPoints[i];
            double dx = p.x() - t.x();
            double dy = p.y() - t.y();
            double d2 = dx * dx + dy * dy;
            if (d2 < bestDist) {
                bestDist = d2;
                bestIdx = i;
            }
        }
        if (bestIdx >= 0) {
            used[bestIdx] = true;
            cornerPoints.append(rawPoints[bestIdx]);
        }
    }

    if (cornerPoints.size() != 4) {
        OGRFeature::DestroyFeature(feature);
        GDALClose(dataset);
        return false;
    }

    points = cornerPoints;
    mapFrameWidth = calculateMapFrameWidth(points);
    OGRFeature::DestroyFeature(feature);
    GDALClose(dataset);
    return points.size() == 4;
}

// 计算内图廓宽度
double ScaleCheckerAlgorithm::calculateMapFrameWidth(const QList<QgsPointXY>& points) {
    if (points.size() != 4) return 0.0;
    double minX = points[0].x(), maxX = points[0].x();
    for (const auto& p : points) {
        minX = qMin(minX, p.x());
        maxX = qMax(maxX, p.x());
    }
    return maxX - minX;
}

// 验证比例尺文本格式
bool ScaleCheckerAlgorithm::isValidScaleText(const QString& text, QString* scaleValue) {
    // 整饰文字可能把“比例尺”和“1:370 000”存成两条要素。
    // 保留带前缀的旧格式；无前缀时仅接受完整的 1:N，避免匹配普通比值或正文。
    QString cleanedText = text;
    cleanedText.remove(QRegExp("\\s+"));
    cleanedText.remove(QChar(0x00a0)).remove(QChar(0x202f)).remove(QChar(0x3000));
    cleanedText.replace("：", ":");
    QRegExp scalePattern("^(比例尺?[0-9]+:|1:)([0-9]+)$");
    if (scalePattern.exactMatch(cleanedText)) {
        bool ok = false;
        const int denominator = scalePattern.cap(2).toInt(&ok);
        if (!ok || denominator <= 0) return false;
        if (scaleValue) {
            cleanedText.remove("比例尺").remove("比例");
            *scaleValue = cleanedText;
        }
        return true;
    }
    return false;
}

// 计算实际地图比例尺
QString ScaleCheckerAlgorithm::calculateActualMapScale(double mapFrameWidth, double attrPixelWidth) {
    const double EPS = 1e-6;
    const double DPI = 72.0;
    const double METER_PER_INCH = 0.0254;

    if (mapFrameWidth <= EPS || attrPixelWidth <= EPS) return "";

    double scaleDenominator = mapFrameWidth / (attrPixelWidth * (METER_PER_INCH / DPI));
    if (scaleDenominator <= 0) return "";

    if (scaleDenominator > 1e12) scaleDenominator = 1e12;
    else if (scaleDenominator < 1) scaleDenominator = 1;

    return QString("1:%1").arg(qRound(scaleDenominator));
}

// 从图例SHP提取比例尺
bool ScaleCheckerAlgorithm::extractScaleFromLegendShp(const QString& shpPath, QString& scaleValue, int& errorFeatureId) {
    GDALAllRegister();
    GDALDataset* dataset = (GDALDataset*)GDALOpenEx(
        shpPath.toStdString().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    if (!dataset) return false;

    OGRLayer* layer = dataset->GetLayer(0);
    if (!layer) {
        GDALClose(dataset);
        return false;
    }

    int textConteIndex = layer->GetLayerDefn()->GetFieldIndex("text_conte");
    if (textConteIndex == -1) {
        GDALClose(dataset);
        return false;
    }

    OGRFeature* feature = nullptr;
    bool hasValidScale = false;
    layer->ResetReading();
    while ((feature = layer->GetNextFeature()) != nullptr) {
        const char* textValue = feature->GetFieldAsString(textConteIndex);
        QString currentScale;
        if (isValidScaleText(QString::fromUtf8(textValue), &currentScale)) {
            scaleValue = currentScale;
            hasValidScale = true;
            errorFeatureId = feature->GetFID();  // 记录有效比例尺所在要素ID
            OGRFeature::DestroyFeature(feature);
            break;
        }
        OGRFeature::DestroyFeature(feature);
    }

    GDALClose(dataset);
    return hasValidScale;
}
