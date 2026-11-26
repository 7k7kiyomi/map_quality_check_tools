#include "IFeaturePointToleranceChecker.h"

#include <QDomDocument>
#include <QFile>
#include <QTextStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QDir>
#include <QtMath>
#include <QDebug>

#include "ogrsf_frmts.h"

// ================================================================
// @class FeaturePointToleranceCheckerImpl
// @brief IFeaturePointToleranceChecker 实现类
// ================================================================
class FeaturePointToleranceCheckerImpl : public IFeaturePointToleranceChecker
{
public:
    FeaturePointToleranceCheckerImpl() { OGRRegisterAll(); }
    ~FeaturePointToleranceCheckerImpl() override = default;

    bool runCheck(const QString& xmlRulePath,
        const QString& baseShpPath,
        const QString& genShpPath,
        const QString& outputJson) override;

    bool exportErrorShapefile(const QString& outputShpPath) override;

    QVector<CheckResult> getResults() const override { return mResults; }
    QStringList getLog() const override { return mLogs; }

private:
    bool loadXmlRule(const QString& path, double& outThresholdMM, double& outScale);
    bool loadShpPoints(const QString& shpPath, QVector<QPointF>& outPoints);
    double calcDistance(const QPointF& a, const QPointF& b) const;
    void appendLog(const QString& msg);

private:
    QVector<CheckResult> mResults;
    QStringList mLogs;
    mutable QMutex mMutex;
};

// ================================================================
// XML 解析：读取限差阈值与比例尺
// ================================================================
bool FeaturePointToleranceCheckerImpl::loadXmlRule(const QString& path, double& outThresholdMM, double& outScale)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        appendLog(QStringLiteral("无法打开规则文件：%1").arg(path));
        return false;
    }

    QDomDocument doc;
    if (!doc.setContent(&file)) {
        appendLog(QStringLiteral("XML 规则文件格式错误"));
        return false;
    }

    QDomElement root = doc.documentElement();
    QDomElement rule = root.firstChildElement("Rule");
    outThresholdMM = rule.attribute("ThresholdMM", "0").toDouble();
    outScale = rule.attribute("Scale", "10000").toDouble();

    appendLog(QStringLiteral("读取规则成功：限差=%1mm, 比例尺=1:%2").arg(outThresholdMM).arg(outScale));
    return true;
}

// ================================================================
// SHP 文件读取：提取所有点要素
// ================================================================
bool FeaturePointToleranceCheckerImpl::loadShpPoints(const QString& shpPath, QVector<QPointF>& outPoints)
{
    GDALDataset* poDS = (GDALDataset*)GDALOpenEx(shpPath.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    if (!poDS) {
        appendLog(QStringLiteral("无法打开 SHP 文件：%1").arg(shpPath));
        return false;
    }

    OGRLayer* poLayer = poDS->GetLayer(0);
    if (!poLayer) {
        appendLog(QStringLiteral("无法读取图层：%1").arg(shpPath));
        GDALClose(poDS);
        return false;
    }

    OGRFeature* poFeature = nullptr;
    poLayer->ResetReading();
    while ((poFeature = poLayer->GetNextFeature()) != nullptr) {
        OGRGeometry* geom = poFeature->GetGeometryRef();
        if (geom && wkbFlatten(geom->getGeometryType()) == wkbPoint) {
            OGRPoint* pt = (OGRPoint*)geom;
            outPoints.append(QPointF(pt->getX(), pt->getY()));
        }
        OGRFeature::DestroyFeature(poFeature);
    }

    appendLog(QStringLiteral("读取点数量：%1").arg(outPoints.size()));
    GDALClose(poDS);
    return true;
}

// ================================================================
// 计算欧氏距离（米）
// ================================================================
double FeaturePointToleranceCheckerImpl::calcDistance(const QPointF& a, const QPointF& b) const
{
    double dx = a.x() - b.x();
    double dy = a.y() - b.y();
    return qSqrt(dx * dx + dy * dy);
}

// ================================================================
// 执行主检查流程
// ================================================================
bool FeaturePointToleranceCheckerImpl::runCheck(const QString& xmlRulePath,
    const QString& baseShpPath,
    const QString& genShpPath,
    const QString& outputJson)
{
    mResults.clear();
    mLogs.clear();

    appendLog("开始执行特征点限差检查...");

    double thresholdMM = 0.0;
    double scale = 10000.0;
    if (!loadXmlRule(xmlRulePath, thresholdMM, scale))
        return false;

    QVector<QPointF> basePts, genPts;
    if (!loadShpPoints(baseShpPath, basePts))
        return false;
    if (!loadShpPoints(genShpPath, genPts))
        return false;

    if (basePts.size() != genPts.size()) {
        appendLog("警告：两个图层点数量不一致，将按较小数量配对。");
    }

    int n = qMin(basePts.size(), genPts.size());
    double thresholdMap = thresholdMM * scale / 1000.0; // mm 转为地图距离（米）

    for (int i = 0; i < n; ++i) {
        double dist = calcDistance(basePts[i], genPts[i]);
        CheckResult res;
        res.featureId = QString::number(i + 1);
        res.x = genPts[i].x();
        res.y = genPts[i].y();
        res.distance = dist;
        res.status = (dist <= thresholdMap) ? "合格" : "不合格";
        mResults.append(res);
    }

    // 输出 JSON 报告
    QJsonObject rootObj;
    QJsonArray arr;
    for (const auto& r : mResults) {
        QJsonObject obj;
        obj["FeatureID"] = r.featureId;
        obj["X"] = r.x;
        obj["Y"] = r.y;
        obj["Distance"] = r.distance;
        obj["Status"] = r.status;
        arr.append(obj);
    }
    rootObj["Results"] = arr;
    rootObj["RuleFile"] = xmlRulePath;
    rootObj["BaseFile"] = baseShpPath;
    rootObj["GenFile"] = genShpPath;
    rootObj["ThresholdMM"] = thresholdMM;
    rootObj["Scale"] = scale;
    rootObj["CreateTime"] = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");

    QFile f(outputJson);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(QJsonDocument(rootObj).toJson(QJsonDocument::Indented));
        f.close();
        appendLog(QStringLiteral("结果报告已输出：%1").arg(outputJson));
    }
    else {
        appendLog(QStringLiteral("无法写入 JSON 报告：%1").arg(outputJson));
        return false;
    }

    appendLog(QStringLiteral("检查完成，共处理 %1 个点").arg(n));
    return true;
}

// ================================================================
// 导出错误点 SHP
// ================================================================
bool FeaturePointToleranceCheckerImpl::exportErrorShapefile(const QString& outputShpPath)
{
    if (mResults.isEmpty()) {
        appendLog("无检查结果，无法导出错误点。");
        return false;
    }

    const char* driverName = "ESRI Shapefile";
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName(driverName);
    if (!poDriver) {
        appendLog("未找到 Shapefile 驱动。");
        return false;
    }

    GDALDataset* poDS = poDriver->Create(outputShpPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!poDS) {
        appendLog(QStringLiteral("无法创建 SHP 文件：%1").arg(outputShpPath));
        return false;
    }

    OGRLayer* poLayer = poDS->CreateLayer("error_points", nullptr, wkbPoint, nullptr);
    if (!poLayer) {
        appendLog("创建图层失败。");
        GDALClose(poDS);
        return false;
    }

    OGRFieldDefn fieldId("FeatureID", OFTString);
    OGRFieldDefn fieldDist("Distance", OFTReal);
    poLayer->CreateField(&fieldId);
    poLayer->CreateField(&fieldDist);

    for (const auto& r : mResults) {
        if (r.status == "合格")
            continue;

        OGRFeature* poFeature = OGRFeature::CreateFeature(poLayer->GetLayerDefn());
        poFeature->SetField("FeatureID", r.featureId.toUtf8().constData());
        poFeature->SetField("Distance", r.distance);

        OGRPoint pt(r.x, r.y);
        poFeature->SetGeometry(&pt);
        if (poLayer->CreateFeature(poFeature) != OGRERR_NONE) {
            appendLog(QStringLiteral("写入错误点失败：%1").arg(r.featureId));
        }
        OGRFeature::DestroyFeature(poFeature);
    }

    GDALClose(poDS);
    appendLog(QStringLiteral("错误点已导出：%1").arg(outputShpPath));
    return true;
}

// ================================================================
// 日志记录
// ================================================================
void FeaturePointToleranceCheckerImpl::appendLog(const QString& msg)
{
    QMutexLocker locker(&mMutex);
    QString timestamp = QDateTime::currentDateTime().toString("[hh:mm:ss] ");
    mLogs.append(timestamp + msg);
}

// ================================================================
// 工厂函数：创建检查器实例
// ================================================================
IFeaturePointToleranceChecker* createFeaturePointToleranceChecker()
{
    return new FeaturePointToleranceCheckerImpl();
}
