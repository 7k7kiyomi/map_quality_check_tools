#include "MapFrameAlignmentCheck.h"
#include <QFile>
#include <QJsonDocument>
#include <QDebug>
#include <QgsFeatureIterator.h>
#include <QgsFeature.h>
#include <QgsGeometry.h>
#include <QMessageBox>
#include <QDir>

// ====================== 构造/析构 ======================
MapFrameAlignmentCheck::MapFrameAlignmentCheck() {}
MapFrameAlignmentCheck::~MapFrameAlignmentCheck() {}

// ====================== 接口 runCheck ======================
void MapFrameAlignmentCheck::runCheck(const QString& xmlRuleFile,
    const QString& baseShp,
    const QString& genShp,
    const QString& outputJson)
{
    Q_UNUSED(xmlRuleFile)

        QgsVectorLayer* featureLayer = new QgsVectorLayer(baseShp, "featureLayer", "ogr");
    QgsVectorLayer* contourLayer = new QgsVectorLayer(genShp, "contourLayer", "ogr");

    if (!featureLayer->isValid() || !contourLayer->isValid()) {
        qWarning() << "加载图层失败";
        delete featureLayer;
        delete contourLayer;
        return;
    }

    QJsonObject result = runAllChecks(featureLayer, contourLayer, 5.0);

    if (!outputJson.isEmpty())
        exportJsonReport(result, outputJson);

    delete featureLayer;
    delete contourLayer;
}

// ====================== 要素超出边界检查 ======================
QList<QString> MapFrameAlignmentCheck::checkFeaturesOutOfBoundary(QgsVectorLayer* featureLayer,
    QgsVectorLayer* contourLayer)
{
    QList<QString> results;

    if (!featureLayer || !contourLayer) {
        results.append("输入图层无效！");
        return results;
    }

    QgsFeature feat;
    QgsFeatureIterator it = featureLayer->getFeatures();
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (!isGeometryInsideContour(geom, contourLayer)) {
            results.append(QString("要素ID %1 超出图廓范围").arg(feat.id()));
        }
    }

    if (results.isEmpty())
        results.append("所有要素均在图廓范围内。");

    return results;
}

// ====================== 内容与图廓错位检查 ======================
QList<QString> MapFrameAlignmentCheck::checkContentMisalignment(QgsVectorLayer* mainLayer,
    QgsVectorLayer* contourLayer,
    double tolerance)
{
    QList<QString> results;

    if (!mainLayer || !contourLayer) {
        results.append("输入图层无效！");
        return results;
    }

    QgsRectangle mbr = calculateBoundingBox(mainLayer);
    QgsRectangle contourMBR = calculateBoundingBox(contourLayer);

    if (!contourMBR.contains(mbr))
        results.append("主区最小外接矩形超出图廓范围，判定为错位。");

    double centerX_main = (mbr.xMinimum() + mbr.xMaximum()) / 2.0;
    double centerY_main = (mbr.yMinimum() + mbr.yMaximum()) / 2.0;
    double centerX_contour = (contourMBR.xMinimum() + contourMBR.xMaximum()) / 2.0;
    double centerY_contour = (contourMBR.yMinimum() + contourMBR.yMaximum()) / 2.0;

    double offsetX = qAbs(centerX_main - centerX_contour);
    double offsetY = qAbs(centerY_main - centerY_contour);

    if (offsetX > tolerance || offsetY > tolerance) {
        results.append(QString("主区与图廓未居中，偏移量：X=%1, Y=%2 (容差=%3)")
            .arg(offsetX).arg(offsetY).arg(tolerance));
    }

    if (results.isEmpty())
        results.append("内容与图廓匹配正常，无错位。");

    return results;
}

// ====================== runAllChecks ======================
QJsonObject MapFrameAlignmentCheck::runAllChecks(QgsVectorLayer* featureLayer,
    QgsVectorLayer* contourLayer,
    double tolerance)
{
    QJsonObject root;

    QList<QString> boundaryResults = checkFeaturesOutOfBoundary(featureLayer, contourLayer);
    QList<QString> misalignResults = checkContentMisalignment(featureLayer, contourLayer, tolerance);

    // --- 要素超出边界检查 ---
    QJsonObject item1;
    bool hasBoundaryError = false;
    for (const QString& msg : boundaryResults) {
        if (msg.contains("超出")) {
            hasBoundaryError = true;
            break;
        }
    }
    item1["name"] = "要素超出边界检查";
    item1["status"] =  "质检完成";
    item1["err_exist"] = hasBoundaryError;
    if (hasBoundaryError) {
        QJsonObject errors;
        int i = 1;
        for (const QString& msg : boundaryResults) {
            if (msg.contains("超出"))
                errors[QString("error%1").arg(i++)] = msg;
        }
        item1["info"] = "存在要素超出图廓范围";
        item1["errors"] = errors;
    }
    else {
        item1["info"] = "所有要素均在图廓范围内。";
    }

    // --- 内容与图廓错位检查 ---
    QJsonObject item2;
    bool hasMisalignError = false;
    for (const QString& msg : misalignResults) {
        if (msg.contains("未居中") || msg.contains("超出") || msg.contains("偏移"))
            hasMisalignError = true;
    }
    item2["name"] = "内容与图廓错位检查";
    item2["status"] = hasMisalignError ? "质检失败" : "质检完成";
    item2["err_exist"] = hasMisalignError;
    if (hasMisalignError) {
        QJsonObject errors;
        int i = 1;
        for (const QString& msg : misalignResults) {
            if (msg.contains("未居中") || msg.contains("超出") || msg.contains("偏移"))
                errors[QString("error%1").arg(i++)] = msg;
        }
        item2["info"] = "主区与图廓存在位置错位";
        item2["errors"] = errors;
    }
    else {
        item2["info"] = "内容与图廓匹配正常，无错位。";
    }

    root["check_item1"] = item1;
    root["check_item2"] = item2;

    return root;
}

// ====================== exportJsonReport ======================
bool MapFrameAlignmentCheck::exportJsonReport(const QJsonObject& resultObj,
    const QString& outputPath)
{
    if (outputPath.isEmpty())
        return false;

    QFile outFile(outputPath);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QJsonDocument doc(resultObj);
    outFile.write(doc.toJson(QJsonDocument::Indented));
    outFile.close();

    qDebug() << "质检报告已输出至:" << outputPath;
    return true;
}

// ====================== 辅助函数 ======================
QgsRectangle MapFrameAlignmentCheck::calculateBoundingBox(QgsVectorLayer* layer)
{
    return layer->extent();
}

bool MapFrameAlignmentCheck::isGeometryInsideContour(const QgsGeometry& geom, QgsVectorLayer* contourLayer)
{
    QgsFeature feat;
    QgsFeatureIterator it = contourLayer->getFeatures();
    while (it.nextFeature(feat)) {
        QgsGeometry contourGeom = feat.geometry();
        if (contourGeom.contains(geom))
            return true;
    }
    return false;
}