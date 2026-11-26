#define _HAS_STD_BYTE 0
#include "se_map_name_match_check_algorithm.h"
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <cpl_string.h>
#include <QtXml/QDomDocument>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QFileInfo>
#include <QtCore/QMap>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <vector>

// 将UTF-8字符串转换为本地编码
static QString toLocalEncoding(const char* utf8Str)
{
    return QString::fromUtf8(utf8Str);
}

// 将本地编码字符串转换为UTF-8
static std::string fromLocalEncoding(const QString& localStr)
{
    QByteArray utf8Bytes = localStr.toUtf8();
    return std::string(utf8Bytes.constData(), utf8Bytes.length());
}

// 写JSON结果
static void WriteJsonResult(
    const char* szOutputJsonPath, 
    const QString& mapName,
    const QString& adminArea,
    bool success,
    bool matched
) {
    QJsonObject item;
    item["id"] = "302";
    item["name"] = QStringLiteral("图名内容准确性");
    
    if (!success) {
        item["status"] = QStringLiteral("质检失败");
    } else {
        item["status"] = QStringLiteral("质检完成");
    }
    
    item["err_exist"] = matched ? QStringLiteral("false") : QStringLiteral("true");
    
    // info 直接是字符串描述
    if (matched) {
        item["info"] = QStringLiteral("综合后地图不存在图名错误");
    } else {
        item["info"] = QStringLiteral("综合后地图存在图名错误");
    }

    QJsonObject root;
    root["check_item"] = item;

    QJsonDocument doc(root);
    QFile jsonFile(szOutputJsonPath);
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        jsonFile.write(doc.toJson(QJsonDocument::Indented));
        jsonFile.close();
    }
}

SE_Error ParseXmlForMapNameMatch(const char* szSchemaXmlPath, char* szFieldName)
{
    QFile file(szSchemaXmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return SE_ERROR_FILE_NOT_FOUND;
    }

    QDomDocument doc;
    QString errorMsg;
    int errorLine, errorColumn;
    if (!doc.setContent(&file, &errorMsg, &errorLine, &errorColumn))
    {
        file.close();
        return SE_ERROR_XML_PARSE_FAILED;
    }
    file.close();

    // 解析XML，查找字段名配置
    QDomElement root = doc.documentElement();
    QDomNodeList items = root.elementsByTagName("check_item");
    for (int i = 0; i < items.count(); ++i)
    {
        QDomElement item = items.at(i).toElement();
        QDomElement nameElement = item.firstChildElement("name");
        if (!nameElement.isNull() && nameElement.text() == "图名与行政区划匹配检查")
        {
            QDomElement fieldElement = item.firstChildElement("field");
            if (!fieldElement.isNull())
            {
                QString fieldName = fieldElement.text();
                strcpy(szFieldName, fieldName.toUtf8().constData());
                return SE_ERROR_NONE;
            }
        }
    }

    // 如果没有找到field配置，使用默认值
    strcpy(szFieldName, "text_conte");
    return SE_ERROR_NONE;
}

// 从图名判断行政级别，返回BOUA代码
static QString GetAdminLevelFromMapName(const QString& mapName)
{
    // 支持 "地图" 和 "地 图" 两种格式
    if (mapName.contains("省") && (mapName.contains("地图") || mapName.contains("地 图")))
        return "BOUA2";
    else if (mapName.contains("市") && (mapName.contains("地图") || mapName.contains("地 图")))
        return "BOUA4";
    else if (mapName.contains("县") && (mapName.contains("地图") || mapName.contains("地 图")))
        return "BOUA5";
    else if ((mapName.contains("乡") || mapName.contains("镇")) && (mapName.contains("地图") || mapName.contains("地 图")))
        return "BOUA6";
    return QString();
}

// 获取BOUA层名称
static QString GetBOUALayerName(const QString& adminLevel)
{
    static const QMap<QString, QString> layerNames = {
        {"BOUA2", "省级行政区划面"},
        {"BOUA4", "市级行政区划面"},
        {"BOUA5", "县级行政区划面"},
        {"BOUA6", "乡镇级行政区划面"}
    };
    return layerNames.value(adminLevel);
}

// 从图名中提取行政区划名称
static QString ExtractAdminAreaFromMapName(const QString& mapName)
{
    QString regionName = mapName;
    
    // 移除所有空格
    regionName.remove(' ');
    
    // 去掉"地图"后缀
    regionName.replace("地图", "");
    
    // 去掉行政级别关键字
    regionName.replace("省", "");
    regionName.replace("市", "");
    regionName.replace("县", "");
    regionName.replace("区", "");
    regionName.replace("乡", "");
    regionName.replace("镇", "");
    
    return regionName.trimmed();
}

SE_Error MapNameMatchCheck(
    const char* szMapNameShpPath,
    const char* szAdminColorShpPath,
    const char* szBouaDataPath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    bool& bMatched
) {
    bMatched = false;
    QString mapName;
    QString extractedAdminName;
    QString matchedRegionName;
    bool success = false;

    // Windows UTF-8 路径支持
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");

    // 1. 解析XML获取字段名
    char fieldName[256] = "text_conte";  // 默认字段名
    ParseXmlForMapNameMatch(szSchemaXmlPath, fieldName);

    // 2. 打开图名图层
    GDALAllRegister();
    GDALDataset* nameDs = static_cast<GDALDataset*>(
        GDALOpenEx(szMapNameShpPath, GDAL_OF_VECTOR, nullptr, nullptr, nullptr)
    );
    if (!nameDs) {
        WriteJsonResult(szOutputJsonPath, mapName, extractedAdminName, false, false);
        return SE_ERROR_LAYER_INVALID;
    }

    OGRLayer* nameLayer = nameDs->GetLayer(0);
    if (!nameLayer) {
        GDALClose(nameDs);
        WriteJsonResult(szOutputJsonPath, mapName, extractedAdminName, false, false);
        return SE_ERROR_LAYER_INVALID;
    }

    // 3. 查找图名（支持"地图"和"地 图"）
    OGRFeatureDefn* poFDefn = nameLayer->GetLayerDefn();
    int fieldIndex = poFDefn->GetFieldIndex(fieldName);
    if (fieldIndex == -1) {
        GDALClose(nameDs);
        WriteJsonResult(szOutputJsonPath, mapName, extractedAdminName, false, false);
        return SE_ERROR_FIELD_NOT_FOUND;
    }

    bool foundMapName = false;
    OGRFeature* poFeature;
    nameLayer->ResetReading();
    while ((poFeature = nameLayer->GetNextFeature()) != nullptr) {
        QString value = QString::fromUtf8(poFeature->GetFieldAsString(fieldIndex));
        value = value.trimmed();
        
        // 检查是否为有效的地图名称（以"地图"或"地 图"结尾）
        if (value.endsWith("地图") || value.endsWith("地 图")) {
            mapName = value;
            foundMapName = true;
            OGRFeature::DestroyFeature(poFeature);
            break;
        }
        OGRFeature::DestroyFeature(poFeature);
    }
    GDALClose(nameDs);

    if (!foundMapName) {
        WriteJsonResult(szOutputJsonPath, mapName, extractedAdminName, false, false);
        return SE_ERROR_NO_MAP_NAME;
    }

    // 4. 判断行政级别
    QString adminLevel = GetAdminLevelFromMapName(mapName);
    if (adminLevel.isEmpty()) {
        WriteJsonResult(szOutputJsonPath, mapName, extractedAdminName, false, false);
        return SE_ERROR_NO_ADMIN_MATCH;
    }

    // 5. 提取行政区划名称（去除所有空格）
    extractedAdminName = ExtractAdminAreaFromMapName(mapName);
    if (extractedAdminName.isEmpty()) {
        WriteJsonResult(szOutputJsonPath, mapName, extractedAdminName, false, false);
        return SE_ERROR_NO_ADMIN_MATCH;
    }

    // 6. 打开行政区划面普色图层（用于空间分析）
    GDALDataset* colorDs = static_cast<GDALDataset*>(
        GDALOpenEx(szAdminColorShpPath, GDAL_OF_VECTOR, nullptr, nullptr, nullptr)
    );
    if (!colorDs) {
        WriteJsonResult(szOutputJsonPath, mapName, extractedAdminName, false, false);
        return SE_ERROR_LAYER_INVALID;
    }

    OGRLayer* colorLayer = colorDs->GetLayer(0);
    if (!colorLayer) {
        GDALClose(colorDs);
        WriteJsonResult(szOutputJsonPath, mapName, extractedAdminName, false, false);
        return SE_ERROR_LAYER_INVALID;
    }

    // 7. 打开BOUA标准数据（GDB或SHP）
    GDALDataset* bouaDs = static_cast<GDALDataset*>(
        GDALOpenEx(szBouaDataPath, GDAL_OF_VECTOR, nullptr, nullptr, nullptr)
    );
    if (!bouaDs) {
        GDALClose(colorDs);
        WriteJsonResult(szOutputJsonPath, mapName, extractedAdminName, false, false);
        return SE_ERROR_LAYER_INVALID;
    }

    // 8. 查找对应的BOUA图层（4层策略）
    OGRLayer* bouaLayer = nullptr;
    QString targetLayerName = GetBOUALayerName(adminLevel);
    
    // 策略1: 尝试精确匹配层名（如"省级行政区划面"）
    if (!targetLayerName.isEmpty()) {
        bouaLayer = bouaDs->GetLayerByName(targetLayerName.toUtf8().constData());
    }
    
    // 策略2: 尝试匹配BOUA代码（如"BOUA2"）
    if (!bouaLayer) {
        bouaLayer = bouaDs->GetLayerByName(adminLevel.toUtf8().constData());
    }
    
    // 策略3: 模糊匹配 - 遍历所有层，查找包含关键字的层
    if (!bouaLayer) {
        int layerCount = bouaDs->GetLayerCount();
        for (int i = 0; i < layerCount; i++) {
            OGRLayer* layer = bouaDs->GetLayer(i);
            if (layer) {
                QString layerName = QString::fromUtf8(layer->GetName());
                // 检查层名是否包含BOUA代码或目标层名的关键字
                if (layerName.contains(adminLevel, Qt::CaseInsensitive) ||
                    layerName.contains(targetLayerName, Qt::CaseInsensitive) ||
                    (adminLevel == "BOUA2" && layerName.contains("省", Qt::CaseInsensitive)) ||
                    (adminLevel == "BOUA4" && layerName.contains("市", Qt::CaseInsensitive)) ||
                    (adminLevel == "BOUA5" && layerName.contains("县", Qt::CaseInsensitive)) ||
                    (adminLevel == "BOUA6" && (layerName.contains("乡", Qt::CaseInsensitive) || layerName.contains("镇", Qt::CaseInsensitive)))) {
                    bouaLayer = layer;
                    break;
                }
            }
        }
    }
    
    // 策略4: 如果还是找不到，使用第一个图层作为兜底
    if (!bouaLayer && bouaDs->GetLayerCount() > 0) {
        bouaLayer = bouaDs->GetLayer(0);
    }
    
    if (!bouaLayer) {
        GDALClose(colorDs);
        GDALClose(bouaDs);
        WriteJsonResult(szOutputJsonPath, mapName, extractedAdminName, false, false);
        return SE_ERROR_LAYER_INVALID;
    }

    // 9. 检查BOUA图层是否有XQ字段
    OGRFeatureDefn* bouaFDefn = bouaLayer->GetLayerDefn();
    int xqFieldIndex = bouaFDefn->GetFieldIndex("XQ");
    
    if (xqFieldIndex == -1) {
        // 如果没有XQ字段，尝试其他可能的字段名
        const char* possibleFieldNames[] = {"NAME", "名称", "name", "NAME_", nullptr};
        for (int i = 0; possibleFieldNames[i] != nullptr && xqFieldIndex == -1; i++) {
            xqFieldIndex = bouaFDefn->GetFieldIndex(possibleFieldNames[i]);
        }
    }
    
    if (xqFieldIndex == -1) {
        GDALClose(colorDs);
        GDALClose(bouaDs);
        WriteJsonResult(szOutputJsonPath, mapName, extractedAdminName, false, false);
        return SE_ERROR_FIELD_NOT_FOUND;
    }

    // 10. 进行空间分析和名称匹配
    bool isMatched = false;
    double maxIntersectArea = 0.0;  // 用于存储最大重叠面积
    OGRFeature* bestMatchFeature = nullptr;  // 用于存储最佳匹配的BOUA要素

    // 先获取所有BOUA要素到内存（提高性能）
    struct BouaFeatureData {
        OGRGeometry* geom;
        QString xqValue;
    };
    std::vector<BouaFeatureData> bouaFeatures;
    
    bouaLayer->ResetReading();
    while ((poFeature = bouaLayer->GetNextFeature()) != nullptr) {
        OGRGeometry* geom = poFeature->GetGeometryRef();
        if (geom) {
            BouaFeatureData data;
            data.geom = geom->clone();  // 克隆几何体
            data.xqValue = QString::fromUtf8(poFeature->GetFieldAsString(xqFieldIndex));
            bouaFeatures.push_back(data);
        }
        OGRFeature::DestroyFeature(poFeature);
    }

    // 遍历行政区划面普色图层的每个要素
    colorLayer->ResetReading();
    while ((poFeature = colorLayer->GetNextFeature()) != nullptr) {
        OGRGeometry* colorGeom = poFeature->GetGeometryRef();
        
        if (colorGeom) {
            // 遍历所有BOUA要素，找到重叠面积最大的
            for (size_t i = 0; i < bouaFeatures.size(); i++) {
                OGRGeometry* bouaGeom = bouaFeatures[i].geom;
                
                // 检查是否有重叠
                if (bouaGeom->Intersects(colorGeom)) {
                    // 计算重叠面积
                    OGRGeometry* intersection = bouaGeom->Intersection(colorGeom);
                    if (intersection) {
                        double area = 0.0;
                        
                        // 根据几何类型计算面积
                        OGRwkbGeometryType geomType = wkbFlatten(intersection->getGeometryType());
                        if (geomType == wkbPolygon) {
                            OGRPolygon* poly = dynamic_cast<OGRPolygon*>(intersection);
                            if (poly) {
                                area = poly->get_Area();
                            }
                        } else if (geomType == wkbMultiPolygon) {
                            OGRMultiPolygon* mpoly = dynamic_cast<OGRMultiPolygon*>(intersection);
                            if (mpoly) {
                                area = mpoly->get_Area();
                            }
                        }
                        
                        // 如果重叠面积更大，更新最佳匹配
                        if (area > maxIntersectArea) {
                            maxIntersectArea = area;
                            matchedRegionName = bouaFeatures[i].xqValue;
                        }
                        
                        delete intersection;
                    }
                }
            }
        }
        
        OGRFeature::DestroyFeature(poFeature);
    }

    // 清理BOUA要素几何体
    for (size_t i = 0; i < bouaFeatures.size(); i++) {
        delete bouaFeatures[i].geom;
    }

    // 11. 如果找到空间上匹配的要素，检查名称匹配
    if (maxIntersectArea > 0) {
        // 移除BOUA地区名中的空格
        QString bouaRegionName = matchedRegionName;
        bouaRegionName.remove(' ');
        bouaRegionName = bouaRegionName.trimmed();
        
        // 移除提取的地区名中的空格
        QString normalizedExtractedName = extractedAdminName;
        normalizedExtractedName.remove(' ');
        normalizedExtractedName = normalizedExtractedName.trimmed();
        
        // 检查名称是否匹配（提取的名称包含在BOUA名称中）
        if (normalizedExtractedName.contains(bouaRegionName) || 
            bouaRegionName.contains(normalizedExtractedName)) {
            isMatched = true;
        }
    }

    // 12. 清理资源
    GDALClose(colorDs);
    GDALClose(bouaDs);

    // 13. 设置匹配结果
    bMatched = isMatched;
    success = true;

    // 14. 写入结果
    WriteJsonResult(szOutputJsonPath, mapName, isMatched ? matchedRegionName : extractedAdminName, success, bMatched);
    
    return SE_ERROR_NONE;
}