#define _HAS_STD_BYTE 0
#include "se_boundary_anomaly_check_algorithm.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include <unordered_map>

#include <QElapsedTimer>
#include <QtGlobal>  // qDebug

// QGIS 几何 API（用于缓冲区分析与求交/求差）
#include "qgsgeometry.h"
#include "qgsabstractgeometry.h"
#include "qgsgeometrycollection.h"
#include "qgscurve.h"
#include "qgslinestring.h"
#include "qgspoint.h"
#include "qgsgeometryfactory.h"

// ============================================================
// 内部辅助函数
// ============================================================

namespace {

// 两点间欧氏距离
inline double Dist2f(const cv::Point2f& a, const cv::Point2f& b) {
    double dx = (double)a.x - (double)b.x;
    double dy = (double)a.y - (double)b.y;
    return std::sqrt(dx * dx + dy * dy);
}

inline double Dist2d(const cv::Point2d& a, const cv::Point2d& b) {
    double dx = a.x - b.x;
    double dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

// 点到线段的最短距离（返回距离和投影参数t in [0,1]）
double PointToSegmentDist(const cv::Point2f& pt,
                          const cv::Point2f& segA,
                          const cv::Point2f& segB,
                          double& outT,
                          cv::Point2f& outProj)
{
    double dx = (double)segB.x - (double)segA.x;
    double dy = (double)segB.y - (double)segA.y;
    double lenSq = dx * dx + dy * dy;
    if (lenSq < 1e-12) {
        outT = 0.0;
        outProj = segA;
        return Dist2f(pt, segA);
    }
    double t = ((double)(pt.x - segA.x) * dx + (double)(pt.y - segA.y) * dy) / lenSq;
    t = std::max(0.0, std::min(1.0, t));
    outT = t;
    outProj = cv::Point2f((float)(segA.x + t * dx), (float)(segA.y + t * dy));
    return Dist2f(pt, outProj);
}

// 折线总长度
double PolylineLength(const std::vector<cv::Point2f>& poly) {
    double len = 0.0;
    for (size_t i = 1; i < poly.size(); ++i) {
        len += Dist2f(poly[i - 1], poly[i]);
    }
    return len;
}

// 折线包围盒
cv::Rect2f PolylineBBox(const std::vector<cv::Point2f>& poly) {
    if (poly.empty()) return cv::Rect2f(0, 0, 0, 0);
    float minX = poly[0].x, maxX = poly[0].x;
    float minY = poly[0].y, maxY = poly[0].y;
    for (size_t i = 1; i < poly.size(); ++i) {
        if (poly[i].x < minX) minX = poly[i].x;
        if (poly[i].x > maxX) maxX = poly[i].x;
        if (poly[i].y < minY) minY = poly[i].y;
        if (poly[i].y > maxY) maxY = poly[i].y;
    }
    return cv::Rect2f(minX, minY, maxX - minX, maxY - minY);
}

// ---- Point2d（地理坐标）版本，用于主检测函数 ----

double PointToSegmentDistD(const cv::Point2d& pt,
                           const cv::Point2d& segA,
                           const cv::Point2d& segB,
                           size_t& outSegIdx)
{
    double dx = segB.x - segA.x;
    double dy = segB.y - segA.y;
    double lenSq = dx * dx + dy * dy;
    if (lenSq < 1e-12) {
        return Dist2d(pt, segA);
    }
    double t = ((pt.x - segA.x) * dx + (pt.y - segA.y) * dy) / lenSq;
    t = std::max(0.0, std::min(1.0, t));
    cv::Point2d proj(segA.x + t * dx, segA.y + t * dy);
    return Dist2d(pt, proj);
}

double PolylineLengthD(const std::vector<cv::Point2d>& poly) {
    double len = 0.0;
    for (size_t i = 1; i < poly.size(); ++i) {
        len += Dist2d(poly[i - 1], poly[i]);
    }
    return len;
}

cv::Rect2d PolylineBBoxD(const std::vector<cv::Point2d>& poly) {
    if (poly.empty()) return cv::Rect2d(0, 0, 0, 0);
    double minX = poly[0].x, maxX = poly[0].x;
    double minY = poly[0].y, maxY = poly[0].y;
    for (size_t i = 1; i < poly.size(); ++i) {
        if (poly[i].x < minX) minX = poly[i].x;
        if (poly[i].x > maxX) maxX = poly[i].x;
        if (poly[i].y < minY) minY = poly[i].y;
        if (poly[i].y > maxY) maxY = poly[i].y;
    }
    return cv::Rect2d(minX, minY, maxX - minX, maxY - minY);
}

// 点到折线最短距离（Point2d）
double PointToPolylineDistD(const cv::Point2d& pt,
                            const std::vector<cv::Point2d>& poly)
{
    if (poly.size() < 2) {
        return poly.empty() ? 0.0 : Dist2d(pt, poly[0]);
    }
    double minDist = 1e18;
    for (size_t i = 1; i < poly.size(); ++i) {
        size_t si;
        double d = PointToSegmentDistD(pt, poly[i - 1], poly[i], si);
        if (d < minDist) minDist = d;
    }
    return minDist;
}

// 等间隔采样（Point2d）
std::vector<cv::Point2d> SamplePolylineD(const std::vector<cv::Point2d>& poly,
                                         double interval)
{
    std::vector<cv::Point2d> samples;
    if (poly.size() < 2 || interval <= 0) return samples;

    double totalLen = PolylineLengthD(poly);
    if (totalLen < interval) {
        samples.push_back(poly.front());
        if (poly.size() > 1) samples.push_back(poly.back());
        return samples;
    }

    int numSamples = std::max(2, (int)std::ceil(totalLen / interval));
    samples.reserve(numSamples);

    std::vector<double> cumLen;
    cumLen.reserve(poly.size());
    cumLen.push_back(0.0);
    for (size_t i = 1; i < poly.size(); ++i) {
        cumLen.push_back(cumLen.back() + Dist2d(poly[i - 1], poly[i]));
    }

    size_t segIdx = 0;
    for (int k = 0; k < numSamples; ++k) {
        double targetLen = (k / (double)(numSamples - 1)) * totalLen;
        while (segIdx + 1 < cumLen.size() && cumLen[segIdx + 1] < targetLen) {
            ++segIdx;
        }
        if (segIdx + 1 >= cumLen.size()) {
            samples.push_back(poly.back());
            break;
        }
        double segStart = cumLen[segIdx];
        double segLen = cumLen[segIdx + 1] - cumLen[segIdx];
        double t = segLen > 1e-12 ? (targetLen - segStart) / segLen : 0.0;
        t = std::max(0.0, std::min(1.0, t));
        samples.push_back(cv::Point2d(
            poly[segIdx].x + t * (poly[segIdx + 1].x - poly[segIdx].x),
            poly[segIdx].y + t * (poly[segIdx + 1].y - poly[segIdx].y)));
    }
    return samples;
}

// 构建空间索引（Point2d）
std::vector<std::vector<std::vector<size_t>>> BuildSpatialIndexD(
    const std::vector<std::vector<cv::Point2d>>& standardBoundaries,
    double gridCellSize)
{
    if (standardBoundaries.empty() || gridCellSize <= 0) return {};

    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    for (auto& poly : standardBoundaries) {
        auto bb = PolylineBBoxD(poly);
        if (bb.x < minX) minX = bb.x;
        if (bb.x + bb.width > maxX) maxX = bb.x + bb.width;
        if (bb.y < minY) minY = bb.y;
        if (bb.y + bb.height > maxY) maxY = bb.y + bb.height;
    }

    int gridW = std::max(1, (int)std::ceil((maxX - minX) / gridCellSize));
    int gridH = std::max(1, (int)std::ceil((maxY - minY) / gridCellSize));

    std::vector<std::vector<std::vector<size_t>>> index(
        gridH, std::vector<std::vector<size_t>>(gridW));

    for (size_t polyIdx = 0; polyIdx < standardBoundaries.size(); ++polyIdx) {
        auto bb = PolylineBBoxD(standardBoundaries[polyIdx]);
        int cx1 = std::max(0, (int)((bb.x - minX) / gridCellSize));
        int cy1 = std::max(0, (int)((bb.y - minY) / gridCellSize));
        int cx2 = std::min(gridW - 1, (int)((bb.x + bb.width - minX) / gridCellSize));
        int cy2 = std::min(gridH - 1, (int)((bb.y + bb.height - minY) / gridCellSize));
        for (int cy = cy1; cy <= cy2; ++cy) {
            for (int cx = cx1; cx <= cx2; ++cx) {
                index[cy][cx].push_back(polyIdx);
            }
        }
    }
    return index;
}

// ---- QGIS 几何 API 辅助 ----

// 将一条 cv::Point2d 折线转为 QgsGeometry（LineString）
// 会去除连续重复点，避免 GEOS 在退化几何上卡死
QgsGeometry PolylineToQgsGeometry(const std::vector<cv::Point2d>& poly)
{
    if (poly.size() < 2) return QgsGeometry();
    QgsPointSequence pts;
    pts.reserve(poly.size());
    for (size_t i = 0; i < poly.size(); ++i) {
        // 跳过与上一个顶点重复的点（避免零长度段）
        if (!pts.isEmpty()) {
            const QgsPoint& last = pts.last();
            if (std::fabs(last.x() - poly[i].x) < 1e-12 &&
                std::fabs(last.y() - poly[i].y) < 1e-12) {
                continue;
            }
        }
        pts.append(QgsPoint(poly[i].x, poly[i].y));
    }
    if (pts.size() < 2) return QgsGeometry();
    return QgsGeometry::fromPolyline(pts);
}

// 将 QgsGeometry（单/多部 LineString）转为多段 cv::Point2d 折线
// 若几何为空或非线类型，返回空向量
std::vector<std::vector<cv::Point2d>> QgsGeometryToPolylines(const QgsGeometry& geom)
{
    std::vector<std::vector<cv::Point2d>> result;
    if (geom.isNull() || geom.isEmpty()) return result;

    // 统一按多部处理
    QgsGeometry multi = geom;
    if (!multi.isMultipart()) {
        multi.convertToMultiType();  // 原地转换，返回 bool
    }

    const QgsAbstractGeometry* ab = multi.constGet();
    if (!ab) return result;

    // QgsMultiLineString 继承自 QgsGeometryCollection
    const QgsGeometryCollection* coll = qgsgeometry_cast<const QgsGeometryCollection*>(ab);
    if (!coll) return result;

    int nParts = coll->numGeometries();
    for (int i = 0; i < nParts; ++i) {
        const QgsAbstractGeometry* partAbs = coll->geometryN(i);
        const QgsCurve* curve = partAbs ? qgsgeometry_cast<const QgsCurve*>(partAbs) : nullptr;
        if (!curve) continue;

        int nVerts = curve->numPoints();
        if (nVerts < 2) continue;
        std::vector<cv::Point2d> part;
        part.reserve(nVerts);
        for (int v = 0; v < nVerts; ++v) {
            part.emplace_back(curve->xAt(v), curve->yAt(v));
        }
        result.push_back(std::move(part));
    }
    return result;
}

} // anonymous namespace

// ============================================================
// 1. 构建空间索引（均匀网格）
// ============================================================
std::vector<std::vector<std::vector<size_t>>> BuildSpatialIndex(
    const std::vector<std::vector<cv::Point2f>>& standardBoundaries,
    double gridCellSize)
{
    if (standardBoundaries.empty() || gridCellSize <= 0) {
        return {};
    }

    // 计算全局范围
    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    for (auto& poly : standardBoundaries) {
        auto bb = PolylineBBox(poly);
        if (bb.x < minX) minX = bb.x;
        if (bb.x + bb.width > maxX) maxX = bb.x + bb.width;
        if (bb.y < minY) minY = bb.y;
        if (bb.y + bb.height > maxY) maxY = bb.y + bb.height;
    }

    int gridW = std::max(1, (int)std::ceil((maxX - minX) / gridCellSize));
    int gridH = std::max(1, (int)std::ceil((maxY - minY) / gridCellSize));

    std::vector<std::vector<std::vector<size_t>>> index(
        gridH, std::vector<std::vector<size_t>>(gridW));

    for (size_t polyIdx = 0; polyIdx < standardBoundaries.size(); ++polyIdx) {
        auto& poly = standardBoundaries[polyIdx];
        auto bb = PolylineBBox(poly);
        int cx1 = std::max(0, (int)((bb.x - minX) / gridCellSize));
        int cy1 = std::max(0, (int)((bb.y - minY) / gridCellSize));
        int cx2 = std::min(gridW - 1, (int)((bb.x + bb.width - minX) / gridCellSize));
        int cy2 = std::min(gridH - 1, (int)((bb.y + bb.height - minY) / gridCellSize));
        for (int cy = cy1; cy <= cy2; ++cy) {
            for (int cx = cx1; cx <= cx2; ++cx) {
                index[cy][cx].push_back(polyIdx);
            }
        }
    }
    return index;
}

// ============================================================
// 2. 点到折线最短距离
// ============================================================
double PointToPolylineDist(const cv::Point2f& pt,
                           const std::vector<cv::Point2f>& poly,
                           size_t& outNearestSegIdx)
{
    if (poly.size() < 2) {
        outNearestSegIdx = 0;
        return poly.empty() ? 0.0 : Dist2f(pt, poly[0]);
    }

    double minDist = 1e18;
    outNearestSegIdx = 0;
    for (size_t i = 1; i < poly.size(); ++i) {
        double t;
        cv::Point2f proj;
        double d = PointToSegmentDist(pt, poly[i - 1], poly[i], t, proj);
        if (d < minDist) {
            minDist = d;
            outNearestSegIdx = i - 1;
        }
    }
    return minDist;
}

// ============================================================
// 3. 缓冲区重叠比例
// ============================================================
double ComputeOverlapRatio(const std::vector<cv::Point2f>& extracted,
                           const std::vector<cv::Point2f>& standard,
                           double bufferRadius)
{
    if (extracted.size() < 2 || standard.size() < 2) return 0.0;

    // 对提取线等间隔采样，统计落在标准线缓冲区内点的比例
    double totalLen = PolylineLength(extracted);
    if (totalLen < 1.0) return 0.0;

    double interval = std::max(5.0, totalLen / 200.0); // 至少200个采样点
    auto samples = SamplePolyline(extracted, interval);

    int inBuffer = 0;
    for (auto& s : samples) {
        size_t segIdx;
        double d = PointToPolylineDist(s, standard, segIdx);
        if (d <= bufferRadius) {
            ++inBuffer;
        }
    }

    return static_cast<double>(inBuffer) / static_cast<double>(samples.size());
}

// ============================================================
// 4. 等间隔采样
// ============================================================
std::vector<cv::Point2f> SamplePolyline(const std::vector<cv::Point2f>& poly,
                                         double interval)
{
    std::vector<cv::Point2f> samples;
    if (poly.size() < 2 || interval <= 0) return samples;

    double totalLen = PolylineLength(poly);
    if (totalLen < interval) {
        // 折线太短，返回首尾两点
        samples.push_back(poly.front());
        if (poly.size() > 1) samples.push_back(poly.back());
        return samples;
    }

    // 沿折线按弧长等间隔采样
    int numSamples = std::max(2, (int)std::ceil(totalLen / interval));
    samples.reserve(numSamples);

    // 为每个线段预计算累计弧长
    std::vector<double> cumLen;
    cumLen.reserve(poly.size());
    cumLen.push_back(0.0);
    for (size_t i = 1; i < poly.size(); ++i) {
        cumLen.push_back(cumLen.back() + Dist2f(poly[i - 1], poly[i]));
    }

    size_t segIdx = 0;
    for (int k = 0; k < numSamples; ++k) {
        double targetLen = (k / (double)(numSamples - 1)) * totalLen;
        // 找到 targetLen 所在的段
        while (segIdx + 1 < cumLen.size() && cumLen[segIdx + 1] < targetLen) {
            ++segIdx;
        }
        if (segIdx + 1 >= cumLen.size()) {
            samples.push_back(poly.back());
            break;
        }
        double segStart = cumLen[segIdx];
        double segLen = cumLen[segIdx + 1] - cumLen[segIdx];
        double t = segLen > 1e-12 ? (targetLen - segStart) / segLen : 0.0;
        t = std::max(0.0, std::min(1.0, t));
        samples.push_back(cv::Point2f(
            (float)(poly[segIdx].x + t * (poly[segIdx + 1].x - poly[segIdx].x)),
            (float)(poly[segIdx].y + t * (poly[segIdx + 1].y - poly[segIdx].y))));
    }

    return samples;
}

// ============================================================
// 5. 方向差异计算
// ============================================================
double ComputeDirectionDifference(const std::vector<cv::Point2f>& poly1,
                                  const std::vector<cv::Point2f>& poly2)
{
    if (poly1.size() < 2 || poly2.size() < 2) return 0.0;

    // 对两条折线等间隔采样，计算对应点处的切线方向偏差
    double interval = std::max(SamplePolyline(poly1, 10.0).size() > 0 ? 10.0 : 5.0,
                               PolylineLength(poly1) / 100.0);
    auto samples = SamplePolyline(poly1, interval);
    if (samples.size() < 2) {
        // 降级：比较首尾方向
        auto dir1 = cv::Point2f(poly1.back().x - poly1.front().x,
                                poly1.back().y - poly1.front().y);
        auto dir2 = cv::Point2f(poly2.back().x - poly2.front().x,
                                poly2.back().y - poly2.front().y);
        double dot = (double)dir1.x * dir2.x + (double)dir1.y * dir2.y;
        double norm = std::sqrt((double)(dir1.x*dir1.x + dir1.y*dir1.y) *
                                (double)(dir2.x*dir2.x + dir2.y*dir2.y));
        if (norm < 1e-12) return 0.0;
        double cosA = std::max(-1.0, std::min(1.0, dot / norm));
        return std::acos(cosA) * 180.0 / 3.14159265358979;
    }

    double totalAngleDiff = 0.0;
    int validCount = 0;

    for (size_t i = 0; i < samples.size(); ++i) {
        // 找到采样点在 poly2 上的最近投影，用该点的切线方向
        size_t segIdx;
        double dist = PointToPolylineDist(samples[i], poly2, segIdx);
        if (dist > 100.0) continue; // 太远不可靠

        // 采样点处的切线方向（poly1 上）
        cv::Point2f dir1;
        if (i == 0) {
            dir1 = cv::Point2f(samples[1].x - samples[0].x, samples[1].y - samples[0].y);
        } else if (i == samples.size() - 1) {
            dir1 = cv::Point2f(samples[i].x - samples[i-1].x, samples[i].y - samples[i-1].y);
        } else {
            dir1 = cv::Point2f(samples[i+1].x - samples[i-1].x, samples[i+1].y - samples[i-1].y);
        }

        // poly2 上对应段的切线方向
        cv::Point2f dir2(poly2[segIdx + 1].x - poly2[segIdx].x,
                         poly2[segIdx + 1].y - poly2[segIdx].y);

        double norm1 = std::sqrt((double)(dir1.x*dir1.x + dir1.y*dir1.y));
        double norm2 = std::sqrt((double)(dir2.x*dir2.x + dir2.y*dir2.y));
        if (norm1 < 0.01 || norm2 < 0.01) continue;

        double dot = (double)dir1.x * dir2.x + (double)dir1.y * dir2.y;
        double cosA = std::max(-1.0, std::min(1.0, dot / (norm1 * norm2)));
        totalAngleDiff += std::acos(cosA);
        ++validCount;
    }

    if (validCount == 0) return 0.0;
    return (totalAngleDiff / validCount) * 180.0 / 3.14159265358979;
}

// ============================================================
// 6. 线段拆分
// ============================================================
std::vector<std::vector<cv::Point2f>> SplitPolyline(
    const std::vector<cv::Point2f>& poly,
    double segmentLength)
{
    std::vector<std::vector<cv::Point2f>> result;
    if (poly.size() < 2 || segmentLength <= 0) {
        if (!poly.empty()) result.push_back(poly);
        return result;
    }

    double totalLen = PolylineLength(poly);
    if (totalLen <= segmentLength) {
        result.push_back(poly);
        return result;
    }

    // 预计算累计弧长
    std::vector<double> cumLen;
    cumLen.reserve(poly.size());
    cumLen.push_back(0.0);
    for (size_t i = 1; i < poly.size(); ++i) {
        cumLen.push_back(cumLen.back() + Dist2f(poly[i - 1], poly[i]));
    }

    int numSegments = std::max(1, (int)std::ceil(totalLen / segmentLength));
    size_t segIdx = 0;
    double segStart = 0.0;
    size_t startVertex = 0;
    double startT = 0.0;

    for (int k = 0; k < numSegments; ++k) {
        double segEnd = std::min(totalLen, segStart + segmentLength);

        // 找 segEnd 对应的顶点和参数 t
        size_t endVertex = startVertex;
        double endT = startT;
        while (endVertex + 1 < cumLen.size() && cumLen[endVertex + 1] < segEnd) {
            ++endVertex;
        }
        if (endVertex + 1 < cumLen.size()) {
            double lenInSeg = segEnd - cumLen[endVertex];
            double segLen = cumLen[endVertex + 1] - cumLen[endVertex];
            endT = segLen > 1e-12 ? lenInSeg / segLen : 0.0;
            endT = std::max(0.0, std::min(1.0, endT));
        }

        // 构建子线段
        std::vector<cv::Point2f> subPoly;
        subPoly.push_back(cv::Point2f(
            (float)(poly[startVertex].x + startT * (poly[startVertex + 1].x - poly[startVertex].x)),
            (float)(poly[startVertex].y + startT * (poly[startVertex + 1].y - poly[startVertex].y))));

        for (size_t v = startVertex + 1; v <= endVertex && v < poly.size(); ++v) {
            subPoly.push_back(poly[v]);
        }

        if (endT > 1e-6 && endVertex + 1 < poly.size()) {
            subPoly.push_back(cv::Point2f(
                (float)(poly[endVertex].x + endT * (poly[endVertex + 1].x - poly[endVertex].x)),
                (float)(poly[endVertex].y + endT * (poly[endVertex + 1].y - poly[endVertex].y))));
        }

        result.push_back(std::move(subPoly));

        // 更新起始状态
        segStart = segEnd;
        startVertex = endVertex;
        startT = endT;
        if (startT >= 0.999999) {
            startVertex++;
            startT = 0.0;
        }
    }

    return result;
}

// ============================================================
// 7. JSON 报告生成
// ============================================================
std::string GenerateAnomalyReport(const BoundaryAnomalyResult& result) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4);
    oss << "{\n";
    oss << "  \"success\": " << (result.success ? "true" : "false") << ",\n";
    if (!result.errorMsg.empty()) {
        oss << "  \"error\": \"" << result.errorMsg << "\",\n";
    }
    oss << "  \"summary\": {\n";
    oss << "    \"total_extracted\": " << result.totalExtractedSegments << ",\n";
    oss << "    \"total_standard\": " << result.totalStandardSegments << ",\n";
    oss << "    \"matched\": " << result.matchedCount << ",\n";
    oss << "    \"normal\": " << result.normalCount << ",\n";
    oss << "    \"position_offset\": " << result.positionOffsetCount << ",\n";
    oss << "    \"shape_anomaly\": " << result.shapeAnomalyCount << ",\n";
    oss << "    \"missing\": " << result.missingCount << ",\n";
    oss << "    \"redundant\": " << result.redundantCount << ",\n";
    oss << "    \"overall_avg_offset\": " << result.overallAvgOffset << ",\n";
    oss << "    \"overall_max_offset\": " << result.overallMaxOffset << "\n";
    oss << "  },\n";
    oss << "  \"segments\": [\n";
    for (size_t i = 0; i < result.segments.size(); ++i) {
        auto& seg = result.segments[i];
        oss << "    {\n";
        oss << "      \"id\": " << seg.segmentId << ",\n";
        const char* typeStr = "normal";
        switch (seg.type) {
            case BoundaryAnomalyType::Normal: typeStr = "normal"; break;
            case BoundaryAnomalyType::PositionOffset: typeStr = "position_offset"; break;
            case BoundaryAnomalyType::ShapeAnomaly: typeStr = "shape_anomaly"; break;
            case BoundaryAnomalyType::Missing: typeStr = "missing"; break;
            case BoundaryAnomalyType::Redundant: typeStr = "redundant"; break;
        }
        oss << "      \"type\": \"" << typeStr << "\",\n";
        oss << "      \"avg_offset\": " << seg.avgOffset << ",\n";
        oss << "      \"max_offset\": " << seg.maxOffset << ",\n";
        oss << "      \"direction_diff\": " << seg.directionDiff << ",\n";
        oss << "      \"match_ref_idx\": " << seg.matchRefIdx << ",\n";
        oss << "      \"overlap_ratio\": " << seg.matchOverlapRatio << ",\n";
        oss << "      \"description\": \"" << seg.description << "\"\n";
        oss << "    }" << (i + 1 < result.segments.size() ? "," : "") << "\n";
    }
    oss << "  ]\n";
    oss << "}\n";
    return oss.str();
}

// ============================================================
// 8. 主检测函数
// ============================================================
SE_Error CheckBoundaryAnomalies(
    const std::vector<std::vector<cv::Point2d>>& extractedBoundaries,
    const std::vector<std::vector<cv::Point2d>>& standardBoundaries,
    const BoundaryCheckConfig& config,
    BoundaryAnomalyResult& outResult)
{
    outResult = BoundaryAnomalyResult{};
    outResult.success = false;

    QElapsedTimer totalTimer; totalTimer.start();

    if (extractedBoundaries.empty() || standardBoundaries.empty()) {
        outResult.errorMsg = "Boundaries are empty";
        return SE_ERROR_FAILURE;
    }
    if (config.bufferRadius <= 0.0) {
        outResult.errorMsg = "bufferRadius must be positive";
        return SE_ERROR_FAILURE;
    }

    // 统计数据量
    size_t stdTotalVerts = 0, extTotalVerts = 0;
    for (auto& s : standardBoundaries) stdTotalVerts += s.size();
    for (auto& e : extractedBoundaries) extTotalVerts += e.size();
    qDebug("[BoundaryCheck] 标准线 %d 条, 总顶点 %zu; 提取线 %d 条, 总顶点 %zu; bufferRadius=%.1fm",
           (int)standardBoundaries.size(), stdTotalVerts,
           (int)extractedBoundaries.size(), extTotalVerts,
           config.bufferRadius);

    // ----------------------------------------------------------
    // 阶段1：将所有标准境界线合并为一个几何对象，并建立 bufferRadius 米缓冲区
    // ----------------------------------------------------------
    QElapsedTimer t; t.start();

    QVector<QgsGeometry> stdGeoms;
    stdGeoms.reserve((int)standardBoundaries.size());
    for (auto& sLine : standardBoundaries) {
        if (sLine.size() < 2) continue;
        QgsGeometry g = PolylineToQgsGeometry(sLine);
        if (!g.isNull()) stdGeoms.append(g);
    }
    if (stdGeoms.isEmpty()) {
        outResult.errorMsg = "No valid standard boundary geometry";
        return SE_ERROR_FAILURE;
    }
    qDebug("[BoundaryCheck] 标准线 QgsGeometry 构造完成: %d 条, 耗时 %lld ms",
           stdGeoms.size(), t.elapsed());

    t.restart();
    int segments = config.bufferSegments > 0 ? config.bufferSegments : 16;

    // 策略：逐条缓冲后再合并（比"先合并再缓冲"更稳健）
    // 原因：47 条标准线合计 50 万+顶点，unaryUnion 后的 MultiLineString 直接 buffer
    //       会在 GEOS 偏移曲线计算中触发 TopologyException（自相交/数值精度问题）
    QVector<QgsGeometry> bufferParts;
    bufferParts.reserve(stdGeoms.size());
    int bufFailCnt = 0;
    for (int i = 0; i < stdGeoms.size(); ++i) {
        QgsGeometry buf = stdGeoms[i].buffer(config.bufferRadius, segments);
        // 单条缓冲失败时尝试修复：移除重复点 -> makeValid 后重试
        if (buf.isNull() || buf.isEmpty()) {
            QgsGeometry cleaned = stdGeoms[i];
            cleaned.removeDuplicateNodes(1e-9, false);
            QgsGeometry valid = cleaned.makeValid();
            if (!valid.isNull() && !valid.isEmpty()) cleaned = valid;
            buf = cleaned.buffer(config.bufferRadius, segments);
        }
        // 仍然失败：简化后重试（容差远小于缓冲半径，不影响精度）
        if (buf.isNull() || buf.isEmpty()) {
            QgsGeometry simplified = stdGeoms[i].simplify(config.bufferRadius * 0.1);
            if (!simplified.isNull() && !simplified.isEmpty()) {
                buf = simplified.buffer(config.bufferRadius, segments);
            }
        }
        if (!buf.isNull() && !buf.isEmpty()) {
            bufferParts.append(buf);
        } else {
            ++bufFailCnt;
            qDebug("[BoundaryCheck]   标准线 %d 缓冲失败, 跳过", i);
        }
    }
    qDebug("[BoundaryCheck] 逐条缓冲完成, 成功 %d/%d, 失败 %d, 耗时 %lld ms",
           (int)bufferParts.size(), (int)stdGeoms.size(), bufFailCnt, t.elapsed());

    if (bufferParts.isEmpty()) {
        outResult.errorMsg = "Failed to build standard buffer (all individual buffers failed)";
        return SE_ERROR_FAILURE;
    }

    // 合并缓冲区多边形（合并后求交/求差更高效）；失败则退化为几何集合
    t.restart();
    QgsGeometry stdBuffer = bufferParts.size() == 1
        ? bufferParts[0]
        : QgsGeometry::unaryUnion(bufferParts);
    if (stdBuffer.isNull() || stdBuffer.isEmpty()) {
        qDebug("[BoundaryCheck] unaryUnion 缓冲区失败, 退化为几何集合");
        stdBuffer = QgsGeometry::collectGeometry(bufferParts);
    }
    qDebug("[BoundaryCheck] 缓冲区构建完成, 耗时 %lld ms, isNull=%d, isEmpty=%d, wkbType=%d",
           t.elapsed(), stdBuffer.isNull(), stdBuffer.isEmpty(), (int)stdBuffer.wkbType());
    if (stdBuffer.isNull() || stdBuffer.isEmpty()) {
        outResult.errorMsg = "Failed to build standard buffer";
        return SE_ERROR_FAILURE;
    }

    outResult.totalExtractedSegments = (int)extractedBoundaries.size();
    outResult.totalStandardSegments = (int)standardBoundaries.size();

    // ----------------------------------------------------------
    // 阶段1.5：构建标准线段网格空间索引（用于快速最近距离查询）
    // 原因：47 条标准线共 50 万+顶点，逐采样点全量遍历会导致性能崩溃
    // ----------------------------------------------------------
    struct SegRef { int lineIdx; int segStart; };
    double cellSize = std::max(config.bufferRadius * 5.0, 1000.0);
    double idxMinX = 1e18, idxMinY = 1e18, idxMaxX = -1e18, idxMaxY = -1e18;
    for (auto& sLine : standardBoundaries) {
        for (auto& p : sLine) {
            if (p.x < idxMinX) idxMinX = p.x;
            if (p.y < idxMinY) idxMinY = p.y;
            if (p.x > idxMaxX) idxMaxX = p.x;
            if (p.y > idxMaxY) idxMaxY = p.y;
        }
    }
    int idxW = std::max(1, (int)std::ceil((idxMaxX - idxMinX) / cellSize));
    int idxH = std::max(1, (int)std::ceil((idxMaxY - idxMinY) / cellSize));
    std::unordered_map<int, std::vector<SegRef>> segGrid;
    for (int li = 0; li < (int)standardBoundaries.size(); ++li) {
        auto& poly = standardBoundaries[li];
        for (int si = 0; si + 1 < (int)poly.size(); ++si) {
            double sx1 = std::min(poly[si].x, poly[si + 1].x);
            double sy1 = std::min(poly[si].y, poly[si + 1].y);
            double sx2 = std::max(poly[si].x, poly[si + 1].x);
            double sy2 = std::max(poly[si].y, poly[si + 1].y);
            int cx1 = std::max(0, (int)((sx1 - idxMinX) / cellSize));
            int cy1 = std::max(0, (int)((sy1 - idxMinY) / cellSize));
            int cx2 = std::min(idxW - 1, (int)((sx2 - idxMinX) / cellSize));
            int cy2 = std::min(idxH - 1, (int)((sy2 - idxMinY) / cellSize));
            for (int cy = cy1; cy <= cy2; ++cy) {
                for (int cx = cx1; cx <= cx2; ++cx) {
                    segGrid[cy * idxW + cx].push_back({li, si});
                }
            }
        }
    }
    qDebug("[BoundaryCheck] 标准线段网格索引构建完成: %dx%d, cellSize=%.0fm, 非空网格=%zu",
           idxW, idxH, cellSize, segGrid.size());

    // 点到最近标准线段的距离（利用网格索引，逐层向外搜索）
    auto pointToStdDist = [&](const cv::Point2d& pt) -> double {
        int cx = (int)((pt.x - idxMinX) / cellSize);
        int cy = (int)((pt.y - idxMinY) / cellSize);
        if (cx < 0) cx = 0; else if (cx >= idxW) cx = idxW - 1;
        if (cy < 0) cy = 0; else if (cy >= idxH) cy = idxH - 1;

        double minDist = 1e18;
        int maxR = std::max(idxW, idxH);
        for (int r = 0; r <= maxR; ++r) {
            for (int dy = -r; dy <= r; ++dy) {
                int ny = cy + dy;
                if (ny < 0 || ny >= idxH) continue;
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue; // 只检查第 r 层环形
                    int nx = cx + dx;
                    if (nx < 0 || nx >= idxW) continue;
                    auto it = segGrid.find(ny * idxW + nx);
                    if (it == segGrid.end()) continue;
                    for (auto& sr : it->second) {
                        auto& poly = standardBoundaries[sr.lineIdx];
                        size_t si;
                        double d = PointToSegmentDistD(pt, poly[sr.segStart], poly[sr.segStart + 1], si);
                        if (d < minDist) minDist = d;
                    }
                }
            }
            // 已搜索范围的外边界超过当前最小距离时，更远的网格不可能有更近线段
            if (minDist < 1e18 && minDist < (double)(r + 1) * cellSize) break;
            // 超过 10 倍缓冲半径仍未找到，放弃（防止极端情况无限搜索）
            if (minDist >= 1e18 && (double)(r + 1) * cellSize > config.bufferRadius * 10.0) break;
        }
        return minDist;
    };

    // ----------------------------------------------------------
    // 阶段2：逐条提取境界线执行 buffer 求交/求差
    // ----------------------------------------------------------
    double allAvgOffsets = 0.0;
    double allMaxOffset = 0.0;
    int    validCount = 0;

    QElapsedTimer loopTimer; loopTimer.start();

    for (size_t eIdx = 0; eIdx < extractedBoundaries.size(); ++eIdx) {
        const auto& eLine = extractedBoundaries[eIdx];
        if (eLine.size() < 2) continue;

        if (eIdx % 5 == 0) {
            qDebug("[BoundaryCheck] 处理提取线 %zu/%zu (已耗时 %lld ms)",
                   eIdx, extractedBoundaries.size(), loopTimer.elapsed());
        }

        QElapsedTimer stepT; stepT.start();

        QgsGeometry eGeom = PolylineToQgsGeometry(eLine);
        if (eGeom.isNull()) continue;

        double totalLen = eGeom.length();
        if (totalLen <= 0.0) continue;

        // 先做快速包围盒判断：如果与缓冲区包围盒不相交，整条线视为异常，跳过 GEOS 求交
        QgsRectangle eBbox = eGeom.boundingBox();
        QgsRectangle bBbox = stdBuffer.boundingBox();
        if (!eBbox.intersects(bBbox)) {
            // 整条线在缓冲区外，全部为异常段
            BoundaryAnomalySegment seg;
            seg.segmentId = (int)eIdx;
            seg.avgOffset = 0.0;
            seg.maxOffset = 0.0;
            seg.matchRefIdx = -1;
            seg.matchOverlapRatio = 0.0;
            seg.anomalyParts.push_back(eLine);
            seg.type = BoundaryAnomalyType::PositionOffset;
            std::ostringstream desc;
            desc << std::fixed << std::setprecision(2)
                 << "异常(包围盒不相交): 总长 " << totalLen << "m";
            seg.description = desc.str();
            outResult.positionOffsetCount++;
            outResult.segments.push_back(std::move(seg));
            ++validCount;
            continue;
        }

        // 与缓冲区求交，得到正常段
        QgsGeometry normalGeom = eGeom.intersection(stdBuffer);
        // 与缓冲区求差，得到异常段
        QgsGeometry anomalyGeom = eGeom.difference(stdBuffer);

        qint64 interMs = stepT.elapsed();
        qDebug("[BoundaryCheck]   线 %zu: intersection+difference 耗时 %lld ms, "
               "normal=%s, anomaly=%s",
               eIdx, interMs,
               normalGeom.isNull() ? "null" : "ok",
               anomalyGeom.isNull() ? "null" : "ok");

        auto normalParts = QgsGeometryToPolylines(normalGeom);
        auto anomalyParts = QgsGeometryToPolylines(anomalyGeom);

        // 正常段与异常段长度
        double normalLen = 0.0;
        for (auto& p : normalParts) normalLen += PolylineLengthD(p);
        double anomalyLen = 0.0;
        for (auto& p : anomalyParts) anomalyLen += PolylineLengthD(p);

        double overlapRatio = (totalLen > 1e-9) ? (normalLen / totalLen) : 0.0;

        // 异常段平均/最大偏移：在异常段上采样，计算到最近标准线的距离
        double avgOffset = 0.0;
        double maxOffset = 0.0;
        if (anomalyLen > 1e-6 && !anomalyParts.empty()) {
            QElapsedTimer distT; distT.start();
            double sumDist = 0.0;
            int sampleCnt = 0;
            int anomalyVertCnt = 0;
            for (auto& aPart : anomalyParts) {
                anomalyVertCnt += (int)aPart.size();
                auto samples = SamplePolylineD(aPart, config.sampleInterval);
                for (auto& s : samples) {
                    double dMin = pointToStdDist(s);
                    if (dMin < 1e18) {
                        sumDist += dMin;
                        if (dMin > maxOffset) maxOffset = dMin;
                        ++sampleCnt;
                    }
                }
            }
            if (sampleCnt > 0) avgOffset = sumDist / sampleCnt;
            qDebug("[BoundaryCheck]   线 %zu: 异常段距离计算 耗时 %lld ms, "
                   "异常段=%d段/%d顶点, 采样=%d点",
                   eIdx, distT.elapsed(),
                   (int)anomalyParts.size(), anomalyVertCnt, sampleCnt);
        }

        BoundaryAnomalySegment seg;
        seg.segmentId = (int)eIdx;
        seg.avgOffset = avgOffset;
        seg.maxOffset = maxOffset;
        seg.directionDiff = 0.0;
        seg.matchRefIdx = -1;
        seg.matchOverlapRatio = overlapRatio;
        seg.normalParts = std::move(normalParts);
        seg.anomalyParts = std::move(anomalyParts);

        std::ostringstream desc;
        desc << std::fixed << std::setprecision(2);
        if (anomalyLen <= 1e-6) {
            seg.type = BoundaryAnomalyType::Normal;
            desc << "正常: 总长 " << totalLen << "m, 正常段占比 "
                 << (overlapRatio * 100.0) << "%";
            outResult.normalCount++;
        } else if (normalLen <= 1e-6) {
            seg.type = BoundaryAnomalyType::PositionOffset;
            desc << "异常: 总长 " << totalLen << "m, 异常段占比 "
                 << ((1.0 - overlapRatio) * 100.0)
                 << "%, 异常段平均偏离 " << avgOffset << "m, 最大 " << maxOffset << "m";
            outResult.positionOffsetCount++;
        } else {
            seg.type = BoundaryAnomalyType::PositionOffset;
            desc << "部分异常: 总长 " << totalLen << "m, 正常段占比 "
                 << (overlapRatio * 100.0) << "%, 异常段平均偏离 "
                 << avgOffset << "m, 最大 " << maxOffset << "m";
            outResult.positionOffsetCount++;
        }
        seg.description = desc.str();

        allAvgOffsets += avgOffset;
        if (maxOffset > allMaxOffset) allMaxOffset = maxOffset;
        ++validCount;

        outResult.segments.push_back(std::move(seg));
    }

    qDebug("[BoundaryCheck] 提取线循环总耗时 %lld ms", loopTimer.elapsed());

    // ----------------------------------------------------------
    // 汇总
    // ----------------------------------------------------------
    outResult.matchedCount = (int)extractedBoundaries.size();
    outResult.overallAvgOffset = validCount > 0 ? allAvgOffsets / validCount : 0.0;
    outResult.overallMaxOffset = allMaxOffset;
    outResult.success = true;

    qDebug("[BoundaryCheck] 检测完成, 总耗时 %lld ms, 正常=%d, 异常=%d",
           totalTimer.elapsed(), outResult.normalCount, outResult.positionOffsetCount);

    return SE_ERROR_NONE;
}

// ============================================================
// C 接口占位（文件路径方式暂不实现核心逻辑，供后续扩展）
// ============================================================
MY_PLUGIN_API SE_Error BoundaryAnomalyCheck(
    const char* /*szExtractedShpPath*/,
    const char* /*szStandardShpPath*/,
    const char* /*szOutputErrorLayerPath*/,
    const char* /*szOutputJsonPath*/,
    const char* /*szOutputLogFilePath*/)
{
    // 文件路径方式预留接口，暂返回不支持
    return SE_ERROR_UNSUPPORTED_OPERATION;
}
