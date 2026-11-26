// 接口版实现（无 UI）：光滑度阈值控制
#define _HAS_STD_BYTE 0
#include "SmoothnessChecker.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDomDocument>
#include <QDomElement>
#include <QDir>
#include <QVector>
#include <QPointF>
#include <QString>
#include <QFileInfo>

#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"

#include <cpl_string.h>
#include <cpl_conv.h>
#include <string>
#include <limits>
#include <cmath>
#include <unordered_map>

// 作用域守卫：设置 CPL 配置并在析构时恢复原值，避免污染全局状态
struct CplConfigGuard
{
    const char *key;
    std::string prev;
    bool hadPrev;
    CplConfigGuard(const char *k, const char *v) : key(k), hadPrev(false)
    {
        const char *p = CPLGetConfigOption(k, nullptr);
        if (p)
        {
            prev = p;
            hadPrev = true;
        }
        CPLSetConfigOption(k, v);
    }
    ~CplConfigGuard()
    {
        CPLSetConfigOption(key, hadPrev ? prev.c_str() : nullptr);
    }
};

// 字符编码辅助（与特征点限差控制保持一致）
static QString fromUtf8ToQString(const char *utf8Str)
{
    if (!utf8Str)
        return QString();
    return QString::fromUtf8(utf8Str);
}

static std::string fromQStringToUtf8(const QString &qstr)
{
    QByteArray utf8Bytes = qstr.toUtf8();
    return std::string(utf8Bytes.constData(), utf8Bytes.length());
}
// 自动判断输入字节是否为 UTF-8；否则按本地编码解码
static bool isValidUtf8(const char *s)
{
    if (!s)
        return false;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(s);
    while (*p)
    {
        if (*p < 0x80)
        {
            ++p;
            continue;
        }
        int extra = 0;
        if ((*p & 0xE0) == 0xC0)
            extra = 1;
        else if ((*p & 0xF0) == 0xE0)
            extra = 2;
        else if ((*p & 0xF8) == 0xF0)
            extra = 3;
        else
            return false;
        ++p;
        for (int i = 0; i < extra; ++i)
        {
            if ((p[i] & 0xC0) != 0x80)
                return false;
        }
        p += extra;
    }
    return true;
}
static QString fromAutoToQString(const char *bytes)
{
    if (!bytes)
        return QString();
    return isValidUtf8(bytes) ? QString::fromUtf8(bytes) : QString::fromLocal8Bit(bytes);
}

static std::string fileNameWithoutExt(const std::string &fullPath)
{
    QString q = fromUtf8ToQString(fullPath.c_str());
    int lastSlash = q.lastIndexOf('/');
    if (lastSlash == -1)
        lastSlash = q.lastIndexOf('\\');
    if (lastSlash != -1)
        q = q.mid(lastSlash + 1);
    int lastDot = q.lastIndexOf('.');
    if (lastDot != -1)
        q = q.left(lastDot);
    return fromQStringToUtf8(q);
}

struct SmoothnessRuleParams
{
    double dMapThresholdMM = 0.0; // 图上阈值（毫米）
    std::string strLayerName;     // 图层名称（XML文件名）
    bool bValid = false;
};

static SmoothnessRuleParams ParseXmlForSmoothnessRuleParams(const std::string &xmlFilePath,
                                                            int scale,
                                                            std::shared_ptr<spdlog::logger> logger)
{
    SmoothnessRuleParams params;
    params.dMapThresholdMM = 0.0;
    params.strLayerName = fileNameWithoutExt(xmlFilePath);
    params.bValid = false;

    QFile file(fromAutoToQString(xmlFilePath.c_str()));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        logger->error("无法打开XML文件：{}", fromQStringToUtf8(fromAutoToQString(xmlFilePath.c_str())));
        return params;
    }

    QDomDocument doc;
    if (!doc.setContent(&file))
    {
        logger->error("XML文件格式错误：{}", fromQStringToUtf8(fromAutoToQString(xmlFilePath.c_str())));
        file.close();
        return params;
    }
    file.close();

    QDomElement root = doc.documentElement();
    if (root.tagName() != "check_list")
    {
        logger->warn("XML根元素不是check_list，按兼容模式解析");
    }

    QDomNodeList checkItems = root.elementsByTagName("check_item");
    for (int j = 0; j < checkItems.count(); ++j)
    {
        QDomElement checkItem = checkItems.at(j).toElement();
        QString name = checkItem.firstChildElement("name").text().trimmed();
        if (name == QStringLiteral("光滑度阈值控制"))
        {
            QDomNodeList ruleNodes = checkItem.elementsByTagName("rule");
            for (int k = 0; k < ruleNodes.count(); ++k)
            {
                QDomElement ruleElement = ruleNodes.at(k).toElement();
                QString rname = ruleElement.firstChildElement("name").text().trimmed();
                QString rvalue = ruleElement.firstChildElement("value").text().trimmed();
                QString runit = ruleElement.firstChildElement("unit").text().trimmed();
                const QStringList toleranceNames = {QStringLiteral("光滑度阈值")};
                if (!toleranceNames.contains(rname))
                    continue;
                bool okv = false;
                double dv = rvalue.toDouble(&okv);
                if (!okv || dv <= 0.0)
                    continue;
                double dv_mm = dv;
                QString u = runit.toLower();
                if (u == QStringLiteral("毫米") || u == QStringLiteral("mm") || u.contains("millimeter"))
                {
                    dv_mm = dv;
                }
                else if (u == QStringLiteral("厘米") || u == QStringLiteral("cm") || u.contains("centimeter"))
                {
                    dv_mm = dv * 10.0;
                }
                else if (u == QStringLiteral("米") || u == QStringLiteral("m") || u.contains("meter"))
                {
                    dv_mm = dv * 1000.0;
                }
                else if (!u.isEmpty())
                {
                    logger->warn("未知单位 '{}', 按毫米处理", fromQStringToUtf8(u));
                }
                params.dMapThresholdMM = dv_mm;
                params.bValid = true;
                logger->info("解析到光滑度阈值：{} 毫米", dv_mm);
                return params;
            }
            QString directVal = checkItem.firstChildElement("value").text().trimmed();
            if (!directVal.isEmpty())
            {
                bool okv2 = false;
                double dv2 = directVal.toDouble(&okv2);
                if (okv2 && dv2 > 0.0)
                {
                    params.dMapThresholdMM = dv2;
                    params.bValid = true;
                    logger->info("解析到光滑度阈值(兼容写法)：{} 毫米", dv2);
                    return params;
                }
            }
            const QStringList altFields = {"ThresholdMM", "ToleranceMm", "Tolerance", "mm", "距离阈值"};
            for (const QString &fn : altFields)
            {
                QString tv = checkItem.firstChildElement(fn).text().trimmed();
                if (!tv.isEmpty())
                {
                    bool okv3 = false;
                    double dv3 = tv.toDouble(&okv3);
                    if (okv3 && dv3 > 0.0)
                    {
                        params.dMapThresholdMM = dv3;
                        params.bValid = true;
                        logger->info("解析到光滑度阈值(字段{})：{} 毫米", fromQStringToUtf8(fn), dv3);
                        return params;
                    }
                }
            }
        }
    }

    QDomNodeList scaleNodes = root.elementsByTagName("scale");
    bool foundScale = false;

    for (int i = 0; i < scaleNodes.count(); ++i)
    {
        QDomElement scaleElement = scaleNodes.at(i).toElement();
        QDomNodeList valueNodes = scaleElement.elementsByTagName("value");
        if (valueNodes.count() == 0)
            continue;

        bool ok = false;
        int xmlScale = valueNodes.at(0).toElement().text().toInt(&ok);
        if (!ok)
            continue;

        if (xmlScale == scale)
        {
            foundScale = true;
            logger->info("按比例尺查找：{}", scale);
            QDomNodeList checkItems = scaleElement.elementsByTagName("check_item");
            for (int j = 0; j < checkItems.count(); ++j)
            {
                QDomElement checkItem = checkItems.at(j).toElement();
                // 仅取检查项的直接子元素 <name>
                QString name = checkItem.firstChildElement("name").text().trimmed();
                if (name == QStringLiteral("光滑度阈值控制"))
                {
                    // 读取规则：<rule><name>光滑度阈值</name><value>0.36</value><unit>毫米</unit>
                    QDomNodeList ruleNodes = checkItem.elementsByTagName("rule");
                    for (int k = 0; k < ruleNodes.count(); ++k)
                    {
                        QDomElement ruleElement = ruleNodes.at(k).toElement();
                        QString rname = ruleElement.firstChildElement("name").text().trimmed();
                        QString rvalue = ruleElement.firstChildElement("value").text().trimmed();
                        QString runit = ruleElement.firstChildElement("unit").text().trimmed();

                        const QStringList toleranceNames = {
                            QStringLiteral("光滑度阈值")};
                        if (!toleranceNames.contains(rname))
                            continue;

                        bool okv = false;
                        double dv = rvalue.toDouble(&okv);
                        if (!okv || dv <= 0.0)
                            continue;

                        // 单位转换到“图上毫米”
                        double dv_mm = dv; // 默认按毫米
                        QString u = runit.toLower();
                        if (u == QStringLiteral("毫米") || u == QStringLiteral("mm") || u.contains("millimeter"))
                        {
                            dv_mm = dv;
                        }
                        else if (u == QStringLiteral("厘米") || u == QStringLiteral("cm") || u.contains("centimeter"))
                        {
                            dv_mm = dv * 10.0; // cm -> mm
                        }
                        else if (u == QStringLiteral("米") || u == QStringLiteral("m") || u.contains("meter"))
                        {
                            dv_mm = dv * 1000.0; // m -> mm
                        }
                        else if (!u.isEmpty())
                        {
                            logger->warn("未知单位 '{}', 按毫米处理", fromQStringToUtf8(u));
                        }

                        params.dMapThresholdMM = dv_mm;
                        params.bValid = true;
                        logger->info("解析到光滑度阈值：{} 毫米 (比例尺1:{})", dv_mm, scale);
                        return params;
                    }
                    // 兼容：直接在检查项上写 value
                    QString directVal = checkItem.firstChildElement("value").text().trimmed();
                    if (!directVal.isEmpty())
                    {
                        bool okv2 = false;
                        double dv2 = directVal.toDouble(&okv2);
                        if (okv2 && dv2 > 0.0)
                        {
                            params.dMapThresholdMM = dv2;
                            params.bValid = true;
                            logger->info("解析到光滑度阈值(兼容写法)：{} 毫米 (比例尺1:{})", dv2, scale);
                            return params;
                        }
                    }
                    // 进一步兼容：常见字段名
                    const QStringList altFields = {"ThresholdMM", "ToleranceMm", "Tolerance", "mm", "距离阈值"};
                    for (const QString &fn : altFields)
                    {
                        QString tv = checkItem.firstChildElement(fn).text().trimmed();
                        if (!tv.isEmpty())
                        {
                            bool okv3 = false;
                            double dv3 = tv.toDouble(&okv3);
                            if (okv3 && dv3 > 0.0)
                            {
                                params.dMapThresholdMM = dv3;
                                params.bValid = true;
                                logger->info("解析到光滑度阈值(字段{}): {} 毫米 (比例尺1:{})", fromQStringToUtf8(fn), dv3, scale);
                                return params;
                            }
                        }
                    }
                }
            }
            logger->warn("在比例尺{}下未找到‘光滑度阈值控制’项或有效阈值", scale);
            break;
        }
    }

    if (!foundScale)
    {
        logger->warn("未找到匹配的比例尺：{}", scale);
    }

    return params;
}

// 替换原有的 computeRadiusUnits 函数
// 增加 epsilon 检查，防止因重合点或极短线段导致的计算异常
static inline double computeRadiusUnits(const QPointF &p1, const QPointF &p2, const QPointF &p3)
{
    // 计算三边长度
    double a = std::hypot(p2.x() - p3.x(), p2.y() - p3.y()); // p2-p3
    double b = std::hypot(p1.x() - p3.x(), p1.y() - p3.y()); // p1-p3 (跨度)
    double c = std::hypot(p1.x() - p2.x(), p1.y() - p2.y()); // p1-p2

    // 【关键修复1】过滤微小线段/重合点
    // 如果边长小于 1e-5 (图面单位)，认为不够构成一个有效的弯曲，跳过（返回无穷大视为光滑）
    // 这能有效解决图片中因数字化节点过密或重叠导致的误报
    const double epsilon = 1e-5;
    if (a < epsilon || c < epsilon)
        return std::numeric_limits<double>::infinity();

    // 海伦公式计算面积 (使用叉积绝对值计算更稳定)
    double cross_product = (p2.x() - p1.x()) * (p3.y() - p1.y()) - (p3.x() - p1.x()) * (p2.y() - p1.y());
    double area = 0.5 * std::abs(cross_product);

    // 【关键修复2】三点共线处理
    // 如果面积极小，说明三点共线，半径无穷大
    if (area < 1e-12)
        return std::numeric_limits<double>::infinity();

    // R = abc / 4S
    return (a * b * c) / (4.0 * area);
}

static bool CreateErrorPointsShapefile(const QString &outPath,
                                       OGRSpatialReference *poSRS,
                                       const std::vector<QPointF> &errorPts,
                                       const std::vector<double> &errorRadiiUnits,
                                       double thresholdUnits,
                                       const std::string &layerName,
                                       std::shared_ptr<spdlog::logger> logger)
{
    // 清理同名Shapefile集合
    QFileInfo fi(outPath);
    QString baseNoExt = fi.path() + QDir::separator() + fi.completeBaseName();
    QStringList exts = {".shp", ".shx", ".dbf", ".prj", ".cpg"};
    for (const QString &ext : exts)
    {
        QString p = baseNoExt + ext;
        QFile::remove(p);
    }

    GDALDriver *poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (!poDriver)
    {
        logger->error("未找到 Shapefile 驱动");
        return false;
    }

    // 以本地编码传递输出路径给GDAL，作用域结束后自动恢复
    CplConfigGuard guardUtf8Yes("GDAL_FILENAME_IS_UTF8", "YES");
    QByteArray outUtf8 = QDir::toNativeSeparators(outPath).toUtf8();
    GDALDataset *poDSOut = poDriver->Create(outUtf8.constData(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!poDSOut)
    {
        logger->error("创建输出Shapefile失败：{}", fromQStringToUtf8(outPath));
        return false;
    }

    // 写CPG指定DBF编码为UTF-8
    {
        QString cpgPath = QDir::toNativeSeparators(baseNoExt + ".cpg");
        QFile cpgFile(cpgPath);
        if (cpgFile.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            cpgFile.write("UTF-8");
            cpgFile.close();
            logger->info("写入CPG编码文件：{}", fromQStringToUtf8(cpgPath));
        }
        else
        {
            logger->warn("无法写入CPG编码文件：{}", fromQStringToUtf8(cpgPath));
        }
    }

    OGRSpatialReference *poCloneSRS = poSRS ? poSRS->Clone() : nullptr;
    char **lco = nullptr;
    lco = CSLSetNameValue(lco, "ENCODING", "UTF-8");
    OGRLayer *poLayer = poDSOut->CreateLayer("error_points", poCloneSRS, wkbPoint, lco);
    CSLDestroy(lco);
    if (!poLayer)
    {
        logger->error("创建图层失败");
        GDALClose(poDSOut);
        if (poCloneSRS)
            poCloneSRS->Release();
        return false;
    }

    OGRFieldDefn fLayerName("LayerName", OFTString);
    fLayerName.SetWidth(80);
    poLayer->CreateField(&fLayerName);
    OGRFieldDefn fFeatureID("FeatureID", OFTString);
    fFeatureID.SetWidth(32);
    poLayer->CreateField(&fFeatureID);
    OGRFieldDefn fIndex("Index", OFTInteger);
    poLayer->CreateField(&fIndex);
    OGRFieldDefn fRadius("Radius", OFTReal);
    fRadius.SetWidth(16);
    fRadius.SetPrecision(6);
    poLayer->CreateField(&fRadius);
    OGRFieldDefn fThreshold("Threshold", OFTReal);
    fThreshold.SetWidth(16);
    fThreshold.SetPrecision(6);
    poLayer->CreateField(&fThreshold);
    OGRFieldDefn fDescription("Description", OFTString);
    fDescription.SetWidth(128);
    poLayer->CreateField(&fDescription);

    for (size_t i = 0; i < errorPts.size(); ++i)
    {
        OGRFeature *oFeat = OGRFeature::CreateFeature(poLayer->GetLayerDefn());
        OGRPoint pt(errorPts[i].x(), errorPts[i].y());
        oFeat->SetGeometry(&pt);
        oFeat->SetField("LayerName", QString::fromStdString(layerName).toUtf8().constData());
        char idbuf[32];
        snprintf(idbuf, sizeof(idbuf), "%zu", i);
        oFeat->SetField("FeatureID", idbuf);
        oFeat->SetField("Index", static_cast<int>(i));
        oFeat->SetField("Radius", errorRadiiUnits[i]);
        oFeat->SetField("Threshold", thresholdUnits);
        oFeat->SetField("Description", QStringLiteral("半径小于阈值").toUtf8().constData());
        if (poLayer->CreateFeature(oFeat) != OGRERR_NONE)
        {
            logger->warn("写入错误点失败：index={}", i);
        }
        OGRFeature::DestroyFeature(oFeat);
    }

    GDALClose(poDSOut);
    if (poCloneSRS)
        poCloneSRS->Release();
    logger->info("已输出错误点图层：{} (数量={})", fromQStringToUtf8(outPath), errorPts.size());
    return true;
}

// ==========================================================================
// 最终完善版：三级分治光滑度检查
// ==========================================================================

extern "C" MY_PLUGIN_API SE_Error SmoothnessThresholdCheck(
    const char *szSchemaXmlPath,
    const char *szParsedDataPath,
    int scale,
    const char *szOutputErrorPointsLayerPath,
    const char *szOutputJsonPath,
    const char *szOutputLogFilePath)
{
    // 1. 初始化日志
    QString logPathQ = fromUtf8ToQString(szOutputLogFilePath);
    QByteArray logPathLocalBytes = logPathQ.toLocal8Bit();
    std::string logPathLocal(logPathLocalBytes.constData(), logPathLocalBytes.size());
    std::shared_ptr<spdlog::logger> logger;
    try
    {
        logger = spdlog::basic_logger_mt("SmoothnessThreshold", logPathLocal);
    }
    catch (...)
    {
        logger = spdlog::get("SmoothnessThreshold");
        if (!logger)
            logger = spdlog::default_logger();
    }
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    logger->info("开始执行光滑度阈值控制检查");

    // 2. 解析 XML 参数
    SmoothnessRuleParams params = ParseXmlForSmoothnessRuleParams(szSchemaXmlPath, scale, logger);
    std::string layerName = params.strLayerName;

    auto reportFailure = [&](const QString &reason)
    {
        logger->error("{}", reason.toStdString());
        QJsonObject jsonResult;
        QJsonObject checkItem;
        checkItem["id"] = "108";
        checkItem["name"] = "光滑度阈值控制";
        checkItem["status"] = "质检失败";
        checkItem["err_exist"] = "false";
        checkItem["info"] = QString("%1图层执行失败（%2）").arg(QString::fromStdString(layerName)).arg(reason);
        checkItem["errors"] = QJsonObject();
        QJsonObject root;
        root["check_item"] = checkItem;
        QJsonDocument doc(root);
        QFile jsonFile(fromAutoToQString(szOutputJsonPath));
        if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            jsonFile.write(doc.toJson());
        }
        spdlog::drop("SmoothnessThreshold");
        return SE_ERROR_FAILURE;
    };

    if (!params.bValid || params.dMapThresholdMM <= 0.0)
    {
        return reportFailure("XML中未解析到有效的光滑度阈值");
    }

    // 3. 计算阈值
    double thresholdMeters = (params.dMapThresholdMM / 1000.0) * static_cast<double>(scale);

    // =======================================================
    // 【三级分治参数配置】
    // =======================================================

    // 等级1：极平缓区域 (> 160度)
    // 容差：0.3mm (非常宽松)。用于忽略长弧线上的所有噪点。
    double tier1_Flat_Angle = 160.0;
    double tier1_Flat_TolMM = 0.3;

    // 等级2：过渡区域 (130度 ~ 160度)
    // 容差：0.05mm (严格，但也允许极微小的抖动)。
    // 核心逻辑：绿色面的密集点虽然角度在此范围，但弓高极小(<0.05mm)，会被放过。
    //          紫色突起虽然角度也在此范围，但弓高稍大(>0.05mm)，会被抓住。
    double tier2_Mid_Angle = 130.0;
    double tier2_Mid_TolMM = 0.05;

    // 等级3：尖锐区域 (<= 130度)
    // 容差：0.0mm (零容忍)。明显的折角，必须报错。

    // 预计算 Cos 值
    double cos_Tier1 = std::cos(tier1_Flat_Angle * M_PI / 180.0); // cos(160) ≈ -0.939
    double cos_Tier2 = std::cos(tier2_Mid_Angle * M_PI / 180.0);  // cos(130) ≈ -0.642

    // 转换为实地单位
    double tier1_TolMeters = (tier1_Flat_TolMM / 1000.0) * static_cast<double>(scale);
    double tier2_TolMeters = (tier2_Mid_TolMM / 1000.0) * static_cast<double>(scale);

    logger->info("阈值设定: 半径={}米", thresholdMeters);
    logger->info("三级策略:");
    logger->info("  [1] 极平缓 (>{}度): 忽略弓高 < {}米 (图上{}mm)", tier1_Flat_Angle, tier1_TolMeters, tier1_Flat_TolMM);
    logger->info("  [2] 过渡区 ({}~{}度): 忽略弓高 < {}米 (图上{}mm)", tier2_Mid_Angle, tier1_Flat_Angle, tier2_TolMeters, tier2_Mid_TolMM);
    logger->info("  [3] 尖锐区 (<{}度): 零容忍", tier2_Mid_Angle);

    // 4. GDAL 初始化
    CplConfigGuard guardShapeEnc("SHAPE_ENCODING", "UTF-8");
    static bool gdalInitialized = false;
    if (!gdalInitialized)
    {
        GDALAllRegister();
        gdalInitialized = true;
    }

    QString parsedPathAll = fromAutoToQString(szParsedDataPath);
    QStringList parsedPaths = parsedPathAll.split(';', Qt::SkipEmptyParts);
    if (parsedPaths.isEmpty())
        return reportFailure("解析数据路径为空");

    std::vector<QPointF> errorPts;
    std::vector<double> errorRadiiUnits;
    long processedFeatures = 0;
    OGRSpatialReference *poSRSFirst = nullptr;

    // 循环内单位换算变量
    double thresholdUnits = 0.0;
    double tolUnits_Tier1 = 0.0;
    double tolUnits_Tier2 = 0.0;

    // =======================================================
    // 核心检查函数
    // =======================================================
    auto checkPointSequence = [&](const std::vector<QPointF> &rawPts, bool isClosed)
    {
        if (rawPts.size() < 3)
            return;

        // 1. 数据清洗：移除重合点 (距离 < 实地0.1mm)
        // 这一步很重要，避免重合点造成计算出的半径极小
        double min_dist_sq = 1e-8; // 近似0
        std::vector<QPointF> pts;
        pts.reserve(rawPts.size());
        pts.push_back(rawPts[0]);
        for (size_t i = 1; i < rawPts.size(); ++i)
        {
            double dx = rawPts[i].x() - pts.back().x();
            double dy = rawPts[i].y() - pts.back().y();
            if ((dx * dx + dy * dy) > min_dist_sq)
                pts.push_back(rawPts[i]);
        }

        int n = static_cast<int>(pts.size());
        if (n < 3)
            return;

        // 2. 闭合性处理
        bool trulyClosed = isClosed;
        if (isClosed)
        {
            double dx = pts[0].x() - pts[n - 1].x();
            double dy = pts[0].y() - pts[n - 1].y();
            if ((dx * dx + dy * dy) < min_dist_sq)
            {
                pts.pop_back();
                n--;
                trulyClosed = true;
            }
            else
            {
                trulyClosed = false;
            }
        }
        if (n < 3)
            return;

        // 3. 遍历检查
        for (int i = 0; i < n; ++i)
        {
            if (!trulyClosed && (i == 0 || i == n - 1))
                continue;

            int idxPrev = (i - 1 + n) % n;
            int idxNext = (i + 1) % n;
            int idxCurr = i;

            const QPointF &p1 = pts[idxPrev];
            const QPointF &p2 = pts[idxCurr];
            const QPointF &p3 = pts[idxNext];

            double a = std::hypot(p2.x() - p3.x(), p2.y() - p3.y());
            double b = std::hypot(p1.x() - p3.x(), p1.y() - p3.y()); // 底边
            double c = std::hypot(p1.x() - p2.x(), p1.y() - p2.y());

            // 避免除零
            if (a < 1e-7 || c < 1e-7)
                continue;

            double cross_product = (p2.x() - p1.x()) * (p3.y() - p1.y()) - (p3.x() - p1.x()) * (p2.y() - p1.y());
            double area = 0.5 * std::abs(cross_product);

            // 计算半径
            double r = (area < 1e-12) ? std::numeric_limits<double>::infinity() : ((a * b * c) / (4.0 * area));

            // 半径达标 -> 合格
            if (r >= thresholdUnits)
                continue;

            // --- 半径不达标，根据角度分级判别 ---

            // 计算 Cos Theta
            // cos值越小，角度越小(越尖)
            double cos_theta = (a * a + c * c - b * b) / (2.0 * a * c);
            if (cos_theta > 1.0)
                cos_theta = 1.0;
            if (cos_theta < -1.0)
                cos_theta = -1.0;

            // 计算弓高 (Sagitta)
            double h = (b > 1e-9) ? ((2.0 * area) / b) : 0.0;

            // 确定当前使用的容差
            double currentTolerance = 0.0;

            if (cos_theta < cos_Tier1)
            {
                // 角度 > 160度 (极平缓)
                // 对应：绿色面大范围
                currentTolerance = tolUnits_Tier1; // 0.3mm
            }
            else if (cos_theta < cos_Tier2)
            {
                // 130度 < 角度 <= 160度 (过渡区)
                // 对应：绿色面密集点 (h很小 -> Pass)
                // 对应：紫色突起 (h较大 -> Fail)
                currentTolerance = tolUnits_Tier2; // 0.05mm
            }
            else
            {
                // 角度 <= 130度 (尖锐)
                // 对应：锐角折点
                currentTolerance = 0.0; // 零容忍
            }

            // 只有当突起高度超过当前等级的容差时，才报错
            if (h > currentTolerance)
            {
                errorPts.push_back(p2);
                errorRadiiUnits.push_back(r);
            }
        }
    };

    // 辅助 lambda
    auto processLineString = [&](const OGRLineString *ls)
    {
        if (!ls)
            return;
        int n = ls->getNumPoints();
        std::vector<QPointF> pts;
        pts.reserve(n);
        for (int i = 0; i < n; ++i)
            pts.emplace_back(ls->getX(i), ls->getY(i));
        checkPointSequence(pts, false);
    };
    auto processRing = [&](const OGRLinearRing *ring)
    {
        if (!ring)
            return;
        int n = ring->getNumPoints();
        std::vector<QPointF> pts;
        pts.reserve(n);
        for (int i = 0; i < n; ++i)
            pts.emplace_back(ring->getX(i), ring->getY(i));
        checkPointSequence(pts, true);
    };

    // 5. 遍历处理文件
    CplConfigGuard guardUtf8No2("GDAL_FILENAME_IS_UTF8", "NO");
    for (const QString &rawPath : parsedPaths)
    {
        QString parsedPath = rawPath.trimmed();
        if (parsedPath.isEmpty())
            continue;

        QString baseName = QFileInfo(parsedPath).completeBaseName();
        QString lowerBase = baseName.toLower();
        if (lowerBase.contains("_point"))
            continue;
        if (!lowerBase.contains("_linestring") && !lowerBase.contains("_polygon"))
            continue;

        logger->info("处理文件：{}", fromQStringToUtf8(parsedPath));
        QByteArray parsedNative = QFile::encodeName(QDir::toNativeSeparators(parsedPath));
        GDALDataset *poParsedDS = (GDALDataset *)GDALOpenEx(parsedNative.constData(), GDAL_OF_VECTOR, NULL, NULL, NULL);

        if (!poParsedDS)
        {
            logger->warn("无法打开数据");
            continue;
        }
        OGRLayer *layer = poParsedDS->GetLayer(0);
        if (!layer)
        {
            GDALClose(poParsedDS);
            logger->warn("无法读取图层");
            continue;
        }

        if (!poSRSFirst)
        {
            OGRSpatialReference *s = layer->GetSpatialRef();
            if (s)
                poSRSFirst = s->Clone();
        }

        // 单位换算
        OGRSpatialReference *poSRSLayer = layer->GetSpatialRef();
        double metersPerUnit = 1.0;
        if (poSRSLayer)
        {
            if (poSRSLayer->IsGeographic())
            {
                metersPerUnit = 111319.5;
            }
            else
            {
                const char *unitName = nullptr;
                metersPerUnit = poSRSLayer->GetLinearUnits(&unitName);
                if (metersPerUnit <= 0.0)
                    metersPerUnit = 1.0;
            }
        }

        thresholdUnits = thresholdMeters / metersPerUnit;
        tolUnits_Tier1 = tier1_TolMeters / metersPerUnit;
        tolUnits_Tier2 = tier2_TolMeters / metersPerUnit;

        logger->info("单位换算系数: {}, 阈值单位: {}", metersPerUnit, thresholdUnits);

        layer->ResetReading();
        OGRFeature *feat = nullptr;
        while ((feat = layer->GetNextFeature()) != nullptr)
        {
            processedFeatures++;
            OGRGeometry *geom = feat->GetGeometryRef();
            if (geom)
            {
                OGRwkbGeometryType gtype = wkbFlatten(geom->getGeometryType());
                if (gtype == wkbLineString)
                    processLineString(geom->toLineString());
                else if (gtype == wkbMultiLineString)
                {
                    OGRGeometryCollection *gc = geom->toGeometryCollection();
                    for (int i = 0; i < gc->getNumGeometries(); ++i)
                        processLineString(gc->getGeometryRef(i)->toLineString());
                }
                else if (gtype == wkbPolygon)
                {
                    OGRPolygon *poly = geom->toPolygon();
                    processRing(poly->getExteriorRing());
                    for (int i = 0; i < poly->getNumInteriorRings(); ++i)
                        processRing(poly->getInteriorRing(i));
                }
                else if (gtype == wkbMultiPolygon)
                {
                    OGRGeometryCollection *gc = geom->toGeometryCollection();
                    for (int i = 0; i < gc->getNumGeometries(); ++i)
                    {
                        OGRPolygon *poly = (OGRPolygon *)gc->getGeometryRef(i);
                        processRing(poly->getExteriorRing());
                        for (int k = 0; k < poly->getNumInteriorRings(); ++k)
                            processRing(poly->getInteriorRing(k));
                    }
                }
            }
            OGRFeature::DestroyFeature(feat);
        }
        GDALClose(poParsedDS);
    }

    logger->info("检查完成，发现错误点：{}", errorPts.size());

    // 输出部分
    bool hasError = !errorPts.empty();
    if (hasError)
    {
        CreateErrorPointsShapefile(fromAutoToQString(szOutputErrorPointsLayerPath), poSRSFirst, errorPts, errorRadiiUnits, thresholdUnits, layerName, logger);
    }

    QJsonObject checkItem;
    checkItem["id"] = "108";
    checkItem["name"] = "光滑度阈值控制";
    checkItem["status"] = "质检完成";
    checkItem["err_exist"] = hasError ? "true" : "false";
    checkItem["info"] = QString("%1图层%2光滑度阈值错误").arg(QString::fromStdString(layerName)).arg(hasError ? QStringLiteral("存在") : QStringLiteral("不存在"));
    checkItem["errors"] = QJsonObject();
    QJsonObject root;
    root["check_item"] = checkItem;
    QJsonDocument doc(root);
    QFile jsonFile(fromAutoToQString(szOutputJsonPath));
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        jsonFile.write(doc.toJson());
    }

    if (poSRSFirst)
        poSRSFirst->Release();
    spdlog::drop("SmoothnessThreshold");
    return SE_ERROR_NONE;
}