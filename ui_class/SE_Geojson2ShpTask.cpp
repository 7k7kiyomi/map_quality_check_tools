//#include "se_annotation_check_interface.h"
//#include "se_annotation_algorithm.h"
//#include <QString>
//#include <QFileInfo>
//#include <QDir>
//#include <QTextStream>
//#include <QDateTime>
//#include <QDebug> // 添加调试输出
//#include <QgsVectorLayer.h>
//#include <QgsVectorFileWriter.h>
//#include <QgsFields.h>
//#include <QgsField.h>
//#include <QgsFeature.h>
//#include <QgsGeometry.h> // 添加几何相关
//#include <QgsPointXY.h>  // 添加点几何
//#include <qgsmessagelog.h>
//#include <spdlog/spdlog.h>
//#include <spdlog/sinks/basic_file_sink.h>
//
//// 内部辅助函数声明
//static bool validateInputPaths(const char *szShpFilePath, const char *szSchemaXmlPath, const char *szOutputLogFilePath);
//static bool createOutputDirectories(const char *szOutputCheckReportPath, const char *szOutputCheckResultLayerPath);
//static SE_Error writeCheckReport(const QList<AnnotationCheckResult> &results, const char *szOutputCheckReportPath, const QString &checkType);
//static SE_Error writeCheckResultLayer(const QList<AnnotationCheckResult> &results, QgsVectorLayer *sourceLayer, const char *szOutputCheckResultLayerPath);
//static std::shared_ptr<spdlog::logger> createLogger(const char *szOutputLogFilePath, const QString &loggerName);
//static QString truncateString(const QString &str, int maxLength);
//
//SE_Error AnnotationCheck(
//    const char *szShpFilePath,
//    const char *szSchemaXmlPath,
//    const char *szOutputCheckReportPath,
//    const char *szOutputCheckResultLayerPath,
//    const char *szOutputLogFilePath)
//{
//    // 参数验证
//    if (!szShpFilePath || !szSchemaXmlPath || !szOutputCheckReportPath ||
//        !szOutputCheckResultLayerPath || !szOutputLogFilePath)
//    {
//        return SE_ERROR_INVALID_PARAMETER;
//    }
//
//    // 验证输入文件路径
//    if (!validateInputPaths(szShpFilePath, szSchemaXmlPath, szOutputLogFilePath))
//    {
//        return SE_ERROR_FILE_NOT_FOUND;
//    }
//
//    // 创建输出目录
//    if (!createOutputDirectories(szOutputCheckReportPath, szOutputCheckResultLayerPath))
//    {
//        return SE_ERROR_WRITE_FILE_FAILED;
//    }
//
//    // 创建日志记录器
//    auto logger = createLogger(szOutputLogFilePath, "AnnotationCheck");
//    if (!logger)
//    {
//        return SE_ERROR_WRITE_FILE_FAILED;
//    }
//
//    logger->info("开始执行注记检查算法");
//    logger->info("输入文件: {}", szShpFilePath);
//    logger->info("质检方案: {}", szSchemaXmlPath);
//
//    try
//    {
//        // 加载shp文件
//        QString layerPath = QString::fromUtf8(szShpFilePath);
//        QgsVectorLayer *layer = new QgsVectorLayer(layerPath, "annotation_layer", "ogr");
//
//        if (!layer->isValid())
//        {
//            logger->error("无法加载shp文件: {}", szShpFilePath);
//            delete layer;
//            spdlog::drop(logger->name()); // 释放 logger
//            return SE_ERROR_CORRUPT_DATA;
//        }
//
//        logger->info("成功加载图层，要素数量: {}", layer->featureCount());
//
//        // 创建算法实例（内置字体映射已自动加载）
//        SE_AnnotationAlgorithm algorithm;
//        logger->info("字体映射已初始化（使用内置映射）");
//
//        // 加载质检规则
//        QString schemaPath = QString::fromUtf8(szSchemaXmlPath);
//        if (!algorithm.loadCheckRules(schemaPath))
//        {
//            logger->error("加载质检规则失败: {}", szSchemaXmlPath);
//            delete layer;
//            spdlog::drop(logger->name()); // 释放 logger
//            return SE_ERROR_CORRUPT_DATA;
//        }
//
//        // 设置检查选项（全部检查）
//        algorithm.setCheckOptions(true, true, true,true);
//
//        // 执行检查
//        logger->info("开始执行质检");
//        QList<AnnotationCheckResult> results = algorithm.checkAnnotationLayer(layer);
//
//        logger->info("质检完成，发现问题数量: {}", results.size());
//
//        // 写入检查报告
//        SE_Error reportResult = writeCheckReport(results, szOutputCheckReportPath, "注记综合检查");
//        if (reportResult != SE_ERROR_NONE)
//        {
//            logger->error("写入检查报告失败");
//            delete layer;
//            spdlog::drop(logger->name()); // 释放 logger
//            return reportResult;
//        }
//
//        // 写入问题要素图层（如果有问题）
//        if (!results.isEmpty())
//        {
//            SE_Error layerResult = writeCheckResultLayer(results, layer, szOutputCheckResultLayerPath);
//            if (layerResult != SE_ERROR_NONE)
//            {
//                logger->error("写入问题要素图层失败");
//                delete layer;
//                spdlog::drop(logger->name()); // 释放 logger
//                return layerResult;
//            }
//        }
//
//        delete layer;
//        logger->info("注记检查算法执行完成");
//
//        // 释放 logger
//        spdlog::drop(logger->name());
//
//        return SE_ERROR_NONE;
//    }
//    catch (const std::exception &e)
//    {
//        logger->error("执行过程中发生异常: {}", e.what());
//        spdlog::drop(logger->name()); // 释放 logger
//        return SE_ERROR_FAILURE;
//    }
//}
//
//SE_Error AnnotationFontCheck(
//    const char *szShpFilePath,
//    const char *szSchemaXmlPath,
//    const char *szOutputCheckReportPath,
//    const char *szOutputCheckResultLayerPath,
//    const char *szOutputLogFilePath)
//{
//    // 参数验证
//    if (!szShpFilePath || !szSchemaXmlPath || !szOutputCheckReportPath ||
//        !szOutputCheckResultLayerPath || !szOutputLogFilePath)
//    {
//        return SE_ERROR_INVALID_PARAMETER;
//    }
//
//    // 验证输入文件路径
//    if (!validateInputPaths(szShpFilePath, szSchemaXmlPath, szOutputLogFilePath))
//    {
//        return SE_ERROR_FILE_NOT_FOUND;
//    }
//
//    // 创建输出目录
//    if (!createOutputDirectories(szOutputCheckReportPath, szOutputCheckResultLayerPath))
//    {
//        return SE_ERROR_WRITE_FILE_FAILED;
//    }
//
//    // 创建日志记录器
//    auto logger = createLogger(szOutputLogFilePath, "AnnotationFontCheck");
//    if (!logger)
//    {
//        return SE_ERROR_WRITE_FILE_FAILED;
//    }
//
//    logger->info("开始执行注记字体检查算法");
//
//    try
//    {
//        // 加载shp文件
//        QString layerPath = QString::fromUtf8(szShpFilePath);
//        QgsVectorLayer *layer = new QgsVectorLayer(layerPath, "annotation_layer", "ogr");
//
//        if (!layer->isValid())
//        {
//            logger->error("无法加载shp文件: {}", szShpFilePath);
//            delete layer;
//            return SE_ERROR_CORRUPT_DATA;
//        }
//
//        // 创建算法实例（内置字体映射已自动加载）
//        SE_AnnotationAlgorithm algorithm;
//
//        // 加载质检规则
//        QString schemaPath = QString::fromUtf8(szSchemaXmlPath);
//        if (!algorithm.loadCheckRules(schemaPath))
//        {
//            logger->error("加载质检规则失败: {}", szSchemaXmlPath);
//            delete layer;
//            return SE_ERROR_CORRUPT_DATA;
//        }
//
//        // 设置只检查字体
//        algorithm.setCheckOptions(true, false, false,false);
//
//        // 执行字体检查
//        QList<AnnotationCheckResult> results = algorithm.checkFont(layer);
//
//        logger->info("字体检查完成，发现问题数量: {}", results.size());
//
//        // 写入检查报告
//        SE_Error reportResult = writeCheckReport(results, szOutputCheckReportPath, "注记字体检查");
//        if (reportResult != SE_ERROR_NONE)
//        {
//            delete layer;
//            return reportResult;
//        }
//
//        // 写入问题要素图层（如果有问题）
//        if (!results.isEmpty())
//        {
//            SE_Error layerResult = writeCheckResultLayer(results, layer, szOutputCheckResultLayerPath);
//            if (layerResult != SE_ERROR_NONE)
//            {
//                delete layer;
//                return layerResult;
//            }
//        }
//
//        delete layer;
//        logger->info("注记字体检查算法执行完成");
//        return SE_ERROR_NONE;
//    }
//    catch (const std::exception &e)
//    {
//        logger->error("执行过程中发生异常: {}", e.what());
//        return SE_ERROR_FAILURE;
//    }
//}
//
//SE_Error AnnotationFontSizeCheck(
//    const char *szShpFilePath,
//    const char *szSchemaXmlPath,
//    const char *szOutputCheckReportPath,
//    const char *szOutputCheckResultLayerPath,
//    const char *szOutputLogFilePath)
//{
//    // 参数验证
//    if (!szShpFilePath || !szSchemaXmlPath || !szOutputCheckReportPath ||
//        !szOutputCheckResultLayerPath || !szOutputLogFilePath)
//    {
//        return SE_ERROR_INVALID_PARAMETER;
//    }
//
//    // 验证输入文件路径
//    if (!validateInputPaths(szShpFilePath, szSchemaXmlPath, szOutputLogFilePath))
//    {
//        return SE_ERROR_FILE_NOT_FOUND;
//    }
//
//    // 创建输出目录
//    if (!createOutputDirectories(szOutputCheckReportPath, szOutputCheckResultLayerPath))
//    {
//        return SE_ERROR_WRITE_FILE_FAILED;
//    }
//
//    // 创建日志记录器
//    auto logger = createLogger(szOutputLogFilePath, "AnnotationFontSizeCheck");
//    if (!logger)
//    {
//        return SE_ERROR_WRITE_FILE_FAILED;
//    }
//
//    logger->info("开始执行注记字号检查算法");
//
//    try
//    {
//        // 加载shp文件
//        QString layerPath = QString::fromUtf8(szShpFilePath);
//        QgsVectorLayer *layer = new QgsVectorLayer(layerPath, "annotation_layer", "ogr");
//
//        if (!layer->isValid())
//        {
//            logger->error("无法加载shp文件: {}", szShpFilePath);
//            delete layer;
//            return SE_ERROR_CORRUPT_DATA;
//        }
//
//        // 创建算法实例（内置字体映射已自动加载）
//        SE_AnnotationAlgorithm algorithm;
//
//        // 加载质检规则
//        QString schemaPath = QString::fromUtf8(szSchemaXmlPath);
//        if (!algorithm.loadCheckRules(schemaPath))
//        {
//            logger->error("加载质检规则失败: {}", szSchemaXmlPath);
//            delete layer;
//            return SE_ERROR_CORRUPT_DATA;
//        }
//
//        // 设置只检查字号
//        algorithm.setCheckOptions(false, true, false,false);
//
//        // 执行字号检查
//        QList<AnnotationCheckResult> results = algorithm.checkFontSize(layer);
//
//        logger->info("字号检查完成，发现问题数量: {}", results.size());
//
//        // 写入检查报告
//        SE_Error reportResult = writeCheckReport(results, szOutputCheckReportPath, "注记字号检查");
//        if (reportResult != SE_ERROR_NONE)
//        {
//            delete layer;
//            return reportResult;
//        }
//
//        // 写入问题要素图层（如果有问题）
//        if (!results.isEmpty())
//        {
//            SE_Error layerResult = writeCheckResultLayer(results, layer, szOutputCheckResultLayerPath);
//            if (layerResult != SE_ERROR_NONE)
//            {
//                delete layer;
//                return layerResult;
//            }
//        }
//
//        delete layer;
//        logger->info("注记字号检查算法执行完成");
//        return SE_ERROR_NONE;
//    }
//    catch (const std::exception &e)
//    {
//        logger->error("执行过程中发生异常: {}", e.what());
//        return SE_ERROR_FAILURE;
//    }
//}
//
//SE_Error AnnotationColorCheck(
//    const char *szShpFilePath,
//    const char *szSchemaXmlPath,
//    const char *szOutputCheckReportPath,
//    const char *szOutputCheckResultLayerPath,
//    const char *szOutputLogFilePath)
//{
//    // 参数验证
//    if (!szShpFilePath || !szSchemaXmlPath || !szOutputCheckReportPath ||
//        !szOutputCheckResultLayerPath || !szOutputLogFilePath)
//    {
//        return SE_ERROR_INVALID_PARAMETER;
//    }
//
//    // 验证输入文件路径
//    if (!validateInputPaths(szShpFilePath, szSchemaXmlPath, szOutputLogFilePath))
//    {
//        return SE_ERROR_FILE_NOT_FOUND;
//    }
//
//    // 创建输出目录
//    if (!createOutputDirectories(szOutputCheckReportPath, szOutputCheckResultLayerPath))
//    {
//        return SE_ERROR_WRITE_FILE_FAILED;
//    }
//
//    // 创建日志记录器
//    auto logger = createLogger(szOutputLogFilePath, "AnnotationColorCheck");
//    if (!logger)
//    {
//        return SE_ERROR_WRITE_FILE_FAILED;
//    }
//
//    logger->info("开始执行注记颜色检查算法");
//
//    try
//    {
//        // 加载shp文件
//        QString layerPath = QString::fromUtf8(szShpFilePath);
//        QgsVectorLayer *layer = new QgsVectorLayer(layerPath, "annotation_layer", "ogr");
//
//        if (!layer->isValid())
//        {
//            logger->error("无法加载shp文件: {}", szShpFilePath);
//            delete layer;
//            return SE_ERROR_CORRUPT_DATA;
//        }
//
//        // 创建算法实例（内置字体映射已自动加载）
//        SE_AnnotationAlgorithm algorithm;
//
//        // 加载质检规则
//        QString schemaPath = QString::fromUtf8(szSchemaXmlPath);
//        if (!algorithm.loadCheckRules(schemaPath))
//        {
//            logger->error("加载质检规则失败: {}", szSchemaXmlPath);
//            delete layer;
//            return SE_ERROR_CORRUPT_DATA;
//        }
//
//        // 设置只检查颜色
//        algorithm.setCheckOptions(false, false, true,false);
//
//        // 执行颜色检查
//        QList<AnnotationCheckResult> results = algorithm.checkColor(layer);
//
//        logger->info("颜色检查完成，发现问题数量: {}", results.size());
//
//        // 写入检查报告
//        SE_Error reportResult = writeCheckReport(results, szOutputCheckReportPath, "注记颜色检查");
//        if (reportResult != SE_ERROR_NONE)
//        {
//            delete layer;
//            return reportResult;
//        }
//
//        // 写入问题要素图层（如果有问题）
//        if (!results.isEmpty())
//        {
//            SE_Error layerResult = writeCheckResultLayer(results, layer, szOutputCheckResultLayerPath);
//            if (layerResult != SE_ERROR_NONE)
//            {
//                delete layer;
//                return layerResult;
//            }
//        }
//
//        delete layer;
//        logger->info("注记颜色检查算法执行完成");
//        return SE_ERROR_NONE;
//    }
//    catch (const std::exception &e)
//    {
//        logger->error("执行过程中发生异常: {}", e.what());
//        return SE_ERROR_FAILURE;
//    }
//}
//
//// 内部辅助函数实现
//static bool validateInputPaths(const char *szShpFilePath, const char *szSchemaXmlPath, const char *szOutputLogFilePath)
//{
//    // 检查shp文件是否存在
//    if (!QFileInfo::exists(QString::fromUtf8(szShpFilePath)))
//    {
//        return false;
//    }
//
//    // 检查质检方案文件是否存在
//    if (!QFileInfo::exists(QString::fromUtf8(szSchemaXmlPath)))
//    {
//        return false;
//    }
//
//    // 检查日志文件目录是否存在
//    QString logDir = QFileInfo(QString::fromUtf8(szOutputLogFilePath)).dir().absolutePath();
//    if (!QDir(logDir).exists())
//    {
//        QDir().mkpath(logDir);
//    }
//
//    return true;
//}
//
//static bool createOutputDirectories(const char *szOutputCheckReportPath, const char *szOutputCheckResultLayerPath)
//{
//    // 创建报告文件目录
//    QString reportDir = QFileInfo(QString::fromUtf8(szOutputCheckReportPath)).dir().absolutePath();
//    if (!QDir(reportDir).exists())
//    {
//        if (!QDir().mkpath(reportDir))
//        {
//            return false;
//        }
//    }
//
//    // 创建结果图层目录
//    QString layerDir = QFileInfo(QString::fromUtf8(szOutputCheckResultLayerPath)).dir().absolutePath();
//    if (!QDir(layerDir).exists())
//    {
//        if (!QDir().mkpath(layerDir))
//        {
//            return false;
//        }
//    }
//
//    return true;
//}
//
//static SE_Error writeCheckReport(const QList<AnnotationCheckResult> &results, const char *szOutputCheckReportPath, const QString &checkType)
//{
//    QString reportPath = QString::fromUtf8(szOutputCheckReportPath);
//
//    // 确保目录存在
//    QFileInfo fileInfo(reportPath);
//    QDir dir = fileInfo.dir();
//    if (!dir.exists())
//    {
//        if (!dir.mkpath("."))
//        {
//            return SE_ERROR_WRITE_FILE_FAILED;
//        }
//    }
//
//    // 如果文件已存在，删除它
//    if (QFile::exists(reportPath))
//    {
//        if (!QFile::remove(reportPath))
//        {
//            return SE_ERROR_WRITE_FILE_FAILED;
//        }
//    }
//
//    QFile reportFile(reportPath);
//    if (!reportFile.open(QIODevice::WriteOnly | QIODevice::Text))
//    {
//        return SE_ERROR_WRITE_FILE_FAILED;
//    }
//
//    // 使用 RAII 确保文件正确关闭
//    QTextStream stream(&reportFile);
//    stream.setCodec("UTF-8");
//    stream.setGenerateByteOrderMark(true);
//
//    // 写入报告头部
//    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
//    stream << checkType << QString::fromUtf8("报告") << "\n";
//    stream << "=" << QString("=").repeated(50) << "\n";
//    stream << QString::fromUtf8("检查时间: ") << timestamp << "\n";
//    stream << QString::fromUtf8("总问题数: ") << results.size() << "\n\n";
//
//    // 按错误类型统计
//    QMap<AnnotationErrorType, int> errorStats;
//    for (const AnnotationCheckResult &result : results)
//    {
//        errorStats[result.errorType]++;
//    }
//
//    stream << QString::fromUtf8("问题统计:") << "\n";
//    stream << "-" << QString("-").repeated(30) << "\n";
//    for (auto it = errorStats.begin(); it != errorStats.end(); ++it)
//    {
//        QString errorTypeName;
//        switch (it.key())
//        {
//        case AnnotationErrorType::FontError:
//            errorTypeName = QString::fromUtf8("字体错误");
//            break;
//        case AnnotationErrorType::FontSizeError:
//            errorTypeName = QString::fromUtf8("字号错误");
//            break;
//        case AnnotationErrorType::ColorError:
//            errorTypeName = QString::fromUtf8("颜色错误");
//            break;
//        default:
//            errorTypeName = QString::fromUtf8("其他错误");
//            break;
//        }
//        stream << errorTypeName << ": " << it.value() << QString::fromUtf8(" 个") << "\n";
//    }
//
//    stream << "\n"
//           << QString::fromUtf8("详细问题列表:") << "\n";
//    stream << "-" << QString("-").repeated(50) << "\n";
//
//    // 写入详细问题列表
//    for (int i = 0; i < results.size(); ++i)
//    {
//        const AnnotationCheckResult &result = results[i];
//        stream << QString::fromUtf8("问题 %1:").arg(i + 1) << "\n";
//        stream << QString::fromUtf8("  图层: ") << result.layerName << "\n";
//        stream << QString::fromUtf8("  要素ID: ") << result.featureId << "\n";
//        stream << QString::fromUtf8("  错误描述: ") << result.errorMessage << "\n";
//        stream << QString::fromUtf8("  实际值: ") << result.actualValue << "\n";
//        stream << QString::fromUtf8("  期望值: ") << result.expectedValue << "\n";
//        stream << QString::fromUtf8("  位置: (") << result.position.x() << ", " << result.position.y() << ")\n";
//        stream << "\n";
//    }
//
//    reportFile.close();
//
//    // 验证文件是否写入成功
//    if (!QFile::exists(reportPath))
//    {
//        return SE_ERROR_WRITE_FILE_FAILED;
//    }
//
//    return SE_ERROR_NONE;
//}
//
//static SE_Error writeCheckResultLayer(const QList<AnnotationCheckResult> &results, QgsVectorLayer *sourceLayer, const char *szOutputCheckResultLayerPath)
//{
//    if (results.isEmpty())
//    {
//        return SE_ERROR_NONE;
//    }
//
//    try
//    {
//        // 生成输出路径
//        QString outputPath = QString::fromUtf8(szOutputCheckResultLayerPath);
//        QFileInfo outputInfo(outputPath);
//        QString sourceLayerName = QFileInfo(sourceLayer->source()).baseName();
//        QString problemLayerName = QString("%1_problems_%2")
//                                       .arg(sourceLayerName)
//                                       .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
//
//        QString fullOutputPath = QString("%1/%2.shp")
//                                     .arg(outputInfo.dir().absolutePath())
//                                     .arg(problemLayerName);
//
//        // 确保输出目录存在
//        QDir outputDir = outputInfo.dir();
//        if (!outputDir.exists())
//        {
//            if (!outputDir.mkpath("."))
//            {
//                return SE_ERROR_WRITE_FILE_FAILED;
//            }
//        }
//
//        // 删除已存在的文件
//        QStringList extensions = {"shp", "shx", "dbf", "prj", "cpg"};
//        QString baseName = QFileInfo(fullOutputPath).baseName();
//        QString dirPath = QFileInfo(fullOutputPath).dir().absolutePath();
//
//        for (const QString &ext : extensions)
//        {
//            QString filePath = QString("%1/%2.%3").arg(dirPath).arg(baseName).arg(ext);
//            if (QFile::exists(filePath))
//            {
//                QFile::remove(filePath);
//            }
//        }
//
//        // 创建问题要素ID到错误列表的映射
//        QMap<QVariant, QList<AnnotationCheckResult>> problemFeatures;
//        for (const AnnotationCheckResult &result : results)
//        {
//            problemFeatures[result.featureId].append(result);
//        }
//
//        // 准备要素列表
//        QList<QgsFeature> features;
//        QString currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm");
//
//        // 创建扩展的字段结构 - 原始字段加上错误信息字段
//        QgsFields outputFields = sourceLayer->fields();
//        outputFields.append(QgsField("FEATURE_ID", QVariant::String, "String", 20));   // 要素ID
//        outputFields.append(QgsField("ERROR_CNT", QVariant::Int, "Integer", 10));      // 错误总数
//        outputFields.append(QgsField("ERROR_TYPES", QVariant::String, "String", 100)); // 所有错误类型（用分号分隔）
//        outputFields.append(QgsField("ERROR_CODES", QVariant::String, "String", 50));  // 所有错误代码（用分号分隔）
//        outputFields.append(QgsField("ERROR_MSGS", QVariant::String, "String", 254));  // 所有错误描述（用分号分隔）
//        outputFields.append(QgsField("ACTUAL_VALS", QVariant::String, "String", 200)); // 所有实际值（用分号分隔）
//        outputFields.append(QgsField("EXPECT_VALS", QVariant::String, "String", 200)); // 所有期望值（用分号分隔）
//        outputFields.append(QgsField("CHECK_TIME", QVariant::String, "String", 20));   // 检查时间
//
//        // 获取原始图层的所有要素
//        QgsFeatureRequest request;
//        QgsFeatureIterator iterator = sourceLayer->getFeatures(request);
//        QgsFeature sourceFeature;
//
//        while (iterator.nextFeature(sourceFeature))
//        {
//            QVariant featureId = sourceFeature.id();
//
//            // 检查这个要素是否有问题
//            if (problemFeatures.contains(featureId))
//            {
//                const QList<AnnotationCheckResult> &featureErrors = problemFeatures[featureId];
//
//                // 创建一个要素，包含所有错误信息
//                QgsFeature newFeature(outputFields);
//                newFeature.setGeometry(sourceFeature.geometry());
//
//                // 先复制所有原始属性
//                QgsAttributes sourceAttrs = sourceFeature.attributes();
//                QgsAttributes newAttrs = sourceAttrs;
//
//                // 收集所有错误信息
//                QStringList errorTypes, errorCodes, errorMsgs, actualVals, expectVals;
//
//                for (const AnnotationCheckResult &error : featureErrors)
//                {
//                    // 确定错误类型信息
//                    QString errorType, errorCode;
//                    switch (error.errorType)
//                    {
//                    case AnnotationErrorType::FontError:
//                        errorType = "FONT_ERROR";
//                        errorCode = "FONT_ERR";
//                        break;
//                    case AnnotationErrorType::FontSizeError:
//                        errorType = "SIZE_ERROR";
//                        errorCode = "SIZE_ERR";
//                        break;
//                    case AnnotationErrorType::ColorError:
//                        errorType = "COLOR_ERROR";
//                        errorCode = "COLOR_ERR";
//                        break;
//                    default:
//                        errorType = "OTHER_ERROR";
//                        errorCode = "OTHER_ERR";
//                        break;
//                    }
//
//                    errorTypes.append(errorType);
//                    errorCodes.append(errorCode);
//                    errorMsgs.append(error.errorMessage);
//                    actualVals.append(error.actualValue);
//                    expectVals.append(error.expectedValue);
//                }
//
//                // 将多个错误信息用分号连接
//                QString allErrorTypes = errorTypes.join("; ");
//                QString allErrorCodes = errorCodes.join("; ");
//                QString allErrorMsgs = errorMsgs.join("; ");
//                QString allActualVals = actualVals.join("; ");
//                QString allExpectVals = expectVals.join("; ");
//
//                // 添加错误信息字段到属性数组
//                newAttrs.append(featureId.toString());               // 要素ID
//                newAttrs.append(featureErrors.size());               // 错误总数
//                newAttrs.append(truncateString(allErrorTypes, 100)); // 所有错误类型
//                newAttrs.append(truncateString(allErrorCodes, 50));  // 所有错误代码
//                newAttrs.append(truncateString(allErrorMsgs, 254));  // 所有错误描述
//                newAttrs.append(truncateString(allActualVals, 200)); // 所有实际值
//                newAttrs.append(truncateString(allExpectVals, 200)); // 所有期望值
//                newAttrs.append(currentTime);                        // 检查时间
//
//                newFeature.setAttributes(newAttrs);
//                features.append(newFeature);
//            }
//        }
//
//        if (features.isEmpty())
//        {
//            return SE_ERROR_NONE;
//        }
//
//        // 使用QgsVectorFileWriter写入
//        QgsVectorFileWriter writer(
//            fullOutputPath,
//            "UTF-8",
//            outputFields,
//            sourceLayer->wkbType(),
//            sourceLayer->crs(),
//            "ESRI Shapefile");
//
//        if (writer.hasError() != QgsVectorFileWriter::NoError)
//        {
//            return SE_ERROR_WRITE_FILE_FAILED;
//        }
//
//        // 逐个添加要素
//        for (QgsFeature &feature : features)
//        {
//            if (!writer.addFeature(feature))
//            {
//                return SE_ERROR_WRITE_FILE_FAILED;
//            }
//        }
//
//        // 验证文件是否成功创建
//        if (!QFile::exists(fullOutputPath))
//        {
//            return SE_ERROR_WRITE_FILE_FAILED;
//        }
//
//        return SE_ERROR_NONE;
//    }
//    catch (...)
//    {
//        QgsMessageLog::logMessage("写入问题图层时发生未知异常",
//                                  "AnnotationCheck", Qgis::Critical);
//        return SE_ERROR_WRITE_FILE_FAILED;
//    }
//}
//
//// 添加字符串截断辅助函数
//static QString truncateString(const QString &str, int maxLength)
//{
//    if (str.length() <= maxLength)
//    {
//        return str;
//    }
//    return str.left(maxLength - 3) + "...";
//}
//
//static std::shared_ptr<spdlog::logger> createLogger(const char *szOutputLogFilePath, const QString &loggerName)
//{
//    try
//    {
//        QString logPath = QString::fromUtf8(szOutputLogFilePath);
//        QString uniqueLoggerName = QString("%1_%2").arg(loggerName).arg(QDateTime::currentMSecsSinceEpoch());
//
//        // 检查 logger 是否已存在，如果存在则删除
//        if (spdlog::get(uniqueLoggerName.toStdString()))
//        {
//            spdlog::drop(uniqueLoggerName.toStdString());
//        }
//
//        // 确保日志目录存在
//        QFileInfo logFileInfo(logPath);
//        QDir logDir = logFileInfo.dir();
//        if (!logDir.exists())
//        {
//            logDir.mkpath(".");
//        }
//
//        // 如果日志文件已存在，尝试删除或重命名
//        if (QFile::exists(logPath))
//        {
//            QFile::remove(logPath);
//        }
//
//        auto logger = spdlog::basic_logger_mt(uniqueLoggerName.toStdString(), logPath.toStdString());
//        logger->set_level(spdlog::level::info);
//        logger->flush_on(spdlog::level::info);
//        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
//
//        return logger;
//    }
//    catch (const spdlog::spdlog_ex &ex)
//    {
//        QgsMessageLog::logMessage(QString("创建日志记录器失败: %1").arg(ex.what()),
//                                  "AnnotationCheck", Qgis::Critical);
//        return nullptr;
//    }
//}