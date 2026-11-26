#ifndef MAPFRAMEALIGNMENTCHECK_H
#define MAPFRAMEALIGNMENTCHECK_H

#include "IMapFrameAlignmentCheck.h"

#include <QDialog>
#include <QString>
#include <QList>
#include <QLineEdit>
#include <QPushButton>
#include <QJsonObject>
#include <qgsmapcanvas.h>
#include <qgsvectorlayer.h>
#include <qgsgeometry.h>
#include <qgsrectangle.h>

/* =====================================================
 * MapFrameAlignmentCheck 类：图廓匹配检查功能实现类
 * ===================================================== */
class MapFrameAlignmentCheck : public IMapFrameAlignmentCheck
{
public:
    MapFrameAlignmentCheck();
    ~MapFrameAlignmentCheck() override;

    // 接口实现
    void runCheck(const QString& xmlRuleFile,
        const QString& baseShp,
        const QString& genShp,
        const QString& outputJson) override;

    QList<QString> checkFeaturesOutOfBoundary(QgsVectorLayer* featureLayer,
        QgsVectorLayer* contourLayer) override;

    QList<QString> checkContentMisalignment(QgsVectorLayer* mainLayer,
        QgsVectorLayer* contourLayer,
        double tolerance) override;

    QJsonObject runAllChecks(QgsVectorLayer* featureLayer,
        QgsVectorLayer* contourLayer,
        double tolerance) override;

    bool exportJsonReport(const QJsonObject& resultObj,
        const QString& outputPath) override;

private:
    // 辅助：计算图层的最小外接矩形
    QgsRectangle calculateBoundingBox(QgsVectorLayer* layer);

    // 辅助：几何是否在图廓范围内
    bool isGeometryInsideContour(const QgsGeometry& geom, QgsVectorLayer* contourLayer);
};

#endif