#include "ContainmentCheckInterface.h"
#include "ContainmentCheckDialog.h"
#include "XmlConfigReader.h"
#include <QString>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <QDateTime>
#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <algorithm>

// 辅助函数：生成关系检查错误JSON
static void generateRelationErrorJson(const char *szOutputJsonPath, const QString &errorInfo)
{
    qDebug() << "=== 开始生成关系检查错误JSON ===";
    if (!szOutputJsonPath)
        return;

    QString jsonPath = QString::fromUtf8(szOutputJsonPath);
    qDebug() << "生成的JSON文件路径:" << jsonPath;

    // 2. 确保输出目录存在
    QFileInfo jsonFileInfo(jsonPath);
    QDir jsonDir = jsonFileInfo.dir();
    if (!jsonDir.exists())
    {
        qDebug() << "创建JSON输出目录:" << jsonDir.path();
        if (!jsonDir.mkpath("."))
        {
            qCritical() << "❌ 无法创建JSON输出目录:" << jsonDir.path();
            return;
        }
    }

    // 3. 创建并写入JSON文件内容
    QJsonObject checkItem;
    checkItem["id"] = "113";
    checkItem["name"] = QString::fromUtf8(u8"关系检查");
    checkItem["status"] = "质检失败";
    checkItem["err_exist"] = true;
    checkItem["info"] = errorInfo;

    QJsonObject jsonLog;
    jsonLog["check_item"] = checkItem;

    QJsonDocument doc(jsonLog);

    QFile jsonFile(jsonPath);
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        jsonFile.write(doc.toJson(QJsonDocument::Indented));
        jsonFile.close();
        qDebug() << "✓ JSON文件已创建:" << jsonPath;
    }
    else
    {
        qCritical() << "❌ 无法创建JSON文件:" << jsonPath;
    }
}

// 辅助函数：写入日志
static void writeRelationLogFile(const QString &logFilePath, const QString &message)
{
    if (logFilePath.isEmpty())
        return;

    try
    {
        QFile file(logFilePath);
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
        qCritical() << "无法写入关系检查日志文件:" << logFilePath;
    }
}

static QStringList splitMultiPaths(const char *multiPaths)
{
    QStringList pathList;
    if (!multiPaths)
        return pathList;

    QString pathsStr = QString::fromUtf8(multiPaths);
    return pathsStr.split(';', Qt::SkipEmptyParts);
}

// 检查必要的SHP文件是否存在
static bool checkRequiredShpFiles(const QStringList &paths, const QString &fileType,
                                  const QString &logFilePath, const char *szOutputJsonPath,
                                  char *szActualJsonFilePath, const char *szXmlConfigPath)
{
    for (const QString &path : paths)
    {
        if (path.isEmpty())
        {
            QString errorMsg = QString(u8"%1路径为空").arg(fileType);
            writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
            generateRelationErrorJson(szOutputJsonPath, errorMsg);
            return false;
        }
        if (!QFileInfo::exists(path))
        {
            QString errorMsg = QString(u8"%1文件不存在: %2").arg(fileType).arg(path);
            writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
            generateRelationErrorJson(szOutputJsonPath, errorMsg);
            return false;
        }
        if (!path.toLower().endsWith(".shp"))
        {
            QString errorMsg = QString(u8"%1文件格式错误(非SHP文件): %2").arg(fileType).arg(path);
            writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
            generateRelationErrorJson(szOutputJsonPath, errorMsg);
            return false;
        }
    }
    return true;
}

SE_Error RelationshipCheck(
    const char *szXmlConfigPath,
    const char *szBasePolygonPath,
    const char *szBasePointPath,
    const char *szParsedPolygonPath,
    const char *szParsedPointPath,
    const char *szRiverPolygonPath,
    const char *szDitchFlowPaths,
    const char *szDitchPaths,
    const char *szBridgePaths,
    const char *szRoadPaths,
    const char *szTunnelEntrancePaths,
    const char *szTunnelPaths,
    const char *szRoadPathsForTunnel,
    const char *szOutputLogPath,   // 运行日志完整路径
    const char *szOutputErrorPath, // 错误要素基础路径
    const char *szOutputJsonPath,
    int scale)
{
    // === 接口层错误检查 ===

    // 1. 检查空指针
    if (!szXmlConfigPath || !szOutputLogPath || !szOutputErrorPath || !szOutputJsonPath)
    {
        generateRelationErrorJson(szOutputJsonPath, "输入参数包含空指针");
        return SE_ERROR_INVALID_PARAMETER;
    }

    // 2. 检查XML文件是否存在
    QString xmlPathStr = QString::fromUtf8(szXmlConfigPath);
    if (!QFileInfo::exists(xmlPathStr))
    {
        QString errorMsg = QString(u8"XML配置文件不存在: %1").arg(szXmlConfigPath);
        generateRelationErrorJson(szOutputJsonPath, errorMsg);
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 3. 检查比例尺有效性
    if (scale <= 0)
    {
        QString errorMsg = QString(u8"无效的比例尺: %1").arg(scale);
        generateRelationErrorJson(szOutputJsonPath, errorMsg);
        return SE_ERROR_INVALID_PARAMETER;
    }

    // 4. 检查输出日志目录
    QString logPathStr = QString::fromUtf8(szOutputLogPath);
    QFileInfo logPathInfo(logPathStr);
    QDir logDir = logPathInfo.dir();
    if (!logDir.exists() && !logDir.mkpath("."))
    {
        QString errorMsg = QString(u8"无法创建日志目录: %1").arg(logDir.path());
        generateRelationErrorJson(szOutputJsonPath, errorMsg);
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    // 5. 检查错误要素输出目录
    QString errorPathStr = QString::fromUtf8(szOutputErrorPath);
    QFileInfo errorFileInfo(errorPathStr);
    QDir errorDir = errorFileInfo.dir();
    if (!errorDir.exists() && !errorDir.mkpath("."))
    {
        QString errorMsg = QString(u8"无法创建错误要素输出目录: %1").arg(errorDir.path());
        generateRelationErrorJson(szOutputJsonPath, errorMsg);
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // === 使用传入的完整路径 ===
    QString logFilePath = QString::fromUtf8(szOutputLogPath);
    QString errorFilePath = QString::fromUtf8(szOutputErrorPath);
    QString jsonFilePath = QString::fromUtf8(szOutputJsonPath);

    // 写入开始日志
    writeRelationLogFile(logFilePath, "开始执行关系检查接口...");
    writeRelationLogFile(logFilePath, QString("XML路径: %1").arg(szXmlConfigPath));
    writeRelationLogFile(logFilePath, QString("输出日志: %1").arg(logFilePath));
    writeRelationLogFile(logFilePath, QString("错误要素路径: %1").arg(errorFilePath));
    writeRelationLogFile(logFilePath, QString("JSON报告路径: %1").arg(jsonFilePath));
    writeRelationLogFile(logFilePath, QString("比例尺: 1:%1").arg(scale));

    try
    {
        // 尝试加载XML配置
        writeRelationLogFile(logFilePath, "正在加载XML配置...");
        XmlConfigReader configReader;
        if (!configReader.loadConfig(xmlPathStr))
        {
            QString errorMsg = QString(u8"无法加载XML配置: %1").arg(xmlPathStr);
            writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
            generateRelationErrorJson(szOutputJsonPath, errorMsg);
            return SE_ERROR_FILE_NOT_FOUND;
        }
        writeRelationLogFile(logFilePath, "✓ XML配置加载成功");

        // 检查比例尺
        writeRelationLogFile(logFilePath, QString("检查比例尺: 1:%1").arg(scale));
        QList<int> availableScales = configReader.getAvailableScales();

        if (!configReader.hasScale(scale))
        {
            QStringList scaleStrings;
            for (int s : availableScales)
                scaleStrings << QString::number(s);
            QString errorMsg = QString(u8"XML配置中不存在比例尺1:%1，可用比例尺: %2").arg(scale).arg(scaleStrings.join(", "));
            writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
            generateRelationErrorJson(szOutputJsonPath, errorMsg);
            return SE_ERROR_INVALID_PARAMETER;
        }
        writeRelationLogFile(logFilePath, "✓ 比例尺检查通过");

        // 检查检查项
        QString relationCheckItemName = QString::fromUtf8(u8"关系检查");
        if (!configReader.hasCheckItem(scale, relationCheckItemName))
        {
            QList<QString> itemNames = configReader.getCheckItemNames(scale);
            QString errorMsg = QString(u8"比例尺1:%1下不存在检查项'%2'，可用检查项: %3")
                                   .arg(scale)
                                   .arg(relationCheckItemName)
                                   .arg(itemNames.join(", "));
            writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
            generateRelationErrorJson(szOutputJsonPath, errorMsg);
            return SE_ERROR_INVALID_PARAMETER;
        }
        writeRelationLogFile(logFilePath, "✓ 检查项存在");

        // 获取检查类型
        QString checkType = configReader.getRuleStringValue(scale, relationCheckItemName, QString::fromUtf8(u8"具体项"), "");
        if (checkType.isEmpty())
        {
            writeRelationLogFile(logFilePath, "❌ 错误: 检查类型为空");
            generateRelationErrorJson(szOutputJsonPath, "检查类型为空");
            return SE_ERROR_INVALID_PARAMETER;
        }
        writeRelationLogFile(logFilePath, QString("检查类型: %1").arg(checkType));

        // === 根据检查类型验证必要的SHP文件 ===
        writeRelationLogFile(logFilePath, "开始验证必要的SHP文件...");

        if (checkType == "行政面包含检查")
        {
            // 行政面包含检查需要的文件
            QStringList requiredFiles;
            if (szBasePolygonPath)
                requiredFiles << QString::fromUtf8(szBasePolygonPath);
            if (szBasePointPath)
                requiredFiles << QString::fromUtf8(szBasePointPath);
            if (szParsedPolygonPath)
                requiredFiles << QString::fromUtf8(szParsedPolygonPath);
            if (szParsedPointPath)
                requiredFiles << QString::fromUtf8(szParsedPointPath);

            if (!checkRequiredShpFiles(requiredFiles, u8"行政面包含检查必要文件",
                                       logFilePath, szOutputJsonPath, nullptr, szXmlConfigPath))
            {
                return SE_ERROR_FILE_NOT_FOUND;
            }
        }
        else if (checkType == "桥梁位置检查")
        {
            // 桥梁位置检查需要的文件
            QStringList requiredFiles;
            if (szBridgePaths)
                requiredFiles.append(splitMultiPaths(szBridgePaths));
            if (szRiverPolygonPath)
                requiredFiles << QString::fromUtf8(szRiverPolygonPath);
            if (szRoadPaths)
                requiredFiles.append(splitMultiPaths(szRoadPaths));

            if (requiredFiles.isEmpty())
            {
                QString errorMsg = u8"桥梁位置检查需要的SHP文件路径都为空";
                writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
                generateRelationErrorJson(szOutputJsonPath, errorMsg);
                return SE_ERROR_INVALID_PARAMETER;
            }

            if (!checkRequiredShpFiles(requiredFiles, u8"桥梁位置检查必要文件",
                                       logFilePath, szOutputJsonPath, nullptr, szXmlConfigPath))
            {
                return SE_ERROR_FILE_NOT_FOUND;
            }
        }
        else if (checkType == "隧道口位置检查")
        {
            // 隧道口位置检查需要的文件
            QStringList requiredFiles;
            if (szTunnelEntrancePaths)
                requiredFiles.append(splitMultiPaths(szTunnelEntrancePaths));
            if (szTunnelPaths)
                requiredFiles.append(splitMultiPaths(szTunnelPaths));
            if (szRoadPathsForTunnel)
                requiredFiles.append(splitMultiPaths(szRoadPathsForTunnel));

            if (requiredFiles.isEmpty())
            {
                QString errorMsg = u8"隧道口位置检查需要的SHP文件路径都为空";
                writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
                generateRelationErrorJson(szOutputJsonPath, errorMsg);
                return SE_ERROR_INVALID_PARAMETER;
            }

            if (!checkRequiredShpFiles(requiredFiles, u8"隧道口位置检查必要文件",
                                       logFilePath, szOutputJsonPath, nullptr, szXmlConfigPath))
            {
                return SE_ERROR_FILE_NOT_FOUND;
            }
        }
        else if (checkType == "渠流向位置检查")
        {
            // 渠流向位置检查需要的文件
            QStringList requiredFiles;
            if (szDitchFlowPaths)
                requiredFiles.append(splitMultiPaths(szDitchFlowPaths));
            if (szDitchPaths)
                requiredFiles.append(splitMultiPaths(szDitchPaths));

            if (requiredFiles.isEmpty())
            {
                QString errorMsg = u8"渠流向位置检查需要的SHP文件路径都为空";
                writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
                generateRelationErrorJson(szOutputJsonPath, errorMsg);
                return SE_ERROR_INVALID_PARAMETER;
            }

            if (!checkRequiredShpFiles(requiredFiles, u8"渠流向位置检查必要文件",
                                       logFilePath, szOutputJsonPath, nullptr, szXmlConfigPath))
            {
                return SE_ERROR_FILE_NOT_FOUND;
            }
        }
        else if (checkType == "河流包含检查")
        {
            // 河流包含检查需要的文件
            QStringList requiredFiles;
            if (szParsedPointPath)
                requiredFiles << QString::fromUtf8(szParsedPointPath);
            if (szRiverPolygonPath)
                requiredFiles << QString::fromUtf8(szRiverPolygonPath);

            if (requiredFiles.isEmpty())
            {
                QString errorMsg = u8"河流包含检查需要的SHP文件路径都为空";
                writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
                generateRelationErrorJson(szOutputJsonPath, errorMsg);
                return SE_ERROR_INVALID_PARAMETER;
            }

            if (!checkRequiredShpFiles(requiredFiles, u8"河流包含检查必要文件",
                                       logFilePath, szOutputJsonPath, nullptr, szXmlConfigPath))
            {
                return SE_ERROR_FILE_NOT_FOUND;
            }
        }

        writeRelationLogFile(logFilePath, "✓ 所有必要的SHP文件验证通过");

        // 创建并配置对话框
        ContainmentCheckDialog dialog(nullptr, nullptr);

        // 设置基本参数
        dialog.setScale(scale);
        dialog.setXmlConfigPath(xmlPathStr);

        // 设置输出路径（完整路径）
        dialog.setOutputLogPath(logFilePath);
        dialog.setOutputErrorPath(errorFilePath);
        dialog.setOutputJsonPath(jsonFilePath);

        writeRelationLogFile(logFilePath, QString("开始执行检查: %1").arg(checkType));

        // 设置所有数据路径
        if (szBasePolygonPath)
        {
            dialog.setBasePolygonPath(QString::fromUtf8(szBasePolygonPath));
            writeRelationLogFile(logFilePath, QString("设置库体面路径: %1").arg(szBasePolygonPath));
        }
        if (szBasePointPath)
        {
            dialog.setBasePointPath(QString::fromUtf8(szBasePointPath));
            writeRelationLogFile(logFilePath, QString("设置库体点路径: %1").arg(szBasePointPath));
        }
        if (szParsedPolygonPath)
        {
            dialog.setParsedPolygonPath(QString::fromUtf8(szParsedPolygonPath));
            writeRelationLogFile(logFilePath, QString("设置解析面路径: %1").arg(szParsedPolygonPath));
        }
        if (szParsedPointPath)
        {
            dialog.setParsedPointPath(QString::fromUtf8(szParsedPointPath));
            writeRelationLogFile(logFilePath, QString("设置解析点路径: %1").arg(szParsedPointPath));
        }
        if (szRiverPolygonPath)
        {
            dialog.setRiverPolygonPath(QString::fromUtf8(szRiverPolygonPath));
            writeRelationLogFile(logFilePath, QString("设置河流面路径: %1").arg(szRiverPolygonPath));
        }

        // 设置多路径数据
        if (szDitchFlowPaths)
        {
            dialog.setDitchFlowPaths(splitMultiPaths(szDitchFlowPaths));
            writeRelationLogFile(logFilePath, QString("设置渠流向路径: %1").arg(szDitchFlowPaths));
        }
        if (szDitchPaths)
        {
            dialog.setDitchPaths(splitMultiPaths(szDitchPaths));
            writeRelationLogFile(logFilePath, QString("设置沟渠路径: %1").arg(szDitchPaths));
        }
        if (szBridgePaths)
        {
            dialog.setBridgePaths(splitMultiPaths(szBridgePaths));
            writeRelationLogFile(logFilePath, QString("设置桥梁路径: %1").arg(szBridgePaths));
        }
        if (szRoadPaths)
        {
            dialog.setRoadPaths(splitMultiPaths(szRoadPaths));
            writeRelationLogFile(logFilePath, QString("设置道路路径: %1").arg(szRoadPaths));
        }
        if (szTunnelEntrancePaths)
        {
            dialog.setTunnelEntrancePaths(splitMultiPaths(szTunnelEntrancePaths));
            writeRelationLogFile(logFilePath, QString("设置隧道口路径: %1").arg(szTunnelEntrancePaths));
        }
        if (szTunnelPaths)
        {
            dialog.setTunnelPaths(splitMultiPaths(szTunnelPaths));
            writeRelationLogFile(logFilePath, QString("设置隧道路径: %1").arg(szTunnelPaths));
        }
        if (szRoadPathsForTunnel)
        {
            dialog.setRoadPathsForTunnel(splitMultiPaths(szRoadPathsForTunnel));
            writeRelationLogFile(logFilePath, QString("设置隧道相关道路路径: %1").arg(szRoadPathsForTunnel));
        }

        writeRelationLogFile(logFilePath, QString("根据XML配置调用检查方法: %1").arg(checkType));

        // 执行具体的检查方法
        if (checkType == "行政面包含检查")
        {
            writeRelationLogFile(logFilePath, "调用行政面包含检查...");
            dialog.runAdministrativeContainmentCheck();
        }
        else if (checkType == "桥梁位置检查")
        {
            writeRelationLogFile(logFilePath, "调用桥梁位置检查...");
            dialog.runBridgePositionCheck();
        }
        else if (checkType == "隧道口位置检查")
        {
            writeRelationLogFile(logFilePath, "调用隧道口位置检查...");
            dialog.runTunnelEntrancePositionCheck();
        }
        else if (checkType == "渠流向位置检查")
        {
            writeRelationLogFile(logFilePath, "调用渠流向位置检查...");
            dialog.runDitchFlowPositionCheck();
        }
        else if (checkType == "河流包含检查")
        {
            writeRelationLogFile(logFilePath, "调用河流包含检查...");
            dialog.runRiverContainmentCheck();
        }
        else
        {
            writeRelationLogFile(logFilePath, QString("❌ 错误: 不支持的检查类型: %1").arg(checkType));
            generateRelationErrorJson(szOutputJsonPath, "不支持的检查类型");
            return SE_ERROR_INVALID_PARAMETER;
        }

        writeRelationLogFile(logFilePath, "✓ 检查执行完成");
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        writeRelationLogFile(logFilePath, QString("接口执行异常: %1").arg(e.what()));
        QString errorMsg = QString("程序异常: %1").arg(e.what());
        generateRelationErrorJson(szOutputJsonPath, errorMsg);
        return SE_ERROR_FAILURE;
    }
    catch (...)
    {
        writeRelationLogFile(logFilePath, "接口执行未知异常");
        generateRelationErrorJson(szOutputJsonPath, "未知异常");
        return SE_ERROR_FAILURE;
    }
}