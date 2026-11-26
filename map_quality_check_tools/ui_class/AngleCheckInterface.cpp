#include "AngleCheckInterface.h"
#include "AngleCheckDialog.h"
#include "GeometryValidator.h"
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

// 辅助函数：生成错误JSON
static void generateErrorJson(const QString &jsonPath, const QString &errorInfo)
{
    // 确保输出目录存在
    QFileInfo jsonFileInfo(jsonPath);
    QDir jsonDir = jsonFileInfo.dir();
    if (!jsonDir.exists() && !jsonDir.mkpath("."))
    {
        qCritical() << "无法创建JSON输出目录:" << jsonDir.path();
        return;
    }

    // 构建质检失败的JSON
    QJsonObject checkItem;
    checkItem["id"] = "110";
    checkItem["name"] = QString::fromUtf8(u8"削尖角度阈值控制");
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
        qDebug() << "已生成错误JSON文件:" << jsonPath;
    }
    else
    {
        qCritical() << "无法生成错误JSON文件:" << jsonPath;
    }
}

// 辅助函数：写入日志
static void writeLogFile(const QString &logFilePath, const QString &message)
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
        else
        {
            qDebug() << "无法打开日志文件:" << logFilePath << "错误:" << file.errorString();
        }
    }
    catch (...)
    {
        qCritical() << "无法写入日志文件:" << logFilePath;
    }
}

SE_Error SharpAngleCheck(
    const char *szShpFilePath,
    const char *szXmlConfigPath,
    const char *szOutputLogPath,
    const char *szOutputErrorShpPath,
    const char *szOutputJsonPath,
    int scale)
{
    return SharpAngleCheckWithSource(szShpFilePath,szXmlConfigPath,szOutputLogPath,
        szOutputErrorShpPath,szOutputJsonPath,scale);
}

SE_Error SharpAngleCheckWithSource(
    const char *szShpFilePath,const char *szXmlConfigPath,
    const char *szOutputLogPath,const char *szOutputErrorShpPath,
    const char *szOutputJsonPath,int scale,
    const char* sourceDataPaths)
{
    // === 接口层错误检查 ===
    if (!szShpFilePath || !szXmlConfigPath || !szOutputLogPath ||
        !szOutputErrorShpPath || !szOutputJsonPath)
    {
        // 由于JSON路径也可能是空指针，无法安全地写入错误JSON，只能返回错误码
        return SE_ERROR_INVALID_PARAMETER;
    }

    const QFileInfo errorInfo(QString::fromUtf8(szOutputErrorShpPath));
    QString pointBase=errorInfo.completeBaseName();
    if(pointBase.endsWith("_errors",Qt::CaseInsensitive))pointBase.chop(7);
    const QString pointPath=errorInfo.dir().filePath(pointBase+"_error_尖角点.shp");
    QStringList outputs{QString::fromUtf8(szOutputLogPath),QString::fromUtf8(szOutputErrorShpPath),QString::fromUtf8(szOutputJsonPath),pointPath};
    QStringList inputs{QString::fromUtf8(szShpFilePath),QString::fromUtf8(szXmlConfigPath)};
    for(const auto& source:QString::fromUtf8(sourceDataPaths?sourceDataPaths:"").split(';',Qt::SkipEmptyParts))inputs<<source.section('|',0,0);
    QSet<QString> keys;
    for(const auto& p:outputs){
        const auto key=QFileInfo(p).absoluteFilePath().toCaseFolded();
        if(p.isEmpty()||keys.contains(key))return SE_ERROR_INVALID_PARAMETER;
        keys.insert(key);
        for(const auto& input:inputs){auto fi=QFileInfo(input);
            if(key==fi.absoluteFilePath().toCaseFolded()||key.startsWith(fi.absoluteFilePath().toCaseFolded()+"/")||
              (QFileInfo(p).absolutePath().compare(fi.absolutePath(),Qt::CaseInsensitive)==0&&QFileInfo(p).completeBaseName().compare(fi.completeBaseName(),Qt::CaseInsensitive)==0))return SE_ERROR_INVALID_PARAMETER;
        }
    }

    // SHP文件不存在检查
    if (!QFileInfo::exists(QString::fromUtf8(szShpFilePath)))
    {
        QString errorMsg = QString("SHP文件不存在: %1").arg(szShpFilePath);
        generateErrorJson(QString::fromUtf8(szOutputJsonPath), errorMsg);
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // XML文件不存在检查
    if (!QFileInfo::exists(QString::fromUtf8(szXmlConfigPath)))
    {
        QString errorMsg = QString("XML配置文件不存在: %1").arg(szXmlConfigPath);
        generateErrorJson(QString::fromUtf8(szOutputJsonPath), errorMsg);
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 创建日志目录
    QFileInfo logFileInfo(QString::fromUtf8(szOutputLogPath));
    QDir logDir = logFileInfo.dir();
    if (!logDir.exists() && !logDir.mkpath("."))
    {
        QString errorMsg = QString("无法创建日志目录: %1").arg(logDir.path());
        // 尝试写入JSON，如果JSON路径也无效，则无能为力
        generateErrorJson(QString::fromUtf8(szOutputJsonPath), errorMsg);
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 写入开始日志
    writeLogFile(QString::fromUtf8(szOutputLogPath), "开始执行尖角检查接口...");

    try
    {
        auto selection=AngleCheckDialog::selectSourceFeatures(QString::fromUtf8(szShpFilePath),
            QString::fromUtf8(sourceDataPaths?sourceDataPaths:""),
            QString::fromUtf8(szXmlConfigPath),scale);
        QgsFeatureIds checkedIds;for(auto id:selection.parsedIds)checkedIds.insert(id);
        if(selection.enabled)writeLogFile(QString::fromUtf8(szOutputLogPath),QString("源属性筛选（缓冲区线长覆盖，图上%1毫米，覆盖率至少%2%）：符合条件源对象%3个，已关联源对象%4个；待检解析要素%5条，部分覆盖未达门槛跳过%6条。未表示或未匹配源对象不作为错误。").arg(AngleCheckDialog::SOURCE_BUFFER_MM,0,'g',12).arg(AngleCheckDialog::MIN_SOURCE_COVERAGE*100,0,'g',12).arg(selection.selectedSources).arg(selection.matchedSources).arg(checkedIds.size()).arg(selection.ambiguousParsed));
        // 获取图层名
        QString layerName = QFileInfo(QString::fromUtf8(szShpFilePath)).baseName();

        // 加载图层
        QgsVectorLayer *inputLayer = new QgsVectorLayer(
            QString::fromUtf8(szShpFilePath),
            layerName,
            "ogr");

        if (!inputLayer || !inputLayer->isValid())
        {
            writeLogFile(QString::fromUtf8(szOutputLogPath), "无法加载矢量图层或图层无效");
            if (inputLayer)
                delete inputLayer;

            QString errorMsg = QString("无法加载SHP文件: %1").arg(szShpFilePath);
            generateErrorJson(QString::fromUtf8(szOutputJsonPath), errorMsg);
            return SE_ERROR_FILE_NOT_FOUND;
        }

        writeLogFile(QString::fromUtf8(szOutputLogPath),
                     QString("图层加载成功，要素数量: %1").arg(inputLayer->featureCount()));

        // 执行检查
        bool success = AngleCheckDialog::executeSharpAngleCheckStatic(
            inputLayer,
            QString::fromUtf8(szXmlConfigPath),
            QString::fromUtf8(szOutputLogPath),
            QString::fromUtf8(szOutputErrorShpPath),
            QString::fromUtf8(szOutputJsonPath),
            scale,
            nullptr,selection.enabled?&checkedIds:nullptr,pointPath);

        writeLogFile(QString::fromUtf8(szOutputLogPath),
                     QString("尖角检查完成，结果: %1").arg(success ? "成功" : "失败"));

        delete inputLayer;
        return success?SE_ERROR_NONE:SE_ERROR_FAILURE;
    }
    catch (const std::exception &e)
    {
        writeLogFile(QString::fromUtf8(szOutputLogPath), QString("接口执行异常: %1").arg(e.what()));
        QString errorMsg = QString("程序异常: %1").arg(e.what());
        generateErrorJson(QString::fromUtf8(szOutputJsonPath), errorMsg);
        return SE_ERROR_FAILURE;
    }
    catch (...)
    {
        writeLogFile(QString::fromUtf8(szOutputLogPath), "接口执行未知异常");
        generateErrorJson(QString::fromUtf8(szOutputJsonPath), "未知异常");
        return SE_ERROR_FAILURE;
    }
}
