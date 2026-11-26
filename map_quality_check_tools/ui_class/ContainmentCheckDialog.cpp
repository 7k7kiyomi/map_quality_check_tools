#include "ContainmentCheckDialog.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QDateTime>
#include <memory>
#include <algorithm>
#include <cmath>
#include "JsonLogger.h"
#include "GeometryValidator.h"
#include "XmlConfigReader.h"

// 构造函数
ContainmentCheckDialog::ContainmentCheckDialog(QgisInterface* iface, QWidget* parent) : QDialog(parent),
mIface(iface),
m_basePolygonLayer(nullptr),
m_basePointLayer(nullptr),
m_parsedPolygonLayer(nullptr),
m_parsedPointLayer(nullptr),
m_riverPolygonLayer(nullptr),
m_geometryValidator(new GeometryValidator()),
m_jsonLogger(nullptr),
m_scale(0)
{

}

// 析构函数
ContainmentCheckDialog::~ContainmentCheckDialog()
{
    cleanupLayers();
    delete m_geometryValidator;
}

// 写入日志方法
void ContainmentCheckDialog::writeDetailedLog(const QString& message)
{
    if (m_outputLogPath.isEmpty())
        return;

    try
    {
        QFile file(m_outputLogPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        {
            QTextStream stream(&file);
            stream.setCodec("UTF-8");
            QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
            stream << timestamp << " - " << message << "\n";
            file.close();
        }
    }
    catch (...)
    {
        qCritical() << "无法写入详细日志文件:" << m_outputLogPath;
    }
}

// 修改几何验证方法
QMap<QString, QList<QgsFeature>> ContainmentCheckDialog::performGeometryValidation(const QString& checkType,
    const QList<QgsVectorLayer*>& layers,
    QTextStream& logStream)
{
    this->writeDetailedLog(QString("【%1】开始几何有效性检查...").arg(checkType));
    logStream << QString(u8"【%1】开始几何有效性检查...\n").arg(checkType).toUtf8();

    QMap<QString, QList<QgsFeature>> geometryErrors = m_geometryValidator->validateLayers(layers, checkType, logStream);

    // 过滤掉没有错误的图层并记录日志
    QMap<QString, QList<QgsFeature>> filteredErrors;
    int errorLayerCount = 0;

    for (auto it = geometryErrors.begin(); it != geometryErrors.end(); ++it)
    {
        if (!it.value().isEmpty())
        {
            filteredErrors[it.key()] = it.value();
            errorLayerCount++;
            this->writeDetailedLog(QString("图层 '%1' 发现 %2 个几何无效要素。")
                .arg(it.key())
                .arg(it.value().size()));
        }
    }

    this->writeDetailedLog(QString("【%1】几何有效性检查完成，发现无效要素图层数: %2").arg(checkType).arg(errorLayerCount));
    logStream << QString(u8"【%1】几何有效性检查完成\n").arg(checkType).toUtf8();

    return filteredErrors;
}

// 修改几何错误报告生成方法
void ContainmentCheckDialog::generateGeometryErrorReport(
    const QMap<QString, QList<QgsFeature>>& geometryErrors,
    const QString& checkType,
    QTextStream& logStream,
    const QString& layerName)
{
    this->writeDetailedLog(QString("因几何无效，正在生成最终错误报告: %1").arg(checkType));

    // 收集所有几何错误
    QJsonObject allErrors;
    QList<QgsFeature> allErrorFeatures;
    int errorCount = 0;

    for (auto it = geometryErrors.begin(); it != geometryErrors.end(); ++it)
    {
        if (!it.value().isEmpty())
        {
            QString errorLayerName = it.key();
            allErrorFeatures.append(it.value());
            for (const QgsFeature& errorFeature : it.value())
            {
                QString errorDesc = errorFeature.attribute("DESCRIPTION").toString();
                allErrors[QString("error%1").arg(++errorCount)] =
                    QString(u8"图层[%1]: %2").arg(errorLayerName).arg(errorDesc);
            }
        }
    }

    // 使用传入的、确切的JSON路径
    QString jsonPath = m_outputJsonPath;

    // 创建检查项
    QJsonObject checkItem;
    checkItem["id"] = "113";
    checkItem["name"] = QString::fromUtf8(u8"关系检查");
    checkItem["status"] = "质检成功"; // 因几何无效而失败
    checkItem["err_exist"] = true;
    checkItem["info"] = QString(u8"存在几何无效要素，质检中止。请修复后重试。");
    checkItem["errors"] = allErrors;

    // 创建主JSON对象
    QJsonObject jsonLog;
    jsonLog["check_item"] = checkItem;

    // 使用 JsonLogger 生成JSON文件
    JsonLogger jsonLogger;
    if (jsonLogger.saveJsonToFile(jsonLog, jsonPath))
    {
        this->writeDetailedLog(QString("几何无效JSON报告已生成: %1").arg(jsonPath));
        logStream << QString(u8"几何无效报告已生成: %1\n").arg(jsonPath);
    }

    // 将所有几何无效要素保存到传入的、确切的错误SHP路径
    if (!allErrorFeatures.isEmpty())
    {
        if (this->saveErrorFeaturesByCheckType(allErrorFeatures, checkType, true))
        {
            this->writeDetailedLog(QString("几何无效要素已全部保存至: %1").arg(m_outputErrorPath));
        }
        else
        {
            this->writeDetailedLog(QString("❌ 几何无效要素保存失败: %1").arg(m_outputErrorPath));
        }
    }
}

// 修改保存错误要素方法
bool ContainmentCheckDialog::saveErrorFeaturesByCheckType(const QList<QgsFeature>& errorFeatures, const QString& relationCheckType, bool isGeometryError)
{
    if (errorFeatures.isEmpty())
    {
        writeDetailedLog("没有错误要素需要保存");
        return true;
    }

    // 记录错误要素统计信息
    writeDetailedLog(QString("开始保存错误要素，检查类型: %1, 总要素数: %2, 是否为几何错误: %3")
        .arg(relationCheckType)
        .arg(errorFeatures.size())
        .arg(isGeometryError ? "是" : "否"));

    // 按图层分组错误要素
    QMap<QString, QList<QgsFeature>> layerErrorMap;
    for (const QgsFeature& feature : errorFeatures)
    {
        QString layerName = getLayerNameFromFeatureSource(feature);
        layerErrorMap[layerName].append(feature);

        // 记录详细信息
        writeDetailedLog(QString("错误要素 - 图层: %1, 要素ID: %2, 错误类型: %3")
            .arg(layerName)
            .arg(feature.id())
            .arg(feature.attribute("ERROR_TYPE").toString()));
    }

    // 为每个图层保存错误要素到同一个文件
    bool allSuccess = true;
    for (auto it = layerErrorMap.begin(); it != layerErrorMap.end(); ++it)
    {
        QString layerName = it.key();
        // 所有错误都写入到 m_outputErrorPath
        QString outputPath = m_outputErrorPath;

        writeDetailedLog(QString("准备将图层 '%1' 的 %2 个错误要素写入: %3")
            .arg(layerName)
            .arg(it.value().size())
            .arg(outputPath));

        if (!saveLayerErrorFeatures(layerName, it.value(), outputPath))
        {
            allSuccess = false;
            writeDetailedLog(QString("❌ 保存错误要素失败: %1").arg(outputPath));
        }
    }

    if (allSuccess)
    {
        writeDetailedLog(QString("✓ 所有错误要素保存成功，共 %1 个图层").arg(layerErrorMap.size()));
    }
    else
    {
        writeDetailedLog("❌ 部分错误要素保存失败");
    }

    return allSuccess;
}
// 添加数据加载方法
void ContainmentCheckDialog::loadAllData()
{
    // 1. 库体面
    if (!m_basePolygonPath.isEmpty())
    {
        m_basePolygonLayer = loadShapefile(m_basePolygonPath);
    }

    // 2. 库体点
    if (!m_basePointPath.isEmpty())
    {
        m_basePointLayer = loadShapefile(m_basePointPath);
    }

    // 3. 解析面
    if (!m_parsedPolygonPath.isEmpty())
    {
        m_parsedPolygonLayer = loadShapefile(m_parsedPolygonPath);
    }

    // 4. 解析点
    if (!m_parsedPointPath.isEmpty())
    {
        m_parsedPointLayer = loadShapefile(m_parsedPointPath);
    }

    // 5. 河流面
    if (!m_riverPolygonPath.isEmpty())
    {
        m_riverPolygonLayer = loadShapefile(m_riverPolygonPath);
    }

    // 6. 沟渠
    if (!m_ditchPaths.isEmpty())
    {
        m_ditchLayers = loadMultipleShapefiles(m_ditchPaths);
    }

    // 7. 渠流向
    if (!m_ditchFlowPaths.isEmpty())
    {
        m_ditchFlowLayers = loadMultipleShapefiles(m_ditchFlowPaths);
    }

    // 8. 桥梁
    if (!m_bridgePaths.isEmpty())
    {
        m_bridgeLayers = loadMultipleShapefiles(m_bridgePaths);
    }

    // 9. 道路要素
    if (!m_roadPaths.isEmpty())
    {
        m_roadLayers = loadMultipleShapefiles(m_roadPaths);
    }

    // 10. 隧道相关数据（统一处理）
    if (!m_tunnelEntrancePaths.isEmpty())
    {
        m_tunnelEntranceLayers = loadMultipleShapefiles(m_tunnelEntrancePaths);
    }
    if (!m_tunnelPaths.isEmpty())
    {
        m_tunnelLayers = loadMultipleShapefiles(m_tunnelPaths);
    }
    if (!m_roadPathsForTunnel.isEmpty())
    {
        m_roadLayersForTunnel = loadMultipleShapefiles(m_roadPathsForTunnel);
    }
}
// 运行行政面检查
void ContainmentCheckDialog::runAdministrativeContainmentCheck()
{
    QString checkTypeStr = u8"行政面包含检查";
    QString layerName = checkTypeStr;
    if (m_parsedPointLayer)
    {
        layerName = m_parsedPointLayer->name();
    }

    this->writeDetailedLog("=== 开始行政面包含检查 ===");
    this->writeDetailedLog(QString("启动时间: %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")));
    this->writeDetailedLog(QString("检测参数: 比例尺=1:%1, XML配置=%2").arg(m_scale).arg(m_xmlConfigPath));
    this->writeDetailedLog(QString("输出日志: %1").arg(m_outputLogPath));
    this->writeDetailedLog(QString("错误要素基础路径: %1").arg(m_outputErrorPath));

    // === XML配置验证 ===
    if (m_xmlConfigPath.isEmpty())
    {
        this->writeDetailedLog("❌ 未提供XML配置路径");
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }

    this->writeDetailedLog(QString("正在加载XML配置: %1").arg(m_xmlConfigPath));
    XmlConfigReader configReader;
    if (!configReader.loadConfig(m_xmlConfigPath))
    {
        this->writeDetailedLog(QString("❌ 无法加载XML配置: %1").arg(m_xmlConfigPath));
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }
    this->writeDetailedLog("✓ XML配置加载成功");

    // === 比例尺验证 ===
    QString relationCheckItemName = QString::fromUtf8(u8"关系检查");
    QList<int> availableScales = configReader.getAvailableScales();

    if (!configReader.hasScale(m_scale))
    {
        QStringList scaleStrings;
        for (int s : availableScales)
            scaleStrings << QString::number(s);
        QString errorMsg = QString(u8"XML配置中不存在比例尺1:%1，可用比例尺: %2")
            .arg(m_scale)
            .arg(scaleStrings.join(", "));
        this->writeDetailedLog("❌ " + errorMsg);
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }
    this->writeDetailedLog(QString("✓ 比例尺检查通过: 1:%1").arg(m_scale));

    // === 检查项验证 ===
    if (!configReader.hasCheckItem(m_scale, relationCheckItemName))
    {
        QList<QString> itemNames = configReader.getCheckItemNames(m_scale);
        QString errorMsg = QString(u8"比例尺1:%1下不存在检查项'%2'，可用检查项: %3")
            .arg(m_scale)
            .arg(relationCheckItemName)
            .arg(itemNames.join(", "));
        this->writeDetailedLog("❌ " + errorMsg);
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }
    this->writeDetailedLog(QString("✓ 检查项存在: %1").arg(relationCheckItemName));

    // === 具体项验证 ===
    QString checkType = configReader.getRuleStringValue(m_scale, relationCheckItemName, QString::fromUtf8(u8"具体项"), "");
    if (checkType != checkTypeStr)
    {
        QString errorMsg = QString(u8"XML配置的检查类型('%1')与当前执行的检查('%2')不匹配")
            .arg(checkType)
            .arg(checkTypeStr);
        this->writeDetailedLog("❌ " + errorMsg);
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }
    this->writeDetailedLog(QString("✓ 检查类型匹配: %1").arg(checkType));

    // === 数据加载 ===
    this->writeDetailedLog("开始加载数据图层...");

    int loadedCount = 0;
    if (!m_basePolygonPath.isEmpty())
    {
        m_basePolygonLayer = loadShapefile(m_basePolygonPath);
        if (m_basePolygonLayer && m_basePolygonLayer->isValid())
        {
            this->writeDetailedLog(QString("✓ 加载库体面: %1, 要素数: %2")
                .arg(m_basePolygonPath)
                .arg(m_basePolygonLayer->featureCount()));
            loadedCount++;
        }
        else
        {
            this->writeDetailedLog(QString("❌ 加载库体面失败: %1").arg(m_basePolygonPath));
        }
    }

    if (!m_basePointPath.isEmpty())
    {
        m_basePointLayer = loadShapefile(m_basePointPath);
        if (m_basePointLayer && m_basePointLayer->isValid())
        {
            this->writeDetailedLog(QString("✓ 加载库体点: %1, 要素数: %2")
                .arg(m_basePointPath)
                .arg(m_basePointLayer->featureCount()));
            loadedCount++;
        }
        else
        {
            this->writeDetailedLog(QString("❌ 加载库体点失败: %1").arg(m_basePointPath));
        }
    }

    if (!m_parsedPolygonPath.isEmpty())
    {
        m_parsedPolygonLayer = loadShapefile(m_parsedPolygonPath);
        if (m_parsedPolygonLayer && m_parsedPolygonLayer->isValid())
        {
            this->writeDetailedLog(QString("✓ 加载解析面: %1, 要素数: %2")
                .arg(m_parsedPolygonPath)
                .arg(m_parsedPolygonLayer->featureCount()));
            loadedCount++;
        }
        else
        {
            this->writeDetailedLog(QString("❌ 加载解析面失败: %1").arg(m_parsedPolygonPath));
        }
    }

    if (!m_parsedPointPath.isEmpty())
    {
        m_parsedPointLayer = loadShapefile(m_parsedPointPath);
        if (m_parsedPointLayer && m_parsedPointLayer->isValid())
        {
            this->writeDetailedLog(QString("✓ 加载解析点: %1, 要素数: %2")
                .arg(m_parsedPointPath)
                .arg(m_parsedPointLayer->featureCount()));
            layerName = QFileInfo(m_parsedPointLayer->source()).baseName();
            loadedCount++;
        }
        else
        {
            this->writeDetailedLog(QString("❌ 加载解析点失败: %1").arg(m_parsedPointPath));
        }
    }

    if (loadedCount < 4)
    {
        this->writeDetailedLog(QString("❌ 数据加载不完整，成功加载 %1/4 个必要图层").arg(loadedCount));
        this->writeDetailedLog("=== 检测中止 ===");
        cleanupLayers();
        return;
    }

    // === 几何有效性检查 ===
    this->writeDetailedLog("开始几何有效性检查...");
    QList<QgsVectorLayer*> checkLayers;
    if (m_parsedPointLayer)
        checkLayers.append(m_parsedPointLayer);
    if (m_basePointLayer)
        checkLayers.append(m_basePointLayer);
    if (m_parsedPolygonLayer)
        checkLayers.append(m_parsedPolygonLayer);
    if (m_basePolygonLayer)
        checkLayers.append(m_basePolygonLayer);

    // 创建临时文本流用于几何验证
    QString tempLogContent;
    QTextStream tempStream(&tempLogContent);
    tempStream.setCodec("UTF-8");

    QMap<QString, QList<QgsFeature>> geometryErrors = performGeometryValidation(checkTypeStr, checkLayers, tempStream);

    // === 检查是否有几何错误 ===
    if (hasGeometryErrors(geometryErrors))
    {
        this->writeDetailedLog("❌ 发现几何无效要素，停止行政面包含检查！");
        generateGeometryErrorReport(geometryErrors, checkTypeStr, tempStream);
        cleanupLayers();
        return;
    }

    this->writeDetailedLog("✅ 几何有效性检查通过");

    // === 执行关系检查 ===
    this->writeDetailedLog("开始行政面包含关系检查...");
    QJsonArray relationErrorsJson;
    QList<QgsFeature> relationErrors = checkAdministrativeContainment(tempStream, relationErrorsJson);

    bool hasRelationErrors = !relationErrors.isEmpty();

    // === 生成JSON报告 ===
    QJsonObject allErrors;
    int errorCount = 0;
    for (const QgsFeature& error : relationErrors)
    {
        QString errorDesc = error.attribute("DESCRIPTION").toString();
        allErrors[QString("error%1").arg(++errorCount)] = errorDesc;
    }

    // 创建检查项
    QJsonObject checkItem;
    checkItem["id"] = "113";
    checkItem["name"] = relationCheckItemName;
    checkItem["status"] = "质检成功";
    checkItem["err_exist"] = hasRelationErrors;
    checkItem["info"] = hasRelationErrors ? QString(u8"%1存在行政包含检查错误").arg(layerName) : QString(u8"%1行政包含检查通过").arg(layerName);

    if (hasRelationErrors)
    {
        checkItem["errors"] = allErrors;
    }

    // 创建主JSON对象
    QJsonObject jsonLog;
    jsonLog["check_item"] = checkItem;

    // === 使用 JsonLogger 生成JSON文件 ===
    QString jsonPath = m_outputJsonPath; // 直接使用成员变量

    JsonLogger jsonLogger;
    if (jsonLogger.saveJsonToFile(jsonLog, jsonPath))
    {
        this->writeDetailedLog(QString("✓ JSON报告已生成: %1").arg(jsonPath));
    }
    else
    {
        this->writeDetailedLog(QString("❌ JSON报告生成失败: %1").arg(jsonPath));
    }

    // [移除] 不再需要设置生成的JSON路径
    // setGeneratedJsonPath(jsonPath);

    // === 保存关系检查错误要素 ===
    if (!relationErrors.isEmpty())
    {
        if (this->saveErrorFeaturesByCheckType(relationErrors, checkTypeStr, false))
        {
            this->writeDetailedLog("✓ 错误要素已保存");
        }
        else
        {
            this->writeDetailedLog("❌ 错误要素保存失败");
        }
    }

    // === 完成日志 ===
    this->writeDetailedLog("行政面包含检查完成");
    this->writeDetailedLog(QString("总要素数: %1").arg(m_parsedPointLayer->featureCount()));
    this->writeDetailedLog(QString("发现关系错误: %1 个").arg(relationErrors.size()));
    this->writeDetailedLog("=== 检测结束 ===");

    cleanupLayers();
}
// 渠流向检查
void ContainmentCheckDialog::runDitchFlowPositionCheck()
{
    QString checkTypeStr = u8"渠流向位置检查";
    // 使用统一的日志系统
    QString layerName = checkTypeStr;
    if (!m_ditchFlowPaths.isEmpty())
    {
        layerName = getLayerNameFromPath(m_ditchFlowPaths.first());
    }

    this->writeDetailedLog("=== 开始渠流向位置检查 ===");
    this->writeDetailedLog(QString("启动时间: %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")));
    this->writeDetailedLog(QString("检测参数: 比例尺=1:%1, XML配置=%2").arg(m_scale).arg(m_xmlConfigPath));
    this->writeDetailedLog(QString("输出日志: %1").arg(m_outputLogPath));
    this->writeDetailedLog(QString("错误要素基础路径: %1").arg(m_outputErrorPath));

    // === XML配置验证 ===
    if (m_xmlConfigPath.isEmpty())
    {
        this->writeDetailedLog("❌ 未提供XML配置路径");
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }

    this->writeDetailedLog(QString("正在加载XML配置: %1").arg(m_xmlConfigPath));
    XmlConfigReader configReader;
    if (!configReader.loadConfig(m_xmlConfigPath))
    {
        this->writeDetailedLog(QString("❌ 无法加载XML配置: %1").arg(m_xmlConfigPath));
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }
    this->writeDetailedLog("✓ XML配置加载成功");

    // === 比例尺验证 ===
    QString relationCheckItemName = QString::fromUtf8(u8"关系检查");
    if (!configReader.hasScale(m_scale))
    {
        QList<int> availableScales = configReader.getAvailableScales();
        QStringList scaleStrings;
        for (int s : availableScales)
            scaleStrings << QString::number(s);
        QString errorMsg = QString(u8"XML配置中不存在比例尺1:%1，可用比例尺: %2")
            .arg(m_scale)
            .arg(scaleStrings.join(", "));
        this->writeDetailedLog("❌ " + errorMsg);
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }
    this->writeDetailedLog(QString("✓ 比例尺检查通过: 1:%1").arg(m_scale));

    // === 检查项验证 ===
    if (!configReader.hasCheckItem(m_scale, relationCheckItemName))
    {
        QList<QString> itemNames = configReader.getCheckItemNames(m_scale);
        QString errorMsg = QString(u8"比例尺1:%1下不存在检查项'%2'，可用检查项: %3")
            .arg(m_scale)
            .arg(relationCheckItemName)
            .arg(itemNames.join(", "));
        this->writeDetailedLog("❌ " + errorMsg);
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }
    this->writeDetailedLog(QString("✓ 检查项存在: %1").arg(relationCheckItemName));

    // === 具体项验证 ===
    QString checkType = configReader.getRuleStringValue(m_scale, relationCheckItemName, QString::fromUtf8(u8"具体项"), "");
    if (checkType != checkTypeStr)
    {
        QString errorMsg = QString(u8"XML配置的检查类型('%1')与当前执行的检查('%2')不匹配")
            .arg(checkType)
            .arg(checkTypeStr);
        this->writeDetailedLog("❌ " + errorMsg);
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }
    this->writeDetailedLog(QString("✓ 检查类型匹配: %1").arg(checkType));

    // === 读取缓冲区距离 ===
    double bufferDistance = configReader.getRuleValue(m_scale, relationCheckItemName, QString::fromUtf8(u8"渠流向缓冲区半径"), -1.0);
    if (bufferDistance <= 0)
    {
        this->writeDetailedLog(QString("❌ XML配置错误：渠流向缓冲区半径必须大于0，当前值: %1").arg(bufferDistance));
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }

    QString unit = configReader.getRuleStringValue(m_scale, relationCheckItemName, QString::fromUtf8(u8"渠流向缓冲区半径_unit"), "毫米");
    bufferDistance = convertMapDistanceToReal(bufferDistance, unit, m_scale);

    this->writeDetailedLog(QString("✓ 缓冲区半径: 图上%1%2 -> 实际%3米")
        .arg(configReader.getRuleValue(m_scale, relationCheckItemName, QString::fromUtf8(u8"渠流向缓冲区半径"), -1.0))
        .arg(unit)
        .arg(bufferDistance));

    // === 数据加载 ===
    this->writeDetailedLog("开始加载数据图层...");

    int loadedCount = 0;
    if (!m_ditchFlowPaths.isEmpty())
    {
        m_ditchFlowLayers = loadMultipleShapefiles(m_ditchFlowPaths);
        if (!m_ditchFlowLayers.isEmpty())
        {
            for (QgsVectorLayer* layer : m_ditchFlowLayers)
            {
                if (layer && layer->isValid())
                {
                    this->writeDetailedLog(QString("✓ 加载渠流向: %1, 要素数: %2")
                        .arg(layer->source())
                        .arg(layer->featureCount()));
                    loadedCount++;
                    layerName = getLayerNameFromPath(layer->source());
                }
            }
        }
        else
        {
            this->writeDetailedLog("❌ 加载渠流向图层失败");
        }
    }

    if (!m_ditchPaths.isEmpty())
    {
        m_ditchLayers = loadMultipleShapefiles(m_ditchPaths);
        if (!m_ditchLayers.isEmpty())
        {
            for (QgsVectorLayer* layer : m_ditchLayers)
            {
                if (layer && layer->isValid())
                {
                    this->writeDetailedLog(QString("✓ 加载沟渠: %1, 要素数: %2")
                        .arg(layer->source())
                        .arg(layer->featureCount()));
                    loadedCount++;
                }
            }
        }
        else
        {
            this->writeDetailedLog("❌ 加载沟渠图层失败");
        }
    }

    if (loadedCount < 2)
    {
        this->writeDetailedLog(QString("❌ 数据加载不完整，成功加载 %1/2 个必要图层").arg(loadedCount));
        this->writeDetailedLog("=== 检测中止 ===");
        cleanupLayers();
        return;
    }

    // === 几何有效性检查 ===
    this->writeDetailedLog("开始几何有效性检查...");
    QList<QgsVectorLayer*> checkLayers;
    checkLayers.append(m_ditchFlowLayers);
    checkLayers.append(m_ditchLayers);

    // 创建临时文本流用于几何验证
    QString tempLogContent;
    QTextStream tempStream(&tempLogContent);
    tempStream.setCodec("UTF-8");

    QMap<QString, QList<QgsFeature>> geometryErrors = performGeometryValidation(checkTypeStr, checkLayers, tempStream);

    // === 检查是否有几何错误 ===
    if (hasGeometryErrors(geometryErrors))
    {
        this->writeDetailedLog("❌ 发现几何无效要素，停止渠流向位置检查！");
        generateGeometryErrorReport(geometryErrors, checkTypeStr, tempStream);
        cleanupLayers();
        return;
    }

    this->writeDetailedLog("✅ 几何有效性检查通过");

    // === 执行关系检查 ===
    this->writeDetailedLog("开始渠流向位置关系检查...");
    QJsonArray relationErrorsJson;
    QList<QgsFeature> relationErrors = checkDitchFlowPosition(tempStream, relationErrorsJson, bufferDistance);

    bool hasRelationErrors = !relationErrors.isEmpty();

    // === 生成JSON报告 ===
    QJsonObject allErrors;
    int errorCount = 0;
    for (const QJsonValue& error : relationErrorsJson)
    {
        QJsonObject errorObj = error.toObject();
        QString errorDesc = errorObj["description"].toString();
        allErrors[QString("error%1").arg(++errorCount)] = errorDesc;
    }

    // 创建检查项
    QJsonObject checkItem;
    checkItem["id"] = "113";
    checkItem["name"] = relationCheckItemName;
    checkItem["status"] = "质检成功";
    checkItem["err_exist"] = hasRelationErrors;
    checkItem["info"] = hasRelationErrors ? QString(u8"%1存在渠流向位置检查错误").arg(layerName) : QString(u8"%1渠流向位置检查通过").arg(layerName);

    if (hasRelationErrors)
    {
        checkItem["errors"] = allErrors;
    }

    // 创建主JSON对象
    QJsonObject jsonLog;
    jsonLog["check_item"] = checkItem;

    // === 使用 JsonLogger 生成JSON文件 ===
    QString jsonPath = m_outputJsonPath; // 直接使用成员变量

    JsonLogger jsonLogger;
    if (jsonLogger.saveJsonToFile(jsonLog, jsonPath))
    {
        this->writeDetailedLog(QString("✓ JSON报告已生成: %1").arg(jsonPath));
    }
    else
    {
        this->writeDetailedLog(QString("❌ JSON报告生成失败: %1").arg(jsonPath));
    }

    // [移除] 不再需要设置生成的JSON路径
    // setGeneratedJsonPath(jsonPath);

    // === 保存关系检查错误要素 ===
    if (!relationErrors.isEmpty())
    {
        if (this->saveErrorFeaturesByCheckType(relationErrors, checkTypeStr, false))
        {
            this->writeDetailedLog("✓ 错误要素已保存");
        }
        else
        {
            this->writeDetailedLog("❌ 错误要素保存失败");
        }
    }

    // === 完成日志 ===
    this->writeDetailedLog("渠流向位置检查完成");
    this->writeDetailedLog(QString("渠流向要素数: %1").arg(m_ditchFlowLayers.isEmpty() ? 0 : m_ditchFlowLayers.first()->featureCount()));
    this->writeDetailedLog(QString("发现关系错误: %1 个").arg(relationErrors.size()));
    this->writeDetailedLog("=== 检测结束 ===");

    cleanupLayers();
}
// 隧道口检查
void ContainmentCheckDialog::runTunnelEntrancePositionCheck()
{
    QString checkTypeStr = u8"隧道口位置检查";
    // 使用统一的日志系统
    QString layerName = checkTypeStr;
    if (!m_tunnelEntrancePaths.isEmpty())
    {
        layerName = getLayerNameFromPath(m_tunnelEntrancePaths.first());
    }

    this->writeDetailedLog("=== 开始隧道口位置检查 ===");
    this->writeDetailedLog(QString("启动时间: %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")));
    this->writeDetailedLog(QString("检测参数: 比例尺=1:%1, XML配置=%2").arg(m_scale).arg(m_xmlConfigPath));
    this->writeDetailedLog(QString("输出日志: %1").arg(m_outputLogPath));
    this->writeDetailedLog(QString("错误要素基础路径: %1").arg(m_outputErrorPath));

    // === XML配置验证 ===
    if (m_xmlConfigPath.isEmpty())
    {
        this->writeDetailedLog("❌ 未提供XML配置路径");
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }

    XmlConfigReader configReader;
    if (!configReader.loadConfig(m_xmlConfigPath))
    {
        this->writeDetailedLog(QString("❌ 无法加载XML配置: %1").arg(m_xmlConfigPath));
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }
    this->writeDetailedLog("✓ XML配置加载成功");

    // === 比例尺和检查项验证 ===
    QString relationCheckItemName = QString::fromUtf8(u8"关系检查");
    if (!configReader.hasScale(m_scale) || !configReader.hasCheckItem(m_scale, relationCheckItemName))
    {
        this->writeDetailedLog("❌ 比例尺或检查项不匹配");
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }

    QString checkType = configReader.getRuleStringValue(m_scale, relationCheckItemName, QString::fromUtf8(u8"具体项"), "");
    if (checkType != checkTypeStr)
    {
        this->writeDetailedLog(QString("❌ 检查类型不匹配: %1, 需要: %2").arg(checkType).arg(checkTypeStr));
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }
    this->writeDetailedLog(QString("✓ 配置检查通过"));

    // === 读取缓冲区距离 ===
    double bufferDistance = configReader.getRuleValue(m_scale, relationCheckItemName, QString::fromUtf8(u8"隧道口缓冲半径"), -1.0);
    if (bufferDistance <= 0)
    {
        this->writeDetailedLog(QString("❌ XML配置错误：隧道口缓冲半径必须大于0，当前值: %1").arg(bufferDistance));
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }

    QString unit = configReader.getRuleStringValue(m_scale, relationCheckItemName, QString::fromUtf8(u8"隧道口缓冲半径_unit"), "毫米");
    bufferDistance = convertMapDistanceToReal(bufferDistance, unit, m_scale);

    this->writeDetailedLog(QString("✓ 缓冲区半径: 图上%1%2 -> 实际%3米")
        .arg(configReader.getRuleValue(m_scale, relationCheckItemName, QString::fromUtf8(u8"隧道口缓冲半径"), -1.0))
        .arg(unit)
        .arg(bufferDistance));

    // === 数据加载 ===
    this->writeDetailedLog("开始加载数据图层...");

    int loadedCount = 0;
    if (!m_tunnelEntrancePaths.isEmpty())
    {
        m_tunnelEntranceLayers = loadMultipleShapefiles(m_tunnelEntrancePaths);
        if (!m_tunnelEntranceLayers.isEmpty())
        {
            for (QgsVectorLayer* layer : m_tunnelEntranceLayers)
            {
                if (layer && layer->isValid())
                {
                    this->writeDetailedLog(QString("✓ 加载隧道口: %1, 要素数: %2")
                        .arg(layer->source())
                        .arg(layer->featureCount()));
                    loadedCount++;
                    layerName = getLayerNameFromPath(layer->source());
                }
            }
        }
    }

    if (!m_tunnelPaths.isEmpty())
    {
        m_tunnelLayers = loadMultipleShapefiles(m_tunnelPaths);
        if (!m_tunnelLayers.isEmpty())
        {
            for (QgsVectorLayer* layer : m_tunnelLayers)
            {
                if (layer && layer->isValid())
                {
                    this->writeDetailedLog(QString("✓ 加载隧道: %1, 要素数: %2")
                        .arg(layer->source())
                        .arg(layer->featureCount()));
                    loadedCount++;
                }
            }
        }
    }

    if (!m_roadPathsForTunnel.isEmpty())
    {
        m_roadLayersForTunnel = loadMultipleShapefiles(m_roadPathsForTunnel);
        if (!m_roadLayersForTunnel.isEmpty())
        {
            for (QgsVectorLayer* layer : m_roadLayersForTunnel)
            {
                if (layer && layer->isValid())
                {
                    this->writeDetailedLog(QString("✓ 加载道路: %1, 要素数: %2")
                        .arg(layer->source())
                        .arg(layer->featureCount()));
                    loadedCount++;
                }
            }
        }
    }

    if (loadedCount < 3)
    {
        this->writeDetailedLog(QString("❌ 数据加载不完整，成功加载 %1/3 个必要图层").arg(loadedCount));
        this->writeDetailedLog("=== 检测中止 ===");
        cleanupLayers();
        return;
    }

    // === 几何有效性检查 ===
    this->writeDetailedLog("开始几何有效性检查...");
    QList<QgsVectorLayer*> allCheckLayers;
    allCheckLayers.append(m_tunnelEntranceLayers);
    allCheckLayers.append(m_tunnelLayers);
    allCheckLayers.append(m_roadLayersForTunnel);

    // 创建临时文本流用于几何验证
    QString tempLogContent;
    QTextStream tempStream(&tempLogContent);
    tempStream.setCodec("UTF-8");

    QMap<QString, QList<QgsFeature>> geometryErrors = performGeometryValidation(checkTypeStr, allCheckLayers, tempStream);

    if (hasGeometryErrors(geometryErrors))
    {
        this->writeDetailedLog("❌ 发现几何无效要素，停止隧道口位置检查！");
        generateGeometryErrorReport(geometryErrors, checkTypeStr, tempStream);
        cleanupLayers();
        return;
    }

    this->writeDetailedLog("✅ 几何有效性检查通过");

    // === 执行关系检查 ===
    this->writeDetailedLog("开始隧道口位置关系检查...");
    QJsonArray relationErrorsJson;
    QList<QgsFeature> relationErrors = checkTunnelEntrancePosition(tempStream, relationErrorsJson, bufferDistance);

    bool hasRelationErrors = !relationErrors.isEmpty();

    // === 生成JSON报告 ===
    QJsonObject allErrors;
    int errorCount = 0;
    for (const QJsonValue& error : relationErrorsJson)
    {
        QJsonObject errorObj = error.toObject();
        QString errorDesc = errorObj["description"].toString();
        allErrors[QString("error%1").arg(++errorCount)] = errorDesc;
    }

    // 创建检查项
    QJsonObject checkItem;
    checkItem["id"] = "113";
    checkItem["name"] = relationCheckItemName;
    checkItem["status"] = "质检成功";
    checkItem["err_exist"] = hasRelationErrors;
    checkItem["info"] = hasRelationErrors ? QString(u8"%1存在隧道口位置检查错误").arg(layerName) : QString(u8"%1隧道口位置检查通过").arg(layerName);

    if (hasRelationErrors)
    {
        checkItem["errors"] = allErrors;
    }

    // 创建主JSON对象
    QJsonObject jsonLog;
    jsonLog["check_item"] = checkItem;

    // === 使用 JsonLogger 生成JSON文件 ===
    QString jsonPath = m_outputJsonPath; // 直接使用成员变量

    JsonLogger jsonLogger;
    if (jsonLogger.saveJsonToFile(jsonLog, jsonPath))
    {
        this->writeDetailedLog(QString("✓ JSON报告已生成: %1").arg(jsonPath));
    }
    else
    {
        this->writeDetailedLog(QString("❌ JSON报告生成失败: %1").arg(jsonPath));
    }

    // [移除] 不再需要设置生成的JSON路径
    // setGeneratedJsonPath(jsonPath);

    // === 保存关系检查错误要素 ===
    if (!relationErrors.isEmpty())
    {
        saveErrorFeaturesByCheckType(relationErrors, checkTypeStr, false);
        this->writeDetailedLog("✓ 错误要素已保存");
    }

    // === 完成日志 ===
    this->writeDetailedLog("隧道口位置检查完成");
    this->writeDetailedLog(QString("隧道口要素数: %1").arg(m_tunnelEntranceLayers.isEmpty() ? 0 : m_tunnelEntranceLayers.first()->featureCount()));
    this->writeDetailedLog(QString("发现关系错误: %1 个").arg(relationErrors.size()));
    this->writeDetailedLog("=== 检测结束 ===");

    cleanupLayers();
}
// 桥梁位置检查
void ContainmentCheckDialog::runBridgePositionCheck()
{
    QString checkTypeStr = u8"桥梁位置检查";
    // 使用统一的日志系统
    QString layerName = checkTypeStr;
    if (!m_bridgePaths.isEmpty())
    {
        layerName = getLayerNameFromPath(m_bridgePaths.first());
    }

    this->writeDetailedLog("=== 开始桥梁位置检查 ===");
    this->writeDetailedLog(QString("启动时间: %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")));
    this->writeDetailedLog(QString("检测参数: 比例尺=1:%1, XML配置=%2").arg(m_scale).arg(m_xmlConfigPath));
    this->writeDetailedLog(QString("输出日志: %1").arg(m_outputLogPath));
    this->writeDetailedLog(QString("错误要素基础路径: %1").arg(m_outputErrorPath));

    // === XML配置验证 ===
    if (m_xmlConfigPath.isEmpty())
    {
        this->writeDetailedLog("❌ 未提供XML配置路径");
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }

    XmlConfigReader configReader;
    if (!configReader.loadConfig(m_xmlConfigPath))
    {
        this->writeDetailedLog(QString("❌ 无法加载XML配置: %1").arg(m_xmlConfigPath));
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }
    this->writeDetailedLog("✓ XML配置加载成功");

    // === 比例尺和检查项验证 ===
    QString relationCheckItemName = QString::fromUtf8(u8"关系检查");
    if (!configReader.hasScale(m_scale) || !configReader.hasCheckItem(m_scale, relationCheckItemName))
    {
        this->writeDetailedLog("❌ 比例尺或检查项不匹配");
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }

    QString checkType = configReader.getRuleStringValue(m_scale, relationCheckItemName, QString::fromUtf8(u8"具体项"), "");
    if (checkType != checkTypeStr)
    {
        this->writeDetailedLog(QString("❌ 检查类型不匹配: %1, 需要: %2").arg(checkType).arg(checkTypeStr));
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }
    this->writeDetailedLog(QString("✓ 配置检查通过"));

    // === 数据加载 ===
    this->writeDetailedLog("开始加载数据图层...");

    int loadedCount = 0;
    if (!m_bridgePaths.isEmpty())
    {
        m_bridgeLayers = loadMultipleShapefiles(m_bridgePaths);
        if (!m_bridgeLayers.isEmpty())
        {
            for (QgsVectorLayer* layer : m_bridgeLayers)
            {
                if (layer && layer->isValid())
                {
                    this->writeDetailedLog(QString("✓ 加载桥梁: %1, 要素数: %2")
                        .arg(layer->source())
                        .arg(layer->featureCount()));
                    loadedCount++;
                    layerName = getLayerNameFromPath(layer->source());
                }
            }
        }
    }

    if (!m_riverPolygonPath.isEmpty())
    {
        m_riverPolygonLayer = loadShapefile(m_riverPolygonPath);
        if (m_riverPolygonLayer && m_riverPolygonLayer->isValid())
        {
            this->writeDetailedLog(QString("✓ 加载河流面: %1, 要素数: %2")
                .arg(m_riverPolygonPath)
                .arg(m_riverPolygonLayer->featureCount()));
            loadedCount++;
        }
    }

    if (!m_roadPaths.isEmpty())
    {
        m_roadLayers = loadMultipleShapefiles(m_roadPaths);
        if (!m_roadLayers.isEmpty())
        {
            for (QgsVectorLayer* layer : m_roadLayers)
            {
                if (layer && layer->isValid())
                {
                    this->writeDetailedLog(QString("✓ 加载道路: %1, 要素数: %2")
                        .arg(layer->source())
                        .arg(layer->featureCount()));
                    loadedCount++;
                }
            }
        }
    }

    if (loadedCount < 3)
    {
        this->writeDetailedLog(QString("❌ 数据加载不完整，成功加载 %1/3 个必要图层").arg(loadedCount));
        this->writeDetailedLog("=== 检测中止 ===");
        cleanupLayers();
        return;
    }

    // === 几何有效性检查 ===
    this->writeDetailedLog("开始几何有效性检查...");
    QList<QgsVectorLayer*> allCheckLayers;
    allCheckLayers.append(m_bridgeLayers);
    if (m_riverPolygonLayer)
        allCheckLayers.append(m_riverPolygonLayer);
    allCheckLayers.append(m_roadLayers);

    // 创建临时文本流用于几何验证
    QString tempLogContent;
    QTextStream tempStream(&tempLogContent);
    tempStream.setCodec("UTF-8");

    QMap<QString, QList<QgsFeature>> geometryErrors = performGeometryValidation(checkTypeStr, allCheckLayers, tempStream);

    if (hasGeometryErrors(geometryErrors))
    {
        this->writeDetailedLog("❌ 发现几何无效要素，停止桥梁位置检查！");
        generateGeometryErrorReport(geometryErrors, checkTypeStr, tempStream);
        cleanupLayers();
        return;
    }

    this->writeDetailedLog("✅ 几何有效性检查通过");

    // === 执行关系检查 ===
    this->writeDetailedLog("开始桥梁位置关系检查...");
    QJsonArray relationErrorsJson;
    QList<QgsFeature> relationErrors = checkBridgePosition(tempStream, relationErrorsJson);

    bool hasRelationErrors = !relationErrors.isEmpty();

    // === 生成JSON报告 ===
    QJsonObject allErrors;
    int errorCount = 0;
    for (const QJsonValue& error : relationErrorsJson)
    {
        QJsonObject errorObj = error.toObject();
        QString errorDesc = errorObj["description"].toString();
        allErrors[QString("error%1").arg(++errorCount)] = errorDesc;
    }

    QJsonObject checkItem;
    checkItem["id"] = "113";
    checkItem["name"] = relationCheckItemName;
    checkItem["status"] = "质检成功";
    checkItem["err_exist"] = hasRelationErrors;
    checkItem["info"] = hasRelationErrors ? QString(u8"%1存在桥梁位置检查错误").arg(layerName) : QString(u8"%1桥梁位置检查通过").arg(layerName);

    if (hasRelationErrors)
    {
        checkItem["errors"] = allErrors;
    }

    QJsonObject jsonLog;
    jsonLog["check_item"] = checkItem;

    // === 使用 JsonLogger 生成JSON文件 ===
    QString jsonPath = m_outputJsonPath; // 直接使用成员变量

    JsonLogger jsonLogger;
    if (jsonLogger.saveJsonToFile(jsonLog, jsonPath))
    {
        this->writeDetailedLog(QString("✓ JSON报告已生成: %1").arg(jsonPath));
    }
    else
    {
        this->writeDetailedLog(QString("❌ JSON报告生成失败: %1").arg(jsonPath));
    }

    // [移除] 不再需要设置生成的JSON路径
    // setGeneratedJsonPath(jsonPath);

    // === 保存关系检查错误要素 ===
    if (!relationErrors.isEmpty())
    {
        saveErrorFeaturesByCheckType(relationErrors, checkTypeStr, false);
        this->writeDetailedLog("✓ 错误要素已保存");
    }

    // === 完成日志 ===
    this->writeDetailedLog("桥梁位置检查完成");
    this->writeDetailedLog(QString("桥梁要素数: %1").arg(m_bridgeLayers.isEmpty() ? 0 : m_bridgeLayers.first()->featureCount()));
    this->writeDetailedLog(QString("发现关系错误: %1 个").arg(relationErrors.size()));
    this->writeDetailedLog("=== 检测结束 ===");

    cleanupLayers();
}
// 河流检查
void ContainmentCheckDialog::runRiverContainmentCheck()
{
    QString checkTypeStr = u8"河流包含检查";
    // 使用统一的日志系统
    QString layerName = checkTypeStr;
    if (m_parsedPointLayer)
    {
        layerName = getLayerNameFromPath(m_parsedPointLayer->source());
    }

    this->writeDetailedLog("=== 开始河流面包含检查 ===");
    this->writeDetailedLog(QString("启动时间: %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")));
    this->writeDetailedLog(QString("检测参数: 比例尺=1:%1, XML配置=%2").arg(m_scale).arg(m_xmlConfigPath));
    this->writeDetailedLog(QString("输出日志: %1").arg(m_outputLogPath));
    this->writeDetailedLog(QString("错误要素基础路径: %1").arg(m_outputErrorPath));

    // === XML配置验证 ===
    if (m_xmlConfigPath.isEmpty())
    {
        this->writeDetailedLog("❌ 未提供XML配置路径");
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }

    XmlConfigReader configReader;
    if (!configReader.loadConfig(m_xmlConfigPath))
    {
        this->writeDetailedLog(QString("❌ 无法加载XML配置: %1").arg(m_xmlConfigPath));
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }
    this->writeDetailedLog("✓ XML配置加载成功");

    // === 比例尺和检查项验证 ===
    QString relationCheckItemName = QString::fromUtf8(u8"关系检查");
    if (!configReader.hasScale(m_scale) || !configReader.hasCheckItem(m_scale, relationCheckItemName))
    {
        this->writeDetailedLog("❌ 比例尺或检查项不匹配");
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }

    QString checkType = configReader.getRuleStringValue(m_scale, relationCheckItemName, QString::fromUtf8(u8"具体项"), "");
    if (checkType != checkTypeStr)
    {
        this->writeDetailedLog(QString("❌ 检查类型不匹配: %1, 需要: %2").arg(checkType).arg(checkTypeStr));
        this->writeDetailedLog("=== 检测中止 ===");
        return;
    }
    this->writeDetailedLog(QString("✓ 配置检查通过"));

    // === 数据加载 ===
    this->writeDetailedLog("开始加载数据图层...");

    int loadedCount = 0;
    if (!m_parsedPointPath.isEmpty())
    {
        m_parsedPointLayer = loadShapefile(m_parsedPointPath);
        if (m_parsedPointLayer && m_parsedPointLayer->isValid())
        {
            this->writeDetailedLog(QString("✓ 加载解析点: %1, 要素数: %2")
                .arg(m_parsedPointPath)
                .arg(m_parsedPointLayer->featureCount()));
            loadedCount++;
            layerName = getLayerNameFromPath(m_parsedPointPath);
        }
        else
        {
            this->writeDetailedLog(QString("❌ 加载解析点失败: %1").arg(m_parsedPointPath));
        }
    }

    if (!m_riverPolygonPath.isEmpty())
    {
        m_riverPolygonLayer = loadShapefile(m_riverPolygonPath);
        if (m_riverPolygonLayer && m_riverPolygonLayer->isValid())
        {
            this->writeDetailedLog(QString("✓ 加载河流面: %1, 要素数: %2")
                .arg(m_riverPolygonPath)
                .arg(m_riverPolygonLayer->featureCount()));
            loadedCount++;
        }
        else
        {
            this->writeDetailedLog(QString("❌ 加载河流面失败: %1").arg(m_riverPolygonPath));
        }
    }

    if (loadedCount < 2)
    {
        this->writeDetailedLog(QString("❌ 数据加载不完整，成功加载 %1/2 个必要图层").arg(loadedCount));
        this->writeDetailedLog("=== 检测中止 ===");
        cleanupLayers();
        return;
    }

    // === 几何有效性检查 ===
    this->writeDetailedLog("开始几何有效性检查...");
    QList<QgsVectorLayer*> checkLayers;
    if (m_parsedPointLayer)
        checkLayers.append(m_parsedPointLayer);
    if (m_riverPolygonLayer)
        checkLayers.append(m_riverPolygonLayer);

    // 创建临时文本流用于几何验证
    QString tempLogContent;
    QTextStream tempStream(&tempLogContent);
    tempStream.setCodec("UTF-8");

    QMap<QString, QList<QgsFeature>> geometryErrors = performGeometryValidation(checkTypeStr, checkLayers, tempStream);

    if (hasGeometryErrors(geometryErrors))
    {
        this->writeDetailedLog("❌ 发现几何无效要素，停止河流面包含检查！");
        generateGeometryErrorReport(geometryErrors, checkTypeStr, tempStream);
        cleanupLayers();
        return;
    }

    this->writeDetailedLog("✅ 几何有效性检查通过");

    // === 执行关系检查 ===
    this->writeDetailedLog("开始河流面包含关系检查...");
    QJsonArray relationErrorsJson;
    QList<QgsFeature> relationErrors = checkRiverContainment(tempStream, relationErrorsJson);

    bool hasRelationErrors = !relationErrors.isEmpty();

    // === 生成JSON报告 ===
    QJsonObject allErrors;
    int errorCount = 0;
    for (const QJsonValue& error : relationErrorsJson)
    {
        QJsonObject errorObj = error.toObject();
        QString errorDesc = errorObj["description"].toString();
        allErrors[QString("error%1").arg(++errorCount)] = errorDesc;
    }

    QJsonObject checkItem;
    checkItem["id"] = "113";
    checkItem["name"] = relationCheckItemName;
    checkItem["status"] = "质检成功";
    checkItem["err_exist"] = hasRelationErrors;
    checkItem["info"] = hasRelationErrors ? QString(u8"%1存在河流面包含检查错误").arg(layerName) : QString(u8"%1河流面包含检查通过").arg(layerName);

    if (hasRelationErrors)
    {
        checkItem["errors"] = allErrors;
    }

    QJsonObject jsonLog;
    jsonLog["check_item"] = checkItem;

    // === 使用 JsonLogger 生成JSON文件 ===
    QString jsonPath = m_outputJsonPath; // 直接使用成员变量

    JsonLogger jsonLogger;
    if (jsonLogger.saveJsonToFile(jsonLog, jsonPath))
    {
        this->writeDetailedLog(QString("✓ JSON报告已生成: %1").arg(jsonPath));
    }
    else
    {
        this->writeDetailedLog(QString("❌ JSON报告生成失败: %1").arg(jsonPath));
    }

    // [移除] 不再需要设置生成的JSON路径
    // setGeneratedJsonPath(jsonPath);

    // === 保存关系检查错误要素 ===
    if (!relationErrors.isEmpty())
    {
        saveErrorFeaturesByCheckType(relationErrors, checkTypeStr, false);
        this->writeDetailedLog("✓ 错误要素已保存");
    }

    // === 完成日志 ===
    this->writeDetailedLog("河流面包含检查完成");
    this->writeDetailedLog(QString("解析点要素数: %1").arg(m_parsedPointLayer->featureCount()));
    this->writeDetailedLog(QString("发现关系错误: %1 个").arg(relationErrors.size()));
    this->writeDetailedLog("=== 检测结束 ===");

    cleanupLayers();
}
// 清理图层
void ContainmentCheckDialog::cleanupLayers()
{
    // 清理单个图层指针
    delete m_basePolygonLayer;
    m_basePolygonLayer = nullptr;
    delete m_basePointLayer;
    m_basePointLayer = nullptr;
    delete m_parsedPolygonLayer;
    m_parsedPolygonLayer = nullptr;
    delete m_parsedPointLayer;
    m_parsedPointLayer = nullptr;
    delete m_riverPolygonLayer;
    m_riverPolygonLayer = nullptr;

    // 清理图层列表
    auto cleanupLayerList = [](QList<QgsVectorLayer*>& layerList)
    {
        for (QgsVectorLayer* layer : layerList)
        {
            delete layer;
        }
        layerList.clear();
    };

    cleanupLayerList(m_ditchFlowLayers);
    cleanupLayerList(m_ditchLayers);
    cleanupLayerList(m_bridgeLayers);
    cleanupLayerList(m_roadLayers);
    cleanupLayerList(m_tunnelEntranceLayers);
    cleanupLayerList(m_tunnelLayers);
    cleanupLayerList(m_roadLayersForTunnel);
}
// 进度
void ContainmentCheckDialog::resetProgressForCheck(const QString& checkName, int totalSteps)
{
    Q_UNUSED(checkName)
        Q_UNUSED(totalSteps)
}
// 记录要素信息到日志
void ContainmentCheckDialog::logFeatureInfo(QTextStream& stream, const QgsFeature& feature, const QString& message)
{
    QgsGeometry geom = feature.geometry();
    QString geomInfo;

    if (!geom.isNull())
    {
        if (geom.type() == QgsWkbTypes::PointGeometry)
        {
            QgsPointXY point = geom.asPoint();
            geomInfo = QString(u8"位置: (%1, %2)").arg(point.x()).arg(point.y());
        }
        else if (geom.type() == QgsWkbTypes::LineGeometry)
        {
            geomInfo = u8"线要素（点符号打散）";
        }
        else if (geom.type() == QgsWkbTypes::PolygonGeometry)
        {
            geomInfo = u8"面要素";
        }
    }

    QString logLine = QString("%1 - %2: %3 - %4 - %5\n")
        .arg(QDateTime::currentDateTime().toString(u8"yyyy-MM-dd hh:mm:ss"))
        .arg(u8"要素ID")
        .arg(feature.id())
        .arg(geomInfo)
        .arg(message);
    stream << logLine.toUtf8();
}
// 记录进度信息到日志
void ContainmentCheckDialog::logProgress(QTextStream& stream, int current, int total, const QString& stage)
{
    int percentage = (total > 0) ? (current * 100) / total : 0;
    QString logLine = QString("%1 - %2: %3 - %4: %5/%6 (%7%)\n")
        .arg(QDateTime::currentDateTime().toString(u8"yyyy-MM-dd hh:mm:ss"))
        .arg(u8"阶段")
        .arg(stage)
        .arg(u8"进度")
        .arg(current)
        .arg(total)
        .arg(percentage);
    stream << logLine.toUtf8();
}
// 保存错误要素
bool ContainmentCheckDialog::saveLayerErrorFeatures(const QString& layerName, const QList<QgsFeature>& features, const QString& outputPath)
{
    if (features.isEmpty())
    {
        return true;
    }

    // 检查文件是否已存在，如果存在，则删除重新创建
    QString baseName = QFileInfo(outputPath).baseName();
    QString baseDir = QFileInfo(outputPath).path() + "/";

    QStringList shpFiles;
    shpFiles << outputPath
        << baseDir + baseName + ".dbf"
        << baseDir + baseName + ".shx"
        << baseDir + baseName + ".prj"
        << baseDir + baseName + ".cpg";

    for (const QString& file : shpFiles)
    {
        if (QFile::exists(file))
        {
            QFile::remove(file);
        }
    }

    // 确保输出目录存在
    QFileInfo outputFileInfo(outputPath);
    QDir outputDir = outputFileInfo.dir();
    if (!outputDir.exists() && !outputDir.mkpath("."))
    {
        return false;
    }

    // === 关键修改：动态确定坐标系 ===
    QgsCoordinateReferenceSystem crs;
    QgsWkbTypes::Type geometryType = QgsWkbTypes::Point;

    // 根据图层名称和内容确定坐标系和几何类型
    if (!features.isEmpty())
    {
        QgsGeometry firstGeom = features.first().geometry();
        if (!firstGeom.isNull())
        {
            geometryType = firstGeom.wkbType();
        }
    }

    // 根据图层名称找到对应的原始图层获取坐标系
    bool crsFound = false;

    // 方法1: 通过图层名称匹配
    QgsVectorLayer* sourceLayer = findLayerByName(layerName);
    if (sourceLayer && sourceLayer->isValid())
    {
        crs = sourceLayer->crs();
        crsFound = true;
        writeDetailedLog(QString("通过图层名称匹配找到坐标系: %1 -> %2").arg(layerName).arg(crs.authid()));
    }

    // 方法2: 通过关键字匹配（备用方案）
    if (!crsFound)
    {
        if (layerName.contains(u8"桥梁") && !m_bridgeLayers.isEmpty() && m_bridgeLayers.first())
        {
            crs = m_bridgeLayers.first()->crs();
            crsFound = true;
        }
        else if (layerName.contains(u8"隧道口") && !m_tunnelEntranceLayers.isEmpty() && m_tunnelEntranceLayers.first())
        {
            crs = m_tunnelEntranceLayers.first()->crs();
            crsFound = true;
        }
        else if (layerName.contains(u8"渠流向") && !m_ditchFlowLayers.isEmpty() && m_ditchFlowLayers.first())
        {
            crs = m_ditchFlowLayers.first()->crs();
            crsFound = true;
        }
        else if (layerName.contains(u8"解析点") && m_parsedPointLayer)
        {
            crs = m_parsedPointLayer->crs();
            crsFound = true;
        }
        else if (layerName.contains(u8"库体面") && m_basePolygonLayer)
        {
            crs = m_basePolygonLayer->crs();
            crsFound = true;
        }
        else if (layerName.contains(u8"库体点") && m_basePointLayer)
        {
            crs = m_basePointLayer->crs();
            crsFound = true;
        }
        else if (layerName.contains(u8"解析面") && m_parsedPolygonLayer)
        {
            crs = m_parsedPolygonLayer->crs();
            crsFound = true;
        }
        else if (layerName.contains(u8"河流面") && m_riverPolygonLayer)
        {
            crs = m_riverPolygonLayer->crs();
            crsFound = true;
        }
        else if (layerName.contains(u8"沟渠") && !m_ditchLayers.isEmpty() && m_ditchLayers.first())
        {
            crs = m_ditchLayers.first()->crs();
            crsFound = true;
        }
        else if (layerName.contains(u8"道路") && !m_roadLayers.isEmpty() && m_roadLayers.first())
        {
            crs = m_roadLayers.first()->crs();
            crsFound = true;
        }
        else if (layerName.contains(u8"隧道") && !m_tunnelLayers.isEmpty() && m_tunnelLayers.first())
        {
            crs = m_tunnelLayers.first()->crs();
            crsFound = true;
        }

        if (crsFound)
        {
            writeDetailedLog(QString("通过关键字匹配找到坐标系: %1 -> %2").arg(layerName).arg(crs.authid()));
        }
    }

    // 方法3: 回退方案
    if (!crsFound)
    {
        // 使用第一个有效图层的坐标系
        if (m_parsedPointLayer)
        {
            crs = m_parsedPointLayer->crs();
        }
        else if (!m_bridgeLayers.isEmpty() && m_bridgeLayers.first())
        {
            crs = m_bridgeLayers.first()->crs();
        }
        else if (m_basePointLayer)
        {
            crs = m_basePointLayer->crs();
        }

        if (crs.isValid())
        {
            writeDetailedLog(QString("使用回退坐标系: %1 -> %2").arg(layerName).arg(crs.authid()));
        }
        else
        {
            writeDetailedLog(QString("警告: 无法为图层 '%1' 确定坐标系").arg(layerName));
        }
    }

    // 创建字段定义
    QgsFields fields;
    fields.append(QgsField("fid", QVariant::Int, "", 10));
    fields.append(QgsField("layer_name", QVariant::String, "", 50));
    fields.append(QgsField("ERROR_TYPE", QVariant::String, "", 50));
    fields.append(QgsField("BASE_NAME", QVariant::String, "", 100));
    fields.append(QgsField("PARSED_NAME", QVariant::String, "", 100));
    fields.append(QgsField("DESCRIPTION", QVariant::String, "", 255));

    // 添加原始字段（从第一个要素复制）
    if (!features.isEmpty())
    {
        const QgsFields originalFields = features.first().fields();
        for (int i = 0; i < originalFields.count(); i++)
        {
            if (fields.indexFromName(originalFields.at(i).name()) == -1)
            {
                fields.append(originalFields.at(i));
            }
        }
    }

    // 创建写入器
    QgsVectorFileWriter::SaveVectorOptions options;
    options.driverName = "ESRI Shapefile";
    options.fileEncoding = "UTF-8";

    std::unique_ptr<QgsVectorFileWriter> writer(
        QgsVectorFileWriter::create(
            outputPath,
            fields,
            geometryType,
            crs,
            QgsCoordinateTransformContext(),
            options));

    if (!writer || writer->hasError() != QgsVectorFileWriter::NoError)
    {
        writeDetailedLog(QString("❌ 创建Shapefile写入器失败: %1").arg(writer ? writer->errorMessage() : "未知错误"));
        return false;
    }

    // 写入要素
    int featureId = 1;
    for (const QgsFeature& feature : features)
    {
        QgsFeature outputFeature(fields);

        // 保留原始几何
        outputFeature.setGeometry(feature.geometry());

        // 设置基本字段
        outputFeature.setAttribute("fid", featureId++);
        outputFeature.setAttribute("layer_name", layerName);

        // 确保ERROR_TYPE字段有正确的值
        QString errorType = feature.attribute("ERROR_TYPE").toString();
        if (errorType.isEmpty() || errorType == "NULL")
        {
            // 如果字段值为空，根据描述推断
            QString description = feature.attribute("DESCRIPTION").toString();
            if (description.contains(u8"几何为空"))
                errorType = u8"几何无效-空几何";
            else if (description.contains(u8"几何无效"))
                errorType = u8"几何无效-自相交或其他问题";
            else if (description.contains(u8"名称不匹配"))
                errorType = u8"行政面包含错误";
            else if (description.contains(u8"无库体点"))
                errorType = u8"无法匹配库体点";
            else if (description.contains(u8"河流面内"))
                errorType = u8"河流面包含错误";
            else if (description.contains(u8"渠流向位置错误"))
                errorType = u8"渠流向位置错误";
            else if (description.contains(u8"桥梁位置错误"))
                errorType = u8"桥梁位置错误";
            else if (description.contains(u8"隧道口位置错误"))
                errorType = u8"隧道口位置错误";
            else
                errorType = u8"未知错误";
        }

        outputFeature.setAttribute("ERROR_TYPE", errorType);
        outputFeature.setAttribute("BASE_NAME", feature.attribute("BASE_NAME").toString());
        outputFeature.setAttribute("PARSED_NAME", feature.attribute("PARSED_NAME").toString());
        outputFeature.setAttribute("DESCRIPTION", feature.attribute("DESCRIPTION").toString());

        // 复制原始字段
        const QgsFields originalFields = feature.fields();
        for (int i = 0; i < originalFields.count(); i++)
        {
            QString fieldName = originalFields.at(i).name();
            int fieldIndex = fields.indexFromName(fieldName);
            if (fieldIndex >= 0)
            {
                outputFeature.setAttribute(fieldName, feature.attribute(fieldName));
            }
        }

        if (!writer->addFeature(outputFeature))
        {
            writeDetailedLog(QString("❌ 写入要素失败: 要素ID=%1").arg(feature.id()));
            return false;
        }
    }

    writeDetailedLog(QString("✓ 成功保存错误要素: %1, 要素数: %2, 坐标系: %3")
        .arg(outputPath)
        .arg(features.size())
        .arg(crs.isValid() ? crs.authid() : "未知"));

    return true;
}
//通过名字寻找图层
QgsVectorLayer* ContainmentCheckDialog::findLayerByName(const QString& layerName)
{
    if (layerName.isEmpty())
    {
        return nullptr;
    }

    // 检查所有图层列表，找到名称匹配的图层
    QList<QgsVectorLayer*> allLayers;

    // 单个图层
    if (m_basePolygonLayer && getLayerNameFromPath(m_basePolygonLayer->source()) == layerName)
        return m_basePolygonLayer;
    if (m_basePointLayer && getLayerNameFromPath(m_basePointLayer->source()) == layerName)
        return m_basePointLayer;
    if (m_parsedPolygonLayer && getLayerNameFromPath(m_parsedPolygonLayer->source()) == layerName)
        return m_parsedPolygonLayer;
    if (m_parsedPointLayer && getLayerNameFromPath(m_parsedPointLayer->source()) == layerName)
        return m_parsedPointLayer;
    if (m_riverPolygonLayer && getLayerNameFromPath(m_riverPolygonLayer->source()) == layerName)
        return m_riverPolygonLayer;

    // 图层列表
    for (QgsVectorLayer* layer : m_ditchFlowLayers)
    {
        if (layer && getLayerNameFromPath(layer->source()) == layerName)
            return layer;
    }
    for (QgsVectorLayer* layer : m_ditchLayers)
    {
        if (layer && getLayerNameFromPath(layer->source()) == layerName)
            return layer;
    }
    for (QgsVectorLayer* layer : m_bridgeLayers)
    {
        if (layer && getLayerNameFromPath(layer->source()) == layerName)
            return layer;
    }
    for (QgsVectorLayer* layer : m_roadLayers)
    {
        if (layer && getLayerNameFromPath(layer->source()) == layerName)
            return layer;
    }
    for (QgsVectorLayer* layer : m_tunnelEntranceLayers)
    {
        if (layer && getLayerNameFromPath(layer->source()) == layerName)
            return layer;
    }
    for (QgsVectorLayer* layer : m_tunnelLayers)
    {
        if (layer && getLayerNameFromPath(layer->source()) == layerName)
            return layer;
    }
    for (QgsVectorLayer* layer : m_roadLayersForTunnel)
    {
        if (layer && getLayerNameFromPath(layer->source()) == layerName)
            return layer;
    }

    return nullptr;
}
// 检查是否有几何错误
bool ContainmentCheckDialog::hasGeometryErrors(const QMap<QString, QList<QgsFeature>>& geometryErrors)
{
    for (auto it = geometryErrors.begin(); it != geometryErrors.end(); ++it)
    {
        if (!it.value().isEmpty())
        {
            return true;
        }
    }
    return false;
}
// 获取有效要素
QList<QgsFeature> ContainmentCheckDialog::getValidFeaturesFromLayer(QgsVectorLayer* layer, const QMap<QString, QList<QgsFeature>>& geometryErrors)
{
    if (!layer)
        return QList<QgsFeature>();

    QString layerName = QFileInfo(layer->source()).baseName();
    QList<QgsFeature> invalidFeatures = geometryErrors.value(layerName);

    return m_geometryValidator->getValidFeatures(layer, invalidFeatures);
}
// 加载Shapefile图层
QgsVectorLayer* ContainmentCheckDialog::loadShapefile(const QString& filePath)
{
    QgsVectorLayer* layer = new QgsVectorLayer(filePath, QFileInfo(filePath).baseName(), "ogr");
    if (!layer->isValid())
    {
        delete layer;
        return nullptr;
    }

    return layer;
}
//加载多种图层
QList<QgsVectorLayer*> ContainmentCheckDialog::loadMultipleShapefiles(const QStringList& filePaths)
{
    QList<QgsVectorLayer*> layers;

    for (const QString& filePath : filePaths)
    {
        QString trimmedPath = filePath.trimmed();
        if (!trimmedPath.isEmpty())
        {
            QgsVectorLayer* layer = loadShapefile(trimmedPath);
            if (layer && layer->isValid())
            {
                layers.append(layer);
            }
        }
    }

    return layers;
}
// 查找最近点要素
QgsFeature ContainmentCheckDialog::findNearestPoint(const QgsGeometry& geometry, QgsVectorLayer* layer)
{
    QList<QgsFeatureId> candidateIds;

    if (layer == m_basePointLayer)
    {

        QgsFeatureIterator features = layer->getFeatures();
        QgsFeature feature;
        while (features.nextFeature(feature))
        {
            candidateIds.append(feature.id());
        }
    }
    else
    {
        QgsFeatureIterator features = layer->getFeatures();
        QgsFeature feature;
        while (features.nextFeature(feature))
        {
            candidateIds.append(feature.id());
        }
    }

    QgsFeature nearestFeature;
    double minDistance = std::numeric_limits<double>::max();

    for (QgsFeatureId id : candidateIds)
    {
        QgsFeature pointFeature;
        if (layer->getFeatures(QgsFeatureRequest().setFilterFid(id)).nextFeature(pointFeature))
        {
            double distance = geometry.distance(pointFeature.geometry());
            if (distance < minDistance)
            {
                minDistance = distance;
                nearestFeature = pointFeature;
            }
        }
    }

    return nearestFeature;
}
// 行政面
QList<QgsFeature> ContainmentCheckDialog::checkAdministrativeContainment(QTextStream& logStream, QJsonArray& jsonErrors)
{
    QList<QgsFeature> errorFeatures;

    if (!m_parsedPointLayer || !m_basePointLayer || !m_parsedPolygonLayer || !m_basePolygonLayer)
    {
        logStream << QString(u8"警告: 必要的图层数据为空，跳过行政面包含检查\n").toUtf8();
        return errorFeatures;
    }

    // === 步骤1: 几何有效性检查 ===
    QList<QgsVectorLayer*> checkLayers;
    checkLayers.append(m_parsedPointLayer);
    checkLayers.append(m_basePointLayer);
    checkLayers.append(m_parsedPolygonLayer);
    checkLayers.append(m_basePolygonLayer);

    QMap<QString, QList<QgsFeature>> geometryErrors = performGeometryValidation(u8"行政面包含检查", checkLayers, logStream);

    // === 步骤2: 分组有效解析点 ===
    QList<QgsFeature> validParsedPoints = getValidFeaturesFromLayer(m_parsedPointLayer, geometryErrors);

    QMap<QVariant, QList<QgsFeature>> featureGroups;
    int symbolIdIdx = m_parsedPointLayer->fields().indexFromName("symbol_id");

    for (const QgsFeature& feat : validParsedPoints)
    {
        QVariant symbolId;
        if (symbolIdIdx != -1)
        {
            symbolId = feat.attribute(symbolIdIdx);
            if (symbolId.isNull() || symbolId.toString().isEmpty())
            {
                symbolId = QString(u8"空ID_%1").arg(feat.id());
            }
        }
        else
        {
            symbolId = feat.id();
        }
        featureGroups[symbolId].append(feat);
    }

    logStream << QString(u8"有效解析点分组完成：共%1个分组\n").arg(featureGroups.size()).toUtf8();

    resetProgressForCheck(u8"行政面包含检查", featureGroups.size());

    // === 步骤3: 只对在解析面内的要素进行检查 ===
    int totalGroups = featureGroups.size();
    int processedGroups = 0;
    int groupErrorCount = 0;
    int notInParsedPolygonGroups = 0;
    int inParsedPolygonGroups = 0;

    // 获取有效的参考要素
    QList<QgsFeature> validBasePoints = getValidFeaturesFromLayer(m_basePointLayer, geometryErrors);
    QList<QgsFeature> validParsedPolygons = getValidFeaturesFromLayer(m_parsedPolygonLayer, geometryErrors);
    QList<QgsFeature> validBasePolygons = getValidFeaturesFromLayer(m_basePolygonLayer, geometryErrors);

    // 获取解析点图层名
    QString parsedPointLayerName;
    if (m_parsedPointLayer)
    {
        parsedPointLayerName = QFileInfo(m_parsedPointLayer->source()).baseName();
    }
    else
    {
        parsedPointLayerName = u8"解析点";
    }

    QgsSpatialIndex localBasePolygonIndex;
    QHash<QgsFeatureId, QString> localBasePolygonNameMap;

    for (const QgsFeature& feat : validBasePolygons)
    {
        QgsFeature nonConstFeature = feat;
        localBasePolygonIndex.addFeature(nonConstFeature);
        localBasePolygonNameMap.insert(feat.id(), feat.attribute("NAME").toString());
    }

    QgsSpatialIndex localParsedPolygonIndex;
    QHash<QgsFeatureId, QString> localParsedPolygonNameMap;

    for (const QgsFeature& feat : validParsedPolygons)
    {
        QgsFeature nonConstFeature = feat;
        localParsedPolygonIndex.addFeature(nonConstFeature);
        // 匹配解析面与库体面
        QgsGeometry parsedGeometry = feat.geometry();
        double parsedArea = parsedGeometry.area();
        QString bestMatchName;
        double maxOverlapRatio = 0;

        QList<QgsFeatureId> candidateIds = localBasePolygonIndex.intersects(parsedGeometry.boundingBox());
        for (QgsFeatureId id : candidateIds)
        {
            QgsFeature basePolygonFeature;
            if (m_basePolygonLayer->getFeatures(QgsFeatureRequest().setFilterFid(id)).nextFeature(basePolygonFeature))
            {
                QgsGeometry baseGeometry = basePolygonFeature.geometry();
                if (baseGeometry.isNull())
                    continue;

                QgsGeometry intersection = parsedGeometry.intersection(baseGeometry);
                if (intersection.isNull())
                    continue;

                double intersectionArea = intersection.area();
                double baseArea = baseGeometry.area();

                double overlapRatio1 = intersectionArea / parsedArea;
                double overlapRatio2 = intersectionArea / baseArea;
                double overlapRatio = qMax(overlapRatio1, overlapRatio2);

                if (overlapRatio > 0.90 && overlapRatio > maxOverlapRatio)
                {
                    maxOverlapRatio = overlapRatio;
                    bestMatchName = localBasePolygonNameMap.value(id);
                }
            }
        }

        if (!bestMatchName.isEmpty())
        {
            localParsedPolygonNameMap.insert(feat.id(), bestMatchName);
        }
    }

    // 构建库体点名称映射
    QHash<QgsFeatureId, QString> localBasePointNameMap;
    for (const QgsFeature& feat : validBasePoints)
    {
        QgsGeometry pointGeometry = feat.geometry();
        QString adminName;

        QList<QgsFeatureId> candidateIds = localBasePolygonIndex.intersects(pointGeometry.boundingBox());
        for (QgsFeatureId id : candidateIds)
        {
            QgsFeature polygonFeature;
            if (m_basePolygonLayer->getFeatures(QgsFeatureRequest().setFilterFid(id)).nextFeature(polygonFeature))
            {
                if (polygonFeature.geometry().contains(pointGeometry))
                {
                    adminName = localBasePolygonNameMap.value(id);
                    break;
                }
            }
        }
        localBasePointNameMap.insert(feat.id(), adminName);
    }

    // 对每个分组进行关系检查
    for (auto groupIt = featureGroups.begin(); groupIt != featureGroups.end(); ++groupIt)
    {
        processedGroups++;
        QVariant symbolId = groupIt.key();
        QList<QgsFeature> groupFeatures = groupIt.value();

        QgsFeature representativeFeat = groupFeatures.first();
        QgsGeometry representativeGeom = getCentroidFromGeometry(representativeFeat.geometry());

        // 检查是否在解析面内
        bool isInParsedPolygon = false;
        QString parsedAdminName;
        QList<QgsFeatureId> candidateIds = localParsedPolygonIndex.intersects(representativeGeom.boundingBox());

        for (QgsFeatureId id : candidateIds)
        {
            QgsFeature parsedPolygonFeature;
            if (m_parsedPolygonLayer->getFeatures(QgsFeatureRequest().setFilterFid(id)).nextFeature(parsedPolygonFeature))
            {
                if (parsedPolygonFeature.geometry().contains(representativeGeom))
                {
                    isInParsedPolygon = true;
                    parsedAdminName = localParsedPolygonNameMap.value(id);
                    break;
                }
            }
        }

        QString errorType, desc;

        if (!isInParsedPolygon)
        {
            // === 修复：不在解析面内的要素，不记录为错误，只记录日志 ===
            notInParsedPolygonGroups++;

            // 只记录信息日志，不添加到错误要素
            desc = QString(u8"symbol_id=%1的要素未落在任何解析面范围内，跳过包含关系检查").arg(symbolId.toString());
            logStream << QString(u8"信息: %1\n").arg(desc).toUtf8();

            // 重要：不调用 addErrorFeature，不添加到 errorFeatures
        }
        else
        {
            // 在解析面内的情况，进行包含关系检查
            inParsedPolygonGroups++;

            QgsFeature nearestBasePoint = findNearestPoint(representativeGeom, m_basePointLayer);
            if (nearestBasePoint.isValid())
            {
                QString baseAdminName = localBasePointNameMap.value(nearestBasePoint.id());
                if (baseAdminName != parsedAdminName)
                {
                    // 包含关系错误
                    groupErrorCount++;
                    errorType = u8"行政面包含错误";
                    QString baseName = baseAdminName.isEmpty() ? QString(u8"未知") : baseAdminName;
                    desc = QString(u8"图层[%1] symbol_id[%2]: 库体面(%3)与解析面(%4)名称不匹配")
                        .arg(parsedPointLayerName)
                        .arg(symbolId.toString())
                        .arg(baseName)
                        .arg(parsedAdminName);

                    logFeatureInfo(logStream, representativeFeat, QString(u8"错误 - %1").arg(desc));
                    jsonErrors.append(JsonLogger::createErrorObject(representativeFeat.id(), desc));

                    for (const QgsFeature& feat : groupFeatures)
                    {
                        addErrorFeature(errorFeatures, feat, errorType, desc, parsedPointLayerName);
                    }
                }
                else
                {
                    // 名称匹配，正常情况
                    logStream << QString(u8"分组 symbol_id[%1]: 行政面包含检查通过\n").arg(symbolId.toString()).toUtf8();
                }
            }
            else
            {
                // 无法匹配库体点
                groupErrorCount++;
                errorType = u8"无法匹配库体点";
                desc = QString(u8"图层[%1] symbol_id[%2]: 解析点位于解析面(%3)，但100米内无有效库体点")
                    .arg(parsedPointLayerName)
                    .arg(symbolId.toString())
                    .arg(parsedAdminName);

                logFeatureInfo(logStream, representativeFeat, QString(u8"错误 - %1").arg(desc));
                jsonErrors.append(JsonLogger::createErrorObject(representativeFeat.id(), desc));

                for (const QgsFeature& feat : groupFeatures)
                {
                    addErrorFeature(errorFeatures, feat, errorType, desc, parsedPointLayerName);
                }
            }
        }

        // 更新进度
        if (processedGroups % 10 == 0)
        {
            logProgress(logStream, processedGroups, totalGroups, QString(u8"行政面包含检查"));
        }
        QApplication::processEvents();
    }

    // 更新统计日志
    logStream << (QString(u8"行政面包含检查分组统计: 总分组%1，在解析面内分组%2，不在解析面内分组%3，关系错误分组%4\n")
        .arg(totalGroups)
        .arg(inParsedPolygonGroups)
        .arg(notInParsedPolygonGroups)
        .arg(groupErrorCount))
        .toUtf8();

    return errorFeatures;
}
// 渠流向
QList<QgsFeature> ContainmentCheckDialog::checkDitchFlowPosition(QTextStream& logStream, QJsonArray& jsonErrors, double bufferDistance)
{
    QList<QgsFeature> errorFeatures;

    if (m_ditchFlowLayers.isEmpty() || m_ditchLayers.isEmpty())
    {
        logStream << QString(u8"警告: 渠流向数据或沟渠数据为空，跳过渠流向位置检查\n").toUtf8();
        return errorFeatures;
    }

    logStream << QString(u8"开始渠流向位置检查(缓冲区半径: %1米)...\n").arg(bufferDistance).toUtf8();

    // === 步骤1: 几何有效性检查 ===
    QList<QgsVectorLayer*> checkLayers;
    checkLayers.append(m_ditchFlowLayers);
    checkLayers.append(m_ditchLayers);

    QMap<QString, QList<QgsFeature>> geometryErrors = performGeometryValidation(u8"渠流向位置检查", checkLayers, logStream);

    // === 步骤2: 关系检查逻辑（使用有效要素）===
    int totalDitchFlowFeatures = 0;
    int errorCount = 0;
    int processedFeatures = 0;

    // 计算总要素数用于进度显示
    for (QgsVectorLayer* layer : m_ditchFlowLayers)
    {
        QList<QgsFeature> validFeatures = getValidFeaturesFromLayer(layer, geometryErrors);
        totalDitchFlowFeatures += validFeatures.size();
    }

    resetProgressForCheck(u8"渠流向位置检查", totalDitchFlowFeatures);

    // 获取有效的沟渠要素
    QList<QgsFeature> validDitchFeatures;
    for (QgsVectorLayer* ditchLayer : m_ditchLayers)
    {
        validDitchFeatures.append(getValidFeaturesFromLayer(ditchLayer, geometryErrors));
    }

    // 检查每个渠流向要素
    for (QgsVectorLayer* ditchFlowLayer : m_ditchFlowLayers)
    {
        QString layerName = QFileInfo(ditchFlowLayer->source()).baseName();
        QList<QgsFeature> validDitchFlowFeatures = getValidFeaturesFromLayer(ditchFlowLayer, geometryErrors);

        for (const QgsFeature& feature : validDitchFlowFeatures)
        {
            processedFeatures++;
            QgsGeometry geometry = feature.geometry();

            // 使用动态缓冲区距离
            QgsGeometry buffer = geometry.buffer(bufferDistance, 10);
            if (buffer.isNull())
            {
                // 缓冲区创建失败，记录错误
                QString errorType = u8"缓冲区创建失败";
                QString desc = QString(u8"图层[%1] 要素ID[%2]: 无法创建%3米缓冲区")
                    .arg(layerName)
                    .arg(feature.id())
                    .arg(bufferDistance);
                addErrorFeature(errorFeatures, feature, errorType, desc, layerName);
                continue;
            }

            // 检查缓冲区范围内是否存在沟渠要素
            bool foundDitch = false;
            for (const QgsFeature& ditchFeature : validDitchFeatures)
            {
                if (ditchFeature.geometry().intersects(buffer))
                {
                    foundDitch = true;
                    break;
                }
            }

            if (!foundDitch)
            {
                errorCount++;
                QString errorType = u8"渠流向位置错误";
                QString desc = QString(u8"图层[%1] 要素ID[%2]: %3米缓冲区内未找到沟渠要素")
                    .arg(layerName)
                    .arg(feature.id())
                    .arg(bufferDistance);

                logFeatureInfo(logStream, feature, QString(u8"错误 - %1").arg(desc));
                jsonErrors.append(JsonLogger::createErrorObject(feature.id(), desc));

                addErrorFeature(errorFeatures, feature, errorType, desc, layerName);
            }

            // 更新进度 - 移除UI进度条
            if (processedFeatures % 10 == 0)
            {
                logProgress(logStream, processedFeatures, totalDitchFlowFeatures, QString(u8"渠流向位置检查"));
                QApplication::processEvents();
            }
        }
    }

    logStream << QString(u8"渠流向位置检查完成: 共检查 %1 个要素，发现 %2 个错误\n")
        .arg(totalDitchFlowFeatures)
        .arg(errorCount)
        .toUtf8();

    return errorFeatures;
}
// 桥梁
QList<QgsFeature> ContainmentCheckDialog::checkBridgePosition(QTextStream& logStream, QJsonArray& jsonErrors)
{
    QList<QgsFeature> errorFeatures;

    // 简化检查条件：桥梁检查只需要桥梁、河流面、道路
    bool hasRequiredData = !m_bridgeLayers.isEmpty() &&
        m_riverPolygonLayer != nullptr &&
        !m_roadLayers.isEmpty();

    if (!hasRequiredData)
    {
        logStream << QString(u8"警告: 桥梁数据、河流面数据或道路数据为空，跳过桥梁位置检查\n").toUtf8();
        return errorFeatures;
    }

    logStream << QString(u8"开始桥梁位置检查...\n");

    // === 步骤1: 几何有效性检查 ===
    QList<QgsVectorLayer*> allCheckLayers;
    allCheckLayers.append(m_bridgeLayers);
    if (m_riverPolygonLayer)
        allCheckLayers.append(m_riverPolygonLayer);
    allCheckLayers.append(m_roadLayers);

    QMap<QString, QList<QgsFeature>> geometryErrors = performGeometryValidation(u8"桥梁位置检查", allCheckLayers, logStream);

    // === 步骤2: 只对几何有效的桥梁要素进行分组 ===
    QMap<QVariant, QList<QgsFeature>> validBridgeGroups;

    for (QgsVectorLayer* bridgeLayer : m_bridgeLayers)
    {
        // 只获取几何有效的要素
        QList<QgsFeature> validFeatures = getValidFeaturesFromLayer(bridgeLayer, geometryErrors);

        // 按 symbol_id 分组
        int symbolIdIdx = bridgeLayer->fields().indexFromName("symbol_id");
        for (const QgsFeature& feat : validFeatures)
        {
            QVariant symbolId;
            if (symbolIdIdx != -1)
            {
                symbolId = feat.attribute(symbolIdIdx);
                if (symbolId.isNull() || symbolId.toString().isEmpty())
                {
                    symbolId = QString(u8"空ID_%1").arg(feat.id());
                }
            }
            else
            {
                symbolId = feat.id();
            }
            validBridgeGroups[symbolId].append(feat);
        }
    }

    logStream << QString(u8"有效桥梁要素分组完成: 共 %1 个分组\n").arg(validBridgeGroups.size()).toUtf8();

    // === 步骤3: 对每个有效分组进行关系检查 ===
    resetProgressForCheck(u8"桥梁位置检查", validBridgeGroups.size());

    int errorCount = 0;
    int processedGroups = 0;
    int totalGroups = validBridgeGroups.size();

    // 获取有效的参考要素
    QList<QgsFeature> validRiverFeatures;
    if (m_riverPolygonLayer)
    {
        validRiverFeatures = getValidFeaturesFromLayer(m_riverPolygonLayer, geometryErrors);
    }

    QList<QgsFeature> validRoadFeatures;
    for (QgsVectorLayer* roadLayer : m_roadLayers)
    {
        validRoadFeatures.append(getValidFeaturesFromLayer(roadLayer, geometryErrors));
    }

    for (auto groupIt = validBridgeGroups.begin(); groupIt != validBridgeGroups.end(); ++groupIt)
    {
        processedGroups++;
        QVariant symbolId = groupIt.key();
        QList<QgsFeature> groupFeatures = groupIt.value();

        // 获取桥梁图层名
        QString bridgeLayerName;
        if (!m_bridgeLayers.isEmpty() && m_bridgeLayers.first())
        {
            bridgeLayerName = QFileInfo(m_bridgeLayers.first()->source()).baseName();
        }
        else
        {
            bridgeLayerName = u8"桥梁图层";
        }

        // 合并桥梁要素
        QgsGeometry mergedBridge = mergeBridgeFeatures(groupFeatures);

        // 检查合并后的几何有效性
        if (mergedBridge.isNull() || !mergedBridge.isGeosValid())
        {
            // 合并失败，记录为关系检查错误
            QString errorType = u8"桥梁合并失败";
            QString desc = QString(u8"symbol_id[%1]: 无法合并桥梁要素").arg(symbolId.toString());

            for (const QgsFeature& feat : groupFeatures)
            {
                addErrorFeature(errorFeatures, feat, errorType, desc, bridgeLayerName);
            }
            continue;
        }

        // 执行相交检查
        bool intersectsRiver = false;
        bool intersectsRoad = false;

        // 检查河流面相交
        for (const QgsFeature& riverFeat : validRiverFeatures)
        {
            if (mergedBridge.intersects(riverFeat.geometry()))
            {
                intersectsRiver = true;
                break;
            }
        }

        // 检查道路相交
        for (const QgsFeature& roadFeat : validRoadFeatures)
        {
            if (mergedBridge.intersects(roadFeat.geometry()))
            {
                intersectsRoad = true;
                break;
            }
        }

        // 判断关系检查结果
        if (!intersectsRiver || !intersectsRoad)
        {
            errorCount++;
            QString errorType = u8"桥梁位置错误";
            QString desc = QString(u8"symbol_id[%1]: ").arg(symbolId.toString());

            if (!intersectsRiver && !intersectsRoad)
            {
                desc += u8"未与河流面和道路相交";
            }
            else if (!intersectsRiver)
            {
                desc += u8"未与河流面相交";
            }
            else
            {
                desc += u8"未与道路相交";
            }
            // === 添加 JSON 错误记录 ===
            QgsFeature representativeFeat = groupFeatures.first();
            logFeatureInfo(logStream, representativeFeat, QString(u8"错误 - %1").arg(desc));
            jsonErrors.append(JsonLogger::createErrorObject(representativeFeat.id(), desc));

            // 记录到关系检查错误
            for (const QgsFeature& feat : groupFeatures)
            {
                addErrorFeature(errorFeatures, feat, errorType, desc, bridgeLayerName);
            }
        }

        // 更新进度
        if (processedGroups % 10 == 0)
        {
            logProgress(logStream, processedGroups, totalGroups, QString(u8"桥梁位置检查"));
            QApplication::processEvents();
        }
    }

    logStream << QString(u8"桥梁位置检查完成: 共检查 %1 个分组，发现 %2 个关系错误\n")
        .arg(totalGroups)
        .arg(errorCount)
        .toUtf8();

    return errorFeatures;
}
// 河流包含检查
QList<QgsFeature> ContainmentCheckDialog::checkRiverContainment(QTextStream& logStream, QJsonArray& jsonErrors)
{
    QList<QgsFeature> errorFeatures;

    if (!m_parsedPointLayer || !m_riverPolygonLayer)
    {
        logStream << QString(u8"警告: 解析点数据或河流面数据为空，跳过河流面包含检查\n").toUtf8();
        return errorFeatures;
    }

    // === 步骤1: 几何有效性检查 ===
    QList<QgsVectorLayer*> checkLayers;
    checkLayers.append(m_parsedPointLayer);
    checkLayers.append(m_riverPolygonLayer);

    QMap<QString, QList<QgsFeature>> geometryErrors = performGeometryValidation(u8"河流面包含检查", checkLayers, logStream);

    // === 步骤2: 只对几何有效的解析点进行分组 ===
    QList<QgsFeature> validParsedPoints = getValidFeaturesFromLayer(m_parsedPointLayer, geometryErrors);

    QMap<QVariant, QList<QgsFeature>> featureGroups;
    int symbolIdIdx = m_parsedPointLayer->fields().indexFromName("symbol_id");

    for (const QgsFeature& feat : validParsedPoints)
    {
        QVariant symbolId;
        if (symbolIdIdx != -1)
        {
            symbolId = feat.attribute(symbolIdIdx);
            if (symbolId.isNull() || symbolId.toString().isEmpty())
            {
                symbolId = QString(u8"空ID_%1").arg(feat.id());
            }
        }
        else
        {
            symbolId = feat.id();
        }
        featureGroups[symbolId].append(feat);
    }

    resetProgressForCheck(u8"河流面包含检查", featureGroups.size());

    // === 步骤3: 关系检查逻辑（使用有效要素）===
    int totalGroups = featureGroups.size();
    int processedGroups = 0;
    int groupErrorCount = 0;

    // 获取有效的河流面要素
    QList<QgsFeature> validRiverFeatures = getValidFeaturesFromLayer(m_riverPolygonLayer, geometryErrors);

    // 获取解析点图层名
    QString parsedPointLayerName;
    if (m_parsedPointLayer)
    {
        parsedPointLayerName = QFileInfo(m_parsedPointLayer->source()).baseName();
    }
    else
    {
        parsedPointLayerName = u8"解析点";
    }

    for (auto groupIt = featureGroups.begin(); groupIt != featureGroups.end(); ++groupIt)
    {
        processedGroups++;
        QVariant symbolId = groupIt.key();
        QList<QgsFeature> groupFeatures = groupIt.value();

        QgsFeature representativeFeat = groupFeatures.first();
        QgsGeometry representativeGeom = getCentroidFromGeometry(representativeFeat.geometry());

        // 只检查河流面包含关系
        bool inRiver = false;
        for (const QgsFeature& riverFeat : validRiverFeatures)
        {
            if (riverFeat.geometry().contains(representativeGeom))
            {
                inRiver = true;
                break;
            }
        }

        if (inRiver)
        {
            groupErrorCount++;
            QString errorType = u8"河流面包含错误";
            QString desc = QString(u8"图层[%1] symbol_id[%2]: 要素位于河流面内")
                .arg(parsedPointLayerName)
                .arg(symbolId.toString());

            logFeatureInfo(logStream, representativeFeat, QString(u8"错误 - %1").arg(desc));
            jsonErrors.append(JsonLogger::createErrorObject(representativeFeat.id(), desc));

            for (const QgsFeature& feat : groupFeatures)
            {
                addErrorFeature(errorFeatures, feat, errorType, desc, parsedPointLayerName);
            }
        }

        // 更新进度 - 移除UI进度条
        if (processedGroups % 10 == 0)
        {
            logProgress(logStream, processedGroups, totalGroups, QString(u8"河流面包含检查"));
        }
        QApplication::processEvents();
    }

    logStream << (QString(u8"河流面包含检查完成: 总分组%1，错误分组%2\n")
        .arg(totalGroups)
        .arg(groupErrorCount))
        .toUtf8();

    return errorFeatures;
}
// 隧道口检查
QList<QgsFeature> ContainmentCheckDialog::checkTunnelEntrancePosition(QTextStream& logStream, QJsonArray& jsonErrors, double bufferDistance)
{
    QList<QgsFeature> errorFeatures;

    // 简化检查条件
    bool hasData = !m_tunnelEntranceLayers.isEmpty() &&
        !m_tunnelLayers.isEmpty() &&
        !m_roadLayersForTunnel.isEmpty();

    if (!hasData)
    {
        logStream << QString(u8"跳过隧道口位置检查：缺少必要数据\n").toUtf8();
        return errorFeatures;
    }

    logStream << QString(u8"开始隧道口位置检查(%1米阈值)...\n").arg(bufferDistance).toUtf8();

    // === 几何有效性检查 ===
    QList<QgsVectorLayer*> allCheckLayers;
    allCheckLayers.append(m_tunnelEntranceLayers);
    allCheckLayers.append(m_tunnelLayers);
    allCheckLayers.append(m_roadLayersForTunnel);

    QMap<QString, QList<QgsFeature>> geometryErrors = performGeometryValidation(u8"隧道口位置检查", allCheckLayers, logStream);

    if (hasGeometryErrors(geometryErrors))
    {
        logStream << QString(u8"❌ 发现几何无效要素，停止隧道口位置检查！\n").toUtf8();
        generateGeometryErrorReport(geometryErrors, u8"隧道口位置检查", logStream);
        return errorFeatures;
    }

    // === 关系检查逻辑 ===
    int totalTunnelEntrances = 0;
    int errorCount = 0;
    int processedCount = 0;

    // 计算总要素数
    for (QgsVectorLayer* layer : m_tunnelEntranceLayers)
    {
        QList<QgsFeature> validFeatures = getValidFeaturesFromLayer(layer, geometryErrors);
        totalTunnelEntrances += validFeatures.size();
    }

    logStream << QString(u8"总计有效隧道口要素: %1 个\n").arg(totalTunnelEntrances).toUtf8();

    resetProgressForCheck(u8"隧道口位置检查", totalTunnelEntrances);

    // 获取有效的参考要素
    QList<QgsFeature> validTunnels, validRoads;

    for (QgsVectorLayer* layer : m_tunnelLayers)
    {
        validTunnels.append(getValidFeaturesFromLayer(layer, geometryErrors));
    }
    for (QgsVectorLayer* layer : m_roadLayersForTunnel)
    {
        validRoads.append(getValidFeaturesFromLayer(layer, geometryErrors));
    }

    // 检查所有隧道口
    for (QgsVectorLayer* tunnelEntranceLayer : m_tunnelEntranceLayers)
    {
        QString layerName = QFileInfo(tunnelEntranceLayer->source()).baseName();
        logStream << QString(u8"检查隧道口图层 '%1'\n").arg(layerName).toUtf8();

        QList<QgsFeature> validFeatures = getValidFeaturesFromLayer(tunnelEntranceLayer, geometryErrors);

        for (const QgsFeature& feature : validFeatures)
        {
            processedCount++;
            QgsGeometry tunnelEntranceGeometry = feature.geometry();

            // 使用动态缓冲区距离
            QgsGeometry buffer = tunnelEntranceGeometry.buffer(bufferDistance, 10);
            if (buffer.isNull())
            {
                QString errorType = u8"缓冲区创建失败";
                QString desc = QString(u8"图层[%1] 要素ID[%2]: 无法创建%3米缓冲区")
                    .arg(layerName)
                    .arg(feature.id())
                    .arg(bufferDistance);
                addErrorFeature(errorFeatures, feature, errorType, desc, layerName);
                continue;
            }

            // 检查条件1：缓冲区是否与隧道相交
            bool intersectsTunnel = false;
            for (const QgsFeature& tunnelFeature : validTunnels)
            {
                if (buffer.intersects(tunnelFeature.geometry()))
                {
                    intersectsTunnel = true;
                    break;
                }
            }

            // 检查条件2：缓冲区是否与道路相交
            bool intersectsRoad = false;
            for (const QgsFeature& roadFeature : validRoads)
            {
                if (buffer.intersects(roadFeature.geometry()))
                {
                    intersectsRoad = true;
                    break;
                }
            }

            // 两个条件必须同时满足
            if (!(intersectsTunnel && intersectsRoad))
            {
                errorCount++;
                QString errorType = u8"隧道口位置错误";
                QString desc;

                if (!intersectsTunnel && !intersectsRoad)
                {
                    desc = QString(u8"图层[%1] 要素ID[%2]: %3米范围内未找到隧道和道路")
                        .arg(layerName)
                        .arg(feature.id())
                        .arg(bufferDistance);
                }
                else if (!intersectsTunnel)
                {
                    desc = QString(u8"图层[%1] 要素ID[%2]: %3米范围已找到道路，但未找到隧道")
                        .arg(layerName)
                        .arg(feature.id())
                        .arg(bufferDistance);
                }
                else
                {
                    desc = QString(u8"图层[%1] 要素ID[%2]: %3米范围内已找到隧道，但未找到道路")
                        .arg(layerName)
                        .arg(feature.id())
                        .arg(bufferDistance);
                }

                logFeatureInfo(logStream, feature, QString(u8"错误 - %1").arg(desc));
                jsonErrors.append(JsonLogger::createErrorObject(feature.id(), desc));
                addErrorFeature(errorFeatures, feature, errorType, desc, layerName);
            }

            // 更新进度
            if (processedCount % 10 == 0)
            {
                logProgress(logStream, processedCount, totalTunnelEntrances, QString(u8"隧道口位置检查"));
                QApplication::processEvents();
            }
        }
    }

    logStream << QString(u8"隧道口位置检查完成: 共检查 %1 个隧道口，发现错误 %2 个\n")
        .arg(processedCount)
        .arg(errorCount)
        .toUtf8();

    return errorFeatures;
}
// 合并桥梁要素
QgsGeometry ContainmentCheckDialog::mergeBridgeFeatures(const QList<QgsFeature>& bridgeFeatures)
{
    if (bridgeFeatures.isEmpty())
    {
        return QgsGeometry();
    }

    QgsGeometry mergedGeometry = bridgeFeatures.first().geometry();
    for (int i = 1; i < bridgeFeatures.size(); i++)
    {
        mergedGeometry = mergedGeometry.combine(bridgeFeatures[i].geometry());
    }

    return mergedGeometry;
}
// 获取几何中心点
QgsGeometry ContainmentCheckDialog::getCentroidFromGeometry(const QgsGeometry& geometry)
{
    if (geometry.isNull())
    {
        return QgsGeometry();
    }

    if (geometry.type() == QgsWkbTypes::PointGeometry)
    {
        return geometry;
    }
    else if (geometry.type() == QgsWkbTypes::LineGeometry)
    {

        return geometry.centroid();
    }
    else if (geometry.type() == QgsWkbTypes::PolygonGeometry)
    {
        return geometry.centroid();
    }

    return QgsGeometry();
}
// 错误要素添加
void ContainmentCheckDialog::addErrorFeature(QList<QgsFeature>& errorFeatures, const QgsFeature& originalFeature,
    const QString& errorType, const QString& description, const QString& layerName)
{
    QgsFields newFields = originalFeature.fields();
    newFields.append(QgsField("ERROR_TYPE", QVariant::String, "", 50)); // 修正：QgsField
    newFields.append(QgsField("BASE_NAME", QVariant::String, "", 100)); // 修正：QgsField
    newFields.append(QgsField("PARSED_NAME", QVariant::String, "", 100));
    newFields.append(QgsField("DESCRIPTION", QVariant::String, "", 255));
    newFields.append(QgsField("SOURCE_LAYER", QVariant::String, "", 100));

    QgsFeature newFeature(newFields);
    newFeature.setGeometry(originalFeature.geometry());
    newFeature.setId(originalFeature.id());

    // 复制原始属性
    for (int i = 0; i < originalFeature.fields().count(); i++)
    {
        newFeature.setAttribute(originalFeature.fields().at(i).name(), originalFeature.attribute(i));
    }

    newFeature.setAttribute("ERROR_TYPE", errorType);
    newFeature.setAttribute("BASE_NAME", u8"无");
    newFeature.setAttribute("PARSED_NAME", u8"无");
    newFeature.setAttribute("DESCRIPTION", description);
    newFeature.setAttribute("SOURCE_LAYER", layerName);

    errorFeatures.append(newFeature);
}
// 获取图层名称
QString ContainmentCheckDialog::getLayerNameFromFeatureSource(const QgsFeature& feature)
{
    // 直接从 SOURCE_LAYER 字段获取图层名
    QString sourceLayer = feature.attribute("SOURCE_LAYER").toString();
    if (!sourceLayer.isEmpty() && sourceLayer != "NULL" && sourceLayer != u8"无")
    {
        return sourceLayer;
    }

    // 如果 SOURCE_LAYER 字段为空，回退到其他方法
    QString description = feature.attribute("DESCRIPTION").toString();

    if (description.contains(u8"图层["))
    {
        int start = description.indexOf(u8"图层[") + 3;
        int end = description.indexOf("]", start);
        if (start > 3 && end > start)
        {
            return description.mid(start, end - start);
        }
    }

    return "未知图层";
}
// 图上距离转实际距离
double ContainmentCheckDialog::convertMapDistanceToReal(double mapDistance, const QString& unit, int scale)
{

    double distanceInMm = mapDistance;
    if (unit == "厘米")
    {
        distanceInMm = mapDistance * 10;
    }
    else if (unit == "米")
    {
        distanceInMm = mapDistance * 1000;
    }

    // 根据比例尺计算实际距离（米）
    // 图上1毫米 = 实际(scale/1000)米
    double realDistance = distanceInMm * (scale / 1000.0);

    qDebug() << "距离转换: 图上" << mapDistance << unit << "-> 实际" << realDistance << "米 (比例尺1:" << scale << ")";
    return realDistance;
}
// 获取检查类型
QString ContainmentCheckDialog::getCheckTypeFromXml(const QString& xmlConfigPath)
{
    XmlConfigReader configReader;
    if (configReader.loadConfig(xmlConfigPath))
    {
        return configReader.getRuleStringValue(m_scale, QString::fromUtf8(u8"关系检查"), QString::fromUtf8(u8"具体项"), "");
    }
    return "";
}
// 获取缓冲区距离
double ContainmentCheckDialog::getBufferDistanceFromXml(const QString& xmlConfigPath, const QString& ruleName, double defaultDistance)
{
    XmlConfigReader configReader;
    if (configReader.loadConfig(xmlConfigPath))
    {
        double mapDistance = configReader.getRuleValue(m_scale, QString::fromUtf8(u8"关系检查"), ruleName, defaultDistance);
        QString unit = configReader.getRuleStringValue(m_scale, QString::fromUtf8(u8"关系检查"), ruleName + "_unit", "毫米");

        return convertMapDistanceToReal(mapDistance, unit, m_scale);
    }
    return defaultDistance;
}
// 获取比例尺
int ContainmentCheckDialog::getScaleFromXml(const QString& xmlConfigPath)
{
    XmlConfigReader configReader;
    if (configReader.loadConfig(xmlConfigPath))
    {
        QList<int> scales = configReader.getAvailableScales();
        if (!scales.isEmpty())
        {
            return scales.first();
        }
    }
    return 50000;
}
// 获取检查项的所有参数
QMap<QString, QVariant> ContainmentCheckDialog::getCheckItemParamsFromXml(const QString& xmlConfigPath)
{
    XmlConfigReader configReader;
    if (configReader.loadConfig(xmlConfigPath))
    {
        return configReader.getCheckItemParams(m_scale, QString::fromUtf8(u8"关系检查"));
    }
    return QMap<QString, QVariant>();
}
// 获取图层名称的辅助方法
QString ContainmentCheckDialog::getLayerNameFromPath(const QString& filePath)
{
    if (filePath.isEmpty())
        return u8"未知图层";
    QFileInfo info(filePath);
    return info.baseName();
}
// 确保目录存在的辅助方法
bool ContainmentCheckDialog::ensureDirectoryExists(const QString& path)
{
    QFileInfo info(path);
    QDir dir = info.dir();
    if (!dir.exists())
    {
        return dir.mkpath(".");
    }
    return true;
}