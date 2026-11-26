#include "se_min_bend_threshold_algorithm.h"
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
// 在Windows下使用宽字符文件名，确保中文日志文件名不乱码
#ifdef _WIN32
#ifndef SPDLOG_WCHAR_FILENAMES
#define SPDLOG_WCHAR_FILENAMES
#endif
#endif
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/logger.h>
#include <QFile>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNodeList>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <cmath>
#include <cpl_string.h>

// 边界值处理常量，避免浮点数精度问题（对米级坐标更合适）
const double EPSILON = 1e-6;

// 解析XML获取最小弯曲阈值和斜率判断阈值
static SE_Error ParseXmlForMinBendThreshold(const char* szSchemaXmlPath, double& dThresholdValue, 
                                    double& dSlopeAngleThreshold, int nScale)
{
    if (!szSchemaXmlPath)
    {
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 使用Qt的QFile打开XML文件
    QFile file(szSchemaXmlPath);
    if (!file.open(QIODevice::ReadOnly))
    {
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 使用Qt的QDomDocument解析XML
    QDomDocument doc;
    QString errorMsg;
    int errorLine, errorColumn;
    if (!doc.setContent(&file, &errorMsg, &errorLine, &errorColumn))
    {
        file.close();
        return SE_ERROR_XML_PARSE_FAILED;
    }
    file.close();

    QDomElement rootElement = doc.documentElement();
    if (rootElement.tagName() != "check_list")
    {
        return SE_ERROR_XML_PARSE_FAILED;
    }
    QDomNodeList checkItemNodes = rootElement.elementsByTagName("check_item");
    bool foundThresholdValue = false;
    bool foundSlopeAngleThreshold = false;
    
    for (int i = 0; i < checkItemNodes.count(); i++)
    {
        QDomElement checkItemElement = checkItemNodes.at(i).toElement();
        QDomElement nameElement = checkItemElement.firstChildElement("name");
        if (!nameElement.isNull() && nameElement.text() == "最小弯曲阈值控制")
        {
            // 查找规则
            QDomNodeList ruleNodes = checkItemElement.elementsByTagName("rule");
            for (int j = 0; j < ruleNodes.count(); j++)
            {
                QDomElement ruleElement = ruleNodes.at(j).toElement();
                QDomElement ruleNameElement = ruleElement.firstChildElement("name");
                
                // 解析最小弯曲阈值
                if (!ruleNameElement.isNull() && ruleNameElement.text() == "最小弯曲阈值")
                {
                    QDomElement valueElement = ruleElement.firstChildElement("value");
                    if (!valueElement.isNull())
                    {
                        bool ok;
                        dThresholdValue = valueElement.text().toDouble(&ok);
                        if (ok)
                        {
                            foundThresholdValue = true;
                        }
                    }
                }
                
                // 解析斜率判断阈值
                if (!ruleNameElement.isNull() && ruleNameElement.text() == "斜率判断阈值")
                {
                    QDomElement valueElement = ruleElement.firstChildElement("value");
                    if (!valueElement.isNull())
                    {
                        bool ok;
                        dSlopeAngleThreshold = valueElement.text().toDouble(&ok);
                        if (ok)
                        {
                            foundSlopeAngleThreshold = true;
                        }
                    }
                }
            }
            
            // 如果找不到斜率判断阈值，使用默认值10度
            if (!foundSlopeAngleThreshold)
            {
                dSlopeAngleThreshold = 10.0;
                foundSlopeAngleThreshold = true;
                spdlog::warn("XML中未找到斜率判断阈值，使用默认值10度");
            }
            
            // 如果两个阈值都找到了，返回成功
            if (foundThresholdValue && foundSlopeAngleThreshold)
            {
                return SE_ERROR_NONE;
            }
        }
    }

    // 如果没有找到任何一个阈值，返回错误
    if (!foundThresholdValue)
    {
        spdlog::error("XML中未找到最小弯曲阈值");
    }
    
    return SE_ERROR_XML_PARSE_FAILED;
}

// 计算三角形面积
double CalculateTriangleArea(double x1, double y1, double x2, double y2, double x3, double y3)
{
    return 0.5 * std::abs((x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2)));
}

// 计算斜率变化角度（转折角）
double CalculateSlopeAngle(double x1, double y1, double x2, double y2, double x3, double y3)
{
    // 处理垂直线的情况
    if (std::abs(x2 - x1) < EPSILON || std::abs(x3 - x2) < EPSILON)
    {
        // 如果两条线段都是垂直的
        if (std::abs(x2 - x1) < EPSILON && std::abs(x3 - x2) < EPSILON)
        {
            return 0.0; // 两条垂直线段共线，角度为0
        }
        
        // 如果只有一条线段是垂直的，角度为90度
        return 90.0;
    }
    
    // 计算两条线段的斜率
    double k1 = (y2 - y1) / (x2 - x1);
    double k2 = (y3 - y2) / (x3 - x2);
    
    // 计算斜率变化角度（转折角）：Δθ = arctan|(k2-k1)/(1+k1*k2)|
    double tanTheta = std::abs((k2 - k1) / (1.0 + k1 * k2));
    
    // 处理分母为0的情况（当k1*k2 = -1时）
    if (std::abs(1.0 + k1 * k2) < EPSILON)
    {
        return 90.0; // 两条线段垂直，角度为90度
    }
    
    // 将弧度转换为角度
    double angleInDegrees = std::atan(tanTheta) * 180.0 / M_PI;
    
    return angleInDegrees;
}

// 提取特征点（拐点）
void ExtractBendPoints(const std::vector<OGRPoint*>& points, double angleThreshold, std::vector<int>& bendPoints, bool isClosedRing)
{
    bendPoints.clear();
    
    // 点数少于3个无法形成拐点
    if (points.size() < 3)
    {
        return;
    }
    
    // 首尾点始终是拐点
    bendPoints.push_back(0);
    
    // 计算所有线段长度，用于弧长比计算
    std::vector<double> segmentLengths(points.size() - 1);
    double totalLength = 0.0;
    for (size_t i = 0; i < points.size() - 1; ++i)
    {
        double dx = points[i + 1]->getX() - points[i]->getX();
        double dy = points[i + 1]->getY() - points[i]->getY();
        segmentLengths[i] = std::sqrt(dx * dx + dy * dy);
        totalLength += segmentLengths[i];
    }
    
    // 最小线段长度比例（占总长度的百分比），小于此值的线段被视为噪声
    const double MIN_SEGMENT_RATIO = 0.05; // 5%
    double minSegmentLength = totalLength * MIN_SEGMENT_RATIO;
    
    // 角度变化的连续性检测
    std::vector<double> angles(points.size() - 2);
    std::vector<int> potentialBendPoints;
    
    // 计算所有中间点的角度变化
    for (size_t i = 1; i < points.size() - 1; ++i)
    {
        double x1 = points[i - 1]->getX();
        double y1 = points[i - 1]->getY();
        double x2 = points[i]->getX();
        double y2 = points[i]->getY();
        double x3 = points[i + 1]->getX();
        double y3 = points[i + 1]->getY();
        
        // 计算斜率变化角度
        angles[i - 1] = CalculateSlopeAngle(x1, y1, x2, y2, x3, y3);
    }
    
    // 检测角度变化的连续性和单调性
    for (size_t i = 1; i < points.size() - 1; ++i)
    {
        // 当前点的角度变化
        double angle = angles[i - 1];
        
        // 检查角度是否超过阈值
        if (angle > angleThreshold)
        {
            // 检查前后线段长度是否足够长（避免短线段抖动）
            double prevSegmentLength = segmentLengths[i - 1];
            double nextSegmentLength = segmentLengths[i];
            
            if (prevSegmentLength >= minSegmentLength && nextSegmentLength >= minSegmentLength)
            {
                // 计算弧长比（前后线段长度比例）
                double lengthRatio = std::min(prevSegmentLength, nextSegmentLength) / 
                                    std::max(prevSegmentLength, nextSegmentLength);
                
                // 弧长比阈值，小于此值的拐点被视为不平衡的拐点
                const double MIN_LENGTH_RATIO = 0.2; // 前后段长度至少为1:5
                
                if (lengthRatio >= MIN_LENGTH_RATIO)
                {
                    // 检查角度变化的单调性（如果有前后点）
                    bool isMonotonic = true;
                    
                    // 如果有前一个点，检查角度变化是否单调
                    if (i > 1)
                    {
                        double prevAngle = angles[i - 2];
                        // 如果前一个点的角度变化方向与当前点不一致，则不是单调的
                        if ((prevAngle > EPSILON && angle < -EPSILON) || 
                            (prevAngle < -EPSILON && angle > EPSILON))
                        {
                            isMonotonic = false;
                        }
                    }
                    
                    // 如果有后一个点，检查角度变化是否单调
                    if (i < points.size() - 2 && isMonotonic)
                    {
                        double nextAngle = angles[i];
                        // 如果后一个点的角度变化方向与当前点不一致，则不是单调的
                        if ((angle > EPSILON && nextAngle < -EPSILON) || 
                            (angle < -EPSILON && nextAngle > EPSILON))
                        {
                            isMonotonic = false;
                        }
                    }
                    
                    // 如果角度变化是单调的，则认为是真正的拐点
                    if (isMonotonic)
                    {
                        bendPoints.push_back(i);
                        spdlog::debug("检测到拐点: 索引={}, 角度={}, 弧长比={}", i, angle, lengthRatio);
                    }
                    else
                    {
                        spdlog::debug("拐点角度变化不单调: 索引={}, 角度={}", i, angle);
                    }
                }
                else
                {
                    spdlog::debug("拐点弧长比过小: 索引={}, 弧长比={}", i, lengthRatio);
                }
            }
            else
            {
                spdlog::debug("拐点线段长度不足: 索引={}, 前段长={}, 后段长={}", 
                             i, prevSegmentLength, nextSegmentLength);
            }
        }
    }
    
    // 添加最后一个点
    bendPoints.push_back(points.size() - 1);
    
    // 如果是闭合环，还需要检查首尾点连接处的角度变化
    if (isClosedRing && points.size() >= 3)
    {
        // 获取最后一个点、第一个点和第二个点
        double x1 = points[points.size() - 2]->getX();
        double y1 = points[points.size() - 2]->getY();
        double x2 = points[points.size() - 1]->getX();
        double y2 = points[points.size() - 1]->getY();
        double x3 = points[1]->getX();
        double y3 = points[1]->getY();
        
        // 计算首尾连接处的角度变化
        double angle1 = CalculateSlopeAngle(x1, y1, x2, y2, x3, y3);
        
        // 获取倒数第二个点、第一个点和第二个点
        x1 = points[points.size() - 1]->getX();
        y1 = points[points.size() - 1]->getY();
        x2 = points[0]->getX();
        y2 = points[0]->getY();
        x3 = points[1]->getX();
        y3 = points[1]->getY();
        
        // 计算首尾连接处的角度变化
        double angle2 = CalculateSlopeAngle(x1, y1, x2, y2, x3, y3);
        
        // 记录首尾连接处的角度变化
        spdlog::debug("闭合环首尾连接处角度变化: {} 度和 {} 度", angle1, angle2);
        
        // 如果首尾连接处的角度变化超过阈值，可能需要调整拐点
        // 注意：这里不需要添加新的拐点，因为首尾点已经在拐点列表中
    }
    
    // 如果没有中间拐点，至少保留首尾点
    if (bendPoints.size() == 2)
    {
        spdlog::info("未检测到中间拐点，仅保留首尾点");
    }
}

// 计算相邻拐点之间形成的面域面积
double CalculateBendArea(const std::vector<OGRPoint*>& points, int startIdx, int endIdx)
{
    // 检查索引有效性
    if (startIdx < 0 || endIdx >= static_cast<int>(points.size()) || startIdx >= endIdx)
    {
        spdlog::error("计算面域面积时索引无效: startIdx={}, endIdx={}, points.size()={}", 
                     startIdx, endIdx, points.size());
        return 0.0;
    }
    
    // 如果只有两个点，无法形成面域
    if (endIdx - startIdx < 2)
    {
        return 0.0;
    }
    
    // 使用梯形法计算弧线与弦线之间的面积
    // 弦线：连接起点和终点的直线
    // 弧线：原始折线
    
    // 获取起点和终点
    double x1 = points[startIdx]->getX();
    double y1 = points[startIdx]->getY();
    double x2 = points[endIdx]->getX();
    double y2 = points[endIdx]->getY();
    
    // 计算弦线方程: ax + by + c = 0
    double a = y2 - y1;
    double b = x1 - x2;
    double c = x2 * y1 - x1 * y2;
    double lineLength = std::sqrt(a * a + b * b);
    
    // 如果弦线长度接近于0，则无法计算面积
    if (lineLength < EPSILON)
    {
        return 0.0;
    }
    
    // 使用梯形法计算面积
    double area = 0.0;
    
    // 计算每个点到弦线的距离，并用梯形法累加面积
    for (int i = startIdx; i < endIdx; ++i)
    {
        // 当前点和下一个点
        double x_curr = points[i]->getX();
        double y_curr = points[i]->getY();
        double x_next = points[i + 1]->getX();
        double y_next = points[i + 1]->getY();
        
        // 计算当前点到弦线的距离
        double dist_curr = std::abs(a * x_curr + b * y_curr + c) / lineLength;
        
        // 计算下一个点到弦线的距离
        double dist_next = std::abs(a * x_next + b * y_next + c) / lineLength;
        
        // 计算当前线段的长度
        double dx = x_next - x_curr;
        double dy = y_next - y_curr;
        double segmentLength = std::sqrt(dx * dx + dy * dy);
        
        // 使用梯形公式计算当前线段与弦线之间的面积
        double segmentArea = 0.5 * (dist_curr + dist_next) * segmentLength;
        
        // 累加面积
        area += segmentArea;
    }
    
    // 返回面积
    return area;
}

// 新版本的CheckFeatureBends函数已经完全替代旧版本

// 检查单个要素的弯曲是否符合阈值要求
bool CheckFeatureBends(OGRGeometry* poGeometry, double dThresholdValue, double dSlopeAngleThreshold,
                      const std::string& layerName, long long featureID,
                      std::vector<ProblemPointInfo>& problemPoints)
{
    bool bHasProblems = false;
    
    // 处理线要素
    if (poGeometry->getGeometryType() == wkbLineString || 
        poGeometry->getGeometryType() == wkbLineString25D)
    {
        OGRLineString* poLine = (OGRLineString*)poGeometry;
        int nPoints = poLine->getNumPoints();
        
        // 至少需要3个点才能形成弯曲
        if (nPoints >= 3)
        {
            // 将线要素的点转换为OGRPoint数组
            std::vector<OGRPoint*> points;
            for (int i = 0; i < nPoints; ++i)
            {
                OGRPoint* point = new OGRPoint();
                poLine->getPoint(i, point);
                points.push_back(point);
            }
            
            // 提取拐点
            std::vector<int> bendPoints;
            ExtractBendPoints(points, dSlopeAngleThreshold, bendPoints, false); // 线要素不是闭合环
            
            // 检查相邻拐点之间的弯曲面积
            for (size_t i = 0; i < bendPoints.size() - 1; ++i)
            {
                int startIdx = bendPoints[i];
                int endIdx = bendPoints[i + 1];
                
                // 计算弯曲面域面积
                double area = CalculateBendArea(points, startIdx, endIdx);
                
                // 如果面积小于阈值，则标记为问题
                if (area <= dThresholdValue + EPSILON && area > 0)
                {
                    bHasProblems = true;
                    
                    // 找出弯曲中的中心点作为问题点
                    int midIdx = (startIdx + endIdx) / 2;
                    
                    ProblemPointInfo problemPoint;
                    problemPoint.x = points[midIdx]->getX();
                    problemPoint.y = points[midIdx]->getY();
                    problemPoint.layerName = layerName;
                    problemPoint.featureID = featureID;
                    problemPoint.errorType = "线要素最小弯曲";
                    problemPoint.area = area;
                    problemPoints.push_back(problemPoint);
                    
                    spdlog::debug("检测到线要素弯曲问题: 面积={}, 阈值={}, 起点索引={}, 终点索引={}", 
                                 area, dThresholdValue, startIdx, endIdx);
                }
            }
            
            // 释放内存
            for (auto point : points)
            {
                delete point;
            }
        }
    }
    // 处理多线要素
    else if (poGeometry->getGeometryType() == wkbMultiLineString ||
             poGeometry->getGeometryType() == wkbMultiLineString25D)
    {
        OGRMultiLineString* poMultiLine = (OGRMultiLineString*)poGeometry;
        for (int i = 0; i < poMultiLine->getNumGeometries(); i++)
        {
            OGRLineString* poLine = (OGRLineString*)poMultiLine->getGeometryRef(i);
            std::vector<ProblemPointInfo> lineProblems;
            if (CheckFeatureBends(poLine, dThresholdValue, dSlopeAngleThreshold, layerName, featureID, lineProblems))
            {
                bHasProblems = true;
                problemPoints.insert(problemPoints.end(), lineProblems.begin(), lineProblems.end());
            }
        }
    }
    // 处理面要素
    else if (poGeometry->getGeometryType() == wkbPolygon ||
             poGeometry->getGeometryType() == wkbPolygon25D)
    {
        OGRPolygon* poPolygon = (OGRPolygon*)poGeometry;
        OGRLinearRing* poRing = poPolygon->getExteriorRing();
        
        // 检查外环
        int nPoints = poRing->getNumPoints();
        if (nPoints >= 4) // 闭合多边形至少需要4个点
        {
            // 将环的点转换为OGRPoint数组（去掉最后一个重复点）
            std::vector<OGRPoint*> points;
            for (int i = 0; i < nPoints - 1; ++i)
            {
                OGRPoint* point = new OGRPoint();
                poRing->getPoint(i, point);
                points.push_back(point);
            }
            
            // 提取拐点
            std::vector<int> bendPoints;
            ExtractBendPoints(points, dSlopeAngleThreshold, bendPoints, true); // 多边形外环是闭合环
            
            // 检查相邻拐点之间的弯曲面积
            for (size_t i = 0; i < bendPoints.size() - 1; ++i)
            {
                int startIdx = bendPoints[i];
                int endIdx = bendPoints[i + 1];
                
                // 计算弯曲面域面积
                double area = CalculateBendArea(points, startIdx, endIdx);
                
                // 如果面积小于阈值，则标记为问题
                if (area <= dThresholdValue + EPSILON && area > 0)
                {
                    bHasProblems = true;
                    
                    // 找出弯曲中的中心点作为问题点
                    int midIdx = (startIdx + endIdx) / 2;
                    
                    ProblemPointInfo problemPoint;
                    problemPoint.x = points[midIdx]->getX();
                    problemPoint.y = points[midIdx]->getY();
                    problemPoint.layerName = layerName;
                    problemPoint.featureID = featureID;
                    problemPoint.errorType = "多边形外环最小弯曲";
                    problemPoint.area = area;
                    problemPoints.push_back(problemPoint);
                    
                    spdlog::debug("检测到多边形外环弯曲问题: 面积={}, 阈值={}, 起点索引={}, 终点索引={}", 
                                 area, dThresholdValue, startIdx, endIdx);
                }
            }
            
            // 释放内存
            for (auto point : points)
            {
                delete point;
            }
        }
        
        // 检查内环
        for (int ringIdx = 0; ringIdx < poPolygon->getNumInteriorRings(); ringIdx++)
        {
            OGRLinearRing* poInnerRing = poPolygon->getInteriorRing(ringIdx);
            nPoints = poInnerRing->getNumPoints();
            
            if (nPoints >= 4)
            {
                // 将环的点转换为OGRPoint数组（去掉最后一个重复点）
                std::vector<OGRPoint*> points;
                for (int i = 0; i < nPoints - 1; ++i)
                {
                    OGRPoint* point = new OGRPoint();
                    poInnerRing->getPoint(i, point);
                    points.push_back(point);
                }
                
                // 提取拐点
                std::vector<int> bendPoints;
                ExtractBendPoints(points, dSlopeAngleThreshold, bendPoints, true); // 多边形内环是闭合环
                
                // 检查相邻拐点之间的弯曲面积
                for (size_t i = 0; i < bendPoints.size() - 1; ++i)
                {
                    int startIdx = bendPoints[i];
                    int endIdx = bendPoints[i + 1];
                    
                    // 计算弯曲面域面积
                    double area = CalculateBendArea(points, startIdx, endIdx);
                    
                    // 如果面积小于阈值，则标记为问题
                    if (area <= dThresholdValue + EPSILON && area > 0)
                    {
                        bHasProblems = true;
                        
                        // 找出弯曲中的中心点作为问题点
                        int midIdx = (startIdx + endIdx) / 2;
                        
                        ProblemPointInfo problemPoint;
                        problemPoint.x = points[midIdx]->getX();
                        problemPoint.y = points[midIdx]->getY();
                        problemPoint.layerName = layerName;
                        problemPoint.featureID = featureID;
                        problemPoint.errorType = "多边形内环最小弯曲";
                        problemPoint.area = area;
                        problemPoints.push_back(problemPoint);
                        
                        spdlog::debug("检测到多边形内环弯曲问题: 面积={}, 阈值={}, 起点索引={}, 终点索引={}", 
                                     area, dThresholdValue, startIdx, endIdx);
                    }
                }
                
                // 释放内存
                for (auto point : points)
                {
                    delete point;
                }
            }
        }
    }
    // 处理多面要素
    else if (poGeometry->getGeometryType() == wkbMultiPolygon ||
             poGeometry->getGeometryType() == wkbMultiPolygon25D)
    {
        OGRMultiPolygon* poMultiPolygon = (OGRMultiPolygon*)poGeometry;
        for (int i = 0; i < poMultiPolygon->getNumGeometries(); i++)
        {
            OGRPolygon* poPolygon = (OGRPolygon*)poMultiPolygon->getGeometryRef(i);
            std::vector<ProblemPointInfo> polygonProblems;
            if (CheckFeatureBends(poPolygon, dThresholdValue, dSlopeAngleThreshold, layerName, featureID, polygonProblems))
            {
                bHasProblems = true;
                problemPoints.insert(problemPoints.end(), polygonProblems.begin(), polygonProblems.end());
            }
        }
    }
    
    return bHasProblems;
}

// 处理单个SHP文件
SE_Error ProcessShpFile(const char* szShpFilePath, double dThresholdValue, double dSlopeAngleThreshold,
                        GDALDataset* poErrorDS, OGRLayer* poErrorLayer, OGRLayer* poBendPointLayer,
                        bool& bHasErrors, int& nTotalFeatures, int& nErrorFeatures,
                        std::vector<ProblemPointInfo>& allProblemPoints)
{
    GDALDataset* poDS = (GDALDataset*)GDALOpenEx(szShpFilePath, GDAL_OF_VECTOR, NULL, NULL, NULL);
    if (!poDS)
    {
        spdlog::error("无法打开SHP文件: {}", szShpFilePath);
        return SE_ERROR_GDAL_OPEN_FAILED;
    }

    OGRLayer* poLayer = poDS->GetLayer(0);
    if (!poLayer)
    {
        GDALClose(poDS);
        spdlog::error("无法获取图层: {}", szShpFilePath);
        return SE_ERROR_LAYER_NOT_FOUND;
    }

    // 获取图层名称
    std::string layerName = poLayer->GetName();
    
    // CRS校验：检查是否为地理坐标系
    OGRSpatialReference* poSRS = poLayer->GetSpatialRef();
    if (poSRS && poSRS->IsGeographic())
    {
        spdlog::error("图层 {} 使用地理坐标系（度），无法直接进行面积计算，请先投影到米单位坐标系", layerName);
        GDALClose(poDS);
        return SE_ERROR_GEOGRAPHIC_CRS;
    }
    spdlog::info("处理图层: {}", layerName);

    // 创建要素定义
    OGRFeatureDefn* poFDefn = poLayer->GetLayerDefn();
    
    // 遍历所有要素
    poLayer->ResetReading();
    OGRFeature* poFeature;
    while ((poFeature = poLayer->GetNextFeature()) != NULL)
    {
        nTotalFeatures++;
        OGRGeometry* poGeometry = poFeature->GetGeometryRef();
        if (!poGeometry)
        {
            OGRFeature::DestroyFeature(poFeature);
            continue;
        }

        // 检查几何类型是否为线或面
        OGRwkbGeometryType geomType = poGeometry->getGeometryType();
        if (wkbFlatten(geomType) != wkbLineString && 
            wkbFlatten(geomType) != wkbMultiLineString && 
            wkbFlatten(geomType) != wkbPolygon && 
            wkbFlatten(geomType) != wkbMultiPolygon)
        {
            OGRFeature::DestroyFeature(poFeature);
            continue;
        }

        // 检查弯曲是否符合阈值要求
        std::vector<ProblemPointInfo> problemPoints;
        if (CheckFeatureBends(poGeometry, dThresholdValue, dSlopeAngleThreshold, layerName, poFeature->GetFID(), problemPoints))
        {
            nErrorFeatures++;
            
            // 将问题点添加到总集合中
            allProblemPoints.insert(allProblemPoints.end(), problemPoints.begin(), problemPoints.end());
            
            // 创建错误要素
            OGRFeature* poErrorFeature = OGRFeature::CreateFeature(poErrorLayer->GetLayerDefn());
            
            // 复制原始几何
            poErrorFeature->SetGeometry(poGeometry);
            
            // 设置属性（直接使用UTF-8编码以避免中文乱码）
            poErrorFeature->SetField("LayerName", layerName.c_str());
            poErrorFeature->SetField("FeatureID", poFeature->GetFID());
            poErrorFeature->SetField("ErrorType", "最小弯曲阈值控制");
            poErrorFeature->SetField("Description", "存在小于阈值的弯曲");
            
            // 添加到错误图层
            if (poErrorLayer->CreateFeature(poErrorFeature) != OGRERR_NONE)
            {
                spdlog::error("创建错误要素失败");
            }
            
            // 为每个问题点创建单独的点要素：已改为统一输出 _ProblemPoints.shp，因此这里仅在提供子图层时才写入
            if (poBendPointLayer)
            {
                for (const auto& problemPoint : problemPoints)
                {
                    OGRFeature* poBendFeature = OGRFeature::CreateFeature(poBendPointLayer->GetLayerDefn());
                    
                    // 创建点几何
                    OGRPoint point;
                    point.setX(problemPoint.x);
                    point.setY(problemPoint.y);
                    poBendFeature->SetGeometry(&point);
                    
                    // 设置属性（直接使用UTF-8编码）
                    poBendFeature->SetField("LayerName", layerName.c_str());
                    poBendFeature->SetField("FeatureID", poFeature->GetFID());
                    poBendFeature->SetField("ErrorType", problemPoint.errorType.c_str());
                    poBendFeature->SetField("Area", problemPoint.area);
                    poBendFeature->SetField("Threshold", dThresholdValue);
                    
                    // 添加到弯曲点子图层
                    if (poBendPointLayer->CreateFeature(poBendFeature) != OGRERR_NONE)
                    {
                        spdlog::error("创建弯曲点要素失败");
                    }
                    
                    OGRFeature::DestroyFeature(poBendFeature);
                }
            }
            
            // 标记存在错误
            bHasErrors = true;
            
            OGRFeature::DestroyFeature(poErrorFeature);
        }
        
        OGRFeature::DestroyFeature(poFeature);
    }
    
    GDALClose(poDS);
    return SE_ERROR_NONE;
}

// 创建问题点图层
SE_Error CreateProblemPointsLayer(const std::vector<ProblemPointInfo>& problemPoints, 
                                  const char* szOutputPath,
                                  OGRSpatialReference* poSRS)
{
    if (problemPoints.empty())
    {
        spdlog::info("没有问题点，跳过问题点图层创建");
        return SE_ERROR_NONE;
    }

    // 获取ESRI Shapefile驱动
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (!poDriver)
    {
        spdlog::error("无法获取ESRI Shapefile驱动");
        return SE_ERROR_GDAL_DRIVER_NOT_FOUND;
    }

    // 创建数据源
    GDALDataset* poDS = poDriver->Create(szOutputPath, 0, 0, 0, GDT_Unknown, NULL);
    if (!poDS)
    {
        spdlog::error("无法创建问题点图层文件: {}", szOutputPath);
        return SE_ERROR_GDAL_CREATE_FAILED;
    }

    // 图层创建选项：强制UTF-8编码以避免中文乱码
    char** papszLCO = NULL;
    papszLCO = CSLAddString(papszLCO, "ENCODING=UTF-8");

    // 创建图层，带坐标系与编码
    OGRLayer* poLayer = poDS->CreateLayer("MinBendThresholdProblemPoints", poSRS, wkbPoint, papszLCO);
    if (!poLayer)
    {
        GDALClose(poDS);
        spdlog::error("无法创建问题点图层");
        return SE_ERROR_LAYER_CREATE_FAILED;
    }

    // 创建字段
    OGRFieldDefn oFieldLayerName("LayerName", OFTString);
    oFieldLayerName.SetWidth(100);
    if (poLayer->CreateField(&oFieldLayerName) != OGRERR_NONE)
    {
        GDALClose(poDS);
        spdlog::error("创建LayerName字段失败");
        return SE_ERROR_FIELD_CREATE_FAILED;
    }

    OGRFieldDefn oFieldFeatureID("FeatureID", OFTInteger64);
    if (poLayer->CreateField(&oFieldFeatureID) != OGRERR_NONE)
    {
        GDALClose(poDS);
        spdlog::error("创建FeatureID字段失败");
        return SE_ERROR_FIELD_CREATE_FAILED;
    }

    OGRFieldDefn oFieldErrorType("ErrorType", OFTString);
    oFieldErrorType.SetWidth(50);
    if (poLayer->CreateField(&oFieldErrorType) != OGRERR_NONE)
    {
        GDALClose(poDS);
        spdlog::error("创建ErrorType字段失败");
        return SE_ERROR_FIELD_CREATE_FAILED;
    }

    OGRFieldDefn oFieldArea("Area", OFTReal);
    if (poLayer->CreateField(&oFieldArea) != OGRERR_NONE)
    {
        GDALClose(poDS);
        spdlog::error("创建Area字段失败");
        return SE_ERROR_FIELD_CREATE_FAILED;
    }

    // 添加问题点要素
    for (const auto& problemPoint : problemPoints)
    {
        OGRFeature* poFeature = OGRFeature::CreateFeature(poLayer->GetLayerDefn());
        
        // 创建点几何
        OGRPoint point;
        point.setX(problemPoint.x);
        point.setY(problemPoint.y);
        poFeature->SetGeometry(&point);
        
        // 设置属性（统一使用 UTF-8）
        std::string layerNameUtf8 = QString::fromUtf8(problemPoint.layerName.c_str()).toUtf8().toStdString();
        std::string errorTypeUtf8 = QString::fromUtf8(problemPoint.errorType.c_str()).toUtf8().toStdString();
        poFeature->SetField("LayerName", layerNameUtf8.c_str());
        poFeature->SetField("FeatureID", problemPoint.featureID);
        poFeature->SetField("ErrorType", errorTypeUtf8.c_str());
        poFeature->SetField("Area", problemPoint.area);
        
        // 添加要素到图层
        if (poLayer->CreateFeature(poFeature) != OGRERR_NONE)
        {
            spdlog::error("创建问题点要素失败");
            OGRFeature::DestroyFeature(poFeature);
            GDALClose(poDS);
            return SE_ERROR_FEATURE_CREATE_FAILED;
        }
        
        OGRFeature::DestroyFeature(poFeature);
    }

    GDALClose(poDS);
    spdlog::info("问题点图层创建完成: {}, 共{}个问题点", szOutputPath, problemPoints.size());
    return SE_ERROR_NONE;
}

// 最小弯曲阈值控制检查主函数
SE_Error MinBendThresholdCheck(
    const char* szParsedDataPath,
    const char* szSchemaXmlPath,
    const char* szOutputCheckReportPath,
    const char* szOutputCheckResultLayerPath,
    const char* szOutputLogFilePath,
    int nScale)
{
    // 初始化GDAL
    GDALAllRegister();
    // 统一GDAL文件名与Shapefile编码为UTF-8，确保中文路径与属性不乱码
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");
    
    // 配置日志
    spdlog::set_level(spdlog::level::info);
    try {
        // 先写入 UTF-8 BOM，避免 Windows 记事本中文乱码
        {
            // 通过 std::filesystem::u8path 使用UTF-8路径安全写入BOM
            std::filesystem::path bomPath = std::filesystem::u8path(szOutputLogFilePath);
            std::ofstream bom_out(bomPath, std::ios::binary | std::ios::trunc);
            const char bom[3] = { char(0xEF), char(0xBB), char(0xBF) };
            bom_out.write(bom, 3);
        }
        // 创建一个文件日志接收器（Windows下宽字符路径），使用不截断方式（保留BOM），追加写入
        #ifdef _WIN32
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_st>(
            QString::fromUtf8(szOutputLogFilePath).toStdWString(), false);
        #else
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_st>(szOutputLogFilePath, false);
        #endif
        // 设置日志格式，包含时间、级别和消息
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        // 创建日志记录器并设置为默认
        auto file_logger = std::make_shared<spdlog::logger>("min_bend_threshold_logger", file_sink);
        // 设置日志级别
        file_logger->set_level(spdlog::level::info);
        // 刷新策略：每条日志立即写入文件
        file_logger->flush_on(spdlog::level::info);
        // 设置为默认日志记录器
        spdlog::set_default_logger(file_logger);
        
        // 写入一条初始日志，确认日志系统已启动
        spdlog::info("日志系统初始化完成");
    } catch (const std::exception& e) {
        // 如果创建文件日志器失败，使用控制台日志器
        spdlog::warn("无法创建文件日志器: {}", e.what());
    }
    
    spdlog::info("开始最小弯曲阈值控制检查");
    
    // 解析XML获取阈值
    double dThresholdValue = 0.0;
    double dSlopeAngleThreshold = 10.0; // 默认值为10度
    SE_Error err = ParseXmlForMinBendThreshold(szSchemaXmlPath, dThresholdValue, dSlopeAngleThreshold, nScale);
    if (err != SE_ERROR_NONE)
    {
        spdlog::error("解析XML获取最小弯曲阈值失败: {}", err);
        return err;
    }
    
    spdlog::info("最小弯曲阈值: {} 平方毫米", dThresholdValue);
    spdlog::info("斜率判断阈值: {} 度", dSlopeAngleThreshold);
    
    // 坐标系为投影坐标（单位米），无需比例尺换算
    double dRealWorldThreshold = dThresholdValue * nScale * nScale * 1e-6;
    spdlog::info("图上最小弯曲阈值: {} 平方毫米", dThresholdValue);
    spdlog::info("实地面积阈值: {} 平方米", dRealWorldThreshold);
    
    // 创建JSON对象
    QJsonObject reportJson;
    
    // 设置检查项信息
    QJsonObject checkItem;
    checkItem["id"] = "106";
    checkItem["name"] = "最小弯曲阈值控制";
    checkItem["status"] = "质检完成"; // 默认状态，如果出错会更新
    checkItem["err_exist"] = false;   // 默认无错误，如果检测到错误会更新
    checkItem["info"] = "不存在过小弯曲要素"; // 默认信息，如果检测到错误会更新
    
    // 在创建错误图层之前，尝试从输入数据中获取坐标系，以保证输出图层坐标系与输入一致
    OGRSpatialReference* poOutSRS = nullptr;
    {
        // 解析输入路径，获取第一个SHP文件用于提取SRS
        std::vector<std::string> shpCandidates;
        std::string dataPathCandidate(szParsedDataPath);
        size_t posCandidate = 0;
        while ((posCandidate = dataPathCandidate.find(';')) != std::string::npos)
        {
            std::string token = dataPathCandidate.substr(0, posCandidate);
            shpCandidates.push_back(token);
            dataPathCandidate.erase(0, posCandidate + 1);
        }
        if (!dataPathCandidate.empty())
        {
            shpCandidates.push_back(dataPathCandidate);
        }

        std::string firstShpPath;
        if (!shpCandidates.empty())
        {
            // 如果是目录，读取目录中的第一个shp；否则直接使用第一个文件
            VSIStatBufL statBuf;
            if (VSIStatL(shpCandidates[0].c_str(), &statBuf) == 0 && VSI_ISDIR(statBuf.st_mode))
            {
                char** papszFiles = VSIReadDir(shpCandidates[0].c_str());
                for (int i = 0; papszFiles && papszFiles[i]; i++)
                {
                    std::string filename = papszFiles[i];
                    if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".shp")
                    {
                        firstShpPath = shpCandidates[0] + "/" + filename;
                        break;
                    }
                }
                CSLDestroy(papszFiles);
            }
            else
            {
                firstShpPath = shpCandidates[0];
            }
        }

        if (!firstShpPath.empty())
        {
            GDALDataset* poFirstDS = (GDALDataset*)GDALOpenEx(firstShpPath.c_str(), GDAL_OF_VECTOR, NULL, NULL, NULL);
            if (poFirstDS)
            {
                OGRLayer* poFirstLayer = poFirstDS->GetLayer(0);
                if (poFirstLayer)
                {
                    OGRSpatialReference* poFirstSRS = poFirstLayer->GetSpatialRef();
                    if (poFirstSRS)
                    {
                        poOutSRS = poFirstSRS->Clone();
                        spdlog::info("输出图层坐标系来自首个输入图层");
                    }
                }
                GDALClose(poFirstDS);
            }
        }
    }

    // 创建错误图层
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (!poDriver)
    {
        spdlog::error("无法获取ESRI Shapefile驱动");
        return SE_ERROR_FAILURE;
    }
    
    GDALDataset* poErrorDS = poDriver->Create(szOutputCheckResultLayerPath, 0, 0, 0, GDT_Unknown, NULL);
    if (!poErrorDS)
    {
        spdlog::error("无法创建错误图层文件: {}", szOutputCheckResultLayerPath);
        return SE_ERROR_FAILURE;
    }
    
    // 图层创建选项：强制UTF-8编码以避免中文乱码
    char** papszLCO = NULL;
    papszLCO = CSLAddString(papszLCO, "ENCODING=UTF-8");

    // 创建错误图层（用于存储原始要素），带坐标系与编码
    OGRLayer* poErrorLayer = poErrorDS->CreateLayer("MinBendThresholdErrors", poOutSRS, wkbUnknown, papszLCO);
    if (!poErrorLayer)
    {
        spdlog::error("无法创建错误图层");
        GDALClose(poErrorDS);
        return SE_ERROR_FAILURE;
    }

    // 不再在同一数据集中创建 BendPoints 子图层，统一由后续独立文件 _ProblemPoints.shp 输出
    OGRLayer* poBendPointLayer = nullptr;

    // 添加属性字段
    OGRFieldDefn oFieldLayerName("LayerName", OFTString);
    oFieldLayerName.SetWidth(100);
    if (poErrorLayer->CreateField(&oFieldLayerName) != OGRERR_NONE)
    {
        spdlog::error("创建字段失败: LayerName");
    }
    
    OGRFieldDefn oFieldFeatureID("FeatureID", OFTInteger);
    if (poErrorLayer->CreateField(&oFieldFeatureID) != OGRERR_NONE)
    {
        spdlog::error("创建字段失败: FeatureID");
    }
    
    OGRFieldDefn oFieldErrorType("ErrorType", OFTString);
    oFieldErrorType.SetWidth(50);
    if (poErrorLayer->CreateField(&oFieldErrorType) != OGRERR_NONE)
    {
        spdlog::error("创建字段失败: ErrorType");
    }
    
    OGRFieldDefn oFieldDescription("Description", OFTString);
    oFieldDescription.SetWidth(255);
    if (poErrorLayer->CreateField(&oFieldDescription) != OGRERR_NONE)
    {
        spdlog::error("创建字段失败: Description");
    }
    
    // BendPoints 子图层字段创建逻辑移除
    
    // 处理输入数据
    std::vector<std::string> shpFiles;
    
    // 检查是否为多个文件路径（用分号分隔）
    std::string dataPath(szParsedDataPath);
    size_t pos = 0;
    while ((pos = dataPath.find(';')) != std::string::npos)
    {
        std::string token = dataPath.substr(0, pos);
        shpFiles.push_back(token);
        dataPath.erase(0, pos + 1);
    }
    
    // 添加最后一个文件或目录
    if (!dataPath.empty())
    {
        shpFiles.push_back(dataPath);
    }
    
    // 统计数据
    int nTotalFeatures = 0;
    int nErrorFeatures = 0;
    
    // 收集所有问题点
    std::vector<ProblemPointInfo> allProblemPoints;
    bool bHasErrors = false;
    
    // 处理所有SHP文件
    for (const auto& shpFile : shpFiles)
    {
        // 检查是否为目录
        VSIStatBufL statBuf;
        if (VSIStatL(shpFile.c_str(), &statBuf) == 0 && VSI_ISDIR(statBuf.st_mode))
        {
            // 是目录，获取所有SHP文件
            char** papszFiles = VSIReadDir(shpFile.c_str());
            for (int i = 0; papszFiles && papszFiles[i]; i++)
            {
                std::string filename = papszFiles[i];
                if (filename.size() > 4 && 
                    filename.substr(filename.size() - 4) == ".shp")
                {
                    std::string fullPath = shpFile + "/" + filename;
                    ProcessShpFile(fullPath.c_str(), dRealWorldThreshold, dSlopeAngleThreshold,
                                  poErrorDS, poErrorLayer, poBendPointLayer, bHasErrors, 
                                  nTotalFeatures, nErrorFeatures, allProblemPoints);
                }
            }
            CSLDestroy(papszFiles);
        }
        else
        {
            // 是文件，直接处理
            ProcessShpFile(shpFile.c_str(), dRealWorldThreshold, dSlopeAngleThreshold,
                          poErrorDS, poErrorLayer, poBendPointLayer, bHasErrors, 
                          nTotalFeatures, nErrorFeatures, allProblemPoints);
        }
    }
    
    // 更新JSON状态
    if (bHasErrors)
    {
        checkItem["err_exist"] = true;
        
        // 从XML文件名获取图层名称
        std::string layerName;
        std::string xmlPath(szSchemaXmlPath);
        size_t lastSlash = xmlPath.find_last_of("/\\");
        if (lastSlash != std::string::npos) {
            layerName = xmlPath.substr(lastSlash + 1);
        } else {
            layerName = xmlPath;
        }
        
        // 移除扩展名
        size_t lastDot = layerName.find_last_of(".");
        if (lastDot != std::string::npos) {
            layerName = layerName.substr(0, lastDot);
        }
        
        checkItem["info"] = QString::fromStdString(layerName + "图层存在过小弯曲要素");
    }
    else
    {
        checkItem["info"] = QString::fromUtf8("不存在过小弯曲要素");
    }
    
    // 创建问题点图层
    if (!allProblemPoints.empty())
    {
        // 生成问题点图层路径
        std::string problemPointsPath = szOutputCheckResultLayerPath;
        size_t lastDot = problemPointsPath.find_last_of('.');
        if (lastDot != std::string::npos)
        {
            problemPointsPath = problemPointsPath.substr(0, lastDot) + "_ProblemPoints.shp";
        }
        else
        {
            problemPointsPath += "_ProblemPoints.shp";
        }
        
        SE_Error result = CreateProblemPointsLayer(allProblemPoints, problemPointsPath.c_str(), poErrorLayer->GetSpatialRef());
        if (result != SE_ERROR_NONE)
        {
            spdlog::warn("创建问题点图层失败，但主检查流程继续");
            checkItem["status"] = "质检失败";
        }
    }
    
    // 关闭资源
    GDALClose(poErrorDS);
    
    // 将检查项添加到报告JSON
    reportJson["check_item"] = checkItem;
    
    // 写入JSON文件
    QFile jsonFile(szOutputCheckReportPath);
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QJsonDocument doc(reportJson);
        jsonFile.write(doc.toJson(QJsonDocument::Indented)); // 缩进格式，美化输出
        jsonFile.close();
        spdlog::info("JSON报告文件已生成: {}", szOutputCheckReportPath);
    }
    else
    {
        spdlog::error("无法创建JSON报告文件: {}", szOutputCheckReportPath);
        return SE_ERROR_FAILURE;
    }
    
    spdlog::info("最小弯曲阈值控制检查完成");
    spdlog::info("总要素数: {}", nTotalFeatures);
    spdlog::info("存在问题要素数: {}", nErrorFeatures);
    spdlog::info("总问题点数: {}", allProblemPoints.size());
    
    return SE_ERROR_NONE;
}