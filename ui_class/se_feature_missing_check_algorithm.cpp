#define _HAS_STD_BYTE 0
#include "se_feature_missing_check_algorithm.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

// XML解析
#include <QDomDocument>
#include <QDomElement>

// GDAL字符串列表（用于设置Shapefile编码）
#include <cpl_string.h>
#include <cpl_conv.h>

/*spdlog日志库头文件*/
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"

// 编码转换辅助函数
static QString fromUtf8ToQString(const char* utf8Str)
{
    if (!utf8Str) return QString();
    return QString::fromUtf8(utf8Str);
}

static std::string fromQStringToUtf8(const QString& qstr)
{
    QByteArray utf8Bytes = qstr.toUtf8();
    return std::string(utf8Bytes.constData(), utf8Bytes.length());
}

static double MapLengthMmToLayerUnits(double mapMm, int scale, double linearUnitsInMeters)
{
    double meters = (mapMm * static_cast<double>(scale)) / 1000.0;
    return meters / linearUnitsInMeters;
}

static double MapAreaSqMmToLayerSqUnits(double mapSqMm, int scale, double linearUnitsInMeters)
{
    double metersPerLayerUnit = linearUnitsInMeters;
    double sqMeters = mapSqMm * static_cast<double>(scale) * static_cast<double>(scale) / 1000000.0;
    return sqMeters / (metersPerLayerUnit * metersPerLayerUnit);
}

static double NormalizeLengthValueToMapMm(double value, const QString& unit, int scale)
{
    if (scale <= 0)
    {
        return value;
    }
    QString u = unit.trimmed().toLower();
    u.replace(" ", "");
    if (u.isEmpty() || u == QStringLiteral("mm") || u.contains(QStringLiteral("毫米")))
    {
        return value;
    }
    if (u == QStringLiteral("cm") || u.contains(QStringLiteral("厘米")))
    {
        return value * 10.0;
    }
    if (u == QStringLiteral("km") || u.contains(QStringLiteral("千米")) || u.contains(QStringLiteral("公里")))
    {
        return value * 1000000.0 / static_cast<double>(scale);
    }
    if (u == QStringLiteral("m") || u == QStringLiteral("米"))
    {
        return value * 1000.0 / static_cast<double>(scale);
    }
    return value;
}

static double NormalizeAreaValueToMapSqMm(double value, const QString& unit, int scale)
{
    if (scale <= 0)
    {
        return value;
    }
    QString u = unit.trimmed().toLower();
    u.replace(" ", "");
    if (u.isEmpty() || u.contains(QStringLiteral("平方毫米")) || u.contains("mm2") || u.contains(QStringLiteral("㎜²")))
    {
        return value;
    }
    if (u.contains(QStringLiteral("平方厘米")) || u.contains("cm2") || u.contains(QStringLiteral("㎝²")))
    {
        return value * 100.0;
    }
    if (u.contains(QStringLiteral("平方千米")) || u.contains("km2") || u.contains(QStringLiteral("㎞²")))
    {
        return value * 1000000000000.0 / (static_cast<double>(scale) * static_cast<double>(scale));
    }
    if (u.contains(QStringLiteral("平方米")) || u.contains("m2") || u.contains(QStringLiteral("㎡")))
    {
        return value * 1000000.0 / (static_cast<double>(scale) * static_cast<double>(scale));
    }
    return value;
}

static bool IsMinWidthRuleName(const QString& ruleName)
{
    QString name = ruleName.trimmed();
    return name == QStringLiteral("最小宽度") ||
           name == QStringLiteral("图上最小宽度") ||
           name == QStringLiteral("图上宽度") ||
           name == QStringLiteral("图上宽度大于") ||
           name == QStringLiteral("图上宽度下限");
}

static double EstimatePolygonWidthUnits(OGRGeometry* geometry)
{
    if (!geometry)
    {
        return 0.0;
    }

    double area = 0.0;
    double perimeter = 0.0;
    OGRwkbGeometryType geomType = wkbFlatten(geometry->getGeometryType());

    auto appendPolygonMetrics = [&](OGRPolygon* polygon) {
        if (!polygon)
        {
            return;
        }
        area += polygon->get_Area();
        OGRLinearRing* exterior = polygon->getExteriorRing();
        if (exterior)
        {
            perimeter += exterior->get_Length();
        }
        for (int i = 0; i < polygon->getNumInteriorRings(); ++i)
        {
            OGRLinearRing* interior = polygon->getInteriorRing(i);
            if (interior)
            {
                perimeter += interior->get_Length();
            }
        }
    };

    if (geomType == wkbPolygon)
    {
        appendPolygonMetrics(static_cast<OGRPolygon*>(geometry));
    }
    else if (geomType == wkbMultiPolygon)
    {
        OGRMultiPolygon* multiPolygon = static_cast<OGRMultiPolygon*>(geometry);
        for (int i = 0; i < multiPolygon->getNumGeometries(); ++i)
        {
            appendPolygonMetrics(static_cast<OGRPolygon*>(multiPolygon->getGeometryRef(i)));
        }
    }

    double widthByArea = (perimeter > 0.0) ? (2.0 * area / perimeter) : 0.0;

    OGREnvelope envelope;
    geometry->getEnvelope(&envelope);
    double envelopeWidth = envelope.MaxX - envelope.MinX;
    double envelopeHeight = envelope.MaxY - envelope.MinY;
    double widthByEnvelope = 0.0;
    if (envelopeWidth > 0.0 && envelopeHeight > 0.0)
    {
        widthByEnvelope = envelopeWidth < envelopeHeight ? envelopeWidth : envelopeHeight;
    }

    if (widthByArea > 0.0 && widthByEnvelope > 0.0)
    {
        return widthByArea < widthByEnvelope ? widthByArea : widthByEnvelope;
    }
    return widthByArea > widthByEnvelope ? widthByArea : widthByEnvelope;
}

// 解析XML文件，获取要素缺失检查的参数

FeatureMissingCheckParams ParseXmlForFeatureMissingParams(const string& strXmlPath, int scale, std::shared_ptr<spdlog::logger> logger)
{
    // 初始化参数结构体
    FeatureMissingCheckParams params;
    params.dBufferRadius = 0.0;  // 必须从XML读取，避免默认值掩盖配置问题
    params.dMinArea = 0.0;
    params.dMinLength = 0.0;
    params.dMinWidth = 0.0;
    params.strAttributeField = "";
    params.strAttributeValue = "";
    params.strLayerName = "";    // 初始化图层名称
    params.bHasMinArea = false;
    params.bHasMinLength = false;
    params.bHasMinWidth = false;
    params.bHasAttributeFilter = false;
    
    QFile file(QString::fromStdString(strXmlPath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        logger->error("无法打开XML文件：{}", fromQStringToUtf8(fromUtf8ToQString(strXmlPath.c_str())));
        return params;
    }

    QDomDocument doc;
    if (!doc.setContent(&file))
    {
        logger->error("XML文件格式错误：{}", fromQStringToUtf8(fromUtf8ToQString(strXmlPath.c_str())));
        file.close();
        return params;
    }
    file.close();

    // 获取根元素
    QDomElement root = doc.documentElement();
    if (root.tagName() != "check_list")
    {
        logger->error("XML根元素不是check_list");
        return params;
    }

    // 获取XML文件名（不含路径和扩展名）作为默认图层名称
    QString xmlFileName = QString::fromStdString(strXmlPath);
    int lastSlash = xmlFileName.lastIndexOf('/');
    if (lastSlash == -1) {
        lastSlash = xmlFileName.lastIndexOf('\\');
    }
    if (lastSlash != -1) {
        xmlFileName = xmlFileName.mid(lastSlash + 1);
    }
    int lastDot = xmlFileName.lastIndexOf('.');
    if (lastDot != -1) {
        xmlFileName = xmlFileName.left(lastDot);
    }
    params.strLayerName = xmlFileName.toStdString();
    logger->info("从XML文件名获取默认图层名称：{}", params.strLayerName);

    auto parseFeatureMissingCheckItem = [&](const QDomElement& checkItem) -> bool
    {
        QString checkName = checkItem.firstChildElement("name").text().trimmed();
        if (checkName != QStringLiteral("要素缺失检查"))
        {
            return false;
        }

        QDomNodeList ruleNodes = checkItem.elementsByTagName("rule");
        bool bFoundBufferRadius = false;
        for (int k = 0; k < ruleNodes.count(); k++)
        {
            QDomElement ruleElement = ruleNodes.at(k).toElement();
            QString ruleName = ruleElement.firstChildElement("name").text().trimmed();
            QString ruleValue = ruleElement.firstChildElement("value").text().trimmed();
            QString ruleUnit = ruleElement.firstChildElement("unit").text().trimmed();
            if (ruleName.isEmpty() || ruleValue.isEmpty())
            {
                continue;
            }

            if (ruleName == QStringLiteral("缓冲区半径"))
            {
                bool ok;
                double dBufferRadius = ruleValue.toDouble(&ok);
                if (ok && dBufferRadius > 0)
                {
                    params.dBufferRadius = NormalizeLengthValueToMapMm(dBufferRadius, ruleUnit, scale);
                    bFoundBufferRadius = true;
                    logger->info("解析到缓冲区半径：{} {} -> 图上{}毫米", dBufferRadius, fromQStringToUtf8(ruleUnit), params.dBufferRadius);
                }
            }
            else if (ruleName == QStringLiteral("最小面积"))
            {
                bool ok;
                double dMinArea = ruleValue.toDouble(&ok);
                if (ok && dMinArea >= 0)
                {
                    params.dMinArea = NormalizeAreaValueToMapSqMm(dMinArea, ruleUnit, scale);
                    params.bHasMinArea = true;
                    logger->info("解析到最小面积：{} {} -> 图上{}平方毫米", dMinArea, fromQStringToUtf8(ruleUnit), params.dMinArea);
                }
            }
            else if (ruleName == QStringLiteral("最小长度"))
            {
                bool ok;
                double dMinLength = ruleValue.toDouble(&ok);
                if (ok && dMinLength >= 0)
                {
                    params.dMinLength = NormalizeLengthValueToMapMm(dMinLength, ruleUnit, scale);
                    params.bHasMinLength = true;
                    logger->info("解析到最小长度：{} {} -> 图上{}毫米", dMinLength, fromQStringToUtf8(ruleUnit), params.dMinLength);
                }
            }
            else if (IsMinWidthRuleName(ruleName))
            {
                bool ok;
                double dMinWidth = ruleValue.toDouble(&ok);
                if (ok && dMinWidth >= 0)
                {
                    params.dMinWidth = NormalizeLengthValueToMapMm(dMinWidth, ruleUnit, scale);
                    params.bHasMinWidth = true;
                    logger->info("解析到最小宽度：{} {} -> 图上{}毫米", dMinWidth, fromQStringToUtf8(ruleUnit), params.dMinWidth);
                }
            }
            else if (ruleName == QStringLiteral("属性项"))
            {
                params.strAttributeField = ruleValue.toStdString();
                logger->info("解析到属性项：{}", params.strAttributeField);
            }
            else if (ruleName == QStringLiteral("属性值"))
            {
                params.strAttributeValue = ruleValue.toStdString();
                logger->info("解析到属性值：{}", params.strAttributeValue);
            }
            else if (ruleName == QStringLiteral("图层名称"))
            {
                params.strLayerName = ruleValue.toStdString();
                logger->info("解析到图层名称：{}", params.strLayerName);
            }
        }

        if (!params.strAttributeField.empty() && !params.strAttributeValue.empty())
        {
            params.bHasAttributeFilter = true;
            logger->info("启用属性过滤：{}={}", params.strAttributeField, params.strAttributeValue);
        }
        if (!bFoundBufferRadius)
        {
            logger->error("未找到必需的缓冲区半径参数");
            return false;
        }
        return true;
    };

    QDomNodeList scaleNodes = root.elementsByTagName("scale");
    bool bFoundScale = false;
    for (int i = 0; i < scaleNodes.count(); ++i)
    {
        QDomElement scaleElement = scaleNodes.at(i).toElement();
        QString scaleValue = scaleElement.firstChildElement("value").text().trimmed();
        if (scaleValue.isEmpty())
        {
            QDomNodeList valueNodes = scaleElement.elementsByTagName("value");
            if (valueNodes.count() > 0)
            {
                scaleValue = valueNodes.at(0).toElement().text().trimmed();
            }
        }

        bool ok = false;
        int xmlScale = scaleValue.toInt(&ok);
        if (!ok)
        {
            continue;
        }

        if (xmlScale == scale)
        {
            bFoundScale = true;
            logger->info("按比例尺查找要素缺失检查参数：{}", scale);
            QDomNodeList checkItems = scaleElement.elementsByTagName("check_item");
            for (int j = 0; j < checkItems.count(); ++j)
            {
                QDomElement checkItem = checkItems.at(j).toElement();
                if (parseFeatureMissingCheckItem(checkItem))
                {
                    return params;
                }
            }
            logger->warn("在比例尺{}下未找到有效的‘要素缺失检查’参数，继续尝试兼容旧结构", scale);
            break;
        }
    }

    if (!bFoundScale && scaleNodes.count() > 0)
    {
        logger->warn("未找到匹配的比例尺：{}，继续尝试兼容旧结构", scale);
    }

    // 兼容没有填写比例尺值的XML结构：<scale><value></value><check_item>...</check_item></scale>
    for (int i = 0; i < scaleNodes.count(); ++i)
    {
        QDomElement scaleElement = scaleNodes.at(i).toElement();
        QString scaleValue = scaleElement.firstChildElement("value").text().trimmed();
        bool ok = false;
        scaleValue.toInt(&ok);
        if (ok)
        {
            continue;
        }

        logger->info("按未指定比例尺的scale节点查找要素缺失检查参数");
        QDomNodeList checkItems = scaleElement.elementsByTagName("check_item");
        for (int j = 0; j < checkItems.count(); ++j)
        {
            QDomElement checkItem = checkItems.at(j).toElement();
            if (parseFeatureMissingCheckItem(checkItem))
            {
                return params;
            }
        }
    }

    bool bFoundFeatureMissingCheck = false;
    for (QDomNode node = root.firstChild(); !node.isNull(); node = node.nextSibling())
    {
        QDomElement checkItem = node.toElement();
        if (checkItem.isNull() || checkItem.tagName() != QStringLiteral("check_item"))
        {
            continue;
        }
        if (checkItem.firstChildElement("name").text().trimmed() == QStringLiteral("要素缺失检查"))
        {
            bFoundFeatureMissingCheck = true;
            if (parseFeatureMissingCheckItem(checkItem))
            {
                return params;
            }
            break;
        }
    }

    if (!bFoundFeatureMissingCheck)
    {
        logger->error("未找到要素缺失检查项，未读取到必需的缓冲区半径参数");
    }
    
    return params;
}

SE_Error FeatureMissingCheck(
    const char* szSourceShpFilePath,
    const char* szParsedDataPath,
    const char* szSchemaXmlPath,
    int scale,
    const char* szOutputCheckResultLayerPath,
    const char* szOutputJsonPath,
    const char* szOutputLogFilePath)
{
    // 创建日志器（使用本地编码路径，避免中文文件名乱码）
    QString logPathQ = fromUtf8ToQString(szOutputLogFilePath);
    QByteArray logPathLocalBytes = logPathQ.toLocal8Bit();
    std::string logPathLocal(logPathLocalBytes.constData(), logPathLocalBytes.size());
    if (spdlog::get("FeatureMissingCheckAlgorithm")) {
        spdlog::drop("FeatureMissingCheckAlgorithm");
    }
    std::shared_ptr<spdlog::logger> logger;
    try {
        logger = spdlog::basic_logger_mt("FeatureMissingCheckAlgorithm", logPathLocal);
    } catch (...) {
        logger = spdlog::get("FeatureMissingCheckAlgorithm");
        if (!logger) logger = spdlog::default_logger();
    }
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    logger->info("开始执行要素缺失检查算法（一对多关系版本）");
    
    // 修改日志输出，确保中文路径正确显示
    logger->info("源数据: {}", fromQStringToUtf8(fromUtf8ToQString(szSourceShpFilePath)));
    logger->info("解析后数据: {}", fromQStringToUtf8(fromUtf8ToQString(szParsedDataPath)));
    logger->info("JSON输出路径: {}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));

    try
    {
        // 解析XML文件，获取指定比例尺下的要素缺失检查参数
        FeatureMissingCheckParams params = ParseXmlForFeatureMissingParams(szSchemaXmlPath, scale, logger);
        if (params.dBufferRadius <= 0.0)
        {
            logger->error("要素缺失检查缺少有效的缓冲区半径参数，停止执行");
            return SE_ERROR_XML_PARSE_FAILED;
        }
        
        logger->info("使用解析数据比例尺：{}", scale);
        logger->info("使用缓冲区距离：{} 毫米", params.dBufferRadius);
        if (params.bHasMinArea)
        {
            logger->info("启用最小面积过滤：{} 平方毫米", params.dMinArea);
        }
        else
        {
            logger->info("未启用最小面积过滤");
        }
        if (params.bHasMinLength)
        {
            logger->info("启用最小长度过滤：{} 毫米", params.dMinLength);
        }
        else
        {
            logger->info("未启用最小长度过滤");
        }
        if (params.bHasMinWidth)
        {
            logger->info("启用最小宽度过滤：{} 毫米", params.dMinWidth);
        }
        else
        {
            logger->info("未启用最小宽度过滤");
        }
        if (params.bHasAttributeFilter)
        {
            logger->info("启用属性过滤：{}={}", params.strAttributeField, params.strAttributeValue);
        }
        else
        {
            logger->info("未启用属性过滤");
        }
        
        // 记录过滤策略
        logger->info("过滤策略：面要素使用最小面积/最小宽度过滤，线要素使用最小长度过滤，所有要素使用属性过滤");

        // 统一GDAL文件名编码为UTF-8，确保包含中文的路径可用
        CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
        CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");

        // 安全注册GDAL驱动 - 避免重复注册
        static bool gdalInitialized = false;
        if (!gdalInitialized)
        {
            GDALAllRegister();
            gdalInitialized = true;
            logger->debug("GDAL驱动已注册");
        }
        else
        {
            logger->debug("GDAL驱动已存在，跳过注册");
        }

        // 步骤1：预处理原始数据（库体数据）
        logger->info("步骤1：开始预处理原始数据");
        
        // 修改GDAL文件打开部分，确保正确处理中文路径
        QString sourcePath = fromUtf8ToQString(szSourceShpFilePath);
        GDALDataset* poSourceDS = (GDALDataset*)GDALOpenEx(sourcePath.toUtf8().constData(), GDAL_OF_VECTOR, NULL, NULL, NULL);
        if (poSourceDS == NULL)
        {
            logger->error("无法打开源数据：{}", fromQStringToUtf8(fromUtf8ToQString(szSourceShpFilePath)));
            
            // 创建JSON输出，报告错误
            QJsonObject jsonResult;
            QJsonObject checkItem;
            checkItem["id"] = "";
            checkItem["name"] = "要素缺失检查";
            checkItem["status"] = "质检失败";
            checkItem["err_exist"] = "true";
            checkItem["info"] = QString("无法打开源数据文件").toStdString().c_str();
            
            QJsonObject errors;
            errors["error1"] = "源数据文件不存在或无法访问";
            errors["error2"] = "";
            errors["error3"] = "";
            checkItem["errors"] = errors;
            
            jsonResult["check_item3"] = checkItem;
            
            QJsonDocument doc(jsonResult);
            QFile jsonFile(szOutputJsonPath);
            if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                jsonFile.write(doc.toJson());
                jsonFile.close();
                logger->info("已生成JSON结果文件：{}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));
            } else {
                logger->error("无法创建JSON结果文件：{}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));
            }
            
            return SE_ERROR_FAILURE;
        }

        // 获取源数据图层
        OGRLayer* poSourceLayer = poSourceDS->GetLayer(0);
        if (poSourceLayer == NULL)
        {
            GDALClose(poSourceDS);
            logger->error("无法获取源数据图层");
            return SE_ERROR_FAILURE;
        }

        // 计算缓冲区距离：将图上毫米转换为实地距离（米），再转换为图层单位
        // 注意：这里的比例尺为解析后数据的比例尺（用户输入），
        // 同时用于在XML中选择对应的scale项，以确保规则匹配。
        OGRSpatialReference* poSRS = poSourceLayer->GetSpatialRef();
        double dLinearUnitsInMeters = 1.0; // 默认按米
        if (poSRS)
        {
            if (poSRS->IsGeographic())
            {
                logger->error("源数据空间参考为地理坐标系(度)。当前实现不支持将图上毫米精确换算为度单位，请使用投影坐标系数据。");
                GDALClose(poSourceDS);
                return SE_ERROR_UNSUPPORTED_SRS;
            }
            else
            {
                const char* pszUnitsName = nullptr;
                dLinearUnitsInMeters = poSRS->GetLinearUnits(&pszUnitsName);
                if (dLinearUnitsInMeters <= 0.0)
                {
                    logger->warn("无法获取有效的线性单位转换系数，默认按米处理");
                    dLinearUnitsInMeters = 1.0;
                }
                logger->info("源数据坐标单位: {} 每单位米数: {}", pszUnitsName ? pszUnitsName : "(未知)", dLinearUnitsInMeters);
            }
        }
        else
        {
            logger->warn("源数据缺少空间参考，默认按米处理缓冲距离");
        }

        double dBufferRadiusMapMm = params.dBufferRadius;              // 图上毫米
        double dBufferRadiusMeters = (dBufferRadiusMapMm * scale) / 1000.0; // 实地米
        double dBufferRadiusUnits = MapLengthMmToLayerUnits(dBufferRadiusMapMm, scale, dLinearUnitsInMeters);
        double dMinLengthUnits = MapLengthMmToLayerUnits(params.dMinLength, scale, dLinearUnitsInMeters);
        double dMinAreaUnits = MapAreaSqMmToLayerSqUnits(params.dMinArea, scale, dLinearUnitsInMeters);
        double dMinWidthUnits = MapLengthMmToLayerUnits(params.dMinWidth, scale, dLinearUnitsInMeters);
        logger->info("缓冲区半径换算: 图上{}毫米 -> 实地{}米 -> 图层单位{}", dBufferRadiusMapMm, dBufferRadiusMeters, dBufferRadiusUnits);
        if (params.bHasMinLength)
        {
            logger->info("最小长度换算: 图上{}毫米 -> 图层单位{}", params.dMinLength, dMinLengthUnits);
        }
        if (params.bHasMinArea)
        {
            logger->info("最小面积换算: 图上{}平方毫米 -> 图层平方单位{}", params.dMinArea, dMinAreaUnits);
        }
        if (params.bHasMinWidth)
        {
            logger->info("最小宽度换算: 图上{}毫米 -> 图层单位{}", params.dMinWidth, dMinWidthUnits);
        }

        // 过滤源数据中小于最小面积的要素并创建缓冲区
        if (params.bHasMinArea)
        {
            logger->info("开始过滤源数据中小于最小面积的要素（最小面积：{} 平方毫米，图层平方单位：{}）...", params.dMinArea, dMinAreaUnits);
        }
        else
        {
            logger->info("未配置最小面积过滤，处理所有多边形要素...");
        }
        poSourceLayer->ResetReading();
        OGRFeature* poSourceFeature;
        int nFilteredFeatureCount = 0;
        int nTotalSourceFeatureCount = 0;
        vector<OGRGeometry*> vBufferedGeometries;
        vector<OGRGeometry*> vOriginalGeometries; // 与缓冲区一一对应的原始几何
        vector<int> vSourceFeatureIDs; // 记录源要素ID用于后续检查

        int nPointCount = 0;
        int nLineCount = 0;
        int nPolygonCount = 0;
        int nValidAreaCount = 0;
        int nValidWidthCount = 0;
        int nBufferSuccessCount = 0;
        
        while ((poSourceFeature = poSourceLayer->GetNextFeature()) != NULL)
        {
            nTotalSourceFeatureCount++;
            OGRGeometry* poGeometry = poSourceFeature->GetGeometryRef();
            if (poGeometry != NULL)
            {
                OGRwkbGeometryType geomType = poGeometry->getGeometryType();
                bool bShouldProcess = true;
                
                // 根据几何类型进行分类统计
                if (wkbFlatten(geomType) == wkbPoint || wkbFlatten(geomType) == wkbMultiPoint)
                {
                    nPointCount++;
                }
                else if (wkbFlatten(geomType) == wkbLineString || wkbFlatten(geomType) == wkbMultiLineString)
                {
                    nLineCount++;
                    
                    // 只有线要素且配置了最小长度时才进行长度过滤
                    if (params.bHasMinLength)
                    {
                        double dLength = 0.0;
                        if (wkbFlatten(geomType) == wkbLineString)
                        {
                            dLength = ((OGRLineString*)poGeometry)->get_Length();
                        }
                        else if (wkbFlatten(geomType) == wkbMultiLineString)
                        {
                            dLength = ((OGRMultiLineString*)poGeometry)->get_Length();
                        }
                        
                        // 添加调试日志
                        if (nLineCount <= 5)
                        {
                            logger->info("线要素ID {}: 长度={}, 最小长度阈值(图层单位)={}", 
                                       poSourceFeature->GetFID(), dLength, dMinLengthUnits);
                        }
                        
                        if (dLength < dMinLengthUnits)
                        {
                            bShouldProcess = false;
                            if (nLineCount <= 5)
                            {
                                logger->info("线要素ID {} 长度不符合要求，跳过", poSourceFeature->GetFID());
                            }
                        }
                    }
                }
                else if (wkbFlatten(geomType) == wkbPolygon || wkbFlatten(geomType) == wkbMultiPolygon)
                {
                    nPolygonCount++;
                    
                    // 只有面要素且配置了最小面积时才进行面积过滤
                    if (params.bHasMinArea)
                    {
                        double dArea = 0.0;
                        if (wkbFlatten(geomType) == wkbPolygon)
                        {
                            dArea = ((OGRSurface*)poGeometry)->get_Area();
                        }
                        else if (wkbFlatten(geomType) == wkbMultiPolygon)
                        {
                            dArea = ((OGRMultiSurface*)poGeometry)->get_Area();
                        }
                        
                        // 添加调试日志
                        if (nPolygonCount <= 5)
                        {
                            logger->info("面要素ID {}: 面积={}, 最小面积阈值(图层平方单位)={}", 
                                       poSourceFeature->GetFID(), dArea, dMinAreaUnits);
                        }
                        
                        if (dArea < dMinAreaUnits)
                        {
                            bShouldProcess = false;
                            if (nPolygonCount <= 5)
                            {
                                logger->info("面要素ID {} 面积不符合要求，跳过", poSourceFeature->GetFID());
                            }
                        }
                        else
                        {
                            nValidAreaCount++;
                        }
                    }

                    if (bShouldProcess && params.bHasMinWidth)
                    {
                        double dWidth = EstimatePolygonWidthUnits(poGeometry);

                        if (nPolygonCount <= 5)
                        {
                            logger->info("面要素ID {}: 估算宽度={}, 最小宽度阈值(图层单位)={}",
                                       poSourceFeature->GetFID(), dWidth, dMinWidthUnits);
                        }

                        if (dWidth <= dMinWidthUnits)
                        {
                            bShouldProcess = false;
                            if (nPolygonCount <= 5)
                            {
                                logger->info("面要素ID {} 图上宽度未大于最小宽度，跳过", poSourceFeature->GetFID());
                            }
                        }
                        else
                        {
                            nValidWidthCount++;
                        }
                    }
                }
                else
                {
                    if (nTotalSourceFeatureCount <= 5)
                    {
                        logger->info("要素ID {}: 几何类型={} (不支持的类型，跳过)", 
                                   poSourceFeature->GetFID(), OGRGeometryTypeToName(geomType));
                    }
                    bShouldProcess = false;
                }
                
                // 属性过滤
                if (bShouldProcess && params.bHasAttributeFilter)
                {
                    const char* pszFieldValue = poSourceFeature->GetFieldAsString(params.strAttributeField.c_str());
                    if (pszFieldValue == NULL || strcmp(pszFieldValue, params.strAttributeValue.c_str()) != 0)
                    {
                        bShouldProcess = false;
                        if (nTotalSourceFeatureCount <= 5)
                        {
                            logger->info("要素ID {} 属性值不符合要求 ({}={})，跳过", 
                                poSourceFeature->GetFID(), 
                                params.strAttributeField, 
                                pszFieldValue ? pszFieldValue : "(null)");
                        }
                    }
                    else if (nTotalSourceFeatureCount <= 5)
                    {
                        logger->info("要素ID {} 属性值符合要求 ({}={})，保留", 
                            poSourceFeature->GetFID(), 
                            params.strAttributeField, 
                            pszFieldValue);
                    }
                }
                
                // 如果要素应该被处理，创建缓冲区
                if (bShouldProcess)
                {
                    OGRGeometry* poBufferedGeometry = poGeometry->Buffer(dBufferRadiusUnits);
                    if (poBufferedGeometry != NULL)
                    {
                        vBufferedGeometries.push_back(poBufferedGeometry);
                        // 保存对应的原始几何以保证索引一致
                        vOriginalGeometries.push_back(poGeometry->clone());
                        vSourceFeatureIDs.push_back(poSourceFeature->GetFID());
                        nFilteredFeatureCount++;
                        nBufferSuccessCount++;
                    }
                    else
                    {
                        logger->error("为要素ID {} 创建缓冲区失败", poSourceFeature->GetFID());
                    }
                }
            }
            else
            {
                logger->warn("要素ID {} 没有几何对象", poSourceFeature->GetFID());
            }
            OGRFeature::DestroyFeature(poSourceFeature);
        }
        
        logger->info("统计信息 - 总要素数:{}, 点要素数:{}, 线要素数:{}, 面要素数:{}, 面积符合条件数:{}, 宽度符合条件数:{}, 缓冲区创建成功数:{}",
                   nTotalSourceFeatureCount, nPointCount, nLineCount, nPolygonCount, nValidAreaCount, nValidWidthCount, nBufferSuccessCount);

        logger->info("源数据总要素数量：{}", nTotalSourceFeatureCount);
        logger->info("过滤后的要素数量：{}", nFilteredFeatureCount);
        
        if (nFilteredFeatureCount == 0)
        {
            logger->warn("没有符合条件的源要素，检查结束");
            GDALClose(poSourceDS);
            return SE_ERROR_NONE;
        }

        // 步骤2：处理解析后数据（缩编数据）
        logger->info("步骤2：开始处理解析后数据");
        
        // 解析多个文件路径（用分号分隔）
        string strParsedDataPaths(szParsedDataPath);
        vector<string> vParsedFilePaths;
        size_t start = 0;
        size_t end = strParsedDataPaths.find(';');
        
        while (end != string::npos) {
            string path = strParsedDataPaths.substr(start, end - start);
            if (!path.empty()) {
                vParsedFilePaths.push_back(path);
            }
            start = end + 1;
            end = strParsedDataPaths.find(';', start);
        }
        // 添加最后一个路径
        string lastPath = strParsedDataPaths.substr(start);
        if (!lastPath.empty()) {
            vParsedFilePaths.push_back(lastPath);
        }
        
        logger->info("解析后数据包含 {} 个文件", vParsedFilePaths.size());
        
        vector<OGRGeometry*> vParsedGeometries;
        int nTotalParsedFeatureCount = 0;
        int nTotalLayerCount = 0;
        
        // 逐个处理每个解析后数据文件
        for (const string& strFilePath : vParsedFilePaths)
        {
            logger->info("处理解析后数据文件：{}", fromQStringToUtf8(fromUtf8ToQString(strFilePath.c_str())));
            
            // 同样修改解析数据文件的打开部分，确保正确处理中文路径
            QString filePathQStr = fromUtf8ToQString(strFilePath.c_str());
            GDALDataset* poParsedDS = (GDALDataset*)GDALOpenEx(filePathQStr.toUtf8().constData(), GDAL_OF_VECTOR, NULL, NULL, NULL);
            if (poParsedDS == NULL)
            {
                logger->error("无法打开解析后数据文件：{}", fromQStringToUtf8(fromUtf8ToQString(strFilePath.c_str())));
                continue; // 继续处理下一个文件
            }
            
            // 读取当前文件所有图层的几何对象
            int nLayerCount = poParsedDS->GetLayerCount();
            nTotalLayerCount += nLayerCount;
            logger->info("文件 {} 包含 {} 个图层", fromQStringToUtf8(fromUtf8ToQString(strFilePath.c_str())), nLayerCount);
        
        for (int i = 0; i < nLayerCount; i++)
        {
            OGRLayer* poParsedLayer = poParsedDS->GetLayer(i);
            if (poParsedLayer == NULL)
            {
                logger->warn("无法获取第 {} 个图层", i);
                continue;
            }
            
            logger->info("处理图层 {}: {}", i, poParsedLayer->GetName());
            poParsedLayer->ResetReading();
            OGRFeature* poParsedFeature;
            int nLayerFeatureCount = 0;
            
            while ((poParsedFeature = poParsedLayer->GetNextFeature()) != NULL)
            {
                OGRGeometry* poGeometry = poParsedFeature->GetGeometryRef();
                if (poGeometry != NULL)
                {
                    // 几何清理：检查并修复无效几何
                    if (!poGeometry->IsValid())
                    {
                        logger->warn("发现无效几何，尝试修复（图层：{}，要素ID：{}）", poParsedLayer->GetName(), poParsedFeature->GetFID());
                        OGRGeometry* poFixedGeometry = poGeometry->Buffer(0); // 使用0缓冲区修复几何
                        if (poFixedGeometry != NULL && poFixedGeometry->IsValid())
                        {
                            vParsedGeometries.push_back(poFixedGeometry);
                            nLayerFeatureCount++;
                        }
                        else
                        {
                            logger->error("无法修复无效几何（图层：{}，要素ID：{}）", poParsedLayer->GetName(), poParsedFeature->GetFID());
                            if (poFixedGeometry != NULL)
                            {
                                OGRGeometryFactory::destroyGeometry(poFixedGeometry);
                            }
                        }
                    }
                    else
                    {
                        vParsedGeometries.push_back(poGeometry->clone());
                        nLayerFeatureCount++;
                    }
                }
                OGRFeature::DestroyFeature(poParsedFeature);
            }
            
            logger->info("图层 {} 包含 {} 个有效要素", poParsedLayer->GetName(), nLayerFeatureCount);
            nTotalParsedFeatureCount += nLayerFeatureCount;
        }
        
        // 关闭当前文件的数据集
        GDALClose(poParsedDS);
        logger->info("文件 {} 处理完成，包含 {} 个有效要素", fromQStringToUtf8(fromUtf8ToQString(strFilePath.c_str())), nTotalParsedFeatureCount);
    }
    
    logger->info("解析后数据总要素数量：{}", nTotalParsedFeatureCount);
    
    // 检查是否成功读取到解析后数据
    if (nTotalParsedFeatureCount == 0)
    {
        logger->error("所有解析后数据文件都无法读取或不包含有效要素");
        // 释放缓冲区几何对象
        for (auto& poGeometry : vBufferedGeometries)
        {
            OGRGeometryFactory::destroyGeometry(poGeometry);
        }
        GDALClose(poSourceDS);
        return SE_ERROR_FAILURE;
    }

        // 步骤3：跨图层空间查询
        logger->info("步骤3：开始跨图层空间查询和缺失要素检测");
        
        vector<int> vMissingFeatureIDs; // 记录缺失的源要素ID
        vector<OGRGeometry*> vMissingGeometries; // 记录缺失要素的原始几何
        int nFoundFeatureCount = 0;
        
        // 使用首遍收集的 vOriginalGeometries 与 vBufferedGeometries 一一对应进行空间检测
        
        logger->info("开始空间相交检查：vBufferedGeometries.size()={}, vOriginalGeometries.size()={}, vParsedGeometries.size()={}", 
                    vBufferedGeometries.size(), vOriginalGeometries.size(), vParsedGeometries.size());
        
        // 对每个原始要素的缓冲区，检查是否与解析后数据相交
        for (size_t i = 0; i < vBufferedGeometries.size(); i++)
        {
            // 每处理100个要素输出一次进度
            if (i % 100 == 0)
            {
                logger->info("空间相交检查进度: {}/{} ({:.1f}%)", i, vBufferedGeometries.size(), 
                           (double)i / vBufferedGeometries.size() * 100.0);
            }
            
            bool bFound = false;
            OGRGeometry* poBufferedGeometry = vBufferedGeometries[i];
            
            // 与所有解析后的几何对象进行空间相交检查
            for (auto& poParsedGeometry : vParsedGeometries)
            {
                if (poBufferedGeometry->Intersects(poParsedGeometry))
                {
                    bFound = true;
                    break;
                }
            }
            
            if (bFound)
            {
                nFoundFeatureCount++;
            }
            else
            {
                // 记录缺失的要素
                vMissingFeatureIDs.push_back(vSourceFeatureIDs[i]);
                if (i < vOriginalGeometries.size())
                {
                    vMissingGeometries.push_back(vOriginalGeometries[i]->clone());
                    logger->debug("添加缺失要素 {} 到结果集", vSourceFeatureIDs[i]);
                }
                else
                {
                    logger->warn("索引 {} 超出vOriginalGeometries范围 {}", i, vOriginalGeometries.size());
                }
            }
        }
        
        // 释放原始几何对象
        for (auto& poGeometry : vOriginalGeometries)
        {
            OGRGeometryFactory::destroyGeometry(poGeometry);
        }
        
        // 步骤5：判断缺失情况
        int nMissingFeatureCount = vMissingFeatureIDs.size();
        
        logger->info("找到的要素数量：{}", nFoundFeatureCount);
        logger->info("缺失的要素数量：{}", nMissingFeatureCount);
        
        // 根据缺失数量判断缺失类型
        bool bFeatureMissingDetected = (nMissingFeatureCount > 0); // 只要有缺失就是要素缺失
        
        string strMissingType;
        if (bFeatureMissingDetected)
        {
            strMissingType = "要素缺失";
        }
        else
        {
            strMissingType = "未检测到缺失";
        }
        
        logger->info("要素缺失检测结果：{}", strMissingType);

        // 创建输出结果图层（仅在检测到缺失时创建）
        GDALDataset* poResultDS = NULL;
        OGRLayer* poResultLayer = NULL;
        
        logger->info("检查输出条件：bFeatureMissingDetected={}, vMissingGeometries.size()={}", 
                    bFeatureMissingDetected, vMissingGeometries.size());
        
        if (bFeatureMissingDetected && !vMissingGeometries.empty())
        {
            logger->info("开始创建输出结果图层：{}", fromQStringToUtf8(fromUtf8ToQString(szOutputCheckResultLayerPath)));
            GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
            if (poDriver == NULL)
            {
                logger->error("无法创建Shapefile驱动");
            }
            else
            {
                // 创建输出数据集
                QString outPathQStr = fromUtf8ToQString(szOutputCheckResultLayerPath);
                poResultDS = poDriver->Create(outPathQStr.toUtf8().constData(), 0, 0, 0, GDT_Unknown, NULL);
                if (poResultDS == NULL)
                {
                    logger->error("无法创建输出数据集：{}", fromQStringToUtf8(fromUtf8ToQString(szOutputCheckResultLayerPath)));
                }
                else
                {
                    // 获取源图层的几何类型
                    OGRwkbGeometryType eGeomType = poSourceLayer->GetGeomType();
                    logger->info("源图层几何类型: {}", (int)eGeomType);
                    
                    // 创建输出图层，使用源图层的几何类型，并指定DBF编码为UTF-8（生成CPG文件）
                    char** papszLCO = NULL;
                    papszLCO = CSLSetNameValue(papszLCO, "ENCODING", "UTF-8");
                    poResultLayer = poResultDS->CreateLayer("feature_missing", poSourceLayer->GetSpatialRef(), eGeomType, papszLCO);
                    CSLDestroy(papszLCO);
                    if (poResultLayer == NULL)
                    {
                        logger->error("无法创建输出图层");
                    }
                    else
                    {
                        // 复制库体图层的属性字段
                        OGRFeatureDefn* poSrcFDefn = poSourceLayer->GetLayerDefn();
                        for (int iField = 0; iField < poSrcFDefn->GetFieldCount(); ++iField)
                        {
                            OGRFieldDefn* poFldDefn = poSrcFDefn->GetFieldDefn(iField);
                            OGRFieldDefn oCopyFld(poFldDefn->GetNameRef(), poFldDefn->GetType());
                            oCopyFld.SetWidth(poFldDefn->GetWidth());
                            oCopyFld.SetPrecision(poFldDefn->GetPrecision());
                            if (poResultLayer->CreateField(&oCopyFld) != OGRERR_NONE)
                            {
                                logger->error("复制字段失败: {}", poFldDefn->GetNameRef());
                            }
                        }

                        // 新增错误类型字段（采用ASCII字段名以避免DBF头中文乱码）
                        OGRFieldDefn oErrorTypeField("ErrorType", OFTString);
                        oErrorTypeField.SetWidth(64);
                        if (poResultLayer->CreateField(&oErrorTypeField) != OGRERR_NONE)
                        {
                            logger->error("创建错误类型字段失败");
                        }
                        
                        // 添加缺失要素到结果图层
                        logger->info("开始写入 {} 个缺失要素到输出图层", vMissingGeometries.size());
                        for (size_t i = 0; i < vMissingGeometries.size(); i++)
                        {
                            // 每写入100个要素输出一次进度
                            if (i % 100 == 0)
                            {
                                logger->info("写入要素进度: {}/{} ({:.1f}%)", i, vMissingGeometries.size(), 
                                           (double)i / vMissingGeometries.size() * 100.0);
                            }
                            
                            OGRFeature* poResultFeature = OGRFeature::CreateFeature(poResultLayer->GetLayerDefn());
                            
                            // 复制库体要素的属性
                            OGRFeature* poSrcFeatureForCopy = nullptr;
                            if (i < vMissingFeatureIDs.size())
                            {
                                poSrcFeatureForCopy = poSourceLayer->GetFeature(vMissingFeatureIDs[i]);
                            }
                            if (poSrcFeatureForCopy)
                            {
                                poResultFeature->SetFrom(poSrcFeatureForCopy);
                                OGRFeature::DestroyFeature(poSrcFeatureForCopy);
                            }
                            else
                            {
                                logger->warn("无法根据FID获取源要素以复制属性，FID={}", i < vMissingFeatureIDs.size() ? vMissingFeatureIDs[i] : -1);
                            }

                            // 设置错误类型字段（值使用UTF-8中文）
                            std::string sErrVal = fromQStringToUtf8(QStringLiteral("要素缺失"));
                            poResultFeature->SetField("ErrorType", sErrVal.c_str());
                            
                            // 检查几何对象有效性
                            OGRGeometry* poGeometry = vMissingGeometries[i];
                            if (poGeometry == nullptr)
                            {
                                logger->error("要素 {} 的几何对象为空", i);
                            }
                            else if (!poGeometry->IsValid())
                            {
                                logger->warn("要素 {} 的几何对象无效，尝试修复", i);
                                OGRGeometry* poValidGeometry = poGeometry->MakeValid();
                                if (poValidGeometry != nullptr)
                                {
                                    poResultFeature->SetGeometry(poValidGeometry);
                                    OGRGeometryFactory::destroyGeometry(poValidGeometry);
                                }
                                else
                                {
                                    poResultFeature->SetGeometry(poGeometry);
                                }
                            }
                            else
                            {
                                poResultFeature->SetGeometry(poGeometry);
                            }
                            
                            OGRErr eErr = poResultLayer->CreateFeature(poResultFeature);
                            if (eErr != OGRERR_NONE)
                            {
                                logger->error("添加缺失要素失败，要素索引: {}, 错误代码: {}, SourceID: {}", 
                                             i, (int)eErr, i < vMissingFeatureIDs.size() ? vMissingFeatureIDs[i] : -1);
                            }
                            else
                            {
                                logger->debug("成功添加要素 {}", i);
                            }
                            
                            OGRFeature::DestroyFeature(poResultFeature);
                        }
                        
                        logger->info("成功创建缺失要素图层，包含 {} 个要素", vMissingGeometries.size());
                    }
                }
            }
        }
        else
        {
            if (bFeatureMissingDetected)
            {
                logger->warn("检测到缺失但vMissingGeometries为空，无法创建输出图层");
            }
            else
            {
                logger->info("未检测到缺失，不创建输出图层");
            }
        }

        // 步骤4：生成检查报告
        logger->info("步骤4：开始生成检查报告");
        
        // 创建JSON输出（去掉 errors 字段）
        QJsonObject jsonResult;
        QJsonObject checkItem;
        checkItem["id"] = "102";
        checkItem["name"] = "要素缺失检查";
        if (bFeatureMissingDetected) {
            checkItem["status"] = "质检完成";
            checkItem["err_exist"] = "true";
            QString infoMsg = QString("%1图层存在要素缺失").arg(QString::fromStdString(params.strLayerName));
            checkItem["info"] = infoMsg;
        } else {
            checkItem["status"] = "质检完成";
            checkItem["err_exist"] = "false";
            QString infoMsg = QString("%1图层不存在要素缺失").arg(QString::fromStdString(params.strLayerName));
            checkItem["info"] = infoMsg;
        }
        jsonResult["check_item"] = checkItem;
        
        // 写入JSON文件
        QJsonDocument doc(jsonResult);
        QFile jsonFile(szOutputJsonPath);
        if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            jsonFile.write(doc.toJson());
            jsonFile.close();
            logger->info("已生成JSON结果文件：{}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));
        } else {
            logger->error("无法创建JSON结果文件：{}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));
        }

        // 资源清理
        logger->info("开始清理资源...");
        
        // 释放缓冲区几何对象
        for (auto& poGeometry : vBufferedGeometries)
        {
            OGRGeometryFactory::destroyGeometry(poGeometry);
        }
        
        // 释放解析后的几何对象
        for (auto& poGeometry : vParsedGeometries)
        {
            OGRGeometryFactory::destroyGeometry(poGeometry);
        }
        
        // 释放缺失要素的几何对象
        for (auto& poGeometry : vMissingGeometries)
        {
            OGRGeometryFactory::destroyGeometry(poGeometry);
        }

        // 关闭数据集
        GDALClose(poSourceDS);
        if (poResultDS != NULL)
        {
            GDALClose(poResultDS);
        }

        logger->info("要素缺失检查完成");
        logger->info("检测结果：{}", strMissingType);
        logger->info("缺失要素数量：{}", nMissingFeatureCount);
        logger->flush();
        
        return SE_ERROR_NONE;
    }
    catch (const std::exception& e)
    {
        logger->error("执行要素缺失检查时发生异常：{}", e.what());
        logger->flush();
        return SE_ERROR_FAILURE;
    }
}
