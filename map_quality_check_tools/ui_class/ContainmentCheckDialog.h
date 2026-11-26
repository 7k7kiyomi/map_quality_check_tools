#ifndef CONTAINMENTCHECKDIALOG_H
#define CONTAINMENTCHECKDIALOG_H

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QJsonArray>
#include <QHash>
#include <QMap>

#include "qgis.h"
#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgsvectorlayer.h"
#include "qgsspatialindex.h"
#include "qgisinterface.h"

#include "qgsvectorfilewriter.h"
#include "qgsproject.h"
#include "qgsfield.h"
#include "qgsfields.h"
#include "qgscoordinatereferencesystem.h"

class GeometryValidator;
class JsonLogger;

namespace Ui
{
    class ContainmentCheckDialog;
}

class ContainmentCheckDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ContainmentCheckDialog(QgisInterface* iface = nullptr, QWidget* parent = nullptr);
    ~ContainmentCheckDialog();

    // 设置数据路径的方法
    void setBasePolygonPath(const QString& path) { m_basePolygonPath = path; }
    void setBasePointPath(const QString& path) { m_basePointPath = path; }
    void setParsedPolygonPath(const QString& path) { m_parsedPolygonPath = path; }
    void setParsedPointPath(const QString& path) { m_parsedPointPath = path; }
    void setRiverPolygonPath(const QString& path) { m_riverPolygonPath = path; }
    void setDitchPaths(const QStringList& paths) { m_ditchPaths = paths; }
    void setDitchFlowPaths(const QStringList& paths) { m_ditchFlowPaths = paths; }
    void setBridgePaths(const QStringList& paths) { m_bridgePaths = paths; }
    void setRoadPaths(const QStringList& paths) { m_roadPaths = paths; }
    void setTunnelEntrancePaths(const QStringList& paths) { m_tunnelEntrancePaths = paths; }
    void setTunnelPaths(const QStringList& paths) { m_tunnelPaths = paths; }
    void setRoadPathsForTunnel(const QStringList& paths) { m_roadPathsForTunnel = paths; }

    // 修改输出路径设置方法
    void setOutputLogPath(const QString& path) { m_outputLogPath = path; }
    void setOutputErrorPath(const QString& path) { m_outputErrorPath = path; }
    void setOutputJsonPath(const QString& path) { m_outputJsonPath = path; }

    // 添加XML配置路径设置方法
    void setXmlConfigPath(const QString& xmlConfigPath) { m_xmlConfigPath = xmlConfigPath; }

    // 检查方法
    void runAdministrativeContainmentCheck();
    void runRiverContainmentCheck();
    void runDitchFlowPositionCheck();
    void runBridgePositionCheck();
    void runTunnelEntrancePositionCheck();
    void setScale(int scale) { m_scale = scale; }
    int scale() const { return m_scale; }

    // 辅助方法声明
    QString getLayerNameFromPath(const QString& filePath);
    bool ensureDirectoryExists(const QString& path);

private:
    void loadAllData();
    void cleanupLayers();
    void resetProgressForCheck(const QString& checkName, int totalSteps);
    bool saveErrorFeaturesByCheckType(const QList<QgsFeature>& errorFeatures, const QString& relationCheckType, bool isGeometryError = false);
    bool saveLayerErrorFeatures(const QString& layerName, const QList<QgsFeature>& features, const QString& outputPath);
    QMap<QString, QList<QgsFeature>> performGeometryValidation(const QString& checkType, const QList<QgsVectorLayer*>& layers, QTextStream& logStream);
    void logFeatureInfo(QTextStream& stream, const QgsFeature& feature, const QString& message);
    void logProgress(QTextStream& stream, int current, int total, const QString& stage);
    QList<QgsFeature> getValidFeaturesFromLayer(QgsVectorLayer* layer, const QMap<QString, QList<QgsFeature>>& geometryErrors);
    QgsVectorLayer* loadShapefile(const QString& filePath);
    QList<QgsVectorLayer*> loadMultipleShapefiles(const QStringList& filePaths);
    QgsFeature findNearestPoint(const QgsGeometry& geometry, QgsVectorLayer* layer);
    QList<QgsFeature> checkAdministrativeContainment(QTextStream& logStream, QJsonArray& jsonErrors);
    QList<QgsFeature> checkRiverContainment(QTextStream& logStream, QJsonArray& jsonErrors);
    QList<QgsFeature> checkDitchFlowPosition(QTextStream& logStream, QJsonArray& jsonErrors);
    QList<QgsFeature> checkBridgePosition(QTextStream& logStream, QJsonArray& jsonErrors);
    QList<QgsFeature> checkTunnelEntrancePosition(QTextStream& logStream, QJsonArray& jsonErrors);
    QgsGeometry mergeBridgeFeatures(const QList<QgsFeature>& bridgeFeatures);
    QgsGeometry getCentroidFromGeometry(const QgsGeometry& geometry);

    QString generateDerivedPath(const QString& basePath, const QString& checkType, const QString& layerName = "", bool isGeometryError = false);
    QString getDirectoryFromPath(const QString& filePath);
    QString getBaseNameFromPath(const QString& filePath);
    QString generateGeometryErrorPath(const QString& layerName, const QString& checkType);
    QString generateOutputPath(const QString& basePath, const QString& layerName, const QString& checkType);
    void addErrorFeature(QList<QgsFeature>& errorFeatures, const QgsFeature& originalFeature, const QString& errorType, const QString& description, const QString& layerName);
    QString getLayerNameFromFeatureSource(const QgsFeature& feature);
    void generateGeometryErrorReport(
        const QMap<QString, QList<QgsFeature>>& geometryErrors,
        const QString& checkType,
        QTextStream& logStream,
        const QString& layerName = "");
    int m_scale;
    bool hasGeometryErrors(const QMap<QString, QList<QgsFeature>>& geometryErrors);
    void writeDetailedLog(const QString& message);
    QgsVectorLayer* findLayerByName(const QString& layerName);
    Ui::ContainmentCheckDialog* ui;
    QgisInterface* mIface;

    // 图层指针
    QgsVectorLayer* m_basePolygonLayer;
    QgsVectorLayer* m_basePointLayer;
    QgsVectorLayer* m_parsedPolygonLayer;
    QgsVectorLayer* m_parsedPointLayer;
    QgsVectorLayer* m_riverPolygonLayer;
    QList<QgsVectorLayer*> m_ditchFlowLayers;
    QList<QgsVectorLayer*> m_ditchLayers;
    QList<QgsVectorLayer*> m_bridgeLayers;
    QList<QgsVectorLayer*> m_roadLayers;
    QList<QgsVectorLayer*> m_tunnelEntranceLayers;
    QList<QgsVectorLayer*> m_tunnelLayers;
    QList<QgsVectorLayer*> m_roadLayersForTunnel;

    // 空间索引
    QgsSpatialIndex m_basePolygonIndex;
    QgsSpatialIndex m_basePointIndex;
    QgsSpatialIndex m_parsedPolygonIndex;
    QHash<QgsFeatureId, QString> m_parsedPolygonNameMap;

    GeometryValidator* m_geometryValidator;
    std::unique_ptr<JsonLogger> m_jsonLogger;

    // 数据路径
    QString m_basePolygonPath;
    QString m_basePointPath;
    QString m_parsedPolygonPath;
    QString m_parsedPointPath;
    QString m_riverPolygonPath;
    QStringList m_ditchPaths;
    QStringList m_ditchFlowPaths;
    QStringList m_bridgePaths;
    QStringList m_roadPaths;
    QStringList m_tunnelEntrancePaths;
    QStringList m_tunnelPaths;
    QStringList m_roadPathsForTunnel;

    // 输出路径（由调用者传递完整路径）
    QString m_outputLogPath;
    QString m_outputErrorPath;
    QString m_outputJsonPath;
    QString m_xmlConfigPath;

    // XML配置相关方法
    QString getCheckTypeFromXml(const QString& xmlConfigPath);
    double getBufferDistanceFromXml(const QString& xmlConfigPath, const QString& ruleName, double defaultDistance);
    int getScaleFromXml(const QString& xmlConfigPath);
    double convertMapDistanceToReal(double mapDistance, const QString& unit, int scale);
    QMap<QString, QVariant> getCheckItemParamsFromXml(const QString& xmlConfigPath);

    QList<QgsFeature> checkDitchFlowPosition(QTextStream& logStream, QJsonArray& jsonErrors, double bufferDistance);
    QList<QgsFeature> checkTunnelEntrancePosition(QTextStream& logStream, QJsonArray& jsonErrors, double bufferDistance);
};

#endif