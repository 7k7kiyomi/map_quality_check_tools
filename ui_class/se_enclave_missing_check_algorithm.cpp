#define _HAS_STD_BYTE 0
#include "se_enclave_missing_check_algorithm.h"

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

// GDAL字符串列表
#include <cpl_string.h>
#include <cpl_conv.h>

/*spdlog日志库头文件*/
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"

// ============================================================================
// 编码转换辅助函数
// ============================================================================
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

// ============================================================================
// 解析XML文件，获取飞地漏绘检查的参数
// ============================================================================
EnclaveMissingCheckParams ParseXmlForEnclaveMissingParams(
    const string& strXmlPath, int scale, std::shared_ptr<spdlog::logger> logger)
{
    EnclaveMissingCheckParams params;
    params.strGdbLayerName = "";             // 必须由CSV传入，不设默认值
    params.strNameField = "NAME";           // 默认名称字段
    params.strResultLayerName = "";         // 成果图层名
    params.strAdminCodeField = "";          // 行政编码字段
    params.strAdminCodeValue = "";          // 目标行政编码值
    params.dSearchRadius = 5.0;             // 默认搜索半径（图上毫米）
    params.dMinEnclaveArea = 0.0;
    params.bHasMinArea = false;
    params.bHasAdminCodeFilter = false;

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

    QDomElement root = doc.documentElement();
    if (root.tagName() != "check_list")
    {
        logger->error("XML根元素不是check_list");
        return params;
    }

    // 从XML文件名获取默认成果图层名称
    QString xmlFileName = QString::fromStdString(strXmlPath);
    int lastSlash = xmlFileName.lastIndexOf('/');
    if (lastSlash == -1) lastSlash = xmlFileName.lastIndexOf('\\');
    if (lastSlash != -1) xmlFileName = xmlFileName.mid(lastSlash + 1);
    int lastDot = xmlFileName.lastIndexOf('.');
    if (lastDot != -1) xmlFileName = xmlFileName.left(lastDot);
    params.strResultLayerName = xmlFileName.toStdString();

    QDomNodeList checkItems = root.elementsByTagName("check_item");
    bool bFound = false;
    for (int j = 0; j < checkItems.count(); j++)
    {
        QDomElement checkItem = checkItems.at(j).toElement();
        QDomNodeList nameNodes = checkItem.elementsByTagName("name");
        if (nameNodes.count() > 0)
        {
            QString checkName = nameNodes.at(0).toElement().text();
            if (checkName == QStringLiteral("飞地漏绘检查"))
            {
                bFound = true;
                QDomNodeList ruleNodes = checkItem.elementsByTagName("rule");
                for (int k = 0; k < ruleNodes.count(); k++)
                {
                    QDomElement ruleElement = ruleNodes.at(k).toElement();
                    QDomNodeList ruleNameNodes = ruleElement.elementsByTagName("name");
                    QDomNodeList ruleValueNodes = ruleElement.elementsByTagName("value");
                    if (ruleNameNodes.count() > 0 && ruleValueNodes.count() > 0)
                    {
                        QString ruleName = ruleNameNodes.at(0).toElement().text();
                        QString ruleValue = ruleValueNodes.at(0).toElement().text();

                        if (ruleName == QStringLiteral("名称字段"))
                        {
                            params.strNameField = ruleValue.toStdString();
                            logger->info("解析到名称字段：{}", params.strNameField);
                        }
                        else if (ruleName == QStringLiteral("搜索半径"))
                        {
                            bool ok;
                            double val = ruleValue.toDouble(&ok);
                            if (ok && val > 0)
                            {
                                params.dSearchRadius = val;
                                logger->info("解析到搜索半径：{} 毫米", val);
                            }
                        }
                        else if (ruleName == QStringLiteral("最小面积"))
                        {
                            bool ok;
                            double val = ruleValue.toDouble(&ok);
                            if (ok && val >= 0)
                            {
                                params.dMinEnclaveArea = val;
                                params.bHasMinArea = true;
                                logger->info("解析到最小飞地面积：{}", val);
                            }
                        }
                        else if (ruleName == QStringLiteral("成果图层名称"))
                        {
                            params.strResultLayerName = ruleValue.toStdString();
                            logger->info("解析到成果图层名称：{}", params.strResultLayerName);
                        }
                        else if (ruleName == QStringLiteral("行政编码字段"))
                        {
                            params.strAdminCodeField = ruleValue.toStdString();
                            logger->info("解析到行政编码字段：{}", params.strAdminCodeField);
                        }
                        else if (ruleName == QStringLiteral("行政编码值"))
                        {
                            params.strAdminCodeValue = ruleValue.toStdString();
                            logger->info("解析到行政编码值：{}", params.strAdminCodeValue);
                        }
                    }
                }
                break;
            }
        }
    }

    if (!bFound)
    {
        logger->warn("未找到飞地漏绘检查项，使用默认参数");
    }

    // 启用行政编码过滤（两者都配置时才生效）
    if (!params.strAdminCodeField.empty() && !params.strAdminCodeValue.empty())
    {
        params.bHasAdminCodeFilter = true;
        logger->info("启用行政编码过滤：{}=<{}>", params.strAdminCodeField, params.strAdminCodeValue);
    }

    return params;
}

// ============================================================================
// 辅助结构体：飞地信息
// ============================================================================
struct EnclaveInfo
{
    qint64 gdbFid;                 // GDB中飞地行的FID
    int objectId;                  // 共享的OBJECTID（主区与飞地相同）
    OGRGeometry* enclaveGeom;      // 飞地几何（clone）
    OGRPoint centroid;             // 飞地中心点
    double area;                   // 飞地面积
    std::string enclaveName;       // 飞地NAME（如"属永登县"）
    std::string parentName;        // 所属主区名称（如"永登县"，即去掉"属"）
    std::string elemId;            // ELEMID
    std::string clasId;            // CLASID
    std::string adminCode;         // 行政编码（XQ/PAC字段值）
};

// ============================================================================
// 辅助函数：计算面几何面积
// ============================================================================
static double computePolygonArea(OGRGeometry* geom)
{
    if (!geom) return 0.0;
    OGRwkbGeometryType type = wkbFlatten(geom->getGeometryType());
    if (type == wkbPolygon)
        return static_cast<OGRPolygon*>(geom)->get_Area();
    if (type == wkbMultiPolygon)
    {
        OGRMultiPolygon* mp = static_cast<OGRMultiPolygon*>(geom);
        double area = 0.0;
        for (int i = 0; i < mp->getNumGeometries(); ++i)
        {
            OGRGeometry* sub = mp->getGeometryRef(i);
            if (sub && wkbFlatten(sub->getGeometryType()) == wkbPolygon)
                area += static_cast<OGRPolygon*>(sub)->get_Area();
        }
        return area;
    }
    return 0.0;
}

// ============================================================================
// 辅助函数：判断 NAME 是否为飞地（以"属"开头）
// ============================================================================
static bool isEnclaveName(const QString& name)
{
    return name.trimmed().startsWith(QStringLiteral("属"));
}

// ============================================================================
// 辅助函数：从飞地NAME提取主区名称（去掉"属"前缀）
// ============================================================================
static std::string extractParentName(const std::string& enclaveName)
{
    QString qName = QString::fromUtf8(enclaveName.c_str()).trimmed();
    if (qName.startsWith(QStringLiteral("属")))
    {
        qName = qName.mid(1); // 去掉"属"
    }
    return qName.toUtf8().constData();
}

// ============================================================================
// 主函数：飞地漏绘检查
// ============================================================================
SE_Error EnclaveMissingCheck(
    const char* szGdbPath,
    const char* szResultShpPath,
    const char* szSchemaXmlPath,
    int scale,
    const char* szOutputShpPath,
    const char* szOutputJsonPath,
    const char* szOutputLogPath,
    const char* szGdbLayerName)
{
    // ---- 日志初始化 ----
    QString logPathQ = fromUtf8ToQString(szOutputLogPath);
    QByteArray logPathLocalBytes = logPathQ.toLocal8Bit();
    std::string logPathLocal(logPathLocalBytes.constData(), logPathLocalBytes.size());

    if (spdlog::get("EnclaveMissingCheckAlgorithm"))
        spdlog::drop("EnclaveMissingCheckAlgorithm");

    std::shared_ptr<spdlog::logger> logger;
    try {
        logger = spdlog::basic_logger_mt("EnclaveMissingCheckAlgorithm", logPathLocal);
    } catch (...) {
        logger = spdlog::get("EnclaveMissingCheckAlgorithm");
        if (!logger) logger = spdlog::default_logger();
    }
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    logger->info("========================================");
    logger->info("开始执行飞地漏绘检查算法");
    logger->info("========================================");

    logger->info("GDB路径（真值）: {}", fromQStringToUtf8(fromUtf8ToQString(szGdbPath)));
    logger->info("成果SHP路径: {}", fromQStringToUtf8(fromUtf8ToQString(szResultShpPath)));
    logger->info("JSON输出路径: {}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));

    try
    {
        // ---- 步骤0: 解析XML参数 ----
        EnclaveMissingCheckParams params = ParseXmlForEnclaveMissingParams(szSchemaXmlPath, scale, logger);

        // GDB图层名称由CSV传入（不从XML读取）
        if (szGdbLayerName && szGdbLayerName[0] != '\0')
        {
            params.strGdbLayerName = szGdbLayerName;
        }
        else
        {
            logger->error("未提供GDB图层名称（CSV gdb图层列），无法继续检查");
            return SE_ERROR_FAILURE;
        }
        logger->info("GDB图层名称（来自CSV）：{}", params.strGdbLayerName);

        logger->info("使用比例尺：{}", scale);
        logger->info("GDB图层名称：{}", params.strGdbLayerName);
        logger->info("名称字段：{}", params.strNameField);
        logger->info("搜索半径：{} 毫米", params.dSearchRadius);
        if (params.bHasMinArea)
            logger->info("最小飞地面积：{} 平方毫米", params.dMinEnclaveArea);
        else
            logger->info("未启用最小飞地面积过滤");

        // ---- GDAL初始化 ----
        CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
        CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");

        static bool gdalInitialized = false;
        if (!gdalInitialized)
        {
            GDALAllRegister();
            gdalInitialized = true;
        }

        // ================================================================
        // 步骤1: 打开成果SHP，收集所有面几何（提前到最前）
        // ================================================================
        logger->info("--- 步骤1: 打开成果SHP，收集面几何 ---");

        QString resultPathQ = fromUtf8ToQString(szResultShpPath);
        GDALDataset* poResultDS = (GDALDataset*)GDALOpenEx(
            resultPathQ.toUtf8().constData(), GDAL_OF_VECTOR, NULL, NULL, NULL);
        if (poResultDS == NULL)
        {
            logger->error("无法打开成果SHP：{}", fromQStringToUtf8(fromUtf8ToQString(szResultShpPath)));
            return SE_ERROR_FILE_NOT_FOUND;
        }

        vector<OGRGeometry*> vResultPolygons;
        int nResultFeatureCount = 0;

        int nResultLayerCount = poResultDS->GetLayerCount();
        logger->info("成果SHP包含 {} 个图层", nResultLayerCount);

        for (int iLayer = 0; iLayer < nResultLayerCount; iLayer++)
        {
            OGRLayer* poResultLayer = poResultDS->GetLayer(iLayer);
            if (!poResultLayer) continue;

            poResultLayer->ResetReading();
            OGRFeature* poResultFeature = nullptr;
            int nLayerCount = 0;

            while ((poResultFeature = poResultLayer->GetNextFeature()) != NULL)
            {
                OGRGeometry* poGeom = poResultFeature->GetGeometryRef();
                if (!poGeom) { OGRFeature::DestroyFeature(poResultFeature); continue; }

                OGRwkbGeometryType type = wkbFlatten(poGeom->getGeometryType());
                if (type == wkbPolygon)
                {
                    vResultPolygons.push_back(poGeom->clone());
                    nLayerCount++;
                }
                else if (type == wkbMultiPolygon)
                {
                    OGRMultiPolygon* mp = static_cast<OGRMultiPolygon*>(poGeom);
                    for (int j = 0; j < mp->getNumGeometries(); j++)
                    {
                        OGRGeometry* part = mp->getGeometryRef(j);
                        if (part && wkbFlatten(part->getGeometryType()) == wkbPolygon)
                        {
                            vResultPolygons.push_back(part->clone());
                            nLayerCount++;
                        }
                    }
                }
                OGRFeature::DestroyFeature(poResultFeature);
            }
            logger->info("成果图层 '{}' 收集到 {} 个有效面", poResultLayer->GetName(), nLayerCount);
            nResultFeatureCount += nLayerCount;
        }

        OGRSpatialReference* poResultSRS = poResultDS->GetLayer(0) ? poResultDS->GetLayer(0)->GetSpatialRef() : nullptr;
        GDALClose(poResultDS);
        logger->info("成果SHP总有效面数: {}", nResultFeatureCount);

        if (nResultFeatureCount == 0)
        {
            logger->warn("成果SHP中没有任何有效面要素，所有飞地将被判定为漏绘");
        }

        // ================================================================
        // 步骤2: 打开GDB → 嗅探PAC → 设置属性过滤器 → 遍历飞地
        //
        // 嗅探逻辑：取成果SHP第一个面的中心点，用SetSpatialFilter反查GDB
        // 读出PAC前6位 → SetAttributeFilter("PAC LIKE '620121%'")
        // GDAL层直接屏蔽外区县数据，内存中不会有任何外区县飞地。
        // ================================================================
        logger->info("--- 步骤2: 打开GDB，嗅探辖区PAC，按属性过滤遍历 ---");

        QString gdbPathQ = fromUtf8ToQString(szGdbPath);
        GDALDataset* poGdbDS = (GDALDataset*)GDALOpenEx(
            gdbPathQ.toUtf8().constData(), GDAL_OF_VECTOR, NULL, NULL, NULL);
        if (poGdbDS == NULL)
        {
            logger->error("无法打开GDB：{}", fromQStringToUtf8(fromUtf8ToQString(szGdbPath)));
            for (auto& g : vResultPolygons) OGRGeometryFactory::destroyGeometry(g);
            return SE_ERROR_FILE_NOT_FOUND;
        }

        OGRLayer* poGdbLayer = poGdbDS->GetLayerByName(params.strGdbLayerName.c_str());
        if (poGdbLayer == NULL)
        {
            poGdbLayer = poGdbDS->GetLayer(0);
            if (poGdbLayer == NULL)
            {
                logger->error("无法获取GDB图层：{}", params.strGdbLayerName);
                GDALClose(poGdbDS);
                for (auto& g : vResultPolygons) OGRGeometryFactory::destroyGeometry(g);
                return SE_ERROR_FAILURE;
            }
            logger->warn("未找到图层'{}'，使用第0个图层：{}", params.strGdbLayerName, poGdbLayer->GetName());
        }

        OGRSpatialReference* poGdbSRS = poGdbLayer->GetSpatialRef();
        double dLinearUnitsInMeters = 1.0;
        if (poGdbSRS)
        {
            if (poGdbSRS->IsGeographic())
            {
                logger->error("GDB为地理坐标系(度)，不支持。");
                GDALClose(poGdbDS);
                for (auto& g : vResultPolygons) OGRGeometryFactory::destroyGeometry(g);
                return SE_ERROR_UNSUPPORTED_SRS;
            }
            const char* pszUnitsName = nullptr;
            dLinearUnitsInMeters = poGdbSRS->GetLinearUnits(&pszUnitsName);
            if (dLinearUnitsInMeters <= 0.0) dLinearUnitsInMeters = 1.0;
            logger->info("GDB坐标单位: {} (每单位={}米)", pszUnitsName ? pszUnitsName : "(未知)", dLinearUnitsInMeters);
        }

        double dSearchRadiusUnits = (params.dSearchRadius * scale) / 1000.0 / dLinearUnitsInMeters;
        logger->info("搜索半径换算: 图上{}mm → 实地{}m → 图层单位{}",
            params.dSearchRadius, (params.dSearchRadius * scale) / 1000.0, dSearchRadiusUnits);

        // ---- 步骤2.1: 嗅探辖区PAC ----
        if (!params.strAdminCodeField.empty())
        {
            OGRPoint samplePt;
            if (!vResultPolygons.empty())
                vResultPolygons[0]->Centroid(&samplePt);

            poGdbLayer->SetSpatialFilter(&samplePt);
            poGdbLayer->ResetReading();

            OGRFeature* poHitFeature = poGdbLayer->GetNextFeature();
            if (poHitFeature)
            {
                int nAdminIdx = poGdbLayer->GetLayerDefn()->GetFieldIndex(params.strAdminCodeField.c_str());
                if (nAdminIdx >= 0)
                {
                    const char* codeVal = poHitFeature->GetFieldAsString(nAdminIdx);
                    if (codeVal && codeVal[0] != '\0')
                    {
                        params.strAdminCodeValue = std::string(codeVal).substr(0, 6);
                        params.bHasAdminCodeFilter = true;
                        logger->info("嗅探成功: 成果面中心点({:.2f},{:.2f}) → PAC前6位=<{}>",
                            samplePt.getX(), samplePt.getY(), params.strAdminCodeValue);
                    }
                }
                OGRFeature::DestroyFeature(poHitFeature);
            }
            poGdbLayer->SetSpatialFilter(nullptr);
        }

        // ---- 步骤2.2: 设置属性过滤器（GDAL层屏蔽外区县） ----
        if (params.bHasAdminCodeFilter)
        {
            std::string filter = "\"" + params.strAdminCodeField + "\" LIKE '" + params.strAdminCodeValue + "%'";
            OGRErr eErr = poGdbLayer->SetAttributeFilter(filter.c_str());
            if (eErr == OGRERR_NONE)
                logger->info("GDAL属性过滤器已设置: {}", filter);
            else
                logger->warn("属性过滤器设置失败: {}", filter);
        }

        // ---- 步骤2.3: 获取字段索引 ----
        int nNameFieldIdx = poGdbLayer->GetLayerDefn()->GetFieldIndex(params.strNameField.c_str());
        int nObjectIdIdx = poGdbLayer->GetLayerDefn()->GetFieldIndex("OBJECTID");
        int nElemIdIdx = poGdbLayer->GetLayerDefn()->GetFieldIndex("ELEMID");
        int nClasIdIdx = poGdbLayer->GetLayerDefn()->GetFieldIndex("CLASID");
        int nAdminCodeIdx = (params.bHasAdminCodeFilter)
            ? poGdbLayer->GetLayerDefn()->GetFieldIndex(params.strAdminCodeField.c_str()) : -1;

        if (nNameFieldIdx < 0)
        {
            logger->error("GDB图层中未找到NAME字段'{}'，无法识别飞地。", params.strNameField);
            GDALClose(poGdbDS);
            for (auto& g : vResultPolygons) OGRGeometryFactory::destroyGeometry(g);
            return SE_ERROR_FAILURE;
        }

        // ---- 步骤2.4: 遍历GDB（属性过滤器已生效，只处理本辖区数据） ----
        poGdbLayer->ResetReading();
        OGRFeature* poGdbFeature = nullptr;
        int nTotalFeatures = 0;
        int nMainAreaCount = 0;
        int nEnclaveCount = 0;
        vector<EnclaveInfo> vEnclaves;

        while ((poGdbFeature = poGdbLayer->GetNextFeature()) != NULL)
        {
            nTotalFeatures++;
            OGRGeometry* poGeom = poGdbFeature->GetGeometryRef();
            if (!poGeom) { OGRFeature::DestroyFeature(poGdbFeature); continue; }

            OGRwkbGeometryType geomType = wkbFlatten(poGeom->getGeometryType());
            if (geomType != wkbPolygon && geomType != wkbMultiPolygon)
            { OGRFeature::DestroyFeature(poGdbFeature); continue; }

            const char* nameVal = poGdbFeature->GetFieldAsString(nNameFieldIdx);
            QString qName = QString::fromUtf8(nameVal ? nameVal : "").trimmed();
            if (qName.isEmpty()) { OGRFeature::DestroyFeature(poGdbFeature); continue; }

            if (isEnclaveName(qName))
            {
                double dArea = computePolygonArea(poGeom);
                if (params.bHasMinArea && dArea < params.dMinEnclaveArea)
                { OGRFeature::DestroyFeature(poGdbFeature); continue; }

                OGRPoint centroid;
                poGeom->Centroid(&centroid);

                EnclaveInfo enclave;
                enclave.gdbFid = poGdbFeature->GetFID();
                enclave.objectId = (nObjectIdIdx >= 0) ? poGdbFeature->GetFieldAsInteger(nObjectIdIdx) : -1;
                enclave.enclaveGeom = poGeom->clone();
                enclave.centroid = centroid;
                enclave.area = dArea;
                enclave.enclaveName = qName.toUtf8().constData();
                enclave.parentName = extractParentName(enclave.enclaveName);
                enclave.elemId = (nElemIdIdx >= 0) ? std::string(poGdbFeature->GetFieldAsString(nElemIdIdx) ? poGdbFeature->GetFieldAsString(nElemIdIdx) : "") : "";
                enclave.clasId = (nClasIdIdx >= 0) ? std::string(poGdbFeature->GetFieldAsString(nClasIdIdx) ? poGdbFeature->GetFieldAsString(nClasIdIdx) : "") : "";
                enclave.adminCode = (nAdminCodeIdx >= 0) ? std::string(poGdbFeature->GetFieldAsString(nAdminCodeIdx) ? poGdbFeature->GetFieldAsString(nAdminCodeIdx) : "").substr(0, 6) : "";

                vEnclaves.push_back(enclave);
                nEnclaveCount++;
            }
            else { nMainAreaCount++; }
            OGRFeature::DestroyFeature(poGdbFeature);
        }

        logger->info("GDB统计: 遍历要素数={}, 主区数={}, 飞地数={}", nTotalFeatures, nMainAreaCount, nEnclaveCount);

        if (nEnclaveCount == 0)
        {
            logger->info("本辖区未发现飞地，检查结束。");
            GDALClose(poGdbDS);
            for (auto& g : vResultPolygons) OGRGeometryFactory::destroyGeometry(g);

            QJsonObject jsonResult;
            QJsonObject checkItem;
            checkItem["id"] = "103";
            checkItem["name"] = QStringLiteral("飞地漏绘检查");
            checkItem["status"] = QStringLiteral("质检完成");
            checkItem["err_exist"] = "false";
            checkItem["info"] = QString("GDB中未发现本辖区飞地要素");
            jsonResult["check_item"] = checkItem;
            QJsonDocument doc(jsonResult);
            QFile jsonFile(fromUtf8ToQString(szOutputJsonPath));
            if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
            { jsonFile.write(doc.toJson(QJsonDocument::Indented)); jsonFile.close(); }
            return SE_ERROR_NONE;
        }

        // ================================================================
        // 步骤3: 缓冲区 + 空间相交检查（核心机制）
        //
        // 说明：成果SHP只有纯图形面，无属性；且因制图综合/抽稀/移位，
        // 图面几何已变形，不能依赖精确 Contains。必须以 GDB 飞地中心点
        // 为基准创建 Buffer 容差圈，吸收位移误差后与成果面做 Intersects。
        // ================================================================
        logger->info("--- 步骤3: Buffer容差圈 → 成果面相交检查 ---");
        logger->info("使用搜索半径: {}图层单位 (吸收制图位移)", dSearchRadiusUnits);

        vector<int> vMissingIndices;           // 缺失飞地在vEnclaves中的索引
        vector<OGRGeometry*> vMissingGeoms;    // 缺失飞地的几何
        int nFoundCount = 0;

        for (size_t i = 0; i < vEnclaves.size(); i++)
        {
            if (i % 50 == 0)
            {
                logger->info("检查进度: {}/{} ({:.1f}%)",
                    i, vEnclaves.size(),
                    (double)i / vEnclaves.size() * 100.0);
            }

            const EnclaveInfo& enclave = vEnclaves[i];
            bool bFound = false;

            // 以飞地中心点为圆心、搜索半径为缓冲创建一个容差面
            OGRGeometry* searchArea = enclave.centroid.Buffer(dSearchRadiusUnits);
            if (searchArea)
            {
                for (auto& resultPoly : vResultPolygons)
                {
                    // 容差圈与任一成果面有交集 → 飞地存在
                    if (searchArea->Intersects(resultPoly))
                    {
                        bFound = true;
                        break;
                    }
                }
                OGRGeometryFactory::destroyGeometry(searchArea);
            }

            if (bFound)
            {
                nFoundCount++;
            }
            else
            {
                vMissingIndices.push_back((int)i);
                vMissingGeoms.push_back(enclave.enclaveGeom->clone());
                logger->info("飞地漏绘: 行政区'{}'(FID={}), 中心点=({:.2f},{:.2f})",
                    enclave.parentName, enclave.gdbFid,
                    enclave.centroid.getX(), enclave.centroid.getY());
            }
        }

        int nMissingCount = (int)vMissingIndices.size();
        logger->info("检查完成: 找到{}个, 漏绘{}个", nFoundCount, nMissingCount);

        // ================================================================
        // 步骤4: 输出缺失飞地SHP
        // ================================================================
        GDALDataset* poOutDS = NULL;
        if (nMissingCount > 0 && !vMissingGeoms.empty())
        {
            logger->info("--- 步骤4: 输出缺失飞地SHP ---");

            GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
            if (poDriver)
            {
                QString outPathQ = fromUtf8ToQString(szOutputShpPath);
                poOutDS = poDriver->Create(outPathQ.toUtf8().constData(), 0, 0, 0, GDT_Unknown, NULL);
                if (poOutDS)
                {
                    char** papszLCO = NULL;
                    papszLCO = CSLSetNameValue(papszLCO, "ENCODING", "UTF-8");
                    OGRLayer* poOutLayer = poOutDS->CreateLayer(
                        "enclave_missing", poGdbSRS, wkbPolygon, papszLCO);
                    CSLDestroy(papszLCO);

                    if (poOutLayer)
                    {
                        // 创建属性字段（与GDB属性表对应）
                        OGRFieldDefn oFidField("GDB_FID", OFTInteger64);
                        poOutLayer->CreateField(&oFidField);

                        OGRFieldDefn oObjIdField("OBJECTID", OFTInteger);
                        poOutLayer->CreateField(&oObjIdField);

                        OGRFieldDefn oNameField("ADMIN_NAME", OFTString);
                        oNameField.SetWidth(128);
                        poOutLayer->CreateField(&oNameField);

                        OGRFieldDefn oEncNameField("ENCLAVE_NAM", OFTString);
                        oEncNameField.SetWidth(128);
                        poOutLayer->CreateField(&oEncNameField);

                        OGRFieldDefn oElemField("ELEMID", OFTString);
                        oElemField.SetWidth(64);
                        poOutLayer->CreateField(&oElemField);

                        OGRFieldDefn oClasField("CLASID", OFTString);
                        oClasField.SetWidth(32);
                        poOutLayer->CreateField(&oClasField);

                        OGRFieldDefn oAdminField("ADMIN_CODE", OFTString);
                        oAdminField.SetWidth(32);
                        poOutLayer->CreateField(&oAdminField);

                        OGRFieldDefn oAreaField("ENCLAVE_AREA", OFTReal);
                        poOutLayer->CreateField(&oAreaField);

                        OGRFieldDefn oErrField("ErrorType", OFTString);
                        oErrField.SetWidth(64);
                        poOutLayer->CreateField(&oErrField);

                        // 写入缺失飞地
                        for (size_t i = 0; i < vMissingGeoms.size(); i++)
                        {
                            int enclaveIdx = vMissingIndices[i];
                            const EnclaveInfo& info = vEnclaves[enclaveIdx];

                            OGRFeature* poOutFeat = OGRFeature::CreateFeature(poOutLayer->GetLayerDefn());
                            poOutFeat->SetField("GDB_FID", (GIntBig)info.gdbFid);
                            poOutFeat->SetField("OBJECTID", info.objectId);
                            poOutFeat->SetField("ADMIN_NAME", info.parentName.c_str());
                            poOutFeat->SetField("ENCLAVE_NAM", info.enclaveName.c_str());
                            poOutFeat->SetField("ELEMID", info.elemId.c_str());
                            poOutFeat->SetField("CLASID", info.clasId.c_str());
                            poOutFeat->SetField("ADMIN_CODE", info.adminCode.c_str());
                            poOutFeat->SetField("ENCLAVE_AREA", info.area);

                            std::string sErrVal = fromQStringToUtf8(QStringLiteral("飞地漏绘"));
                            poOutFeat->SetField("ErrorType", sErrVal.c_str());

                            // 设置几何
                            OGRGeometry* outGeom = vMissingGeoms[i];
                            if (outGeom && !outGeom->IsValid())
                            {
                                OGRGeometry* fixed = outGeom->Buffer(0);
                                if (fixed) { poOutFeat->SetGeometryDirectly(fixed); }
                                else { poOutFeat->SetGeometry(outGeom); }
                            }
                            else
                            {
                                poOutFeat->SetGeometry(outGeom);
                            }

                            if (poOutLayer->CreateFeature(poOutFeat) != OGRERR_NONE)
                            {
                                logger->error("写入缺失飞地要素失败，索引: {}", i);
                            }
                            OGRFeature::DestroyFeature(poOutFeat);
                        }

                        logger->info("成功输出缺失飞地SHP，包含 {} 个要素", vMissingGeoms.size());
                    }
                    else
                    {
                        logger->error("无法创建输出图层");
                    }
                }
                else
                {
                    logger->error("无法创建输出SHP：{}", fromQStringToUtf8(fromUtf8ToQString(szOutputShpPath)));
                }
            }
            else
            {
                logger->error("无法获取ESRI Shapefile驱动");
            }
        }

        // ================================================================
        // 步骤5: 生成JSON报告
        // ================================================================
        logger->info("--- 步骤5: 生成JSON报告 ---");

        QJsonObject jsonResult;
        QJsonObject checkItem;
        checkItem["id"] = "103";
        checkItem["name"] = QStringLiteral("飞地漏绘检查");

        if (nMissingCount > 0)
        {
            checkItem["status"] = QStringLiteral("质检完成");
            checkItem["err_exist"] = "true";
            QString infoMsg = QString("%1图层存在%2处飞地漏绘")
                .arg(QString::fromStdString(params.strResultLayerName))
                .arg(nMissingCount);
            checkItem["info"] = infoMsg;
        }
        else
        {
            checkItem["status"] = QStringLiteral("质检完成");
            checkItem["err_exist"] = "false";
            QString infoMsg = QString("%1图层不存在飞地漏绘")
                .arg(QString::fromStdString(params.strResultLayerName));
            checkItem["info"] = infoMsg;
        }

        jsonResult["check_item"] = checkItem;

        QJsonDocument jsonDoc(jsonResult);
        QFile jsonFile(fromUtf8ToQString(szOutputJsonPath));
        if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            jsonFile.write(jsonDoc.toJson(QJsonDocument::Indented));
            jsonFile.close();
            logger->info("已生成JSON报告：{}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));
        }
        else
        {
            logger->error("无法创建JSON文件：{}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));
        }

        // ================================================================
        // 资源清理
        // ================================================================
        logger->info("--- 资源清理 ---");

        for (auto& e : vEnclaves)
            OGRGeometryFactory::destroyGeometry(e.enclaveGeom);
        for (auto& g : vResultPolygons)
            OGRGeometryFactory::destroyGeometry(g);
        for (auto& g : vMissingGeoms)
            OGRGeometryFactory::destroyGeometry(g);

        GDALClose(poGdbDS);
        if (poOutDS) GDALClose(poOutDS);

        logger->info("飞地漏绘检查完成。漏绘数={}", nMissingCount);
        logger->flush();

        return SE_ERROR_NONE;
    }
    catch (const std::exception& e)
    {
        logger->error("执行飞地漏绘检查时发生异常：{}", e.what());
        logger->flush();
        return SE_ERROR_FAILURE;
    }
}
