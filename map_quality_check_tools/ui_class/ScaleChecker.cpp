// 算法版：投影参数准确性检查
#include "ScaleChecker.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <cpl_string.h>
#include <cpl_conv.h>

#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QMap>
#include <QSet>
#include <QtMath>
#include <limits>

/*spdlog日志库*/
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"

// 作用域守卫：设置 CPL 配置并在析构时恢复原值，避免污染全局状态
struct CplConfigGuard {
    const char* key;
    std::string prev;
    bool hadPrev;
    CplConfigGuard(const char* k, const char* v) : key(k), hadPrev(false) {
        const char* p = CPLGetConfigOption(k, nullptr);
        if (p) { prev = p; hadPrev = true; }
        CPLSetConfigOption(k, v);
    }
    ~CplConfigGuard() {
        CPLSetConfigOption(key, hadPrev ? prev.c_str() : nullptr);
    }
};

// 编码转换辅助函数（参考其他算法实现）
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

// 原始字符串正则：匹配 PARAMETER["name", value]
static QRegularExpression paramRegex(
    R"regex(PARAMETER\[\s*"([^"]+)"\s*,\s*([^,\]]+)\s*\])regex",
    QRegularExpression::CaseInsensitiveOption);

// 归一化参数名
static QString normalizeParamName(const QString& name) {
    QString s = name.trimmed().toLower();
    s.replace(QRegularExpression(R"regex([^\w]+)regex"), "_");
    s.replace(QRegularExpression(R"regex(_{2,})regex"), "_");
    s.replace(QRegularExpression(R"regex(^_+|_+$)regex"), "");
    return s;
}

// 宽松解析数字
static bool parseDoubleLoose(const QString& token, double& out)
{
    QString t = token.trimmed();
    if ((t.startsWith("\"") && t.endsWith("\"")) || (t.startsWith("'") && t.endsWith("'")))
        t = t.mid(1, t.length() - 2).trimmed();

    QRegularExpression rxNumber(R"(^\s*([-+]?\d+(\.\d+)?([eE][-+]?\d+)?))");
    QRegularExpressionMatch m = rxNumber.match(t);
    if (m.hasMatch()) {
        bool ok = false;
        double v = m.captured(1).toDouble(&ok);
        if (ok) { out = v; return true; }
    }
    return false;
}

// 从 WKT 中提取参数值（若无返回 NaN），含常见别名
static double extractWktParamNaN(const QString& wkt, const QString& paramName)
{
    QRegularExpressionMatchIterator it = paramRegex.globalMatch(wkt);

    QMap<QString, double> params;
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        QString name = m.captured(1);
        QString valToken = m.captured(2);

        QString key = normalizeParamName(name);
        double v;
        if (parseDoubleLoose(valToken, v)) {
            params.insert(key, v);
        }
    }

    QString key = normalizeParamName(paramName);
    QStringList aliases; aliases << key;
    if (key.contains("central") || key.contains("meridian") || key.contains("longitude")) {
        aliases << "longitude_of_center" << "longitude_of_centre" << "central_meridian"
                << "longitude_of_origin" << "longitude_of_central_meridian";
    }
    if (key.contains("latitude") || key.contains("origin") || key.contains("center")) {
        aliases << "latitude_of_origin" << "latitude_of_center" << "latitude_of_centre" << "latitude_of_center_of_projection";
    }
    if (key.contains("standard_parallel") || key.contains("std_parallel")) {
        aliases << "standard_parallel_1" << "standard_parallel_2" << "std_parallel_1" << "std_parallel_2";
    }

    QSet<QString> seen;
    for (const QString& a : aliases) {
        QString kk = normalizeParamName(a);
        if (seen.contains(kk)) continue;
        seen.insert(kk);
        if (params.contains(kk)) return params.value(kk);
    }

    return std::numeric_limits<double>::quiet_NaN();
}

// PROJECTION 名称到中文映射（目前支持 Albers 等积割圆锥）
static QString projectionToChinese(const char* projName)
{
    QString p = QString::fromUtf8(projName ? projName : "");
    QString lower = p.toLower();
    if (lower.contains("albers") || (lower.contains("equal") && lower.contains("area") && lower.contains("conic")))
        return QStringLiteral("正轴等积割圆锥投影");
    return p; // 回退：返回原始英文名称
}

// 解析 XML 规则（按比例尺）
ProjectionRule ParseXmlForProjectionRule(const std::string& xmlFilePath, int scale, std::shared_ptr<spdlog::logger> logger)
{
    ProjectionRule rule;

    QFile file(QString::fromStdString(xmlFilePath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (logger) logger->error("无法打开XML文件：{}", xmlFilePath);
        return rule;
    }

    QDomDocument doc;
    if (!doc.setContent(&file)) {
        file.close();
        if (logger) logger->error("XML文件格式错误：{}", xmlFilePath);
        return rule;
    }
    file.close();

    QDomElement root = doc.documentElement();
    if (root.tagName() != "check_list") {
        if (logger) logger->error("XML根元素不是check_list");
        return rule;
    }

    QDomNodeList checkItems = root.elementsByTagName("check_item");
    for (int j = 0; j < checkItems.count(); ++j) {
        QDomElement item = checkItems.at(j).toElement();
        QString itemName = item.elementsByTagName("name").isEmpty() ? QString() : item.elementsByTagName("name").at(0).toElement().text();
        QString itemId = item.elementsByTagName("id").isEmpty() ? QString() : item.elementsByTagName("id").at(0).toElement().text();
        if (itemName.trimmed() != QStringLiteral("投影参数准确性")) continue;

        rule.id = itemId.toStdString();
        rule.name = itemName.toStdString();

        QDomNodeList rules = item.elementsByTagName("rule");
        for (int k = 0; k < rules.count(); ++k) {
            QDomElement r = rules.at(k).toElement();
            QString rname = r.elementsByTagName("name").isEmpty() ? QString() : r.elementsByTagName("name").at(0).toElement().text();
            QString rval = r.elementsByTagName("value").isEmpty() ? QString() : r.elementsByTagName("value").at(0).toElement().text();

            if (rname == QStringLiteral("投影类型")) {
                rule.projectionTypeCN = rval.toStdString();
            } else if (rname == QStringLiteral("中央经线")) {
                bool ok2=false; rule.centralMeridian = rval.toDouble(&ok2);
                if (!ok2 && logger) logger->warn("XML中央经线无法解析为数字：{}", fromQStringToUtf8(rval));
            } else if (rname == QStringLiteral("原点纬度")) {
                bool ok2=false; rule.latitudeOfOrigin = rval.toDouble(&ok2);
                if (!ok2 && logger) logger->warn("XML原点纬度无法解析为数字：{}", fromQStringToUtf8(rval));
            } else if (rname == QStringLiteral("第一标准纬线")) {
                bool ok2=false; rule.standardParallel1 = rval.toDouble(&ok2);
                if (!ok2 && logger) logger->warn("XML第一标准纬线无法解析为数字：{}", fromQStringToUtf8(rval));
            } else if (rname == QStringLiteral("第二标准纬线")) {
                bool ok2=false; rule.standardParallel2 = rval.toDouble(&ok2);
                if (!ok2 && logger) logger->warn("XML第二标准纬线无法解析为数字：{}", fromQStringToUtf8(rval));
            }
        }
        if (logger) logger->info("解析投影规则完成：id={}, name={}", rule.id, rule.name);
        return rule;
    }
    if (logger) logger->warn("未找到投影参数准确性规则");
    return rule;
}

// 导出接口实现
extern "C" MY_PLUGIN_API SE_Error ProjectionParameterCheck(
    const char* szShpFilePath,
    const char* szSchemaXmlPath,
    int scale,
    const char* szOutputJsonPath,
    const char* szOutputLogFilePath)
{
    // 日志器（使用本地编码路径，避免中文文件名乱码）
    QString logPathQ = fromUtf8ToQString(szOutputLogFilePath);
    QByteArray logPathLocalBytes = logPathQ.toLocal8Bit();
    std::string logPathLocal(logPathLocalBytes.constData(), logPathLocalBytes.size());
    if (spdlog::get("ProjectionParameterCheck")) {
        spdlog::drop("ProjectionParameterCheck");
    }
    auto logger = spdlog::basic_logger_mt("ProjectionParameterCheck", logPathLocal);
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    logger->info("开始执行投影参数准确性检查");

    logger->info("输入SHP: {}", fromQStringToUtf8(fromUtf8ToQString(szShpFilePath)));
    logger->info("XML规则: {}", fromQStringToUtf8(fromUtf8ToQString(szSchemaXmlPath)));
    logger->info("JSON输出: {}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));
    logger->info("比例尺: {}", scale);
    GDALDataset* poDS = nullptr; // 统一管理生命周期，确保异常/早退均能关闭
    try {
        // 加载规则
        ProjectionRule rule = ParseXmlForProjectionRule(szSchemaXmlPath, scale, logger);
        if (rule.name.empty()) {
            logger->error("未解析到投影检查规则（比例尺不匹配或XML缺失）");
        }

        // 与项目其他算法保持一致：将GDAL文件名按本地编码传递（作用域守卫）
        CplConfigGuard guardUtf8("GDAL_FILENAME_IS_UTF8", "NO");

        static bool gdalInitialized = false;
        if (!gdalInitialized) {
            GDALAllRegister();
            gdalInitialized = true;
            logger->debug("GDAL驱动已注册");
        }

        // 打开数据
        QString sourcePath = fromUtf8ToQString(szShpFilePath);
        QByteArray localBytes = QFile::encodeName(sourcePath);
        poDS = (GDALDataset*)GDALOpenEx(localBytes.constData(), GDAL_OF_VECTOR, NULL, NULL, NULL);
        if (poDS == NULL) {
            logger->error("无法打开源数据：{}", fromQStringToUtf8(fromUtf8ToQString(szShpFilePath)));

            QJsonObject root; QJsonObject checkItem;
            checkItem["id"] = QString::fromStdString(rule.id);
            checkItem["name"] = QString::fromStdString(rule.name.empty()?"投影参数准确性":rule.name);
            checkItem["status"] = "质检失败";
            checkItem["err_exist"] = "false"; // 无法质检，不判定为错误存在
            checkItem["info"] = "质检失败：无法打开源数据文件";
            root["check_item"] = checkItem;

            QFile jsonFile(fromUtf8ToQString(szOutputJsonPath));
            if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QJsonDocument doc(root);
                jsonFile.write(doc.toJson(QJsonDocument::Indented));
                jsonFile.close();
            }
            spdlog::drop("ProjectionParameterCheck");
            return SE_ERROR_FAILURE;
        }

        OGRLayer* poLayer = poDS->GetLayer(0);
        if (poLayer == NULL) {
            GDALClose(poDS); poDS = nullptr;
            logger->error("无法获取图层");

            QJsonObject root; QJsonObject checkItem;
            checkItem["id"] = QString::fromStdString(rule.id);
            checkItem["name"] = QString::fromStdString(rule.name.empty()?"投影参数准确性":rule.name);
            checkItem["status"] = "质检失败";
            checkItem["err_exist"] = "false";
            checkItem["info"] = "质检失败：无法获取图层";
            root["check_item"] = checkItem;

            QFile jsonFile(fromUtf8ToQString(szOutputJsonPath));
            if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QJsonDocument doc(root);
                jsonFile.write(doc.toJson(QJsonDocument::Indented));
                jsonFile.close();
            }
            spdlog::drop("ProjectionParameterCheck");
            return SE_ERROR_FAILURE;
        }

        OGRSpatialReference* poSRS = poLayer->GetSpatialRef();
        if (!poSRS) {
            GDALClose(poDS); poDS = nullptr;
            logger->error("图层缺少空间参考（SRS）");

            QJsonObject root; QJsonObject checkItem;
            checkItem["id"] = QString::fromStdString(rule.id);
            checkItem["name"] = QString::fromStdString(rule.name.empty()?"投影参数准确性":rule.name);
            checkItem["status"] = "质检失败";
            checkItem["err_exist"] = "false";
            checkItem["info"] = "质检失败：图层缺少空间参考";
            root["check_item"] = checkItem;

            QFile jsonFile(fromUtf8ToQString(szOutputJsonPath));
            if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QJsonDocument doc(root);
                jsonFile.write(doc.toJson(QJsonDocument::Indented));
                jsonFile.close();
            }
            spdlog::drop("ProjectionParameterCheck");
            return SE_ERROR_FAILURE;
        }

        // 提取 WKT 与参数
        char* pszWkt = nullptr;
        poSRS->exportToWkt(&pszWkt);
        QString wkt = QString::fromUtf8(pszWkt ? pszWkt : "");
        if (pszWkt) CPLFree(pszWkt);

        const char* projName = poSRS->GetAttrValue("PROJECTION");
        QString projCN = projectionToChinese(projName);

        double centralMeridian = extractWktParamNaN(wkt, "Central_Meridian");
        if (std::isnan(centralMeridian)) centralMeridian = extractWktParamNaN(wkt, "Longitude_Of_Center");
        double latOrigin = extractWktParamNaN(wkt, "Latitude_Of_Origin");
        if (std::isnan(latOrigin)) latOrigin = extractWktParamNaN(wkt, "Latitude_Of_Center");
        double stdPar1 = extractWktParamNaN(wkt, "Standard_Parallel_1");
        if (std::isnan(stdPar1)) stdPar1 = extractWktParamNaN(wkt, "Std_Parallel_1");
        double stdPar2 = extractWktParamNaN(wkt, "Standard_Parallel_2");
        if (std::isnan(stdPar2)) stdPar2 = extractWktParamNaN(wkt, "Std_Parallel_2");

        logger->info("图层投影: {}", fromQStringToUtf8(projCN));
        logger->info("中央经线: {}", centralMeridian);
        logger->info("原点纬度: {}", latOrigin);
        logger->info("标准纬线: {}, {}", stdPar1, stdPar2);

        // 比对
        auto approxEq = [](double a, double b, double tol = 1e-3) { return std::fabs(a - b) <= tol; };
        bool typeOk = (!projCN.isEmpty() && !QString::fromStdString(rule.projectionTypeCN).isEmpty()) ?
            (projCN.trimmed() == QString::fromStdString(rule.projectionTypeCN).trimmed()) : false;
        bool cmOk = approxEq(centralMeridian, rule.centralMeridian);
        bool latOk = approxEq(latOrigin, rule.latitudeOfOrigin);
        bool stdOk = approxEq(stdPar1, rule.standardParallel1) && approxEq(stdPar2, rule.standardParallel2);

        bool hasError = !(typeOk && cmOk && latOk && stdOk);

        // JSON 结果
        QJsonObject root; QJsonObject checkItem;
        checkItem["id"] = QString::fromStdString(rule.id.empty()?"401":rule.id);
        checkItem["name"] = QString::fromStdString(rule.name.empty()?"投影参数准确性":rule.name);
        checkItem["status"] = "质检完成";
        checkItem["err_exist"] = hasError ? "true" : "false";
        checkItem["info"] = hasError ? "综合后地图存在投影参数准确性错误" : "综合后地图不存在投影参数准确性错误";

        if (hasError) {
            QJsonObject errors;
            int idx = 1;
            auto pushErr = [&](const QString& desc, const QString& tval, const QString& fval){
                QJsonObject e; e["description"] = desc; e["ture_value"] = tval; e["false_value"] = fval; // 注意：按需求使用 ture_value
                errors[QString("error%1").arg(idx)] = e; idx++;
            };

            if (!typeOk && idx <= 3) pushErr("投影类型不一致",
                QString::fromStdString(rule.projectionTypeCN), projCN);
            if (!cmOk && idx <= 3) pushErr("中央经线不一致",
                QString::number(rule.centralMeridian, 'f', 6), QString::number(centralMeridian, 'f', 6));
            if (!latOk && idx <= 3) pushErr("原点纬度不一致",
                QString::number(rule.latitudeOfOrigin, 'f', 6), QString::number(latOrigin, 'f', 6));
            if (!stdOk && idx <= 3) pushErr("标准纬线不一致",
                QString("%1,%2").arg(QString::number(rule.standardParallel1, 'f', 6)).arg(QString::number(rule.standardParallel2, 'f', 6)),
                QString("%1,%2").arg(QString::number(stdPar1, 'f', 6)).arg(QString::number(stdPar2, 'f', 6)));

            checkItem["errors"] = errors;
        }

        root["check_item"] = checkItem;

        QFile jsonFile(fromUtf8ToQString(szOutputJsonPath));
        if (!jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            logger->error("无法创建JSON结果文件：{}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));
            GDALClose(poDS); poDS = nullptr;
            spdlog::drop("ProjectionParameterCheck");
            return SE_ERROR_FAILURE;
        }
        QJsonDocument doc(root);
        jsonFile.write(doc.toJson(QJsonDocument::Indented));
        jsonFile.close();
        GDALClose(poDS); poDS = nullptr;
        logger->info("投影参数检查完成，JSON已生成");
        spdlog::drop("ProjectionParameterCheck");
        return SE_ERROR_NONE;
    }
    catch (const std::exception& ex) {
        logger->error("异常: {}", ex.what());
    }
    catch (...) {
        logger->error("未知异常");
    }
    if (poDS) { GDALClose(poDS); poDS = nullptr; }
    spdlog::drop("ProjectionParameterCheck");
    return SE_ERROR_FAILURE;
}
