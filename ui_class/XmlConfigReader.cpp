#include "XmlConfigReader.h"
#include <QFile>
#include <QDomElement>
#include <QDebug>
#include <QTextCodec>
#include <QRegularExpression>
#include <QStringList>

static const int DEFAULT_SCALE_KEY = 0; // 通用规则使用 key = 0
XmlConfigReader::XmlConfigReader(QObject *parent)
    : QObject(parent), m_isLoaded(false)
{
}

XmlConfigReader::~XmlConfigReader()
{
}

bool XmlConfigReader::loadConfig(const QString &xmlFilePath)
{
    QFile file(xmlFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qWarning() << "无法打开XML配置文件:" << xmlFilePath;
        return false;
    }

    QString errorMsg;
    int errorLine, errorColumn;

    if (!m_doc.setContent(&file, true, &errorMsg, &errorLine, &errorColumn))
    {
        qWarning() << "XML解析错误:" << errorMsg
                   << "行:" << errorLine << "列:" << errorColumn;
        file.close();
        return false;
    }
    file.close();

    m_scaleConfigs.clear();

    QDomElement root = m_doc.documentElement();
    if (root.tagName() != "check_list")
    {
        qWarning() << "根元素不是check_list";
        return false;
    }

    m_isLoaded = parseCheckList(root);
    return m_isLoaded;
}

bool XmlConfigReader::parseCheckList(const QDomElement &checkListElement)
{

    QDomNodeList scaleNodes = checkListElement.elementsByTagName("scale");
    qDebug() << "找到scale元素数量:" << scaleNodes.count();

    for (int i = 0; i < scaleNodes.count(); ++i)
    {
        QDomElement scaleElement = scaleNodes.at(i).toElement();
        if (!scaleElement.isNull())
        {
            if (!parseScale(scaleElement))
            {
                qWarning() << "解析scale元素失败";
                return false;
            }
        }
    }
    return true;
}

bool XmlConfigReader::parseScale(const QDomElement &scaleElement)
{
    // 获取比例尺值
    QDomElement valueElement = scaleElement.firstChildElement("value");
    if (valueElement.isNull())
    {
        qWarning() << "scale元素缺少value子元素";
        // 如果不需要严格要求，可以允许空 value 作为通用规则（DEFAULT_SCALE_KEY）
        // return false;
    }

    QString valueText;
    if (!valueElement.isNull())
    {
        valueText = valueElement.text().trimmed();
    }

    int scaleValue = DEFAULT_SCALE_KEY;
    if (!valueText.isEmpty())
    {
        bool ok;
        scaleValue = valueText.toInt(&ok);
        if (!ok)
        {
            qWarning() << "比例尺值不是有效的整数:" << valueElement.text();
            return false;
        }
    }
    else
    {
        // 空字符串视为通用规则（默认比例尺）
        qDebug() << "检测到空比例尺 value，作为通用规则(默认比例尺)";
        scaleValue = DEFAULT_SCALE_KEY;
    }

    qDebug() << "解析比例尺:" << scaleValue;

    // 初始化该比例尺的配置映射
    QMap<QString, QMap<QString, QVariant>> scaleItems;

    // 遍历该比例尺下的所有check_item
    QDomNodeList itemNodes = scaleElement.elementsByTagName("check_item");
    qDebug() << "找到check_item元素数量:" << itemNodes.count();

    for (int i = 0; i < itemNodes.count(); ++i)
    {
        QDomElement itemElement = itemNodes.at(i).toElement();
        if (!itemElement.isNull())
        {
            QMap<QString, QVariant> itemParams;
            if (parseCheckItem(itemElement, itemParams))
            {
                // 获取检查项名称
                QDomElement nameElement = itemElement.firstChildElement("name");
                if (!nameElement.isNull())
                {
                    QString itemName = nameElement.text();

                    // 编码诊断和修复
                    qDebug() << "原始检查项名称:" << itemName;

                    // 如果是乱码，尝试修复
                    if (itemName.contains("å³ç³»æ£æ¥"))
                    {
                        qDebug() << "检测到乱码，修复为'关系检查'";
                        itemName = QString::fromUtf8(u8"关系检查");
                    }

                    scaleItems[itemName] = itemParams;

                    qDebug() << "加载检查项 - 比例尺:" << scaleValue
                             << "名称:" << itemName
                             << "参数数量:" << itemParams.keys().size();
                }
            }
        }
    }

    // 如果该比例尺已存在（例如有重复 scale 元素），合并条目而不是覆盖（更安全）
    if (m_scaleConfigs.contains(scaleValue))
    {
        QMap<QString, QMap<QString, QVariant>> &existing = m_scaleConfigs[scaleValue];
        for (auto it = scaleItems.begin(); it != scaleItems.end(); ++it)
        {
            existing[it.key()] = it.value();
        }
    }
    else
    {
        // 存储该比例尺的所有检查项
        m_scaleConfigs[scaleValue] = scaleItems;
    }
    return true;
}

bool XmlConfigReader::parseCheckItem(const QDomElement &itemElement, QMap<QString, QVariant> &params)
{
    // 获取检查项ID
    QDomElement idElement = itemElement.firstChildElement("id");
    if (!idElement.isNull())
    {
        params["id"] = idElement.text();
    }

    // 获取检查项名称
    QDomElement nameElement = itemElement.firstChildElement("name");
    if (!nameElement.isNull())
    {
        QString itemName = nameElement.text();
        params["name"] = itemName;

        // 编码诊断
        qDebug() << "解析检查项名称:" << itemName;
        QByteArray nameBytes = itemName.toUtf8();
        QString hexBytes;
        for (int i = 0; i < nameBytes.size(); ++i)
        {
            hexBytes += QString("%1 ").arg((quint8)nameBytes[i], 2, 16, QChar('0'));
        }
        qDebug() << "名称原始字节:" << hexBytes;
    }

    // 解析所有规则
    QDomNodeList ruleNodes = itemElement.elementsByTagName("rule");
    for (int i = 0; i < ruleNodes.count(); ++i)
    {
        QDomElement ruleElement = ruleNodes.at(i).toElement();
        if (ruleElement.isNull())
            continue;

        QDomElement ruleNameElement = ruleElement.firstChildElement("name");
        QDomElement ruleValueElement = ruleElement.firstChildElement("value");

        if (ruleNameElement.isNull() || ruleValueElement.isNull())
        {
            continue;
        }

        QString ruleName = ruleNameElement.text().trimmed();
        QString ruleValueStr = ruleValueElement.text().trimmed();

        qDebug() << "解析规则 - 名称:" << ruleName << "值:" << ruleValueStr;

        bool ok;
        double ruleValue = ruleValueStr.toDouble(&ok);
        if (ok)
        {
            params[ruleName] = ruleValue;
        }
        else if (ruleValueStr.compare("true", Qt::CaseInsensitive) == 0 || ruleValueStr.compare("false", Qt::CaseInsensitive) == 0)
        {
            params[ruleName] = (ruleValueStr.compare("true", Qt::CaseInsensitive) == 0);
        }
        else
        {
            params[ruleName] = ruleValueStr;
        }

        QDomElement descElement = ruleElement.firstChildElement("description");
        if (!descElement.isNull())
        {
            params[ruleName + "_description"] = descElement.text().trimmed();
        }

        // 如果有单位，也存储
        QDomElement unitElement = ruleElement.firstChildElement("unit");
        if (!unitElement.isNull())
        {
            params[ruleName + "_unit"] = unitElement.text();
        }
    }

    return true;
}

QList<int> XmlConfigReader::getAvailableScales() const
{
    QList<int> keys = m_scaleConfigs.keys();
    // 过滤掉 DEFAULT_SCALE_KEY（通用规则）防止向外暴露 0
    QList<int> filtered;
    for (int k : keys)
    {
        if (k != DEFAULT_SCALE_KEY)
            filtered.append(k);
    }
    return filtered;
}

bool XmlConfigReader::hasScale(int scale) const
{
    // 如果没有该 scale 的专属规则，但存在默认（通用）规则，也认为有效（兼容处理）
    return m_scaleConfigs.contains(scale) || m_scaleConfigs.contains(DEFAULT_SCALE_KEY);
}

QList<QString> XmlConfigReader::getCheckItemNames(int scale) const
{
    QSet<QString> names;
    if (m_scaleConfigs.contains(scale))
    {
        for (const QString &n : m_scaleConfigs[scale].keys())
            names.insert(n);
    }
    // 始终合并通用规则中的检查项到返回结果（以支持回退）
    if (m_scaleConfigs.contains(DEFAULT_SCALE_KEY))
    {
        for (const QString &n : m_scaleConfigs[DEFAULT_SCALE_KEY].keys())
            names.insert(n);
    }
    return names.values();
}

bool XmlConfigReader::isCheckItemAvailable(int scale, const QString &itemName) const
{
    // 与 hasCheckItem 的逻辑保持一致：当 scale 指定项不存在但存在默认通用规则时也返回 true
    return hasCheckItem(scale, itemName);
}

QMap<QString, QVariant> XmlConfigReader::getCheckItemParams(int scale, const QString &itemName) const
{
    // 首先查找指定比例尺下的项
    if (m_scaleConfigs.contains(scale))
    {
        const QMap<QString, QMap<QString, QVariant>> &scaleItems = m_scaleConfigs[scale];
        if (scaleItems.contains(itemName))
        {
            return scaleItems[itemName];
        }
    }

    // 回退到默认比例尺（通用规则）
    if (m_scaleConfigs.contains(DEFAULT_SCALE_KEY))
    {
        const QMap<QString, QMap<QString, QVariant>> &defaultItems = m_scaleConfigs[DEFAULT_SCALE_KEY];
        if (defaultItems.contains(itemName))
        {
            return defaultItems[itemName];
        }
    }

    return QMap<QString, QVariant>();
}

double XmlConfigReader::getRuleValue(int scale, const QString &itemName, const QString &ruleName, double defaultValue) const
{
    QMap<QString, QVariant> params = getCheckItemParams(scale, itemName);
    if (params.contains(ruleName))
    {
        QVariant value = params[ruleName];
        if (value.canConvert<double>())
        {
            return value.toDouble();
        }
    }
    return defaultValue;
}

QString XmlConfigReader::getRuleStringValue(int scale, const QString &itemName, const QString &ruleName, const QString &defaultValue) const
{
    QMap<QString, QVariant> params = getCheckItemParams(scale, itemName);
    if (params.contains(ruleName))
    {
        return params[ruleName].toString();
    }
    return defaultValue;
}

QMap<QString, QVariant> XmlConfigReader::getCheckItemParams(const QString &itemName)
{
    for (const auto &scaleItems : m_scaleConfigs)
    {
        if (scaleItems.contains(itemName))
        {
            return scaleItems[itemName];
        }
    }
    return QMap<QString, QVariant>();
}

QList<QString> XmlConfigReader::getCheckItemNames() const
{
    QList<QString> allNames;
    QSet<QString> nameSet;

    for (const auto &scaleItems : m_scaleConfigs)
    {
        for (const auto &itemName : scaleItems.keys())
        {
            nameSet.insert(itemName);
        }
    }

    allNames = nameSet.values();
    return allNames;
}

double XmlConfigReader::getRuleValue(const QString &itemName, const QString &ruleName, double defaultValue)
{
    for (const auto &scaleItems : m_scaleConfigs)
    {
        if (scaleItems.contains(itemName))
        {
            QMap<QString, QVariant> params = scaleItems[itemName];
            if (params.contains(ruleName))
            {
                QVariant value = params[ruleName];
                if (value.canConvert<double>())
                {
                    return value.toDouble();
                }
            }
        }
    }
    return defaultValue;
}

QString XmlConfigReader::getRuleStringValue(const QString &itemName, const QString &ruleName, const QString &defaultValue)
{
    for (const auto &scaleItems : m_scaleConfigs)
    {
        if (scaleItems.contains(itemName))
        {
            QMap<QString, QVariant> params = scaleItems[itemName];
            if (params.contains(ruleName))
            {
                return params[ruleName].toString();
            }
        }
    }
    return defaultValue;
}

bool XmlConfigReader::getRuleBoolValue(int scale, const QString &itemName, const QString &ruleName, bool defaultValue) const
{
    const QVariant value = getCheckItemParams(scale, itemName).value(ruleName);
    if (!value.isValid()) return defaultValue;
    if (value.type() == QVariant::Bool) return value.toBool();
    const QString text = value.toString().trimmed().toLower();
    if (text == "true" || text == "1" || text == QStringLiteral("是")) return true;
    if (text == "false" || text == "0" || text == QStringLiteral("否")) return false;
    return defaultValue;
}

QString XmlConfigReader::getRuleUnit(int scale, const QString &itemName, const QString &ruleName, const QString &defaultValue) const
{
    if (!m_scaleConfigs.contains(scale) || !m_scaleConfigs[scale].contains(itemName)) return defaultValue;
    return m_scaleConfigs[scale][itemName].value(ruleName + "_unit", defaultValue).toString();
}

QStringList XmlConfigReader::getRuleStringList(int scale, const QString &itemName, const QString &ruleName) const
{
    return getRuleStringValue(scale, itemName, ruleName, QString()).split(QRegularExpression(QStringLiteral("[;,；，、]+")), Qt::SkipEmptyParts);
}

bool XmlConfigReader::hasCheckItem(int scale, const QString &itemName) const
{
    if (m_scaleConfigs.contains(scale) && m_scaleConfigs[scale].contains(itemName)) return true;
    if (m_scaleConfigs.contains(DEFAULT_SCALE_KEY) && m_scaleConfigs[DEFAULT_SCALE_KEY].contains(itemName)) return true;
    return false;
}