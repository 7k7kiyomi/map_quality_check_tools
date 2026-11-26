#ifndef IMAPFRAMEALIGNMENTCHECK_H
#define IMAPFRAMEALIGNMENTCHECK_H

#include <QJsonObject>
#include <QList>
#include <QString>

class QgsVectorLayer;

/**
 * @brief 图廓匹配检查接口（纯虚类）
 */
class IMapFrameAlignmentCheck
{
public:
    virtual ~IMapFrameAlignmentCheck() = default;

    virtual void runCheck(const QString& xmlRuleFile,//"D:\Desktop\xmll\铁路隧道.xml"
        const QString& baseShp,//D:\Desktop\项目\甘肃地图成果质检系统\数据部分\永登县_gdb\永登县_gdb\LFCL.shp
        const QString& genShp,//D:\Desktop\项目\甘肃地图成果质检系统\数据部分\最新_永登县新版数据\永登县shp新版\铁路隧道.shp
        const QString& outputJson) = 0;

    virtual QList<QString> checkFeaturesOutOfBoundary(QgsVectorLayer* featureLayer,
        QgsVectorLayer* contourLayer) = 0;

    virtual QList<QString> checkContentMisalignment(QgsVectorLayer* mainLayer,
        QgsVectorLayer* contourLayer,
        double tolerance) = 0;

    virtual QJsonObject runAllChecks(QgsVectorLayer* featureLayer,
        QgsVectorLayer* contourLayer,
        double tolerance) = 0;

    virtual bool exportJsonReport(const QJsonObject& resultObj,
        const QString& outputPath) = 0;
};

/**
 * @brief 工厂函数声明（用于创建接口实例）
 */
extern "C" IMapFrameAlignmentCheck * createMapFrameAlignmentChecker();

#endif // IMAPFRAMEALIGNMENTCHECK_H
