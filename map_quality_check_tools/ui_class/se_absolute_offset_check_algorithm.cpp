#define _HAS_STD_BYTE 0
#include "se_absolute_offset_check_algorithm.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <QtXml/QDomDocument>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QDateTime>
#include <QTextCodec>
#include <QJsonDocument>
#include <QJsonObject>
#include <cpl_string.h>
#include <cpl_conv.h>
#ifdef _WIN32
#ifndef SPDLOG_WCHAR_FILENAMES
#define SPDLOG_WCHAR_FILENAMES
#endif
#endif
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <vector>
#include <memory>
#include <cmath>

// 将UTF-8字符串转换为QString
static QString fromUtf8ToQString(const char* utf8Str)
{
    return QString::fromUtf8(utf8Str);
}

// 将QString转换为UTF-8字符串（统一的编码转换函数）
static std::string fromQStringToUtf8(const QString& qstr)
{
    return qstr.toUtf8().toStdString();
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

// 绝对偏移检查临时替换了进程默认日志器；结束时必须恢复原日志器，
// 不能将默认日志器置空，也不能在仍作为默认日志器时直接释放。
static std::shared_ptr<spdlog::logger> g_absoluteOffsetPreviousDefaultLogger;

// 清理日志器资源的辅助函数
static void CleanupLogger() {
    try {
        if (g_absoluteOffsetPreviousDefaultLogger) {
            spdlog::set_default_logger(g_absoluteOffsetPreviousDefaultLogger);
            g_absoluteOffsetPreviousDefaultLogger.reset();
        }
        spdlog::drop("absolute_offset_check");
    } catch (...) {
        // 忽略清理异常
    }
}

// XML解析函数：获取绝对偏移检查的缓冲区半径
SE_Error ParseXmlForAbsoluteOffsetBufferRadius(const char* szSchemaXmlPath, double& dBufferRadius, int nScale)
{
    QFile file(szSchemaXmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        spdlog::error("无法打开XML文件: {}", fromQStringToUtf8(fromUtf8ToQString(szSchemaXmlPath)));
        return SE_ERROR_FILE_NOT_FOUND;
    }

    QDomDocument doc;
    QString errorMsg;
    int errorLine, errorColumn;
    if (!doc.setContent(&file, &errorMsg, &errorLine, &errorColumn))
    {
        spdlog::error("XML解析失败: {} (行: {}, 列: {})", errorMsg.toStdString(), errorLine, errorColumn);
        file.close();
        return SE_ERROR_XML_PARSE_FAILED;
    }
    file.close();

    QDomElement root = doc.documentElement();
    auto parseAbsoluteOffsetCheckItem = [&](const QDomElement& checkItem) -> bool
    {
        QDomElement nameElement = checkItem.firstChildElement("name");
        if (nameElement.isNull() || nameElement.text().trimmed() != QStringLiteral("绝对偏移检查"))
        {
            return false;
        }

        QDomNodeList rules = checkItem.elementsByTagName("rule");
        for (int k = 0; k < rules.count(); ++k)
        {
            QDomElement rule = rules.at(k).toElement();
            QDomElement ruleNameElement = rule.firstChildElement("name");
            if (!ruleNameElement.isNull() && ruleNameElement.text().trimmed() == QStringLiteral("缓冲区半径"))
            {
                QDomElement ruleValueElement = rule.firstChildElement("value");
                QDomElement ruleUnitElement = rule.firstChildElement("unit");
                if (!ruleValueElement.isNull())
                {
                    bool ok;
                    double rawBufferRadius = ruleValueElement.text().trimmed().toDouble(&ok);
                    if (ok && rawBufferRadius > 0.0)
                    {
                        if (nScale <= 0)
                        {
                            spdlog::error("比例尺无效: {}", nScale);
                            return false;
                        }
                        QString unit = ruleUnitElement.isNull() ? QString() : ruleUnitElement.text().trimmed();
                        dBufferRadius = NormalizeLengthValueToMapMm(rawBufferRadius, unit, nScale);
                        spdlog::info("成功解析绝对偏移检查缓冲区半径: {} {} -> 图上{}毫米", rawBufferRadius, fromQStringToUtf8(unit), dBufferRadius);
                        return true;
                    }
                    spdlog::error("缓冲区半径值格式错误: {}", ruleValueElement.text().toStdString());
                    return false;
                }
            }
        }
        spdlog::error("绝对偏移检查配置中未找到有效的缓冲区半径");
        return false;
    };

    QDomNodeList scaleNodes = root.elementsByTagName("scale");
    bool foundScale = false;
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

        if (xmlScale == nScale)
        {
            foundScale = true;
            spdlog::info("按比例尺查找绝对偏移检查参数: {}", nScale);
            QDomNodeList checkItems = scaleElement.elementsByTagName("check_item");
            for (int j = 0; j < checkItems.count(); ++j)
            {
                QDomElement checkItem = checkItems.at(j).toElement();
                if (parseAbsoluteOffsetCheckItem(checkItem))
                {
                    return SE_ERROR_NONE;
                }
            }
            spdlog::warn("在比例尺{}下未找到有效的‘绝对偏移检查’参数，继续尝试兼容旧结构", nScale);
            break;
        }
    }

    if (!foundScale && scaleNodes.count() > 0)
    {
        spdlog::warn("未找到匹配的比例尺: {}，继续尝试兼容旧结构", nScale);
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

        spdlog::info("按未指定比例尺的scale节点查找绝对偏移检查参数");
        QDomNodeList checkItems = scaleElement.elementsByTagName("check_item");
        for (int j = 0; j < checkItems.count(); ++j)
        {
            QDomElement checkItem = checkItems.at(j).toElement();
            if (parseAbsoluteOffsetCheckItem(checkItem))
            {
                return SE_ERROR_NONE;
            }
        }
    }

    for (QDomNode node = root.firstChild(); !node.isNull(); node = node.nextSibling())
    {
        QDomElement checkItem = node.toElement();
        if (checkItem.isNull() || checkItem.tagName() != QStringLiteral("check_item"))
        {
            continue;
        }
        if (parseAbsoluteOffsetCheckItem(checkItem))
        {
            return SE_ERROR_NONE;
        }
    }
    spdlog::error("未找到绝对偏移检查配置");
    return SE_ERROR_XML_PARSE_FAILED;
}

/**
 * @brief 将图上毫米单位的缓冲区半径转换为实地米
 * @param dBufferRadiusMM 缓冲区半径（毫米）
 * @param nParsedMapScale 解析后地图的比例尺
 * @return 转换后的实地距离（米）
 * @note 缓冲区半径基于解析后地图比例尺计算，例如：3毫米 * 100000比例尺 = 300米
 */
double ConvertMMToGeoUnits(double dBufferRadiusMM, int nParsedMapScale)
{
    // 将毫米转换为米，再乘以解析后地图的比例尺
    return (dBufferRadiusMM / 1000.0) * nParsedMapScale;
}

// 检查几何体是否完全在缓冲区内
bool IsGeometryWithinBuffer(void* pGeometry, void* pBufferGeometry)
{
    if (!pGeometry || !pBufferGeometry)
        return false;
        
    OGRGeometry* geom = static_cast<OGRGeometry*>(pGeometry);
    OGRGeometry* buffer = static_cast<OGRGeometry*>(pBufferGeometry);
    
    // 性能优化：先进行边界框检查
    OGREnvelope geomEnv, bufferEnv;
    geom->getEnvelope(&geomEnv);
    buffer->getEnvelope(&bufferEnv);
    
    // 如果几何体的边界框不在缓冲区边界框内，则肯定不在缓冲区内
    if (geomEnv.MinX < bufferEnv.MinX || geomEnv.MaxX > bufferEnv.MaxX ||
        geomEnv.MinY < bufferEnv.MinY || geomEnv.MaxY > bufferEnv.MaxY)
    {
        return false;
    }
    
    // 边界框检查通过后，再进行精确的几何体检查
    return buffer->Contains(geom);
}


/**
 * @brief 合并多个缓冲区几何体为一个整体
 * @param buffers 缓冲区几何体向量
 * @return 合并后的几何体指针，失败返回nullptr
 * @note 使用UnionCascaded操作合并所有几何体，提高后续差集运算效率
 */
void* MergeBuffers(const std::vector<OGRGeometry*>& buffers)
{
    if (buffers.empty())
        return nullptr;
    
    try
    {
        if (buffers.size() == 1 && buffers[0])
        {
            OGRGeometry* g = buffers[0]->clone();
            if (g && !g->IsValid()) { OGRGeometry* v = g->MakeValid(); if(!v) v = g->Buffer(0); if (v && !v->IsEmpty()){ delete g; g = v; } }
            return g;
        }
        OGRGeometryCollection* mergedGeom = new OGRMultiPolygon();
        for (size_t i = 0; i < buffers.size(); ++i)
        {
            if (!buffers[i] || buffers[i]->IsEmpty())
                continue;
            OGRGeometry* bufferClone = buffers[i]->clone();
            if (bufferClone)
            {
                if (!bufferClone->IsValid()){ OGRGeometry* v = bufferClone->MakeValid(); if(!v) v = bufferClone->Buffer(0); if (v && !v->IsEmpty()){ delete bufferClone; bufferClone = v; } }
                mergedGeom->addGeometryDirectly(bufferClone);
            }
        }
        if (mergedGeom->IsEmpty())
        {
            delete mergedGeom;
            return nullptr;
        }
        OGRGeometry* unionResult = mergedGeom->UnionCascaded();
        if (!unionResult || unionResult->IsEmpty())
        {
            if (unionResult) { delete unionResult; unionResult = nullptr; }
            OGRGeometry* acc = nullptr;
            for (int i = 0; i < mergedGeom->getNumGeometries(); ++i)
            {
                OGRGeometry* gi = mergedGeom->getGeometryRef(i)->clone();
                if (!gi || gi->IsEmpty()) continue;
                if (!gi->IsValid()){ OGRGeometry* v = gi->MakeValid(); if(!v) v = gi->Buffer(0); if (v && !v->IsEmpty()){ delete gi; gi = v; } }
                if (!acc) { acc = gi; }
                else {
                    OGRGeometry* u = acc->Union(gi);
                    delete gi;
                    if (u) { delete acc; acc = u; }
                }
            }
            unionResult = acc;
        }
        delete mergedGeom;
        if (!unionResult || unionResult->IsEmpty()) return nullptr;
        spdlog::info("成功合并{}个缓冲区为一个整体几何体", buffers.size());
        return unionResult;
    }
    catch (...)
    {
        spdlog::error("合并缓冲区几何体时发生异常");
        return nullptr;
    }
}

/**
 * @brief 计算解析后图层与整体缓冲区的差集，识别超出缓冲区的要素
 * @param pParsedLayer 解析后的图层指针
 * @param pBufferGeometry 整体缓冲区几何体指针
 * @param outErrorCount 输出参数：超出缓冲区的要素数量
 * @param outTotalCount 输出参数：总要素数量
 * @return 包含差集结果的数据集指针，无错误时返回nullptr
 * @note 创建内存数据集存储超出缓冲区的几何体部分，用于生成错误图层
 */
void* ComputeLayerDifference(void* pParsedLayer, void* pBufferGeometry, int& outErrorCount, int& outTotalCount)
{
    // 初始化计数器
    outErrorCount = 0;
    outTotalCount = 0;
    
    if (!pParsedLayer || !pBufferGeometry)
        return nullptr;
    
    OGRLayer* parsedLayer = static_cast<OGRLayer*>(pParsedLayer);
    OGRGeometry* bufferGeom = static_cast<OGRGeometry*>(pBufferGeometry);
    
    if (!parsedLayer || !bufferGeom || bufferGeom->IsEmpty())
        return nullptr;
    
    try
    {
        // 获取解析后图层的空间参考
        OGRSpatialReference* pSRS = parsedLayer->GetSpatialRef();
        
        // 创建内存数据源来存储差集结果
        GDALDriver* pMemDriver = GetGDALDriverManager()->GetDriverByName("Memory");
        if (!pMemDriver)
        {
            spdlog::error("无法获取内存驱动");
            return nullptr;
        }
        
        static int s_diff_ds_counter = 0;
        std::string memName = std::string("diff_result_") + std::to_string(++s_diff_ds_counter);
        GDALDataset* pDiffDataset = pMemDriver->Create(memName.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
        if (!pDiffDataset)
        {
            spdlog::error("无法创建内存数据源");
            return nullptr;
        }
        
        // 创建差集结果图层
        OGRLayer* pDiffLayer = pDiffDataset->CreateLayer("difference", pSRS, parsedLayer->GetGeomType(), nullptr);
        if (!pDiffLayer)
        {
            spdlog::error("无法创建差集结果图层");
            GDALClose(pDiffDataset);
            return nullptr;
        }
        
        // 复制字段定义
        OGRFeatureDefn* pParsedLayerDefn = parsedLayer->GetLayerDefn();
        for (int i = 0; i < pParsedLayerDefn->GetFieldCount(); ++i)
        {
            OGRFieldDefn* pFieldDefn = pParsedLayerDefn->GetFieldDefn(i);
            pDiffLayer->CreateField(pFieldDefn);
        }
        
        // 添加错误类型字段
        OGRFieldDefn errorTypeField("ERROR_TYPE", OFTString);
        errorTypeField.SetWidth(50);
        pDiffLayer->CreateField(&errorTypeField);
        
        // 重置解析后图层的读取位置
        parsedLayer->ResetReading();
        
        // 处理每个要素
        OGRFeature* pFeature;
        bool hasError = false;
        
        while ((pFeature = parsedLayer->GetNextFeature()) != nullptr)
        {
            // 增加总要素计数
            outTotalCount++;
            
            OGRGeometry* pGeom = pFeature->GetGeometryRef();
            if (pGeom && !pGeom->IsEmpty())
            {
                OGRGeometry* pGeomUsed = pGeom;
                if (!pGeom->IsValid())
                {
                    OGRGeometry* pValid = pGeom->MakeValid();
                    if (!pValid)
                    {
                        pValid = pGeom->Buffer(0);
                    }
                    if (pValid && !pValid->IsEmpty())
                    {
                        pGeomUsed = pValid;
                    }
                }
                OGRGeometry* pDiffGeom = nullptr;
                try {
                    pDiffGeom = pGeomUsed->Difference(bufferGeom);
                } catch (...) {
                    pDiffGeom = nullptr;
                }
                
                // 如果差集结果有效且非空，说明有部分超出缓冲区
                if (pDiffGeom && !pDiffGeom->IsEmpty())
                {
                    // 增加错误要素计数
                    outErrorCount++;
                    hasError = true;
                    
                    // 创建新要素，包含超出缓冲区的几何体部分
                    OGRFeature* pDiffFeature = OGRFeature::CreateFeature(pDiffLayer->GetLayerDefn());
                    
                    // 复制属性
                    for (int i = 0; i < pParsedLayerDefn->GetFieldCount(); ++i)
                    {
                        pDiffFeature->SetField(i, pFeature->GetRawFieldRef(i));
                    }
                    
                    // 设置几何体和错误类型
                    pDiffFeature->SetGeometry(pDiffGeom);
                    std::string errText = fromQStringToUtf8(QStringLiteral("绝对偏移超出缓冲区"));
                    pDiffFeature->SetField("ERROR_TYPE", errText.c_str());
                    
                    // 添加到差集图层
                    pDiffLayer->CreateFeature(pDiffFeature);
                    
                    // 释放资源
                    OGRFeature::DestroyFeature(pDiffFeature);
                }
                
                // 释放差集几何体
                if (pDiffGeom)
                {
                    delete pDiffGeom;
                }
                if (pGeomUsed != pGeom)
                {
                    delete pGeomUsed;
                }
            }
            
            // 释放要素
            OGRFeature::DestroyFeature(pFeature);
        }
        
        // 如果没有错误，释放数据源并返回nullptr
        if (!hasError)
        {
            GDALClose(pDiffDataset);
            return nullptr;
        }
        
        spdlog::info("成功计算解析后图层与整体缓冲区的差集");
        return pDiffDataset;
    }
    catch (...)
    {
        spdlog::error("计算图层差集时发生异常");
        return nullptr;
    }
}

void* ComputeLayerDifferenceWithBuffers(void* pParsedLayer, const std::vector<OGRGeometry*>& buffers, int& outErrorCount, int& outTotalCount)
{
    outErrorCount = 0;
    outTotalCount = 0;
    if (!pParsedLayer || buffers.empty()) return nullptr;
    OGRLayer* parsedLayer = static_cast<OGRLayer*>(pParsedLayer);
    parsedLayer->ResetReading();
    OGRSpatialReference* pSRS = parsedLayer->GetSpatialRef();
    GDALDriver* pMemDriver = GetGDALDriverManager()->GetDriverByName("Memory");
    if (!pMemDriver) return nullptr;
    static int s_diff_ds_counter2 = 0;
    std::string memName = std::string("diff_result_buffers_") + std::to_string(++s_diff_ds_counter2);
    GDALDataset* pDiffDataset = pMemDriver->Create(memName.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!pDiffDataset) return nullptr;
    OGRLayer* pDiffLayer = pDiffDataset->CreateLayer("difference", pSRS, parsedLayer->GetGeomType(), nullptr);
    if (!pDiffLayer){ GDALClose(pDiffDataset); return nullptr; }
    OGRFeatureDefn* pParsedLayerDefn = parsedLayer->GetLayerDefn();
    for (int i = 0; i < pParsedLayerDefn->GetFieldCount(); ++i){ pDiffLayer->CreateField(pParsedLayerDefn->GetFieldDefn(i)); }
    OGRFieldDefn errorTypeField("ERROR_TYPE", OFTString); errorTypeField.SetWidth(50); pDiffLayer->CreateField(&errorTypeField);
    OGRFeature* pFeature; bool hasError = false;
    while ((pFeature = parsedLayer->GetNextFeature()) != nullptr){
        outTotalCount++;
        OGRGeometry* pGeom = pFeature->GetGeometryRef();
        if (pGeom && !pGeom->IsEmpty()){
            OGRGeometry* pGeomUsed = pGeom;
            if (!pGeom->IsValid()){ OGRGeometry* pValid = pGeom->MakeValid(); if (!pValid) pValid = pGeom->Buffer(0); if (pValid && !pValid->IsEmpty()) pGeomUsed = pValid; }
            OGRGeometry* work = pGeomUsed->clone();
            for (size_t bi = 0; bi < buffers.size(); ++bi){
                if (!work || work->IsEmpty()) break;
                OGRGeometry* buf = buffers[bi]; if (!buf || buf->IsEmpty()) continue;
                OGRGeometry* tmp = nullptr; try { tmp = work->Difference(buf);} catch(...) { tmp = nullptr; }
                delete work; work = tmp;
            }
            if (work && !work->IsEmpty()){
                outErrorCount++; hasError = true;
                OGRFeature* pDiffFeature = OGRFeature::CreateFeature(pDiffLayer->GetLayerDefn());
                for (int i = 0; i < pParsedLayerDefn->GetFieldCount(); ++i){ pDiffFeature->SetField(i, pFeature->GetRawFieldRef(i)); }
                pDiffFeature->SetGeometry(work);
                std::string errText = fromQStringToUtf8(QStringLiteral("绝对偏移超出缓冲区"));
                pDiffFeature->SetField("ERROR_TYPE", errText.c_str());
                pDiffLayer->CreateFeature(pDiffFeature);
                OGRFeature::DestroyFeature(pDiffFeature);
            }
            if (work) delete work;
            if (pGeomUsed != pGeom) delete pGeomUsed;
        }
        OGRFeature::DestroyFeature(pFeature);
    }
    if (!hasError){ GDALClose(pDiffDataset); return nullptr; }
    return pDiffDataset;
}

/**
 * @brief 绝对偏移检查主函数 - 检查解析后数据是否超出库体数据的缓冲区范围
 * 
 * 算法流程：
 * 1. 解析XML配置获取缓冲区半径参数
 * 2. 为库体数据的每个要素创建缓冲区
 * 3. 合并所有缓冲区为一个整体几何体
 * 4. 计算解析后数据与整体缓冲区的差集
 * 5. 生成检查报告和错误图层
 * 
 * @param szSourceShpFilePath 库体数据文件路径（Shapefile格式）
 * @param szParsedDataPath 解析后数据路径（支持单个文件或分号分隔的多个文件）
 * @param szSchemaXmlPath XML规则配置文件路径
 * @param szOutputCheckReportPath 输出检查报告文件路径
 * @param szOutputCheckResultLayerPath 输出错误图层文件路径
 * @param szOutputLogFilePath 输出日志文件路径
 * @param nScale 地图比例尺
 * @return SE_Error 错误码，SE_ERROR_NONE表示成功
 */
SE_Error AbsoluteOffsetCheck(
    const char* szSourceShpFilePath,
    const char* szParsedDataPath,
    const char* szSchemaXmlPath,
    const char* szOutputCheckResultLayerPath,
    const char* szOutputJsonPath,
    const char* szOutputLogFilePath,
    int nScale)
{
    try {
        // 初始化GDAL
        GDALAllRegister();
        
        // 设置日志
        try {
            CleanupLogger();
            const std::shared_ptr<spdlog::logger> previousDefaultLogger = spdlog::default_logger();
#ifdef _WIN32
            std::wstring wLogPath = fromUtf8ToQString(szOutputLogFilePath).toStdWString();
            auto logger = spdlog::basic_logger_mt("absolute_offset_check", wLogPath);
#else
            auto logger = spdlog::basic_logger_mt("absolute_offset_check", szOutputLogFilePath);
#endif
            g_absoluteOffsetPreviousDefaultLogger = previousDefaultLogger;
            spdlog::set_default_logger(logger);
            
            // 设置为同步写入，确保日志实时显示
            // 设置日志级别为info及以上级别时自动刷新
            spdlog::set_level(spdlog::level::info);
            // 每隔1秒自动刷新一次日志
            spdlog::flush_every(std::chrono::seconds(1));
            
            spdlog::info("开始绝对偏移检查，比例尺: {}", nScale);
        } catch (const std::exception& e) {
            spdlog::error("日志初始化失败: {}", e.what());
            return SE_ERROR_FAILURE;
        }

        // 参数验证
        if (!szSourceShpFilePath || !szParsedDataPath || !szSchemaXmlPath || 
            !szOutputJsonPath || !szOutputCheckResultLayerPath)
        {
            spdlog::error("输入参数无效");
            CleanupLogger();
            return SE_ERROR_INVALID_PARAMETER;
        }

        spdlog::info("源数据路径: {}", fromQStringToUtf8(fromUtf8ToQString(szSourceShpFilePath)));
        spdlog::info("解析后数据路径: {}", fromQStringToUtf8(fromUtf8ToQString(szParsedDataPath)));
        spdlog::info("XML配置路径: {}", szSchemaXmlPath);
        spdlog::info("地图比例尺: 1:{}", nScale);

        // 检查输入文件是否存在
        QFileInfo sourceFile(szSourceShpFilePath);
        QFileInfo schemaFile(szSchemaXmlPath);
        
        if (!sourceFile.exists())
        {
            spdlog::error("库体数据文件不存在: {}", szSourceShpFilePath);
            CleanupLogger();
            return SE_ERROR_FILE_NOT_FOUND;
        }
        
        if (!schemaFile.exists())
        {
            spdlog::error("XML规则文件不存在: {}", szSchemaXmlPath);
            CleanupLogger();
            return SE_ERROR_FILE_NOT_FOUND;
        }

        // 解析XML获取缓冲区半径
        double dBufferRadius = 0.0;
        SE_Error xmlResult = ParseXmlForAbsoluteOffsetBufferRadius(szSchemaXmlPath, dBufferRadius, nScale);
        if (xmlResult != SE_ERROR_NONE)
        {
            spdlog::error("XML解析失败");
            CleanupLogger();
            return xmlResult;
        }

        // 转换缓冲区半径为实地米，打开源图层后再换算为图层坐标单位。
        double dBufferRadiusMeters = ConvertMMToGeoUnits(dBufferRadius, nScale);
        spdlog::info("缓冲区半径换算: 图上{}毫米 -> 实地{}米", dBufferRadius, dBufferRadiusMeters);

        // 解析多个文件路径（用分号分隔）或单个文件路径
        QString strParsedDataPaths(szParsedDataPath);
        QStringList parsedFilePaths;
        
        // 检查是否包含分号分隔的多个文件路径
        if (strParsedDataPaths.contains(";"))
        {
            // 分号分隔的多个文件路径
            QStringList pathList = strParsedDataPaths.split(";", Qt::SkipEmptyParts);
            
            for (const QString& path : pathList)
            {
                QString trimmedPath = path.trimmed();
                QFileInfo fileInfo(trimmedPath);
                
                if (fileInfo.exists() && fileInfo.isFile() && fileInfo.suffix().toLower() == "shp")
                {
                    parsedFilePaths << trimmedPath;
                    spdlog::info("添加解析数据文件: {}", fromQStringToUtf8(trimmedPath));
                }
                else
                {
                    spdlog::error("无效的shapefile路径: {}", trimmedPath.toStdString());
                    CleanupLogger();
                    return SE_ERROR_FILE_NOT_FOUND;
                }
            }
            spdlog::info("使用{}个解析数据文件", parsedFilePaths.size());
        }
        else
        {
            // 单个文件路径
            QFileInfo parsedInfo(szParsedDataPath);
            
            if (parsedInfo.isFile() && parsedInfo.suffix().toLower() == "shp")
            {
                parsedFilePaths << szParsedDataPath;
                spdlog::info("使用单个解析数据文件: {}", szParsedDataPath);
            }
            else
            {
                spdlog::error("解析数据文件不存在或不是shapefile格式: {}", szParsedDataPath);
                CleanupLogger();
                return SE_ERROR_FILE_NOT_FOUND;
            }
        }

        // 打开库体数据文件
        spdlog::info("尝试打开源数据文件: {}", szSourceShpFilePath);
        GDALDataset* pSourceDataset = (GDALDataset*)GDALOpenEx(szSourceShpFilePath, GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
        if (!pSourceDataset)
        {
            spdlog::error("无法打开库体数据文件: {}", szSourceShpFilePath);
            spdlog::error("GDAL错误信息: ");
            CleanupLogger();
            return SE_ERROR_GDAL_OPEN_FAILED;
        }
        spdlog::info("成功打开源数据文件");

        // 获取源数据图层
        OGRLayer* pSourceLayer = pSourceDataset->GetLayer(0);
        
        if (!pSourceLayer)
        {
            spdlog::error("无法获取源数据图层");
            GDALClose(pSourceDataset);
            CleanupLogger();
            return SE_ERROR_LAYER_NOT_FOUND;
        }

        OGRSpatialReference* pSourceSRS = pSourceLayer->GetSpatialRef();
        double dLinearUnitsInMeters = 1.0;
        if (pSourceSRS)
        {
            if (pSourceSRS->IsGeographic())
            {
                spdlog::error("库体数据空间参考为地理坐标系(度)。当前实现不支持将图上毫米精确换算为度单位，请使用投影坐标系数据。");
                GDALClose(pSourceDataset);
                CleanupLogger();
                return SE_ERROR_UNSUPPORTED_SRS;
            }
            const char* pszUnitsName = nullptr;
            dLinearUnitsInMeters = pSourceSRS->GetLinearUnits(&pszUnitsName);
            if (dLinearUnitsInMeters <= 0.0)
            {
                spdlog::warn("无法获取有效的线性单位转换系数，默认按米处理");
                dLinearUnitsInMeters = 1.0;
            }
            spdlog::info("库体坐标单位: {} 每单位米数: {}", pszUnitsName ? pszUnitsName : "(未知)", dLinearUnitsInMeters);
        }
        else
        {
            spdlog::warn("库体数据缺少空间参考，默认按米处理缓冲距离");
        }

        double dBufferRadiusUnits = dBufferRadiusMeters / dLinearUnitsInMeters;
        spdlog::info("缓冲区半径换算: 实地{}米 -> 图层单位{}", dBufferRadiusMeters, dBufferRadiusUnits);

        // 为每个源要素创建缓冲区
        std::vector<OGRGeometry*> sourceBuffers;
        pSourceLayer->ResetReading();
        OGRFeature* pSourceFeature;

        spdlog::info("开始创建源数据缓冲区，半径: {} 图层单位", dBufferRadiusUnits);

        while ((pSourceFeature = pSourceLayer->GetNextFeature()) != nullptr)
        {
            OGRGeometry* pGeom = pSourceFeature->GetGeometryRef();
            if (pGeom)
            {
                OGRGeometry* pSrcGeomUsed = pGeom;
                if (!pGeom->IsValid())
                {
                    OGRGeometry* pValid = pGeom->MakeValid();
                    if (!pValid)
                    {
                        pValid = pGeom->Buffer(0);
                    }
                    if (pValid && !pValid->IsEmpty())
                    {
                        pSrcGeomUsed = pValid;
                    }
                }
                OGRGeometry* pBuffer = pSrcGeomUsed->Buffer(dBufferRadiusUnits);
                if (pBuffer)
                {
                    sourceBuffers.push_back(pBuffer);
                }
                if (pSrcGeomUsed != pGeom)
                {
                    delete pSrcGeomUsed;
                }
            }
            OGRFeature::DestroyFeature(pSourceFeature);
        }

        if (sourceBuffers.empty())
        {
            spdlog::error("源数据中没有有效的几何体");
            GDALClose(pSourceDataset);
            CleanupLogger();
            return SE_ERROR_BUFFER_FAILED;
        }
        
        spdlog::info("创建了 {} 个源数据缓冲区", sourceBuffers.size());
        
        // 合并所有缓冲区为一个整体
        spdlog::info("开始合并缓冲区为一个整体...");
        OGRGeometry* pMergedBuffer = static_cast<OGRGeometry*>(MergeBuffers(sourceBuffers));
        if (!pMergedBuffer)
        {
            spdlog::warn("合并缓冲区失败，改用逐缓冲差集");
        }
        else if (!pMergedBuffer->IsValid())
        {
            OGRGeometry* pValidMerged = pMergedBuffer->MakeValid();
            if (!pValidMerged)
            {
                pValidMerged = pMergedBuffer->Buffer(0);
            }
            if (pValidMerged && !pValidMerged->IsEmpty())
            {
                delete pMergedBuffer;
                pMergedBuffer = pValidMerged;
            }
        }
        
        spdlog::info("成功合并缓冲区为一个整体");
        
        // 标记是否存在偏移错误
        bool hasOffsetError = false;
        
        // 统计总要素数和错误要素数
        int totalFeatures = 0;
        int errorFeatures = 0;
        
        // 获取XML文件名作为图层名
        QFileInfo xmlFileInfo(szSchemaXmlPath);
        QString layerName = xmlFileInfo.baseName();
        
        std::vector<std::pair<GDALDataset*, QString>> diffResults;
            
        // 遍历所有解析数据文件
            for (const QString& parsedFilePath : parsedFilePaths)
            {
                spdlog::info("尝试打开解析数据文件: {}", fromQStringToUtf8(parsedFilePath));
                
                // 打开解析数据文件
                GDALDataset* pParsedDataset = (GDALDataset*)GDALOpenEx(parsedFilePath.toStdString().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
                if (!pParsedDataset)
                {
                    spdlog::error("无法打开解析数据文件: {}", fromQStringToUtf8(parsedFilePath));
                    continue;
                }
                spdlog::info("成功打开解析数据文件");
                
                // 获取解析数据图层
                OGRLayer* pParsedLayer = pParsedDataset->GetLayer(0);
                if (!pParsedLayer)
                {
                    spdlog::error("无法获取解析数据图层");
                    GDALClose(pParsedDataset);
                    continue;
                }
                
                // 计算解析后图层与整体缓冲区的差集
                spdlog::info("计算解析后图层与整体缓冲区的差集...");
                int fileErrorCount = 0;
                int fileTotalCount = 0;
                GDALDataset* pDiffDataset = nullptr;
                if (pMergedBuffer)
                    pDiffDataset = static_cast<GDALDataset*>(ComputeLayerDifference(pParsedLayer, pMergedBuffer, fileErrorCount, fileTotalCount));
                else
                    pDiffDataset = static_cast<GDALDataset*>(ComputeLayerDifferenceWithBuffers(pParsedLayer, sourceBuffers, fileErrorCount, fileTotalCount));
                
                // 更新总计数
                totalFeatures += fileTotalCount;
                errorFeatures += fileErrorCount;
                
                // 如果差集结果为空，说明没有偏移错误
                if (!pDiffDataset)
                {
                    spdlog::info("解析后图层完全在缓冲区内，没有偏移错误");
                }
                else
                {
                    // 有偏移错误，收集差集结果
                    hasOffsetError = true;
                    spdlog::info("解析后图层有部分超出缓冲区，存在偏移错误");
                    
                    QFileInfo parsedInfo(parsedFilePath);
                    QString parsedBase = parsedInfo.baseName();
                    diffResults.push_back(std::make_pair(pDiffDataset, parsedBase));
                }
                
                // 关闭解析数据集
                GDALClose(pParsedDataset);
            }
        
        if (hasOffsetError && !diffResults.empty())
        {
            GDALDriver* pDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
            if (pDriver)
            {
                QString baseOutput = fromUtf8ToQString(szOutputCheckResultLayerPath);
                QFileInfo outInfo(baseOutput);
                QString outDir = outInfo.dir().absolutePath();
                QString outBase = outInfo.completeBaseName();

                for (auto& pair : diffResults)
                {
                    GDALDataset* pDiffDataset = pair.first;
                    QString parsedBase = pair.second;
                    OGRLayer* pDiffLayer = pDiffDataset->GetLayer(0);
                    if (!pDiffLayer)
                    {
                        spdlog::error("无法获取差集图层");
                        GDALClose(pDiffDataset);
                        continue;
                    }

                    QString finalPath = outDir + QDir::separator() + outBase + "_" + parsedBase + ".shp";
                    if (QFile::exists(finalPath))
                    {
                        pDriver->Delete(finalPath.toStdString().c_str());
                    }
                    CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");
                    char** lco = nullptr;
                    lco = CSLSetNameValue(lco, "ENCODING", "UTF-8");
                    GDALDataset* pOutputDataset = pDriver->Create(finalPath.toStdString().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
                    if (!pOutputDataset)
                    {
                        spdlog::error("无法创建输出文件: {}", fromQStringToUtf8(finalPath));
                        GDALClose(pDiffDataset);
                        CSLDestroy(lco);
                        continue;
                    }
                    OGRLayer* pOutputLayer = pOutputDataset->CreateLayer("absolute_offset_errors", pDiffLayer->GetSpatialRef(), pDiffLayer->GetGeomType(), lco);
                    CSLDestroy(lco);
                    if (!pOutputLayer)
                    {
                        spdlog::error("无法创建输出图层");
                        GDALClose(pOutputDataset);
                        GDALClose(pDiffDataset);
                        continue;
                    }
                    OGRFeatureDefn* pDiffLayerDefn = pDiffLayer->GetLayerDefn();
                    for (int i = 0; i < pDiffLayerDefn->GetFieldCount(); ++i)
                    {
                        pOutputLayer->CreateField(pDiffLayerDefn->GetFieldDefn(i));
                    }
                    pDiffLayer->ResetReading();
                    OGRFeature* pFeature;
                    while ((pFeature = pDiffLayer->GetNextFeature()) != nullptr)
                    {
                        OGRFeature* pNewFeature = OGRFeature::CreateFeature(pOutputLayer->GetLayerDefn());
                        pNewFeature->SetGeometry(pFeature->GetGeometryRef());
                        for (int i = 0; i < pDiffLayerDefn->GetFieldCount(); ++i)
                        {
                            pNewFeature->SetField(i, pFeature->GetRawFieldRef(i));
                        }
                        pOutputLayer->CreateFeature(pNewFeature);
                        OGRFeature::DestroyFeature(pNewFeature);
                        OGRFeature::DestroyFeature(pFeature);
                    }
                    GDALClose(pOutputDataset);
                    GDALClose(pDiffDataset);
                    spdlog::info("错误图层已生成: {}", fromQStringToUtf8(finalPath));
                }
            }
            else
            {
                spdlog::error("无法获取Shapefile驱动");
                for (auto& pair : diffResults) { GDALClose(pair.first); }
            }
        }
        else
        {
            // 没有偏移错误，释放所有差集结果但不生成Shapefile
            for (auto& pair : diffResults) { GDALClose(pair.first); }
            spdlog::info("未发现绝对偏移错误，不生成错误图层");
        }
        for (auto* buffer : sourceBuffers)
        {
            if (buffer) delete buffer;
        }
        sourceBuffers.clear();
        
        // 生成JSON输出
        QJsonObject checkItem;
        checkItem["err_exist"] = hasOffsetError ? "true" : "false";
        checkItem["id"] = "104";
        checkItem["name"] = "绝对偏移检查";
        checkItem["status"] = "质检完成";
        
        QString infoMsg = hasOffsetError ? 
            QString("%1图层存在绝对偏移错误").arg(layerName) : 
            QString("%1图层不存在绝对偏移错误").arg(layerName);
        checkItem["info"] = infoMsg;
        
        QJsonObject jsonResult;
        jsonResult["check_item"] = checkItem;
        
        // 写入JSON文件
        QJsonDocument doc(jsonResult);
        QFile jsonFile(fromUtf8ToQString(szOutputJsonPath));
        if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            jsonFile.write(doc.toJson(QJsonDocument::Compact));
            jsonFile.close();
            spdlog::info("JSON结果已生成: {}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));
        } else {
            spdlog::error("无法创建JSON结果文件: {}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));
        }
        
        // 释放合并后的缓冲区
        if (pMergedBuffer){ delete pMergedBuffer; pMergedBuffer = nullptr; }
        
        // 确保关闭所有数据集
        if (pSourceDataset)
        {
            GDALClose(pSourceDataset);
            pSourceDataset = nullptr;
        }
        
        spdlog::info("绝对偏移检查完成，结果: {}", hasOffsetError ? "存在偏移错误" : "无偏移错误");
    CleanupLogger();
    
    return SE_ERROR_NONE;
    } catch (const std::exception& e) {
        spdlog::error("绝对偏移检查发生异常: {}", e.what());
        CleanupLogger();
        return SE_ERROR_FAILURE;
    } catch (...) {
        spdlog::error("绝对偏移检查发生未知异常");
        CleanupLogger();
        return SE_ERROR_FAILURE;
    }
}
