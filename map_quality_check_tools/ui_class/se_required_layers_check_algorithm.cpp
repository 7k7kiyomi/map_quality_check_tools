#include "se_required_layers_check_algorithm.h"
#include <QString>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QXmlStreamReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTextStream>
#include <QMap>
#include <QList>

/**
 * @brief 必需图层检查主函数（输出JSON）
 */
SE_Error RequiredLayersCheck(
    const char* szSchemaXmlPath,
    const char* szDirectoryPath,
    const char* szOutputJsonPath,
    bool& bPassed)
{
    bPassed = false;

    // 1. 检查XML文件是否存在
    QString xmlPath = QString::fromUtf8(szSchemaXmlPath);
    QFile xmlFile(xmlPath);
    if (!xmlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString errMsg = QString::fromUtf8("无法打开XML文件: ") + xmlPath;
        
        QJsonObject checkItem;
        checkItem["id"] = "101";
        checkItem["name"] = "图层完整性检查";
        checkItem["err_exist"] = "true";
        checkItem["status"] = "质检失败";
        checkItem["info"] = errMsg;
        
        QJsonObject resultJson;
        resultJson["check_item"] = checkItem;
        
        QFile outFile(QString::fromUtf8(szOutputJsonPath));
        if (outFile.open(QIODevice::WriteOnly)) {
            QTextStream out(&outFile);
            out.setCodec("UTF-8");
            out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
            outFile.close();
        }
        
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 2. 解析XML配置，获取必需图层列表
    struct LayerCheckItem {
        QString name;    // 检查项名称（规则名称）
        QString value;   // 图层名称
    };
    QList<LayerCheckItem> requiredLayerItems;

    QXmlStreamReader xml(&xmlFile);
    bool inCheckItem = false;
    
    while (!xml.atEnd()) {
        xml.readNext();
        
        if (xml.isStartElement()) {
            if (xml.name() == "check_item") {
                inCheckItem = true;
            }
            else if (inCheckItem && xml.name() == "rule") {
                // 解析单个rule节点
                LayerCheckItem item;
                
                while (!xml.atEnd()) {
                    xml.readNext();
                    
                    if (xml.isStartElement()) {
                        if (xml.name() == "name") {
                            item.name = xml.readElementText().trimmed();
                        }
                        else if (xml.name() == "value") {
                            item.value = xml.readElementText().trimmed();
                        }
                    }
                    else if (xml.isEndElement() && xml.name() == "rule") {
                        // rule节点结束，保存数据
                        if (!item.name.isEmpty() && !item.value.isEmpty()) {
                            requiredLayerItems.append(item);
                        }
                        break;
                    }
                }
            }
        }
        else if (xml.isEndElement() && xml.name() == "check_item") {
            inCheckItem = false;
        }
    }
    xmlFile.close();

    if (xml.hasError()) {
        QString errMsg = QString::fromUtf8("XML解析错误: ") + xml.errorString();
        
        QJsonObject checkItem;
        checkItem["id"] = "101";
        checkItem["name"] = "图层完整性检查";
        checkItem["err_exist"] = "true";
        checkItem["status"] = "质检失败";
        checkItem["info"] = errMsg;
        
        QJsonObject resultJson;
        resultJson["check_item"] = checkItem;
        
        QFile outFile(QString::fromUtf8(szOutputJsonPath));
        if (outFile.open(QIODevice::WriteOnly)) {
            QTextStream out(&outFile);
            out.setCodec("UTF-8");
            out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
            outFile.close();
        }
        
        return SE_ERROR_XML_PARSE_FAILED;
    }

    if (requiredLayerItems.isEmpty()) {
        QString errMsg = QString::fromUtf8("XML文件中未找到有效的图层检查配置");
        
        QJsonObject checkItem;
        checkItem["id"] = "101";
        checkItem["name"] = "图层完整性检查";
        checkItem["err_exist"] = "true";
        checkItem["status"] = "质检失败";
        checkItem["info"] = errMsg;
        
        QJsonObject resultJson;
        resultJson["check_item"] = checkItem;
        
        QFile outFile(QString::fromUtf8(szOutputJsonPath));
        if (outFile.open(QIODevice::WriteOnly)) {
            QTextStream out(&outFile);
            out.setCodec("UTF-8");
            out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
            outFile.close();
        }
        
        return SE_ERROR_FAILURE;
    }

    // 3. 检查目录是否存在
    QString dirPath = QString::fromUtf8(szDirectoryPath);
    QDir dir(dirPath);
    if (!dir.exists()) {
        QString errMsg = QString::fromUtf8("检查目录不存在: ") + dirPath;
        
        QJsonObject checkItem;
        checkItem["id"] = "101";
        checkItem["name"] = "图层完整性检查";
        checkItem["err_exist"] = "true";
        checkItem["status"] = "质检失败";
        checkItem["info"] = errMsg;
        
        QJsonObject resultJson;
        resultJson["check_item"] = checkItem;
        
        QFile outFile(QString::fromUtf8(szOutputJsonPath));
        if (outFile.open(QIODevice::WriteOnly)) {
            QTextStream out(&outFile);
            out.setCodec("UTF-8");
            out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
            outFile.close();
        }
        
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 4. 扫描目录，获取所有shp文件
    QStringList filters;
    filters << "*.shp";
    QFileInfoList files = dir.entryInfoList(filters, QDir::Files);

    // 5. 检查每个必需图层是否存在
    QMap<QString, bool> layerFound;
    QMap<QString, QString> layerDisplayName;  // 存储显示名称
    
    for (const auto& item : requiredLayerItems) {
        layerFound[item.value] = false;
        layerDisplayName[item.value] = item.name;
    }

    // 遍历shp文件，标记找到的图层
    for (const QFileInfo& fileInfo : files) {
        QString baseName = fileInfo.baseName();
        for (const auto& item : requiredLayerItems) {
            if (baseName.contains(item.value)) {
                layerFound[item.value] = true;
                break;
            }
        }
    }

    // 6. 统计结果
    int totalLayers = requiredLayerItems.size();
    int foundLayers = 0;
    QJsonObject errorsObj;
    int errorCount = 0;

    for (const auto& item : requiredLayerItems) {
        if (layerFound[item.value]) {
            foundLayers++;
        } else {
            errorCount++;
            QString errorKey = QString("error%1").arg(errorCount);
            QString errorMsg = QString("%1图层缺失（需要图层名包含：%2）")
                .arg(item.name)
                .arg(item.value);
            errorsObj[errorKey] = errorMsg;
        }
    }

    // 7. 生成JSON结果
    QJsonObject checkItem;
    checkItem["id"] = "101";
    checkItem["name"] = "图层完整性检查";
    
    if (foundLayers == totalLayers) {
        checkItem["err_exist"] = "false";
        checkItem["status"] = "质检完成";
        checkItem["info"] = QString("所有必需图层都存在（共%1个）").arg(totalLayers);
        bPassed = true;
    } else {
        checkItem["err_exist"] = "true";
        checkItem["status"] = "质检完成";
        checkItem["info"] = QString("存在图层缺失（缺失%1个，共需%2个）")
            .arg(totalLayers - foundLayers)
            .arg(totalLayers);
        checkItem["errors"] = errorsObj;
        bPassed = false;
    }

    // 构建最终JSON结构
    QJsonObject resultJson;
    resultJson["check_item"] = checkItem;

    // 8. 写入JSON文件
    QFile outFile(QString::fromUtf8(szOutputJsonPath));
    if (!outFile.open(QIODevice::WriteOnly)) {
        return SE_ERROR_FAILURE;
    }

    QTextStream out(&outFile);
    out.setCodec("UTF-8");
    out << QJsonDocument(resultJson).toJson(QJsonDocument::Compact);
    outFile.close();

    return SE_ERROR_NONE;
}
