// GeometryValidator.h
#ifndef GEOMETRYVALIDATOR_H
#define GEOMETRYVALIDATOR_H

#include <QObject>
#include <QList>
#include <QTextStream>
#include <QMap>
#include "qgsvectorlayer.h"
#include "qgsfeature.h"

class GeometryValidator
{
public:
    explicit GeometryValidator();  // 移除QObject* parent参数
    ~GeometryValidator();  // 添加析构函数

    // 检查单个图层的几何有效性，返回无效要素列表
    QList<QgsFeature> validateLayer(QgsVectorLayer* layer, const QString& checkType, QTextStream& logStream);

    // 检查多个图层的几何有效性，按图层名分组返回无效要素
    QMap<QString, QList<QgsFeature>> validateLayers(const QList<QgsVectorLayer*>& layers, const QString& checkType, QTextStream& logStream);

    // 保存几何有效性错误到文件
    bool saveGeometryErrors(const QMap<QString, QList<QgsFeature>>& layerErrors, const QString& baseOutputPath);

    // 获取有效的要素列表（从图层中排除无效要素）
    QList<QgsFeature> getValidFeatures(QgsVectorLayer* layer, const QList<QgsFeature>& invalidFeatures);

private:
    bool isFeatureInErrorList(const QgsFeature& feature, const QList<QgsFeature>& errorFeatures);
};

#endif // GEOMETRYVALIDATOR_H