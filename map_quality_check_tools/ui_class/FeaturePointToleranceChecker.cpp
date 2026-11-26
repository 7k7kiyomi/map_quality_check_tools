#define _HAS_STD_BYTE 0
#include "FeaturePointToleranceChecker.h"

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

#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"

#include <cpl_string.h>
#include <cpl_conv.h>
#include <limits>
#include <cmath>
#include <unordered_map>
#include <algorithm>

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
// 自动判断输入字节是否为 UTF-8；否则按本地编码解码
static bool isValidUtf8(const char* s)
{
    if (!s) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    while (*p)
    {
        if (*p < 0x80) { ++p; continue; }
        int extra = 0;
        if ((*p & 0xE0) == 0xC0) extra = 1;
        else if ((*p & 0xF0) == 0xE0) extra = 2;
        else if ((*p & 0xF8) == 0xF0) extra = 3;
        else return false;
        ++p;
        for (int i = 0; i < extra; ++i)
        {
            if ((p[i] & 0xC0) != 0x80) return false;
        }
        p += extra;
    }
    return true;
}
static QString fromAutoToQString(const char* bytes)
{
    if (!bytes) return QString();
    return isValidUtf8(bytes) ? QString::fromUtf8(bytes) : QString::fromLocal8Bit(bytes);
}

static std::string fileNameWithoutExt(const std::string& fullPath)
{
    QString q = fromUtf8ToQString(fullPath.c_str());
    int lastSlash = q.lastIndexOf('/');
    if (lastSlash == -1) lastSlash = q.lastIndexOf('\\');
    if (lastSlash != -1) q = q.mid(lastSlash + 1);
    int lastDot = q.lastIndexOf('.');
    if (lastDot != -1) q = q.left(lastDot);
    return fromQStringToUtf8(q);
}

FeaturePointToleranceParams ParseXmlForFeaturePointToleranceParams(const std::string& xmlFilePath,
                                                                  int scale,
                                                                  std::shared_ptr<spdlog::logger> logger)
{
    FeaturePointToleranceParams params;
    params.dMapThresholdMM = 0.0;
    // 图层名称与要素缺失检查一致：取XML文件名（不含扩展名）
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
        // 与要素缺失检查保持一致：非严格要求根标签，但给出提示
        logger->warn("XML根元素不是check_list，继续按兼容模式解析");
    }

    auto parseThresholds = [&](const QDomElement& checkItem)->bool{
        QDomNodeList ruleNodes = checkItem.elementsByTagName("rule");
        bool gotAny = false;
        for (int k = 0; k < ruleNodes.count(); ++k)
        {
            QDomElement ruleElement = ruleNodes.at(k).toElement();
            QString rname = ruleElement.firstChildElement("name").text().trimmed();
            QString rvalue = ruleElement.firstChildElement("value").text().trimmed();
            if (rname.isEmpty() || rvalue.isEmpty()) continue;
            bool ok = false; double dv = rvalue.toDouble(&ok); if (!ok) continue;
            if (rname == QStringLiteral("特征点限差控制阈值") || rname == QStringLiteral("距离阈值") || rname == QStringLiteral("ThresholdMM") || rname == QStringLiteral("ToleranceMm") || rname == QStringLiteral("mm")) { params.dMapThresholdMM = dv; gotAny = true; logger->info("解析到距离阈值：{} 毫米", dv); }
            else if (rname == QStringLiteral("角度阈值") || rname == QStringLiteral("AngleThreshold") ) { params.angleThresholdDeg = dv; gotAny = true; logger->info("解析到角度阈值：{} 度", dv); }
            else if (rname == QStringLiteral("最小有效边长") || rname == QStringLiteral("MinEdgeLenMM")) { params.minEdgeLenMM = dv; gotAny = true; logger->info("解析到最小有效边长：{} 毫米", dv); }
            else if (rname == QStringLiteral("特征角容差") || rname == QStringLiteral("FeatureAngleTolerance") ) { params.featureAngleToleranceDeg = dv; gotAny = true; logger->info("解析到特征角容差：{} 度", dv); }
        }
        return gotAny;
    };

    QDomNodeList checkItems = root.elementsByTagName("check_item");
    for (int j = 0; j < checkItems.count(); ++j)
    {
        QDomElement checkItem = checkItems.at(j).toElement();
        QString name = checkItem.firstChildElement("name").text().trimmed();
        if (name == QStringLiteral("特征点限差控制"))
        {
            if (parseThresholds(checkItem)) { params.bValid = true; return params; }
        }
    }

    QDomNodeList scaleNodes = root.elementsByTagName("scale");
    bool foundScale = false;

    for (int i = 0; i < scaleNodes.count(); ++i)
    {
        QDomElement scaleElement = scaleNodes.at(i).toElement();
        QDomNodeList valueNodes = scaleElement.elementsByTagName("value");
        if (valueNodes.count() == 0) continue;

        bool ok = false;
        int xmlScale = valueNodes.at(0).toElement().text().toInt(&ok);
        if (!ok) continue;

        if (xmlScale == scale)
        {
            foundScale = true;
            logger->info("按比例尺查找：{}", scale);
            QDomNodeList checkItems = scaleElement.elementsByTagName("check_item");
            for (int j = 0; j < checkItems.count(); ++j)
            {
                QDomElement checkItem = checkItems.at(j).toElement();
                QDomNodeList nameNodes = checkItem.elementsByTagName("name");
                if (nameNodes.count() == 0) continue;
                QString name = nameNodes.at(0).toElement().text().trimmed();
                if (name == QStringLiteral("特征点限差控制"))
                {
                    if (parseThresholds(checkItem)) { params.bValid = true; return params; }
                }
            }
            logger->warn("在比例尺{}下未找到‘特征点限差控制’项或有效阈值", scale);
            break;
        }
    }

    if (!foundScale)
    {
        logger->warn("未找到匹配的比例尺：{}", scale);
    }

    return params;
}

// 收集Shapefile所有顶点（点/线/面/多*），输出为点集合
static bool LoadAllVerticesAsPoints(const QString& filePath,
                                    QVector<QPointF>& outPoints,
                                    std::shared_ptr<spdlog::logger> logger,
                                    bool& openedUtf8)
{
    outPoints.clear();
    openedUtf8 = true;

    if (!QFile::exists(filePath)) {
        logger->error("文件不存在: {}", fromQStringToUtf8(filePath));
        return false;
    }
    CplConfigGuard guardUtf8("GDAL_FILENAME_IS_UTF8", "NO");
    GDALAllRegister();
    QByteArray localPath = QFile::encodeName(QDir::toNativeSeparators(filePath));
    GDALDataset* ds = static_cast<GDALDataset*>(GDALOpenEx(localPath.constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (!ds) {
        // 尝试UTF-8
        CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
        QByteArray utf8Path = filePath.toUtf8();
        ds = static_cast<GDALDataset*>(GDALOpenEx(utf8Path.constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
        openedUtf8 = true;
        if (!ds) {
            logger->error("GDAL无法打开文件（本地/UTF-8均失败）：{}", fromQStringToUtf8(filePath));
            return false;
        }
    } else {
        openedUtf8 = false;
    }

    OGRLayer* layer = ds->GetLayer(0);
    if (!layer) {
        logger->error("无法读取图层：{}", fromQStringToUtf8(filePath));
        GDALClose(ds);
        return false;
    }

    layer->ResetReading();
    OGRFeature* feat = nullptr;
    int totalPoints = 0;

    auto addLineString = [&](OGRLineString* ls) {
        if (!ls) return;
        int n = ls->getNumPoints();
        for (int ip = 0; ip < n; ++ip) {
            outPoints.append(QPointF(ls->getX(ip), ls->getY(ip)));
            totalPoints++;
        }
    };

    auto addPolygon = [&](OGRPolygon* poly) {
        if (!poly) return;
        OGRLinearRing* ext = poly->getExteriorRing();
        if (ext) {
            for (int ip = 0; ip < ext->getNumPoints(); ++ip) {
                outPoints.append(QPointF(ext->getX(ip), ext->getY(ip)));
                totalPoints++;
            }
        }
        int innerCount = poly->getNumInteriorRings();
        for (int ii = 0; ii < innerCount; ++ii) {
            OGRLinearRing* ir = poly->getInteriorRing(ii);
            if (ir) {
                for (int ip = 0; ip < ir->getNumPoints(); ++ip) {
                    outPoints.append(QPointF(ir->getX(ip), ir->getY(ip)));
                    totalPoints++;
                }
            }
        }
    };

    while ((feat = layer->GetNextFeature()) != nullptr) {
        OGRGeometry* geom = feat->GetGeometryRef();
        if (!geom) {
            OGRFeature::DestroyFeature(feat);
            continue;
        }
        OGRwkbGeometryType gtype = wkbFlatten(geom->getGeometryType());
        if (gtype == wkbPoint) {
            OGRPoint* p = geom->toPoint();
            if (p) { outPoints.append(QPointF(p->getX(), p->getY())); totalPoints++; }
        }
        else if (gtype == wkbLineString) { addLineString(geom->toLineString()); }
        else if (gtype == wkbPolygon) { addPolygon(geom->toPolygon()); }
        else if (gtype == wkbMultiLineString) {
            OGRGeometryCollection* gc = geom->toGeometryCollection();
            if (gc) {
                int ng = gc->getNumGeometries();
                for (int gi = 0; gi < ng; ++gi) {
                    OGRGeometry* sub = gc->getGeometryRef(gi);
                    if (sub && wkbFlatten(sub->getGeometryType()) == wkbLineString) addLineString(sub->toLineString());
                }
            }
        }
        else if (gtype == wkbMultiPolygon) {
            OGRGeometryCollection* gc = geom->toGeometryCollection();
            if (gc) {
                int ng = gc->getNumGeometries();
                for (int gi = 0; gi < ng; ++gi) {
                    OGRGeometry* sub = gc->getGeometryRef(gi);
                    if (sub && wkbFlatten(sub->getGeometryType()) == wkbPolygon) addPolygon(sub->toPolygon());
                }
            }
        }
        OGRFeature::DestroyFeature(feat);
    }

    GDALClose(ds);
    logger->info("读取点数量：{}", totalPoints);
    return totalPoints > 0;
}

static inline double euclideanDistanceUnits(const QPointF& a, const QPointF& b)
{
    double dx = a.x() - b.x();
    double dy = a.y() - b.y();
    return std::hypot(dx, dy);
}

// 线端点捕捉：将几何坐标按网格吸附，消除微小缝隙
static inline double snapCoord(double v, double grid)
{
    if (grid <= 0) return v;
    return std::round(v / grid) * grid;
}

static OGRGeometry* SnapGeometryToGrid(OGRGeometry* g, double gridSize)
{
    if (!g) return nullptr;
    OGRwkbGeometryType gt = wkbFlatten(g->getGeometryType());
    if (gt == wkbPoint)
    {
        OGRPoint* p = g->toPoint();
        if (!p) return nullptr;
        return new OGRPoint(snapCoord(p->getX(), gridSize), snapCoord(p->getY(), gridSize));
    }
    if (gt == wkbLineString)
    {
        OGRLineString* ls = g->toLineString(); if (!ls) return nullptr;
        OGRLineString* out = new OGRLineString();
        int n = ls->getNumPoints();
        for (int i=0;i<n;++i) out->addPoint(snapCoord(ls->getX(i), gridSize), snapCoord(ls->getY(i), gridSize));
        return out;
    }
    if (gt == wkbPolygon)
    {
        OGRPolygon* poly = g->toPolygon(); if (!poly) return nullptr;
        OGRPolygon* out = new OGRPolygon();
        auto addRing = [&](OGRLinearRing* ring){ if (!ring) return; OGRLinearRing r; int n = ring->getNumPoints(); for (int i=0;i<n;++i){ r.addPoint(snapCoord(ring->getX(i), gridSize), snapCoord(ring->getY(i), gridSize)); } out->addRing(&r); };
        addRing(poly->getExteriorRing());
        for (int k=0;k<poly->getNumInteriorRings();++k) addRing(poly->getInteriorRing(k));
        return out;
    }
    if (gt == wkbMultiLineString)
    {
        OGRGeometryCollection* gc = g->toGeometryCollection(); if (!gc) return nullptr;
        OGRMultiLineString* out = new OGRMultiLineString();
        for (int i=0;i<gc->getNumGeometries();++i){ OGRGeometry* gi = gc->getGeometryRef(i); if (!gi) continue; OGRGeometry* snapped = SnapGeometryToGrid(gi, gridSize); if (snapped && wkbFlatten(snapped->getGeometryType())==wkbLineString){ out->addGeometry(snapped); } if (snapped) delete snapped; }
        return out;
    }
    if (gt == wkbMultiPolygon)
    {
        OGRGeometryCollection* gc = g->toGeometryCollection(); if (!gc) return nullptr;
        OGRMultiPolygon* out = new OGRMultiPolygon();
        for (int i=0;i<gc->getNumGeometries();++i){ OGRGeometry* gi = gc->getGeometryRef(i); if (!gi) continue; OGRGeometry* snapped = SnapGeometryToGrid(gi, gridSize); if (snapped && wkbFlatten(snapped->getGeometryType())==wkbPolygon){ out->addGeometry(snapped); } if (snapped) delete snapped; }
        return out;
    }
    // 其他集合：尽量处理为折线
    OGRGeometry* c = g->clone();
    if (c && !c->IsValid()){ OGRGeometry* v = c->MakeValid(); if (!v) v = c->Buffer(0); if (v && !v->IsEmpty()){ delete c; c = v; } }
    return c;
}

void* PreprocessParsedData(const QStringList& parsedPaths, double snapToleranceUnits, std::shared_ptr<spdlog::logger> logger)
{
    OGRMultiLineString* ml = new OGRMultiLineString();
    for (const QString& path : parsedPaths)
    {
        QByteArray localPath = QFile::encodeName(QDir::toNativeSeparators(path));
        GDALDataset* ds = static_cast<GDALDataset*>(GDALOpenEx(localPath.constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
        if (!ds){ logger->warn("解析数据无法打开：{}", fromQStringToUtf8(path)); continue; }
        OGRLayer* lyr = ds->GetLayer(0);
        if (!lyr){ GDALClose(ds); logger->warn("解析数据图层为空：{}", fromQStringToUtf8(path)); continue; }
        lyr->ResetReading(); OGRFeature* f;
        while ((f = lyr->GetNextFeature()) != nullptr){
            OGRGeometry* g = f->GetGeometryRef();
            if (g && !g->IsEmpty()){
                OGRGeometry* used = g;
                if (!g->IsValid()){ OGRGeometry* v = g->MakeValid(); if (!v) v = g->Buffer(0); if (v && !v->IsEmpty()) used = v; }
                OGRGeometry* snapped = SnapGeometryToGrid(used, snapToleranceUnits);
                OGRGeometryCollection* gc = dynamic_cast<OGRGeometryCollection*>(snapped);
                if (gc){
                    for (int i = 0; i < gc->getNumGeometries(); ++i){
                        OGRGeometry* gi = gc->getGeometryRef(i);
                        OGRwkbGeometryType gt = wkbFlatten(gi->getGeometryType());
                        if (gt == wkbLineString){ ml->addGeometry(gi); }
                        else if (gt == wkbPolygon){
                            OGRPolygon* p = gi->toPolygon(); if (p){ OGRLinearRing* ext = p->getExteriorRing(); if (ext){ OGRLineString* ls = new OGRLineString(); ls->addSubLineString(ext, 0, ext->getNumPoints()); ml->addGeometry(ls); delete ls; } }
                        }
                    }
                } else if (snapped){
                    OGRwkbGeometryType gt = wkbFlatten(snapped->getGeometryType());
                    if (gt == wkbLineString){ ml->addGeometry(snapped); }
                    else if (gt == wkbPolygon){
                        OGRPolygon* p = snapped->toPolygon(); if (p){ OGRLinearRing* ext = p->getExteriorRing(); if (ext){ OGRLineString* ls = new OGRLineString(); ls->addSubLineString(ext, 0, ext->getNumPoints()); ml->addGeometry(ls); delete ls; } }
                    }
                }
                if (snapped) delete snapped;
                if (used != g) delete used;
            }
            OGRFeature::DestroyFeature(f);
        }
        GDALClose(ds);
    }
    if (ml->IsEmpty()){ delete ml; return nullptr; }
    return ml;
}

// 计算三点的夹角（度）：P_{i-1} -> P_i -> P_{i+1}
static double angleAt(const QPointF& prev, const QPointF& curr, const QPointF& next)
{
    double v1x = curr.x() - prev.x(); double v1y = curr.y() - prev.y();
    double v2x = next.x() - curr.x(); double v2y = next.y() - curr.y();
    double dot = v1x*v2x + v1y*v2y;
    double n1 = std::hypot(v1x, v1y); double n2 = std::hypot(v2x, v2y);
    if (n1 <= 0 || n2 <= 0) return 0.0;
    double c = std::max(-1.0, std::min(1.0, dot/(n1*n2)));
    double ang = std::acos(c) * 180.0 / M_PI;
    return ang;
}

std::vector<FeaturePointCandidate> ExtractSourceFeaturePoints(const QString& sourcePath,
                                                             double angleThresholdDeg,
                                                             double minEdgeLenUnits,
                                                             std::shared_ptr<spdlog::logger> logger)
{
    std::vector<FeaturePointCandidate> out;
    int globalOrder = 0;
    auto processPointList = [&](const QVector<QPointF>& pts, bool closed){
        int n = pts.size(); if (n < 3) return;
        auto findPrevIndex = [&](int i){ double acc = 0.0; int j = i; while (acc < minEdgeLenUnits){ int jprev = closed ? ( (j-1+n)%n ) : (j-1); if (jprev < 0) return -1; acc += euclideanDistanceUnits(pts[jprev], pts[j]); j = jprev; } return j; };
        auto findNextIndex = [&](int i){ double acc = 0.0; int k = i; while (acc < minEdgeLenUnits){ int knext = closed ? ( (k+1)%n ) : (k+1); if (knext >= n) return -1; acc += euclideanDistanceUnits(pts[k], pts[knext]); k = knext; } return k; };
        int start = closed ? 0 : 1; int end = closed ? n : n-1;
        for (int i = start; i < end; ++i){ int j = findPrevIndex(i); int k = findNextIndex(i); if (j == -1 || k == -1) continue; double ang = angleAt(pts[j], pts[i], pts[k]); if (ang >= angleThresholdDeg){ FeaturePointCandidate c; c.pos = pts[i]; c.sourceAngleDeg = ang; c.order = globalOrder++; out.push_back(c); } else { globalOrder++; } }
    };

    QByteArray localPath = QFile::encodeName(QDir::toNativeSeparators(sourcePath));
    GDALDataset* ds = static_cast<GDALDataset*>(GDALOpenEx(localPath.constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (!ds){ logger->error("无法打开库体数据：{}", fromQStringToUtf8(sourcePath)); return out; }
    OGRLayer* lyr = ds->GetLayer(0); if (!lyr){ GDALClose(ds); return out; }
    lyr->ResetReading(); OGRFeature* f;
    size_t geomCount = 0;
    while ((f = lyr->GetNextFeature()) != nullptr){
        OGRGeometry* g = f->GetGeometryRef();
        if (!g){ OGRFeature::DestroyFeature(f); continue; }
        ++geomCount;
        OGRwkbGeometryType gt = wkbFlatten(g->getGeometryType());
        if (gt == wkbLineString){
            OGRLineString* ls = g->toLineString(); if (ls){ QVector<QPointF> pts; int n = ls->getNumPoints(); for(int i=0;i<n;++i) pts.append(QPointF(ls->getX(i), ls->getY(i))); processPointList(pts, false); }
        }
        else if (gt == wkbMultiLineString){
            OGRGeometryCollection* gc = g->toGeometryCollection(); if (gc){ for(int i=0;i<gc->getNumGeometries();++i){ OGRGeometry* gi = gc->getGeometryRef(i); if (gi && wkbFlatten(gi->getGeometryType())==wkbLineString){ OGRLineString* ls = gi->toLineString(); QVector<QPointF> pts; int n = ls->getNumPoints(); for(int k=0;k<n;++k) pts.append(QPointF(ls->getX(k), ls->getY(k))); processPointList(pts, false); } } }
        }
        else if (gt == wkbPolygon){
            OGRPolygon* p = g->toPolygon(); if (p){ OGRLinearRing* ext = p->getExteriorRing(); if (ext){ QVector<QPointF> pts; int n = ext->getNumPoints(); for(int i=0;i<n;++i) pts.append(QPointF(ext->getX(i), ext->getY(i))); processPointList(pts, true); } for(int k=0;k<p->getNumInteriorRings();++k){ OGRLinearRing* ir = p->getInteriorRing(k); if (ir){ QVector<QPointF> pts2; int n2 = ir->getNumPoints(); for(int i=0;i<n2;++i) pts2.append(QPointF(ir->getX(i), ir->getY(i))); processPointList(pts2, true); } } }
        }
        else if (gt == wkbMultiPolygon){
            OGRGeometryCollection* gc = g->toGeometryCollection(); if (gc){ for(int i=0;i<gc->getNumGeometries();++i){ OGRGeometry* gi = gc->getGeometryRef(i); if (gi && wkbFlatten(gi->getGeometryType())==wkbPolygon){ OGRPolygon* p = gi->toPolygon(); OGRLinearRing* ext = p->getExteriorRing(); if (ext){ QVector<QPointF> pts; int n = ext->getNumPoints(); for(int k=0;k<n;++k) pts.append(QPointF(ext->getX(k), ext->getY(k))); processPointList(pts, true); } for(int r=0;r<p->getNumInteriorRings();++r){ OGRLinearRing* ir = p->getInteriorRing(r); if (ir){ QVector<QPointF> pts2; int n2 = ir->getNumPoints(); for(int k=0;k<n2;++k) pts2.append(QPointF(ir->getX(k), ir->getY(k))); processPointList(pts2, true); } } } } }
        }
        OGRFeature::DestroyFeature(f);
    }
    GDALClose(ds);
    logger->info("库体几何数量：{}，提取库体特征点数量：{}", geomCount, out.size());
    return out;
}

static bool CreateErrorPointsShapefile(const QString& outPath,
                                       OGRSpatialReference* poSRS,
                                       const std::vector<QPointF>& errorPts,
                                       const std::vector<double>& errorDistancesUnits,
                                       double thresholdUnits,
                                       const std::string& layerName,
                                       std::shared_ptr<spdlog::logger> logger)
{
    // 若存在同名文件，先清理同名Shapefile集合
    QFileInfo fi(outPath);
    QString baseNoExt = fi.path() + QDir::separator() + fi.completeBaseName();
    QStringList exts = {".shp", ".shx", ".dbf", ".prj", ".cpg"};
    for (const QString& ext : exts) {
        QString p = baseNoExt + ext;
        QFile::remove(p);
    }

    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (!poDriver) { logger->error("未找到 Shapefile 驱动"); return false; }

    // 以本地编码传递输出路径给GDAL，防止中文文件名乱码
    CplConfigGuard guardUtf8("GDAL_FILENAME_IS_UTF8", "NO");
    QByteArray outNative = QFile::encodeName(QDir::toNativeSeparators(outPath));
    GDALDataset* poDSOut = poDriver->Create(outNative.constData(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!poDSOut) { logger->error("创建输出Shapefile失败：{}", fromQStringToUtf8(outPath)); return false; }

    // 明确写入 CPG 文件，指定 DBF 字符编码为 UTF-8，避免中文字段值乱码
    {
        QString cpgPath = baseNoExt + ".cpg";
        QFile cpgFile(cpgPath);
        if (cpgFile.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            // 按标准写入 "UTF-8"；不写 BOM
            cpgFile.write("UTF-8");
            cpgFile.close();
            logger->info("写入CPG编码文件：{}", fromQStringToUtf8(cpgPath));
        }
        else
        {
            logger->warn("无法写入CPG编码文件：{}", fromQStringToUtf8(cpgPath));
        }
    }

    OGRSpatialReference* poCloneSRS = poSRS ? poSRS->Clone() : nullptr;
    OGRLayer* poLayer = poDSOut->CreateLayer("error_points", poCloneSRS, wkbPoint, nullptr);
    if (!poLayer) { logger->error("创建图层失败"); GDALClose(poDSOut); if(poCloneSRS) poCloneSRS->Release(); return false; }
    if (poCloneSRS) poCloneSRS->Release();

    OGRFieldDefn fLayerName("LayerName", OFTString); fLayerName.SetWidth(80); poLayer->CreateField(&fLayerName);
    OGRFieldDefn fFeatureID("FeatureID", OFTString); fFeatureID.SetWidth(32); poLayer->CreateField(&fFeatureID);
    OGRFieldDefn fDistance("Distance", OFTReal); fDistance.SetWidth(16); fDistance.SetPrecision(6); poLayer->CreateField(&fDistance);
    OGRFieldDefn fThreshold("Threshold", OFTReal); fThreshold.SetWidth(16); fThreshold.SetPrecision(6); poLayer->CreateField(&fThreshold);
    OGRFieldDefn fDescription("Description", OFTString); fDescription.SetWidth(128); poLayer->CreateField(&fDescription);

    for (size_t i = 0; i < errorPts.size(); ++i)
    {
        OGRFeature* oFeat = OGRFeature::CreateFeature(poLayer->GetLayerDefn());
        OGRPoint pt(errorPts[i].x(), errorPts[i].y());
        oFeat->SetGeometry(&pt);
        // 以 UTF-8 写入中文字段，结合 CPG=UTF-8 防止乱码
        oFeat->SetField("LayerName", QString::fromStdString(layerName).toUtf8().constData());
        char idbuf[32]; snprintf(idbuf, sizeof(idbuf), "%zu", i);
        oFeat->SetField("FeatureID", idbuf);
        oFeat->SetField("Distance", errorDistancesUnits[i]);
        oFeat->SetField("Threshold", thresholdUnits);
        oFeat->SetField("Description", QStringLiteral("距离超出阈值").toUtf8().constData());
        if (poLayer->CreateFeature(oFeat) != OGRERR_NONE)
        {
            logger->warn("写入错误点失败：index={}", i);
        }
        OGRFeature::DestroyFeature(oFeat);
    }

    GDALClose(poDSOut);
    logger->info("已输出错误点图层：{} (数量={})", fromQStringToUtf8(outPath), errorPts.size());
    return true;
}

extern "C" MY_PLUGIN_API SE_Error FeaturePointToleranceCheck(
    const char* szSourceShpFilePath,
    const char* szParsedDataPath,
    const char* szSchemaXmlPath,
    int scale,
    const char* szOutputErrorPointsLayerPath,
    const char* szOutputJsonPath,
    const char* szOutputLogFilePath)
{
    // 创建日志器（使用本地编码路径，避免中文文件名乱码）
    QString logPathQ = fromUtf8ToQString(szOutputLogFilePath);
    QByteArray logPathLocalBytes = logPathQ.toLocal8Bit();
    std::string logPathLocal(logPathLocalBytes.constData(), logPathLocalBytes.size());
    auto logger = spdlog::basic_logger_mt("FeaturePointTolerance", logPathLocal);
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    logger->info("开始执行特征点限差控制检查");

    // 记录输入（自动识别UTF-8或本地编码）
    logger->info("库体数据: {}", fromQStringToUtf8(fromAutoToQString(szSourceShpFilePath)));
    logger->info("解析数据: {}", fromQStringToUtf8(fromAutoToQString(szParsedDataPath)));
    logger->info("XML规则: {}", fromQStringToUtf8(fromAutoToQString(szSchemaXmlPath)));
    logger->info("JSON输出: {}", fromQStringToUtf8(fromAutoToQString(szOutputJsonPath)));
    logger->info("错误点图层: {}", fromQStringToUtf8(fromAutoToQString(szOutputErrorPointsLayerPath)));

    // 解析XML（角度阈值、最小有效边长、距离阈值T、特征角容差）
    FeaturePointToleranceParams params = ParseXmlForFeaturePointToleranceParams(szSchemaXmlPath, scale, logger);
    std::string layerName = params.strLayerName;
    logger->info("图层名称(取XML文件名): {}", layerName);
    if (!params.bValid || params.dMapThresholdMM <= 0.0 || params.angleThresholdDeg <= 0.0 || params.minEdgeLenMM <= 0.0)
    {
        logger->error("XML中未解析到有效参数（距离阈值/角度阈值/最小有效边长）");
        // 写失败JSON
        QJsonObject jsonResult;
        QJsonObject checkItem;
        checkItem["id"] = "107";
        checkItem["name"] = "特征点限差控制";
        checkItem["status"] = "质检失败";
        checkItem["err_exist"] = "false";
        checkItem["info"] = QString("%1图层执行失败（XML参数解析失败）").arg(QString::fromStdString(layerName));
        checkItem["errors"] = QJsonObject();
        jsonResult["check_item"] = checkItem;
        QJsonDocument doc(jsonResult);
        QFile jsonFile(fromAutoToQString(szOutputJsonPath));
        if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) { jsonFile.write(doc.toJson()); jsonFile.close(); }
        spdlog::drop("FeaturePointTolerance");
        return SE_ERROR_FAILURE;
    }

    // GDAL配置与注册（作用域守卫确保退出后恢复原值）
    CplConfigGuard guardUtf8Yes("GDAL_FILENAME_IS_UTF8", "YES");
    CplConfigGuard guardShapeEnc("SHAPE_ENCODING", "UTF-8");
    static bool gdalInitialized = false;
    if (!gdalInitialized) { GDALAllRegister(); gdalInitialized = true; }

    // 打开库体数据，获取SRS与单位换算（按本地编码传递给GDAL）
    QString sourcePath = fromAutoToQString(szSourceShpFilePath);
    CplConfigGuard guardUtf8No("GDAL_FILENAME_IS_UTF8", "NO");
    QByteArray sourceNative = QFile::encodeName(QDir::toNativeSeparators(sourcePath));
    GDALDataset* poSourceDS = (GDALDataset*)GDALOpenEx(sourceNative.constData(), GDAL_OF_VECTOR, NULL, NULL, NULL);
    if (!poSourceDS)
    {
        logger->error("无法打开库体数据：{}", fromQStringToUtf8(sourcePath));
        // 写失败JSON
        QJsonObject jsonResult; QJsonObject checkItem;
        checkItem["id"] = "107"; checkItem["name"] = "特征点限差控制"; checkItem["status"] = "质检失败";
        checkItem["err_exist"] = "false"; checkItem["info"] = QString("%1图层执行失败（库体数据无法打开）").arg(QString::fromStdString(layerName));
        checkItem["errors"] = QJsonObject(); jsonResult["check_item"] = checkItem;
        QJsonDocument doc(jsonResult); QFile jsonFile(fromAutoToQString(szOutputJsonPath));
        if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) { jsonFile.write(doc.toJson()); jsonFile.close(); }
        spdlog::drop("FeaturePointTolerance");
        return SE_ERROR_FAILURE;
    }

    OGRLayer* poSourceLayer = poSourceDS->GetLayer(0);
    if (!poSourceLayer)
    {
        GDALClose(poSourceDS);
        logger->error("无法获取库体图层");
        QJsonObject jsonResult; QJsonObject checkItem;
        checkItem["id"] = "107"; checkItem["name"] = "特征点限差控制"; checkItem["status"] = "质检失败";
        checkItem["err_exist"] = "false"; checkItem["info"] = QString("%1图层执行失败（库体图层为空）").arg(QString::fromStdString(layerName));
        checkItem["errors"] = QJsonObject(); jsonResult["check_item"] = checkItem;
        QJsonDocument doc(jsonResult); QFile jsonFile(szOutputJsonPath);
        if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) { jsonFile.write(doc.toJson()); jsonFile.close(); }
        spdlog::drop("FeaturePointTolerance");
        return SE_ERROR_FAILURE;
    }

    OGRSpatialReference* poSRS = poSourceLayer->GetSpatialRef();
    double dLinearUnitsInMeters = 1.0;
    if (poSRS)
    {
        if (poSRS->IsGeographic())
        {
            logger->error("库体数据为地理坐标系(度)，当前实现不支持将图上毫米精确换算为度单位。请使用投影坐标系数据。");
            GDALClose(poSourceDS);
            spdlog::drop("FeaturePointTolerance");
            return SE_ERROR_UNSUPPORTED_SRS;
        }
        else
        {
            const char* pszUnitsName = nullptr;
            dLinearUnitsInMeters = poSRS->GetLinearUnits(&pszUnitsName);
            if (dLinearUnitsInMeters <= 0.0) { logger->warn("线性单位转换系数无效，按米处理"); dLinearUnitsInMeters = 1.0; }
            logger->info("库体坐标单位: {} 每单位米数: {}", pszUnitsName ? pszUnitsName : "(未知)", dLinearUnitsInMeters);
        }
    }
    else
    {
        logger->warn("库体数据缺少空间参考，默认按米处理阈值换算");
    }

    // 图上到图层单位的换算（毫米 -> 米 -> 图层单位）
    auto mmToUnits = [&](double mm){ double meters = (mm * scale) / 1000.0; return meters / dLinearUnitsInMeters; };
    double dThresholdUnits = mmToUnits(params.dMapThresholdMM);
    double minEdgeLenUnits = mmToUnits(params.minEdgeLenMM);
    logger->info("距离阈值T: 图上{}毫米 -> 图层单位{}", params.dMapThresholdMM, dThresholdUnits);
    logger->info("最小有效边长: 图上{}毫米 -> 图层单位{}", params.minEdgeLenMM, minEdgeLenUnits);

    // 第一阶段：解析后数据预处理（端点捕捉+融合）
    QString parsedPathsStr = fromUtf8ToQString(szParsedDataPath);
    QStringList parsedFilePaths = parsedPathsStr.split(";", Qt::SkipEmptyParts);
    for (int i=0;i<parsedFilePaths.size();++i) parsedFilePaths[i] = parsedFilePaths[i].trimmed();
    void* preprocessed = PreprocessParsedData(parsedFilePaths, std::max(dThresholdUnits*0.02, 0.001), logger);
    OGRGeometry* parsedGeom = static_cast<OGRGeometry*>(preprocessed);
    if (!parsedGeom){
        GDALClose(poSourceDS);
        logger->error("解析后数据预处理失败");
        QJsonObject jsonResult; QJsonObject checkItem;
        checkItem["id"] = "107"; checkItem["name"] = "特征点限差控制"; checkItem["status"] = "质检失败";
        checkItem["err_exist"] = "false"; checkItem["info"] = QString("%1图层执行失败（解析后数据预处理失败）").arg(QString::fromStdString(layerName));
        checkItem["errors"] = QJsonObject(); jsonResult["check_item"] = checkItem;
        QJsonDocument doc(jsonResult); QFile jsonFile(szOutputJsonPath);
        if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) { jsonFile.write(doc.toJson()); jsonFile.close(); }
        spdlog::drop("FeaturePointTolerance");
        return SE_ERROR_FAILURE;
    }

    // 第二阶段：库体特征点提取
    std::vector<FeaturePointCandidate> srcPts = ExtractSourceFeaturePoints(fromUtf8ToQString(szSourceShpFilePath), params.angleThresholdDeg, minEdgeLenUnits, logger);

    // 最近邻距离计算（单位：图层单位）— 使用栅格哈希索引加速
    const double EPS = 1e-9;
    size_t errorCount = 0;
    std::vector<QPointF> errorPts;
    std::vector<double> errorDistancesUnits;
    std::vector<QString> errorTypes;
    std::vector<int> errorOrders;
    errorPts.reserve(srcPts.size());
    errorDistancesUnits.reserve(srcPts.size());
    errorTypes.reserve(srcPts.size());
    errorOrders.reserve(srcPts.size());

    // 遍历特征点，构建缓冲并判定
    for (size_t i = 0; i < srcPts.size(); ++i)
    {
        const QPointF bp = srcPts[i].pos;
        OGRPoint pt(bp.x(), bp.y());
        OGRGeometry* buf = pt.Buffer(dThresholdUnits);
        if (!buf || buf->IsEmpty()) { if(buf) delete buf; continue; }
        bool intersects = false; try { intersects = parsedGeom->Intersects(buf);} catch(...) { intersects = false; }
        if (!intersects)
        {
            errorPts.push_back(bp); errorDistancesUnits.push_back(std::numeric_limits<double>::infinity()); errorTypes.push_back(QStringLiteral("特征点丢失")); errorOrders.push_back(srcPts[i].order); ++errorCount; delete buf; continue;
        }
        double dmin = std::numeric_limits<double>::infinity();
        try { dmin = parsedGeom->Distance(&pt); } catch(...) { dmin = std::numeric_limits<double>::infinity(); }
        if (dmin > dThresholdUnits + EPS)
        {
            errorPts.push_back(bp); errorDistancesUnits.push_back(dmin); errorTypes.push_back(QStringLiteral("位置超限")); errorOrders.push_back(srcPts[i].order); ++errorCount; delete buf; continue;
        }
        // 形状保留验证：简化实现—在缓冲边界与解析线求交，近似得到前后点
        OGRGeometry* inter = nullptr; try { inter = parsedGeom->Intersection(buf);} catch(...) { inter = nullptr; }
        bool shapeFail = false; double bdeg = 0.0;
        if (inter && !inter->IsEmpty()){
            // 取交集中的线或多线，选取离点最近的两点
            std::vector<QPointF> sample;
            OGRGeometryCollection* gc = dynamic_cast<OGRGeometryCollection*>(inter);
            auto addLS = [&](OGRLineString* ls){ if (!ls) return; int n = ls->getNumPoints(); if (n>=2){ sample.push_back(QPointF(ls->getX(0), ls->getY(0))); sample.push_back(QPointF(ls->getX(n-1), ls->getY(n-1))); } };
            if (gc){ for(int gi=0; gi<gc->getNumGeometries(); ++gi){ OGRGeometry* giGeom = gc->getGeometryRef(gi); if (giGeom && wkbFlatten(giGeom->getGeometryType())==wkbLineString) addLS(giGeom->toLineString()); } }
            else if (wkbFlatten(inter->getGeometryType())==wkbLineString){ addLS(inter->toLineString()); }
            if (sample.size()>=2){
                // 构造近似角度：以最近的两个采样点为 prev/next
                // 选择距离Psrc最近的两个点
                std::sort(sample.begin(), sample.end(), [&](const QPointF& a, const QPointF& b){ return euclideanDistanceUnits(a, bp) < euclideanDistanceUnits(b, bp); });
                QPointF prev = sample[0]; QPointF next = sample[1];
                bdeg = angleAt(prev, bp, next);
                double diff = std::fabs(srcPts[i].sourceAngleDeg - bdeg);
                if (params.featureAngleToleranceDeg > 0.0 && diff > params.featureAngleToleranceDeg) shapeFail = true;
            }
        }
        if (inter) delete inter;
        if (shapeFail){ errorPts.push_back(bp); errorDistancesUnits.push_back(dmin); errorTypes.push_back(QStringLiteral("特征未保留")); errorOrders.push_back(srcPts[i].order); ++errorCount; }
        delete buf;
        if ((i % 1000) == 0) { logger->info(fmt::format("已处理特征点: {} / {}", i, srcPts.size())); }
    }

    // 导出错误点图层（若有不合格点）
    if (errorCount > 0)
    {
        // 写入错误点：确保中文字段不乱码
        QString outPathQ = fromUtf8ToQString(szOutputErrorPointsLayerPath);
        QFileInfo fi(outPathQ);
        QString baseNoExt = fi.path() + QDir::separator() + fi.completeBaseName();
        QStringList exts = {".shp", ".shx", ".dbf", ".prj", ".cpg"}; for(const QString& ext: exts) QFile::remove(baseNoExt + ext);
        GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
        if (poDriver){
            CplConfigGuard guardUtf8("GDAL_FILENAME_IS_UTF8", "NO");
            CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");
            QByteArray outNative = QFile::encodeName(QDir::toNativeSeparators(outPathQ));
            GDALDataset* poDSOut = poDriver->Create(outNative.constData(), 0,0,0, GDT_Unknown, nullptr);
            if (poDSOut){
                QString cpgPath = baseNoExt + ".cpg"; QFile cpgFile(cpgPath); if (cpgFile.open(QIODevice::WriteOnly|QIODevice::Text)) { cpgFile.write("UTF-8"); cpgFile.close(); }
                OGRSpatialReference* poCloneSRS = poSRS ? poSRS->Clone() : nullptr;
                char** lco = nullptr; lco = CSLSetNameValue(lco, "ENCODING", "UTF-8");
                OGRLayer* poLayer = poDSOut->CreateLayer("error_points", poCloneSRS, wkbPoint, lco); CSLDestroy(lco); if (poCloneSRS) poCloneSRS->Release();
                if (poLayer){
                    OGRFieldDefn fLayerName("LayerName", OFTString); fLayerName.SetWidth(80); poLayer->CreateField(&fLayerName);
                    OGRFieldDefn fDistance("Distance", OFTReal); fDistance.SetWidth(16); fDistance.SetPrecision(6); poLayer->CreateField(&fDistance);
                    OGRFieldDefn fThreshold("Threshold", OFTReal); fThreshold.SetWidth(16); fThreshold.SetPrecision(6); poLayer->CreateField(&fThreshold);
                    OGRFieldDefn fError("ErrorType", OFTString); fError.SetWidth(64); poLayer->CreateField(&fError);
                    OGRFieldDefn fSrcAngle("SourceAngle", OFTReal); fSrcAngle.SetWidth(16); fSrcAngle.SetPrecision(6); poLayer->CreateField(&fSrcAngle);
                    for (size_t i=0;i<errorPts.size();++i){ OGRFeature* oFeat = OGRFeature::CreateFeature(poLayer->GetLayerDefn()); OGRPoint pt(errorPts[i].x(), errorPts[i].y()); oFeat->SetGeometry(&pt); oFeat->SetField("LayerName", QString::fromStdString(layerName).toUtf8().constData()); oFeat->SetField("Distance", errorDistancesUnits[i]); oFeat->SetField("Threshold", dThresholdUnits); oFeat->SetField("ErrorType", errorTypes[i].toUtf8().constData()); oFeat->SetField("SourceAngle", srcPts[i].sourceAngleDeg); poLayer->CreateFeature(oFeat); OGRFeature::DestroyFeature(oFeat);}            
                }
                GDALClose(poDSOut);
            }
        }

        // 额外输出误差线段：将相邻错误点聚合为折线
        QString segPathQ = baseNoExt + QStringLiteral("_segments.shp");
        for(const QString& ext: exts) QFile::remove(fi.path() + QDir::separator() + fi.completeBaseName() + "_segments" + ext);
        if (poDriver){
            CplConfigGuard guardUtf82("GDAL_FILENAME_IS_UTF8", "NO");
            CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");
            QByteArray segNative = QFile::encodeName(QDir::toNativeSeparators(segPathQ));
            GDALDataset* segDS = poDriver->Create(segNative.constData(), 0,0,0, GDT_Unknown, nullptr);
            if (segDS){
                OGRSpatialReference* poCloneSRS2 = poSRS ? poSRS->Clone() : nullptr;
                char** lco2 = nullptr; lco2 = CSLSetNameValue(lco2, "ENCODING", "UTF-8");
                OGRLayer* segLayer = segDS->CreateLayer("error_segments", poCloneSRS2, wkbLineString, lco2); CSLDestroy(lco2); if (poCloneSRS2) poCloneSRS2->Release();
                if (segLayer){
                    OGRFieldDefn fLayerName2("LayerName", OFTString); fLayerName2.SetWidth(80); segLayer->CreateField(&fLayerName2);
                    OGRFieldDefn fError2("ErrorType", OFTString); fError2.SetWidth(64); segLayer->CreateField(&fError2);
                    OGRFieldDefn fCount("Count", OFTInteger); segLayer->CreateField(&fCount);
                    OGRFieldDefn fLength("Length", OFTReal); fLength.SetWidth(16); fLength.SetPrecision(6); segLayer->CreateField(&fLength);
                    // 按源点顺序聚合
                    struct Item { int order; QPointF pt; QString type; };
                    std::vector<Item> items; items.reserve(errorPts.size());
                    for(size_t i=0;i<errorPts.size();++i){ Item it; it.order = (i < errorOrders.size() ? errorOrders[i] : (int)i); it.pt = errorPts[i]; it.type = errorTypes[i]; items.push_back(it);} 
                    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b){ return a.order < b.order; });
                    const int maxGap = 3;
                    const double maxDist = dThresholdUnits * 3.0;
                    size_t start = 0;
                    while (start < items.size()){
                        size_t end = start;
                        QString curType = items[start].type;
                        while (end+1 < items.size() && items[end+1].order - items[end].order <= maxGap && items[end+1].type == curType && euclideanDistanceUnits(items[end].pt, items[end+1].pt) <= maxDist){ ++end; }
                        if (end > start){
                            OGRLineString ls;
                            double len = 0.0;
                            ls.addPoint(items[start].pt.x(), items[start].pt.y());
                            for (size_t k = start+1; k <= end; ++k){ ls.addPoint(items[k].pt.x(), items[k].pt.y()); len += euclideanDistanceUnits(items[k-1].pt, items[k].pt); }
                            OGRFeature* segF = OGRFeature::CreateFeature(segLayer->GetLayerDefn());
                            segF->SetGeometry(&ls);
                            segF->SetField("LayerName", QString::fromStdString(layerName).toUtf8().constData());
                            segF->SetField("ErrorType", curType.toUtf8().constData());
                            segF->SetField("Count", (int)(end - start + 1));
                            segF->SetField("Length", len);
                            segLayer->CreateFeature(segF);
                            OGRFeature::DestroyFeature(segF);
                            start = end + 1;
                        } else {
                            start = end + 1;
                        }
                    }
                }
                GDALClose(segDS);
            }
        }
    }
    else
    {
        logger->info("无不合格点，跳过错误图层输出");
    }

    // 写JSON报告
    QJsonObject jsonResult;
    QJsonObject checkItem;
    checkItem["id"] = "107";
    checkItem["name"] = "特征点限差控制";
    checkItem["status"] = "质检完成";
    checkItem["err_exist"] = (errorCount > 0) ? "true" : "false";
    checkItem["info"] = QString("%1图层%2超限或特征未保留")
                              .arg(QString::fromStdString(layerName))
                              .arg((errorCount > 0) ? QStringLiteral("存在") : QStringLiteral("不存在"));
    checkItem["errors"] = QJsonObject();
    jsonResult["check_item"] = checkItem;

    QJsonDocument doc(jsonResult);
    QFile jsonFile(szOutputJsonPath);
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        jsonFile.write(doc.toJson());
        jsonFile.close();
        logger->info("已生成JSON结果：{}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));
    }
    else
    {
        logger->error("无法创建JSON结果文件：{}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));
    }

    if (parsedGeom) delete parsedGeom;
    GDALClose(poSourceDS);
    spdlog::drop("FeaturePointTolerance");
    return SE_ERROR_NONE;
}
