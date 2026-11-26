#include "ScaleCheckInterface.h"
#include "ScaleChecker.h"

// QGIS
#include "qgsmapcanvas.h"
#include "qgsvectorlayer.h"

// Qt
#include <QDebug>
#include <QFile>
#include <QDomDocument>

// ---------------- 静态成员初始化 ----------------
QString ScaleCheckInterface::m_xmlFilePath = QString();
QMap<QString, QString> ScaleCheckInterface::m_rules;

// ---------------- 检查比例尺 ----------------
bool ScaleCheckInterface::checkScale(QgsMapCanvas* canvas, double expectScale, QString& errMsg)
{
    if (!canvas) {
        errMsg = QObject::tr("无效的地图画布对象");
        return false;
    }
    return ScaleChecker::checkScale(canvas, expectScale, errMsg);
}

// ---------------- 加载 XML 规则 ----------------
bool ScaleCheckInterface::loadRules(const QString& xmlPath, QString& errMsg)
{
    if (xmlPath.isEmpty()) {
        errMsg = QObject::tr("XML 文件路径为空");
        return false;
    }

    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly)) {
        errMsg = QObject::tr("无法打开 XML 文件：%1").arg(xmlPath);
        return false;
    }

    QDomDocument doc;
    if (!doc.setContent(&file)) {
        file.close();
        errMsg = QObject::tr("XML 文件格式错误");
        return false;
    }

    file.close();

    // 解析比例尺或其他参数，可按需要扩展
    // 简单示例：读取 <Scale> 标签
    QDomNodeList scaleNodes = doc.elementsByTagName("Scale");
    if (!scaleNodes.isEmpty()) {
        QString scaleStr = scaleNodes.at(0).toElement().text().trimmed();
        m_rules["scale"] = scaleStr;
    }

    m_xmlFilePath = xmlPath;
    errMsg.clear();
    qDebug() << "规则文件加载成功：" << xmlPath;
    return true;
}

// ---------------- 检查投影参数 ----------------
bool ScaleCheckInterface::checkProjection(QgsVectorLayer* layer, QString& errMsg)
{
    if (!layer || !layer->isValid()) {
        errMsg = QObject::tr("图层无效或未加载");
        return false;
    }

    // 直接使用 ScaleChecker 的核心函数
    ScaleChecker checker; // 临时对象，不显示 UI
    return checker.checkProjection(layer, errMsg);
}

// ---------------- 获取当前规则 ----------------
QMap<QString, QString> ScaleCheckInterface::currentRules()
{
    return m_rules;
}
