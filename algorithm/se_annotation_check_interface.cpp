#include "se_annotation_check_interface.h"
#include "se_annotation_algorithm.h"
#include <QString>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <QDateTime>
#include <QDebug> // 添加调试输出
#include <QgsVectorLayer.h>
#include <QgsVectorFileWriter.h>
#include <QgsFields.h>
#include <QgsField.h>
#include <QgsFeature.h>
#include <QgsGeometry.h> // 添加几何相关
#include <QgsPointXY.h>  // 添加点几何
#include <qgsmessagelog.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#ifdef _WIN32
#include <spdlog/sinks/basic_file_sink.h> // 确保包含
#endif
#include "se_feature_matching.h"
#include <QDomDocument>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

// 内部辅助函数声明
static bool validateInputPaths(const char *szShpFilePath, const char *szSchemaXmlPath, const char *szOutputLogFilePath);
static bool createOutputDirectories(const char *szOutputCheckResultLayerPath);
static SE_Error writeCheckResultLayer(const QList<AnnotationCheckResult> &results, QgsVectorLayer *sourceLayer, const char *szOutputCheckResultLayerPath);
std::shared_ptr<spdlog::logger> createLogger(const char *szOutputLogFilePath, const QString &loggerName);
static QString truncateString(const QString &str, int maxLength);
static SE_Error writeJsonReportToFile(const QString &jsonString, const char *szOutputJsonPath, std::shared_ptr<spdlog::logger> logger);

/**
 * @brief 通用JSON报告生成函数 - 返回JSON字符串
 * @param results 检查结果列表
 * @param statistics 统计信息（可选）
 * @param checkId 检查项ID
 * @param checkName 检查项名称
 * @param logger 日志记录器
 * @return QString JSON字符串
 */
static QString generateJsonReport(
    const QList<AnnotationCheckResult> &results,
    const QMap<QString, QString> &statistics,
    const QString &checkId,
    const QString &checkName,
    std::shared_ptr<spdlog::logger> logger)
{
    try
    {
        QJsonObject root;

        // 基本信息
        root["id"] = checkId;
        root["name"] = checkName;
        root["status"] = "质检完成";
        root["err_exist"] = !results.isEmpty();

        // 构造 info 文本（包含统计信息）
        QString infoText;
        if (results.isEmpty())
        {
            infoText = QString("未发现%1问题").arg(checkName);
        }
        else
        {
            infoText = QString("发现%1个%2问题").arg(results.size()).arg(checkName);
        }

        if (!statistics.isEmpty())
        {
            infoText += "\n统计信息：";
            for (auto it = statistics.constBegin(); it != statistics.constEnd(); ++it)
            {
                infoText += QString("\n- %1: %2").arg(it.key(), it.value());
            }
        }
        root["info"] = infoText;

        // 错误信息部分
        if (!results.isEmpty())
        {
            QJsonObject errors;
            for (int i = 0; i < results.size(); ++i)
            {
                const AnnotationCheckResult &result = results[i];

                QJsonObject errorDetail;
                errorDetail["description"] = result.errorMessage;
                errorDetail["actual_value"] = result.actualValue;
                errorDetail["expected_value"] = result.expectedValue;
                errorDetail["feature_id"] = result.featureId;
                errorDetail["position_x"] = result.position.x();
                errorDetail["position_y"] = result.position.y();

                errors[QString("error%1").arg(i + 1)] = errorDetail;
            }
            root["errors"] = errors;
        }

        QJsonDocument doc(root);
        QString jsonString = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));

        logger->info("成功生成JSON报告，大小: {} 字节", jsonString.toUtf8().size());
        return jsonString;
    }
    catch (const std::exception &e)
    {
        logger->error("生成JSON报告时发生异常: {}", e.what());
        return QString("{}"); // 返回空JSON对象
    }
}

static SE_Error writeJsonReportToFile(const QString &jsonString, const char *szOutputJsonPath, std::shared_ptr<spdlog::logger> logger)
{
    try
    {
        QString path = QString::fromUtf8(szOutputJsonPath);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        {
            logger->error("无法打开JSON报告文件进行写入: {}", szOutputJsonPath);
            return SE_ERROR_WRITE_FILE_FAILED;
        }
        QTextStream out(&file);
        out.setCodec("UTF-8");
        out << jsonString;
        file.close();
        logger->info("成功将JSON报告写入文件: {}", szOutputJsonPath);
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("写入JSON报告文件时发生异常: {}", e.what());
        return SE_ERROR_WRITE_FILE_FAILED;
    }
}

SE_Error AnnotationCheck(
    const char *szShpFilePath,
    const char *szSchemaXmlPath,
    int scale, // 修改为 int
    const char *szOutputCheckResultLayerPath,
    const char *szOutputLogFilePath,
    const char *szOutputJsonPath)
{
    // 参数验证
    if (!szShpFilePath || !szSchemaXmlPath || scale <= 0 ||
        !szOutputCheckResultLayerPath || !szOutputLogFilePath ||
        !szOutputJsonPath)
    {
        return SE_ERROR_INVALID_PARAMETER;
    }

    // 验证输入文件路径
    if (!validateInputPaths(szShpFilePath, szSchemaXmlPath, szOutputLogFilePath))
    {
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 创建输出目录
    if (!createOutputDirectories(szOutputCheckResultLayerPath))
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    // 创建日志记录器
    auto logger = createLogger(szOutputLogFilePath, "AnnotationCheck");
    if (!logger)
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    logger->info("开始执行注记检查算法");
    logger->info("输入文件: {}", szShpFilePath);
    logger->info("质检方案: {}", szSchemaXmlPath);
    logger->info("地图比例尺: {}", scale);

    try
    {
        // 加载shp文件
        QString layerPath = QString::fromUtf8(szShpFilePath);
        QgsVectorLayer *layer = new QgsVectorLayer(layerPath, "annotation_layer", "ogr");

        if (!layer->isValid())
        {
            logger->error("无法加载shp文件: {}", szShpFilePath);
            delete layer;
            spdlog::drop(logger->name()); // 释放 logger
            return SE_ERROR_CORRUPT_DATA;
        }

        logger->info("成功加载图层，要素数量: {}", layer->featureCount());

        // 创建算法实例（内置字体映射已自动加载）
        SE_AnnotationAlgorithm algorithm;
        logger->info("字体映射已初始化（使用内置映射）");

        // 加载质检规则,传入比例尺参数
        QString schemaPath = QString::fromUtf8(szSchemaXmlPath);
        if (!algorithm.loadCheckRules(schemaPath, QString::number(scale))) // 传递分母字符串
        {
            logger->error("加载质检规则失败: {}", szSchemaXmlPath);
            delete layer;
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }

        logger->info("已加载比例尺 {} 的质检规则", scale);

        // 设置检查选项（全部检查）
        algorithm.setCheckOptions(true, true, true, true);

        // 执行检查
        logger->info("开始执行质检");
        QList<AnnotationCheckResult> results = algorithm.checkAnnotationLayer(layer);

        logger->info("质检完成，发现问题数量: {}", results.size());

        // 生成JSON报告
        QMap<QString, QString> statistics; // 此检查不生成统计信息
        QString jsonString = generateJsonReport(results, statistics, "201,202,203,205", "注记综合检查", logger);

        // 写入JSON文件
        SE_Error jsonResult = writeJsonReportToFile(jsonString, szOutputJsonPath, logger);
        if (jsonResult != SE_ERROR_NONE)
        {
            delete layer;
            spdlog::drop(logger->name());
            return jsonResult;
        }

        // 写入问题要素图层（如果有问题）
        if (!results.isEmpty())
        {
            SE_Error layerResult = writeCheckResultLayer(results, layer, szOutputCheckResultLayerPath);
            if (layerResult != SE_ERROR_NONE)
            {
                logger->error("写入问题要素图层失败");
                delete layer;
                spdlog::drop(logger->name()); // 释放 logger
                return layerResult;
            }
        }

        delete layer;
        logger->info("注记检查算法执行完成");

        // 释放 logger
        spdlog::drop(logger->name());

        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("执行过程中发生异常: {}", e.what());
        spdlog::drop(logger->name()); // 释放 logger
        return SE_ERROR_FAILURE;
    }
}

SE_Error AnnotationFontCheck(
    const char *szShpFilePath,
    const char *szSchemaXmlPath,
    int scale,
    const char *szOutputCheckResultLayerPath,
    const char *szOutputLogFilePath,
    const char *szOutputJsonPath)
{
    // 参数验证
    if (!szShpFilePath || !szSchemaXmlPath || scale <= 0 ||
        !szOutputCheckResultLayerPath || !szOutputLogFilePath ||
        !szOutputJsonPath)
    {
        return SE_ERROR_INVALID_PARAMETER;
    }

    // 验证输入文件路径
    if (!validateInputPaths(szShpFilePath, szSchemaXmlPath, szOutputLogFilePath))
    {
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 创建输出目录
    if (!createOutputDirectories(szOutputCheckResultLayerPath))
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    // 创建日志记录器
    auto logger = createLogger(szOutputLogFilePath, "AnnotationFontCheck");
    if (!logger)
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    logger->info("开始执行注记字体检查算法");
    logger->info("地图比例尺: {}", scale);

    try
    {
        // 加载shp文件
        QString layerPath = QString::fromUtf8(szShpFilePath);
        QgsVectorLayer *layer = new QgsVectorLayer(layerPath, "annotation_layer", "ogr");

        if (!layer->isValid())
        {
            logger->error("无法加载shp文件: {}", szShpFilePath);
            delete layer;
            return SE_ERROR_CORRUPT_DATA;
        }

        // 创建算法实例（内置字体映射已自动加载）
        SE_AnnotationAlgorithm algorithm;

        // 加载质检规则
        QString schemaPath = QString::fromUtf8(szSchemaXmlPath);
        if (!algorithm.loadCheckRules(schemaPath, QString::number(scale)))
        {
            logger->error("加载质检规则失败: {}", szSchemaXmlPath);
            delete layer;
            return SE_ERROR_CORRUPT_DATA;
        }

        // 设置只检查字体
        algorithm.setCheckOptions(true, false, false, false);

        // 执行字体检查
        QList<AnnotationCheckResult> results = algorithm.checkFont(layer);

        logger->info("字体检查完成，发现问题数量: {}", results.size());

        // 生成JSON报告
        QMap<QString, QString> statistics;
        QString jsonString = generateJsonReport(results, statistics, "201", "注记字体检查", logger);

        // 写入JSON文件
        SE_Error jsonResult = writeJsonReportToFile(jsonString, szOutputJsonPath, logger);
        if (jsonResult != SE_ERROR_NONE)
        {
            delete layer;
            spdlog::drop(logger->name());
            return jsonResult;
        }

        // 写入问题要素图层（如果有问题）
        if (!results.isEmpty())
        {
            SE_Error layerResult = writeCheckResultLayer(results, layer, szOutputCheckResultLayerPath);
            if (layerResult != SE_ERROR_NONE)
            {
                delete layer;
                return layerResult;
            }
        }

        delete layer;
        logger->info("注记字体检查算法执行完成");
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("执行过程中发生异常: {}", e.what());
        return SE_ERROR_FAILURE;
    }
}

SE_Error AnnotationFontSizeCheck(
    const char *szShpFilePath,
    const char *szSchemaXmlPath,
    int scale,
    const char *szOutputCheckResultLayerPath,
    const char *szOutputLogFilePath,
    const char *szOutputJsonPath)
{
    // 参数验证
    if (!szShpFilePath || !szSchemaXmlPath || scale <= 0 ||
        !szOutputCheckResultLayerPath || !szOutputLogFilePath ||
        !szOutputJsonPath)
    {
        return SE_ERROR_INVALID_PARAMETER;
    }

    // 验证输入文件路径
    if (!validateInputPaths(szShpFilePath, szSchemaXmlPath, szOutputLogFilePath))
    {
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 创建输出目录
    if (!createOutputDirectories(szOutputCheckResultLayerPath))
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    // 创建日志记录器
    auto logger = createLogger(szOutputLogFilePath, "AnnotationFontSizeCheck");
    if (!logger)
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    logger->info("开始执行注记字号检查算法");
    logger->info("地图比例尺: {}", scale);

    try
    {
        // 加载shp文件
        QString layerPath = QString::fromUtf8(szShpFilePath);
        QgsVectorLayer *layer = new QgsVectorLayer(layerPath, "annotation_layer", "ogr");

        if (!layer->isValid())
        {
            logger->error("无法加载shp文件: {}", szShpFilePath);
            delete layer;
            return SE_ERROR_CORRUPT_DATA;
        }

        // 创建算法实例（内置字体映射已自动加载）
        SE_AnnotationAlgorithm algorithm;

        // 加载质检规则
        QString schemaPath = QString::fromUtf8(szSchemaXmlPath);
        if (!algorithm.loadCheckRules(schemaPath, QString::number(scale)))
        {
            logger->error("加载质检规则失败: {}", szSchemaXmlPath);
            delete layer;
            return SE_ERROR_CORRUPT_DATA;
        }

        // 设置只检查字号
        algorithm.setCheckOptions(false, true, false, false);

        // 执行字号检查
        QList<AnnotationCheckResult> results = algorithm.checkFontSize(layer);

        logger->info("字号检查完成，发现问题数量: {}", results.size());

        // 生成JSON报告
        QMap<QString, QString> statistics;
        QString jsonString = generateJsonReport(results, statistics, "202", "注记字号检查", logger);

        // 写入JSON文件
        SE_Error jsonResult = writeJsonReportToFile(jsonString, szOutputJsonPath, logger);
        if (jsonResult != SE_ERROR_NONE)
        {
            delete layer;
            spdlog::drop(logger->name());
            return jsonResult;
        }

        // 写入问题要素图层（如果有问题）
        if (!results.isEmpty())
        {
            SE_Error layerResult = writeCheckResultLayer(results, layer, szOutputCheckResultLayerPath);
            if (layerResult != SE_ERROR_NONE)
            {
                delete layer;
                return layerResult;
            }
        }

        delete layer;
        logger->info("注记字号检查算法执行完成");
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("执行过程中发生异常: {}", e.what());
        return SE_ERROR_FAILURE;
    }
}

SE_Error AnnotationColorCheck(
    const char *szShpFilePath,
    const char *szSchemaXmlPath,
    int scale,
    const char *szOutputCheckResultLayerPath,
    const char *szOutputLogFilePath,
    const char *szOutputJsonPath)
{
    // 参数验证
    if (!szShpFilePath || !szSchemaXmlPath || scale <= 0 ||
        !szOutputCheckResultLayerPath || !szOutputLogFilePath ||
        !szOutputJsonPath)
    {
        return SE_ERROR_INVALID_PARAMETER;
    }

    // 验证输入文件路径
    if (!validateInputPaths(szShpFilePath, szSchemaXmlPath, szOutputLogFilePath))
    {
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 创建输出目录
    if (!createOutputDirectories(szOutputCheckResultLayerPath))
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    // 创建日志记录器
    auto logger = createLogger(szOutputLogFilePath, "AnnotationColorCheck");
    if (!logger)
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    logger->info("开始执行注记颜色检查算法");
    logger->info("地图比例尺: {}", scale);

    try
    {
        // 加载shp文件
        QString layerPath = QString::fromUtf8(szShpFilePath);
        QgsVectorLayer *layer = new QgsVectorLayer(layerPath, "annotation_layer", "ogr");

        if (!layer->isValid())
        {
            logger->error("无法加载shp文件: {}", szShpFilePath);
            delete layer;
            return SE_ERROR_CORRUPT_DATA;
        }

        // 创建算法实例（内置字体映射已自动加载）
        SE_AnnotationAlgorithm algorithm;

        // 加载质检规则
        QString schemaPath = QString::fromUtf8(szSchemaXmlPath);
        if (!algorithm.loadCheckRules(schemaPath, QString::number(scale)))
        {
            logger->error("加载质检规则失败: {}", szSchemaXmlPath);
            delete layer;
            return SE_ERROR_CORRUPT_DATA;
        }

        // 设置只检查颜色
        algorithm.setCheckOptions(false, false, true, false);

        // 执行颜色检查
        QList<AnnotationCheckResult> results = algorithm.checkColor(layer);

        logger->info("颜色检查完成，发现问题数量: {}", results.size());

        // 生成JSON报告
        QMap<QString, QString> statistics;
        QString jsonString = generateJsonReport(results, statistics, "203", "注记颜色检查", logger);

        // 写入JSON文件
        SE_Error jsonResult = writeJsonReportToFile(jsonString, szOutputJsonPath, logger);
        if (jsonResult != SE_ERROR_NONE)
        {
            delete layer;
            spdlog::drop(logger->name());
            return jsonResult;
        }

        // 写入问题要素图层（如果有问题）
        if (!results.isEmpty())
        {
            SE_Error layerResult = writeCheckResultLayer(results, layer, szOutputCheckResultLayerPath);
            if (layerResult != SE_ERROR_NONE)
            {
                delete layer;
                return layerResult;
            }
        }

        delete layer;
        logger->info("注记颜色检查算法执行完成");
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("执行过程中发生异常: {}", e.what());
        return SE_ERROR_FAILURE;
    }
}

SE_Error AnnotationItalicCheck(
    const char *szShpFilePath,
    const char *szSchemaXmlPath,
    int scale,
    const char *szOutputCheckResultLayerPath,
    const char *szOutputLogFilePath,
    const char *szOutputJsonPath)
{
    // 参数验证
    if (!szShpFilePath || !szSchemaXmlPath || scale <= 0 ||
        !szOutputCheckResultLayerPath || !szOutputLogFilePath ||
        !szOutputJsonPath)
    {
        return SE_ERROR_INVALID_PARAMETER;
    }

    // 验证输入文件路径
    if (!validateInputPaths(szShpFilePath, szSchemaXmlPath, szOutputLogFilePath))
    {
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 创建输出目录
    if (!createOutputDirectories(szOutputCheckResultLayerPath))
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    // 创建日志记录器
    auto logger = createLogger(szOutputLogFilePath, "AnnotationItalicCheck");
    if (!logger)
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    logger->info("开始执行注记斜体检查算法");
    logger->info("地图比例尺: {}", scale);

    try
    {
        // 加载shp文件
        QString layerPath = QString::fromUtf8(szShpFilePath);
        QgsVectorLayer *layer = new QgsVectorLayer(layerPath, "annotation_layer", "ogr");

        if (!layer->isValid())
        {
            logger->error("无法加载shp文件: {}", szShpFilePath);
            delete layer;
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }

        // 创建算法实例（内置字体映射已自动加载）
        SE_AnnotationAlgorithm algorithm;

        // 加载质检规则
        QString schemaPath = QString::fromUtf8(szSchemaXmlPath);
        if (!algorithm.loadCheckRules(schemaPath, QString::number(scale)))
        {
            logger->error("加载质检规则失败: {}", szSchemaXmlPath);
            delete layer;
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }

        // 设置只检查斜体
        algorithm.setCheckOptions(false, false, false, true);

        // 执行斜体检查
        QList<AnnotationCheckResult> results = algorithm.checkItalic(layer);

        logger->info("斜体检查完成，发现问题数量: {}", results.size());

        // 生成JSON报告
        QMap<QString, QString> statistics;
        QString jsonString = generateJsonReport(results, statistics, "205", "注记斜体检查", logger);

        // 写入JSON文件
        SE_Error jsonResult = writeJsonReportToFile(jsonString, szOutputJsonPath, logger);
        if (jsonResult != SE_ERROR_NONE)
        {
            delete layer;
            spdlog::drop(logger->name());
            return jsonResult;
        }

        // 写入问题要素图层（如果有问题）
        if (!results.isEmpty())
        {
            SE_Error layerResult = writeCheckResultLayer(results, layer, szOutputCheckResultLayerPath);
            if (layerResult != SE_ERROR_NONE)
            {
                delete layer;
                spdlog::drop(logger->name());
                return layerResult;
            }
        }

        delete layer;
        logger->info("注记斜体检查算法执行完成");
        spdlog::drop(logger->name());
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("执行过程中发生异常: {}", e.what());
        spdlog::drop(logger->name());
        return SE_ERROR_FAILURE;
    }
}

// 内部辅助函数实现
static bool validateInputPaths(const char *szShpFilePath, const char *szSchemaXmlPath, const char *szOutputLogFilePath)
{
    // 检查shp文件是否存在
    if (!QFileInfo::exists(QString::fromUtf8(szShpFilePath)))
    {
        return false;
    }

    // 检查质检方案文件是否存在
    if (!QFileInfo::exists(QString::fromUtf8(szSchemaXmlPath)))
    {
        return false;
    }

    // 检查日志文件目录是否存在
    // 关键修复：确保这里也使用 fromUtf8 来解析路径，与接口约定一致
    QString logDir = QFileInfo(QString::fromUtf8(szOutputLogFilePath)).dir().absolutePath();
    if (!QDir(logDir).exists())
    {
        QDir().mkpath(logDir);
    }

    return true;
}

static bool createOutputDirectories(const char *szOutputCheckResultLayerPath)
{
    // 创建结果图层目录
    QString layerDir = QFileInfo(QString::fromUtf8(szOutputCheckResultLayerPath)).dir().absolutePath();
    if (!QDir(layerDir).exists())
    {
        if (!QDir().mkpath(layerDir))
        {
            return false;
        }
    }

    return true;
}

// 删除 writeCheckReport 函数，因为它不再被使用
// SE_Error writeCheckReport(...) { ... }

static SE_Error writeCheckResultLayer(const QList<AnnotationCheckResult> &results, QgsVectorLayer *sourceLayer, const char *szOutputCheckResultLayerPath)
{
    if (results.isEmpty())
    {
        return SE_ERROR_NONE;
    }

    try
    {
        // 使用传入的路径直接作为输出路径
        QString fullOutputPath = QString::fromUtf8(szOutputCheckResultLayerPath);

        // 如果传入的是目录路径，则生成完整文件路径
        QFileInfo outputInfo(fullOutputPath);
        if (outputInfo.isDir() || fullOutputPath.endsWith("/") || fullOutputPath.endsWith("\\"))
        {
            QString sourceLayerName = QFileInfo(sourceLayer->source()).baseName();
            QString problemLayerName = QString("%1_problems_%2")
                                           .arg(sourceLayerName)
                                           .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));

            fullOutputPath = QString("%1/%2.shp")
                                 .arg(outputInfo.isDir() ? outputInfo.absoluteFilePath() : fullOutputPath)
                                 .arg(problemLayerName);
        }
        else if (!fullOutputPath.endsWith(".shp", Qt::CaseInsensitive))
        {
            // 如果传入的是文件名但没有扩展名，添加 .shp
            fullOutputPath += ".shp";
        }

        // 确保输出目录存在
        QFileInfo finalOutputInfo(fullOutputPath);
        QDir outputDir = finalOutputInfo.dir();
        if (!outputDir.exists())
        {
            if (!outputDir.mkpath("."))
            {
                return SE_ERROR_WRITE_FILE_FAILED;
            }
        }

        // 删除已存在的文件
        QStringList extensions = {"shp", "shx", "dbf", "prj", "cpg"};
        QString baseName = finalOutputInfo.baseName();
        QString dirPath = finalOutputInfo.dir().absolutePath();

        for (const QString &ext : extensions)
        {
            QString filePath = QString("%1/%2.%3").arg(dirPath).arg(baseName).arg(ext);
            if (QFile::exists(filePath))
            {
                QFile::remove(filePath);
            }
        }

        // 创建问题要素ID到错误列表的映射
        QMap<QVariant, QList<AnnotationCheckResult>> problemFeatures;
        for (const AnnotationCheckResult &result : results)
        {
            problemFeatures[result.featureId].append(result);
        }

        // 准备要素列表
        QList<QgsFeature> features;
        QString currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm");

        // 创建扩展的字段结构 - 原始字段加上错误信息字段
        QgsFields outputFields = sourceLayer->fields();
        outputFields.append(QgsField("FEATURE_ID", QVariant::String, "String", 20));   // 要素ID
        outputFields.append(QgsField("ERROR_CNT", QVariant::Int, "Integer", 10));      // 错误总数
        outputFields.append(QgsField("ERROR_TYPES", QVariant::String, "String", 100)); // 所有错误类型（用分号分隔）
        outputFields.append(QgsField("ERROR_CODES", QVariant::String, "String", 50));  // 所有错误代码（用分号分隔）
        outputFields.append(QgsField("ERROR_MSGS", QVariant::String, "String", 254));  // 所有错误描述（用分号分隔）
        outputFields.append(QgsField("ACTUAL_VALS", QVariant::String, "String", 200)); // 所有实际值（用分号分隔）
        outputFields.append(QgsField("EXPECT_VALS", QVariant::String, "String", 200)); // 所有期望值（用分号分隔）
        outputFields.append(QgsField("CHECK_TIME", QVariant::String, "String", 20));   // 检查时间

        // 获取原始图层的所有要素
        QgsFeatureRequest request;
        QgsFeatureIterator iterator = sourceLayer->getFeatures(request);
        QgsFeature sourceFeature;

        while (iterator.nextFeature(sourceFeature))
        {
            QVariant featureId = sourceFeature.id();

            // 检查这个要素是否有问题
            if (problemFeatures.contains(featureId))
            {
                const QList<AnnotationCheckResult> &featureErrors = problemFeatures[featureId];

                // 创建一个要素，包含所有错误信息
                QgsFeature newFeature(outputFields);
                newFeature.setGeometry(sourceFeature.geometry());

                // 先复制所有原始属性
                QgsAttributes sourceAttrs = sourceFeature.attributes();
                QgsAttributes newAttrs = sourceAttrs;

                // 收集所有错误信息
                QStringList errorTypes, errorCodes, errorMsgs, actualVals, expectVals;

                for (const AnnotationCheckResult &error : featureErrors)
                {
                    // 确定错误类型信息
                    QString errorType, errorCode;
                    switch (error.errorType)
                    {
                    case AnnotationErrorType::FontError:
                        errorType = "FONT_ERROR";
                        errorCode = "FONT_ERR";
                        break;
                    case AnnotationErrorType::FontSizeError:
                        errorType = "SIZE_ERROR";
                        errorCode = "SIZE_ERR";
                        break;
                    case AnnotationErrorType::ColorError:
                        errorType = "COLOR_ERROR";
                        errorCode = "COLOR_ERR";
                        break;
                    case AnnotationErrorType::ItalicError:
                        errorType = "ITALIC_ERROR";
                        errorCode = "ITALIC_ERR";
                        break;
                    default:
                        errorType = "OTHER_ERROR";
                        errorCode = "OTHER_ERR";
                        break;
                    }

                    errorTypes.append(errorType);
                    errorCodes.append(errorCode);
                    errorMsgs.append(error.errorMessage);
                    actualVals.append(error.actualValue);
                    expectVals.append(error.expectedValue);
                }

                // 将多个错误信息用分号连接
                QString allErrorTypes = errorTypes.join("; ");
                QString allErrorCodes = errorCodes.join("; ");
                QString allErrorMsgs = errorMsgs.join("; ");
                QString allActualVals = actualVals.join("; ");
                QString allExpectVals = expectVals.join("; ");

                // 添加错误信息字段到属性数组
                newAttrs.append(featureId.toString());               // 要素ID
                newAttrs.append(featureErrors.size());               // 错误总数
                newAttrs.append(truncateString(allErrorTypes, 100)); // 所有错误类型
                newAttrs.append(truncateString(allErrorCodes, 50));  // 所有错误代码
                newAttrs.append(truncateString(allErrorMsgs, 254));  // 所有错误描述
                newAttrs.append(truncateString(allActualVals, 200)); // 所有实际值
                newAttrs.append(truncateString(allExpectVals, 200)); // 所有期望值
                newAttrs.append(currentTime);                        // 检查时间

                newFeature.setAttributes(newAttrs);
                features.append(newFeature);
            }
        }

        if (features.isEmpty())
        {
            return SE_ERROR_NONE;
        }

        // 使用QgsVectorFileWriter写入
        QgsVectorFileWriter writer(
            fullOutputPath,
            "UTF-8",
            outputFields,
            sourceLayer->wkbType(),
            sourceLayer->crs(),
            "ESRI Shapefile");

        if (writer.hasError() != QgsVectorFileWriter::NoError)
        {
            return SE_ERROR_WRITE_FILE_FAILED;
        }

        // 逐个添加要素
        for (QgsFeature &feature : features)
        {
            if (!writer.addFeature(feature))
            {
                return SE_ERROR_WRITE_FILE_FAILED;
            }
        }

        // 验证文件是否成功创建
        if (!QFile::exists(fullOutputPath))
        {
            return SE_ERROR_WRITE_FILE_FAILED;
        }

        return SE_ERROR_NONE;
    }
    catch (...)
    {
        QgsMessageLog::logMessage("写入问题图层时发生未知异常",
                                  "AnnotationCheck", Qgis::Critical);
        return SE_ERROR_WRITE_FILE_FAILED;
    }
}

// 添加字符串截断辅助函数
static QString truncateString(const QString &str, int maxLength)
{
    if (str.length() <= maxLength)
    {
        return str;
    }
    return str.left(maxLength - 3) + "...";
}

std::shared_ptr<spdlog::logger> createLogger(const char *szOutputLogFilePath, const QString &loggerName)
{
    try
    {
#ifdef _WIN32
        QString originalPath = QString::fromUtf8(szOutputLogFilePath);
        // 如果是 .log 结尾，改为 .txt
        QString logPath = originalPath;
        // if (logPath.endsWith(".log", Qt::CaseInsensitive))
        // {
        //     logPath.chop(4);
        //     logPath += ".txt";
        // }
        std::string nativePath = logPath.toLocal8Bit().toStdString();
#else
        QString originalPath = QString::fromUtf8(szOutputLogFilePath);
        QString logPath = originalPath;
        if (logPath.endsWith(".log", Qt::CaseInsensitive))
        {
            logPath.chop(4);
            logPath += ".txt";
        }
        std::string nativePath = logPath.toStdString();
#endif
        QString uniqueLoggerName = QString("%1_%2")
                                       .arg(loggerName)
                                       .arg(QDateTime::currentMSecsSinceEpoch());

        if (auto existing = spdlog::get(uniqueLoggerName.toStdString()))
        {
            spdlog::drop(uniqueLoggerName.toStdString());
        }

        // 目录
        QFileInfo fi(logPath);
        QDir dir = fi.dir();
        if (!dir.exists())
        {
            dir.mkpath(".");
        }

        // 预创建文件并写入 BOM（避免后续 truncate 覆盖）
        {
#ifdef _WIN32
            QFile f(logPath);
#else
            QFile f(logPath);
#endif
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            {
                static const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
                f.write(reinterpret_cast<const char *>(bom), 3);
                f.close();
            }
        }

#ifdef _WIN32
        // 使用追加模式（truncate=false），保留刚写入的 BOM
        spdlog::file_event_handlers handlers;
        // 不再在 after_open 写 BOM，已预写
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(nativePath, false, handlers);
        auto logger = std::make_shared<spdlog::logger>(uniqueLoggerName.toStdString(), sink);
        spdlog::register_logger(logger);
#else
        auto logger = spdlog::basic_logger_mt(uniqueLoggerName.toStdString(), nativePath);
#endif

        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");

        logger->info("日志文件已创建 (UTF-8+BOM): {}", logPath.toUtf8().constData());

        return logger;
    }
    catch (const spdlog::spdlog_ex &ex)
    {
        QgsMessageLog::logMessage(QString("创建日志记录器失败: %1").arg(ex.what()),
                                  "AnnotationCheck", Qgis::Critical);
        return nullptr;
    }
}

static SE_Error AnnotationMatchFeaturesInternal(
    const char *standardLayer,
    const char *gdbLayerPath,
    const char *szRuleXmlPath,
    const char *actualLayerPath,
    const char *actualAnnoLayerPath,
    int scale, // 修改为 int
    const char *outputTempLayerPath,
    bool outputUnmatchedAnnotations)
{
    if (!standardLayer || !gdbLayerPath || !szRuleXmlPath || /* actualLayerPath can be empty */
        !actualAnnoLayerPath || scale <= 0 || !outputTempLayerPath)
    {
        QgsMessageLog::logMessage(
            "注记匹配接口：参数无效，无法执行未匹配注记输出判定后的算法调用",
            "AnnotationCheck", Qgis::Info);
        return SE_ERROR_INVALID_PARAMETER;
    }

    QgsMessageLog::logMessage(
        QString("注记匹配接口：standardLayer=%1，actualAnnoLayerPath=%2，outputTempLayerPath=%3，outputUnmatchedAnnotations=%4")
            .arg(QString::fromUtf8(standardLayer),
                 QString::fromUtf8(actualAnnoLayerPath),
                 QString::fromUtf8(outputTempLayerPath),
                 outputUnmatchedAnnotations ? "true" : "false"),
        "AnnotationCheck", Qgis::Info);

    // 检查规则 XML 文件是否存在
    QString ruleXmlPath = QString::fromUtf8(szRuleXmlPath);
    if (!QFileInfo::exists(ruleXmlPath))
    {
        qWarning() << "Rule XML not found:" << ruleXmlPath;
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 解析规则 XML，从 id=200 的 check_item 中读出三个配置
    QString levelAttr;
    QString levelValuesCsv;
    QString gdbLabelField;

    QFile xmlFile(ruleXmlPath);
    if (!xmlFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qWarning() << "Cannot open rule XML:" << ruleXmlPath;
        return SE_ERROR_FILE_NOT_FOUND;
    }

    QDomDocument doc;
    QString domErrorMsg;
    int errLine = 0, errColumn = 0;
    if (!doc.setContent(&xmlFile, false, &domErrorMsg, &errLine, &errColumn))
    {
        qWarning() << "Failed to parse rule XML:" << domErrorMsg << "line:" << errLine << "col:" << errColumn;
        xmlFile.close();
        return SE_ERROR_CORRUPT_DATA;
    }
    xmlFile.close();

    QDomNodeList scaleNodes = doc.documentElement().elementsByTagName("scale");
    if (scaleNodes.isEmpty())
    {
        qWarning() << "No scale node found in rule XML:" << ruleXmlPath;
        return SE_ERROR_CORRUPT_DATA;
    }

    QDomElement selectedScale;
    QDomElement emptyScale;
    for (int i = 0; i < scaleNodes.count(); ++i)
    {
        QDomElement scaleElement = scaleNodes.at(i).toElement();
        QString scaleValue = scaleElement.firstChildElement("value").text().trimmed();
        if (scaleValue.isEmpty() && emptyScale.isNull())
            emptyScale = scaleElement;

        bool scaleOk = false;
        double configuredScale = scaleValue.toDouble(&scaleOk);
        if (scaleOk && qFuzzyCompare(configuredScale + 1.0, static_cast<double>(scale) + 1.0))
        {
            selectedScale = scaleElement;
            break;
        }
    }

    if (selectedScale.isNull())
        selectedScale = !emptyScale.isNull() ? emptyScale : scaleNodes.at(0).toElement();

    bool found = false;
    for (QDomElement checkItem = selectedScale.firstChildElement("check_item");
         !checkItem.isNull();
         checkItem = checkItem.nextSiblingElement("check_item"))
    {
        QString idText = checkItem.firstChildElement("id").text().trimmed();
        if (idText == "200")
        {
            found = true;
            for (QDomElement rule = checkItem.firstChildElement("rule");
                 !rule.isNull();
                 rule = rule.nextSiblingElement("rule"))
            {
                QString name = rule.firstChildElement("name").text().trimmed();
                QString val = rule.firstChildElement("value").text().trimmed();
                if (name.contains(QStringLiteral("属性项")))
                {
                    levelAttr = val;
                }
                else if (name.contains(QStringLiteral("属性值")))
                {
                    levelValuesCsv = val;
                }
                else if (name.contains(QStringLiteral("标注字段")))
                {
                    gdbLabelField = val;
                }
            }
            break;
        }
    }

    if (!found)
    {
        qWarning() << "Rule id=200 not found in XML:" << ruleXmlPath;
        return SE_ERROR_CORRUPT_DATA;
    }

    // 规整化 levelValues 的分隔符（兼容中文逗号/分号）
    if (!levelValuesCsv.isEmpty())
    {
        levelValuesCsv.replace(QChar(u'，'), ',');
        levelValuesCsv.replace(QChar(u'；'), ';');
    }

    qDebug() << "Parsed rule 200:" << "levelAttr=" << levelAttr << "levelValuesCsv=" << levelValuesCsv << "gdbLabelField=" << gdbLabelField;

    try
    {
        QString errMsg;
        // 调用现有匹配逻辑（matchSingleRule 接受上面解析得到的三个字符串）
        MatchResult matchResult = FeatureMatcher::instance().matchSingleRule(
            QString::fromUtf8(standardLayer),
            QString::fromUtf8(gdbLayerPath),
            levelAttr,
            levelValuesCsv,
            QString::fromUtf8(actualLayerPath),
            QString::fromUtf8(actualAnnoLayerPath),
            gdbLabelField,
            QString::number(scale), // 直接传分母字符串
            errMsg);

        FeatureMatcher::instance().clear();

        if (matchResult.links.isEmpty())
        {
            QgsMessageLog::logMessage(
                QString("注记匹配接口：匹配结果为空，standardLayer=%1，outputUnmatchedAnnotations=%2，本次不会生成临时匹配图层")
                    .arg(QString::fromUtf8(standardLayer), outputUnmatchedAnnotations ? "true" : "false"),
                "AnnotationCheck", Qgis::Info);
            qWarning() << "Feature matching returned no results:" << errMsg;
            return SE_ERROR_NONE;
        }

        // 之后原先流程不变：重载图层、createTempStandardLayer 等
        // 重新加载图层用于输出 (这是必须的，因为 clear() 已经释放了它们)
        QgsVectorLayer *annoLayer = new QgsVectorLayer(QString::fromUtf8(actualAnnoLayerPath), "anno", "ogr");
        QgsVectorLayer *gdbLayer = new QgsVectorLayer(QString::fromUtf8(gdbLayerPath), "gdb", "ogr");
        QgsVectorLayer *actualLayer = nullptr;
        if (actualLayerPath && strlen(actualLayerPath) > 0)
        {
            actualLayer = new QgsVectorLayer(QString::fromUtf8(actualLayerPath), "actual", "ogr");
        }

        if (!annoLayer->isValid() || !gdbLayer->isValid() || (actualLayer && !actualLayer->isValid()))
        {
            qWarning() << "Failed to reload layers for output creation.";
            delete annoLayer;
            delete gdbLayer;
            delete actualLayer;
            return SE_ERROR_CORRUPT_DATA;
        }

        // 计算搜索半径（与匹配阶段保持一致：0.02 * scaleDenominator）
        double nearbyRadius = (20.0 / 1000.0) * static_cast<double>(scale);

        // 解析TYPE过滤参数（与匹配阶段的levelValues一致）
        QSet<QString> levelValues;
        int gdbLevelIdx = -1;
        if (!levelValuesCsv.trimmed().isEmpty())
        {
            gdbLevelIdx = gdbLayer->fields().indexOf(levelAttr);
            const QStringList parts = levelValuesCsv.split(QRegularExpression("[,;]"), Qt::SkipEmptyParts);
            for (const QString &p : parts)
            {
                QString norm = p.trimmed().replace(" ", "").toLower();
                levelValues.insert(norm);
            }
        }

        // 3. 调用新的 createTempStandardLayer，传入分组信息
        bool success = FeatureMatcher::instance().createTempStandardLayer(
            matchResult.links,
            matchResult.annoToGroupMap, // <-- 传入分组Map
            annoLayer, gdbLayer, actualLayer, QString::fromUtf8(outputTempLayerPath),
            outputUnmatchedAnnotations,
            nearbyRadius,
            gdbLevelIdx,
            levelValues,
            errMsg);

        delete annoLayer;
        delete gdbLayer;
        delete actualLayer;

        if (!success)
        {
            qWarning() << "Failed to create temp layer:" << errMsg;
            return SE_ERROR_WRITE_FILE_FAILED;
        }

        qDebug() << "Successfully created temp standard layer with" << matchResult.links.size() << "matches.";
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        qCritical() << "Exception in AnnotationMatchFeatures:" << e.what();
        return SE_ERROR_FAILURE;
    }
    catch (...)
    {
        qCritical() << "Unknown exception in AnnotationMatchFeatures";
        return SE_ERROR_FAILURE;
    }
}

MY_PLUGIN_API SE_Error AnnotationMatchFeatures(
    const char *standardLayer,
    const char *gdbLayerPath,
    const char *szRuleXmlPath,
    const char *actualLayerPath,
    const char *actualAnnoLayerPath,
    int scale, // 修改为 int
    const char *outputTempLayerPath,
    int outputUnmatchedAnnotations)
{
    return AnnotationMatchFeaturesInternal(
        standardLayer,
        gdbLayerPath,
        szRuleXmlPath,
        actualLayerPath,
        actualAnnoLayerPath,
        scale,
        outputTempLayerPath,
        outputUnmatchedAnnotations != 0);
}