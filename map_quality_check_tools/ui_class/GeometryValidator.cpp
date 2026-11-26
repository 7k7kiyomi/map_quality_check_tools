// GeometryValidator.cpp
#include "GeometryValidator.h"
#include <QFileInfo>
#include <QDir>

GeometryValidator::GeometryValidator()
{
    // 构造函数实现
}

GeometryValidator::~GeometryValidator()
{
    // 析构函数实现
}

QList<QgsFeature> GeometryValidator::validateLayer(QgsVectorLayer* layer, const QString& checkType, QTextStream& logStream)
{
    QList<QgsFeature> invalidFeatures;
    if (!layer) return invalidFeatures;

    QString layerName = QFileInfo(layer->source()).baseName();
    logStream << QString(u8"几何有效性检查 - 图层 '%1'...\n").arg(layerName).toUtf8();

    QgsFeatureIterator iter = layer->getFeatures();
    QgsFeature feat;
    int layerInvalidCount = 0;

    while (iter.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();

        if (geom.isNull() || !geom.isGeosValid()) {
            layerInvalidCount++;

            // 创建错误要素记录
            QgsFields newFields = feat.fields();
            newFields.append(QgsField("ERROR_TYPE", QVariant::String, "", 50));
            newFields.append(QgsField("BASE_NAME", QVariant::String, "", 100));
            newFields.append(QgsField("PARSED_NAME", QVariant::String, "", 100));
            newFields.append(QgsField("DESCRIPTION", QVariant::String, "", 255));

            QgsFeature errorFeature(newFields);
            errorFeature.setGeometry(feat.geometry());
            errorFeature.setId(feat.id());

            // 复制原始属性
            for (int i = 0; i < feat.fields().count(); i++) {
                errorFeature.setAttribute(feat.fields().at(i).name(), feat.attribute(i));
            }

            QString errorDesc = geom.isNull() ? u8"几何为空" : u8"几何无效";
            errorFeature.setAttribute("ERROR_TYPE", u8"几何无效");
            errorFeature.setAttribute("BASE_NAME", u8"无");
            errorFeature.setAttribute("PARSED_NAME", u8"无");
            errorFeature.setAttribute("DESCRIPTION",
                QString(u8"图层[%1] 要素ID[%2] %3").arg(layerName).arg(feat.id()).arg(errorDesc));

            invalidFeatures.append(errorFeature);
        }
    }

    if (layerInvalidCount > 0) {
        logStream << QString(u8"图层 '%1': 发现 %2 个无效几何要素\n")
            .arg(layerName).arg(layerInvalidCount).toUtf8();
    }
    else {
        logStream << QString(u8"图层 '%1': 几何有效性检查通过\n")
            .arg(layerName).toUtf8();
    }

    return invalidFeatures;
}

QMap<QString, QList<QgsFeature>> GeometryValidator::validateLayers(const QList<QgsVectorLayer*>& layers, const QString& checkType, QTextStream& logStream)
{
    QMap<QString, QList<QgsFeature>> layerErrors;

    for (QgsVectorLayer* layer : layers) {
        if (layer) {
            QString layerName = QFileInfo(layer->source()).baseName();
            QList<QgsFeature> errors = validateLayer(layer, checkType, logStream);

  
            if (!errors.isEmpty()) {
                layerErrors[layerName] = errors;
            }
        }
    }

    return layerErrors;
}

bool GeometryValidator::saveGeometryErrors(const QMap<QString, QList<QgsFeature>>& layerErrors, const QString& baseOutputPath)
{
    bool success = true;

    for (auto it = layerErrors.begin(); it != layerErrors.end(); ++it) {
        QString layerName = it.key();
        QList<QgsFeature> errors = it.value();

        if (!errors.isEmpty()) {
            QString outputPath = baseOutputPath;
            outputPath = outputPath.replace(".shp", "_" + layerName + "_几何有效性.shp");

            // 这里需要调用主对话框的保存方法，或者实现独立的保存逻辑
            // 暂时返回true，实际使用时需要完善
        }
    }

    return success;
}

QList<QgsFeature> GeometryValidator::getValidFeatures(QgsVectorLayer* layer, const QList<QgsFeature>& invalidFeatures)
{
    QList<QgsFeature> validFeatures;
    if (!layer) return validFeatures;

    QgsFeatureIterator iter = layer->getFeatures();
    QgsFeature feat;

    while (iter.nextFeature(feat)) {
        if (!isFeatureInErrorList(feat, invalidFeatures)) {
            validFeatures.append(feat);
        }
    }

    return validFeatures;
}

bool GeometryValidator::isFeatureInErrorList(const QgsFeature& feature, const QList<QgsFeature>& errorFeatures)
{
    for (const QgsFeature& errorFeature : errorFeatures) {
        if (errorFeature.id() == feature.id()) {
            return true;
        }
    }
    return false;
}