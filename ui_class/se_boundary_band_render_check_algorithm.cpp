#define _HAS_STD_BYTE 0
#include "se_boundary_band_render_check_algorithm.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <cpl_string.h>
#include <QtXml/QDomDocument>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QVector>
#include <QMap>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QSet>
#include <algorithm>
#include <limits>
#include <cmath>
#include <functional>
#include <cstring>

static void setDefaultBoundaryBandConfig(SE_BoundaryBandRenderRuleConfig& config)
{
    std::memset(&config, 0, sizeof(SE_BoundaryBandRenderRuleConfig));
    std::strncpy(config.szBoundaryLevelValue, "", sizeof(config.szBoundaryLevelValue) - 1);
    std::strncpy(config.szBoundaryLevelField, "boundary_lv", sizeof(config.szBoundaryLevelField) - 1);
    std::strncpy(config.szColorField, "fill_rgb", sizeof(config.szColorField) - 1);
    std::strncpy(config.szGradientStartColorField, "start_rgb", sizeof(config.szGradientStartColorField) - 1);
    std::strncpy(config.szGradientEndColorField, "end_rgb", sizeof(config.szGradientEndColorField) - 1);
    std::strncpy(config.szGradientDirectionField, "grad_dir", sizeof(config.szGradientDirectionField) - 1);
    std::strncpy(config.szGradientFunctionField, "grad_func", sizeof(config.szGradientFunctionField) - 1);
    std::strncpy(config.szExpectedSide, "inside", sizeof(config.szExpectedSide) - 1);
    std::strncpy(config.szStandardFillColor, "", sizeof(config.szStandardFillColor) - 1);
    std::strncpy(config.szStandardGradientStartColor, "", sizeof(config.szStandardGradientStartColor) - 1);
    std::strncpy(config.szStandardGradientEndColor, "", sizeof(config.szStandardGradientEndColor) - 1);
    std::strncpy(config.szStandardGradientDirection, "", sizeof(config.szStandardGradientDirection) - 1);
    std::strncpy(config.szStandardGradientFunction, "", sizeof(config.szStandardGradientFunction) - 1);
    config.fStandardWidthMm = 0.8;
    config.fWidthToleranceMm = 0.1;
    config.fUniformityCvThreshold = 0.15;
    config.fSampleIntervalMm = 5.0;
    config.fSampleDensityPerMm = 0.0;
    config.fSideOverflowToleranceRatio = 0.05;
    config.fSideProfileLengthFactor = 4.0;
    config.fMinFragmentAreaMm2 = 0.05;
    config.fComplexGeometryToleranceMm = 0.0;
    config.fSharpCornerMinAngleDeg = 45.0;
    config.fSharpCornerLocalOverlapToleranceMm = 0.10;
    config.fDistortionWidthRatioThreshold = 2.0;
    config.fColorTolerance = 8.0;
    config.nScale = 0;
    config.bCheckGeometryValidity = true;
    config.bCheckColor = false;
    config.bCheckGradient = false;
}

static void copyRuleValue(char* target, size_t targetSize, const QString& value)
{
    if (target == nullptr || targetSize == 0) {
        return;
    }
    QByteArray bytes = value.trimmed().toUtf8();
    std::strncpy(target, bytes.constData(), targetSize - 1);
    target[targetSize - 1] = '\0';
}

static bool parseBoolRule(const QString& value, bool defaultValue)
{
    const QString v = value.trimmed().toLower();
    if (v.isEmpty()) {
        return defaultValue;
    }
    return v == QStringLiteral("1") || v == QStringLiteral("true") ||
           v == QStringLiteral("是") || v == QStringLiteral("yes");
}

static bool isBoundaryBandCheckItemName(const QString& checkName)
{
    return checkName.isEmpty() ||
           checkName == QStringLiteral("境界线色带渲染合规性检查") ||
           checkName == QStringLiteral("境界线色带检查") ||
           checkName == QStringLiteral("色带渲染合规性检查");
}

static void applyBoundaryBandRule(const QString& ruleName, const QString& ruleValue, SE_BoundaryBandRenderRuleConfig& config)
{
    const QString name = ruleName.trimmed();
    const QString value = ruleValue.trimmed();

    if (name == QStringLiteral("边界等级") || name == QStringLiteral("适用边界等级")) {
        copyRuleValue(config.szBoundaryLevelValue, sizeof(config.szBoundaryLevelValue), value);
    } else if (name == QStringLiteral("边界等级字段")) {
        copyRuleValue(config.szBoundaryLevelField, sizeof(config.szBoundaryLevelField), value);
    } else if (name == QStringLiteral("颜色字段") || name == QStringLiteral("填充颜色字段")) {
        copyRuleValue(config.szColorField, sizeof(config.szColorField), value);
    } else if (name == QStringLiteral("渐变起始色字段")) {
        copyRuleValue(config.szGradientStartColorField, sizeof(config.szGradientStartColorField), value);
    } else if (name == QStringLiteral("渐变终止色字段")) {
        copyRuleValue(config.szGradientEndColorField, sizeof(config.szGradientEndColorField), value);
    } else if (name == QStringLiteral("渐变方向字段")) {
        copyRuleValue(config.szGradientDirectionField, sizeof(config.szGradientDirectionField), value);
    } else if (name == QStringLiteral("渐变函数字段") || name == QStringLiteral("渐变类型字段")) {
        copyRuleValue(config.szGradientFunctionField, sizeof(config.szGradientFunctionField), value);
    } else if (name == QStringLiteral("挂接方向") || name == QStringLiteral("色带方向")) {
        copyRuleValue(config.szExpectedSide, sizeof(config.szExpectedSide), value);
    } else if (name == QStringLiteral("标准填充颜色") || name == QStringLiteral("标准颜色")) {
        copyRuleValue(config.szStandardFillColor, sizeof(config.szStandardFillColor), value);
    } else if (name == QStringLiteral("标准渐变起始色")) {
        copyRuleValue(config.szStandardGradientStartColor, sizeof(config.szStandardGradientStartColor), value);
    } else if (name == QStringLiteral("标准渐变终止色")) {
        copyRuleValue(config.szStandardGradientEndColor, sizeof(config.szStandardGradientEndColor), value);
    } else if (name == QStringLiteral("标准渐变方向")) {
        copyRuleValue(config.szStandardGradientDirection, sizeof(config.szStandardGradientDirection), value);
    } else if (name == QStringLiteral("标准渐变函数类型") || name == QStringLiteral("标准渐变类型")) {
        copyRuleValue(config.szStandardGradientFunction, sizeof(config.szStandardGradientFunction), value);
    } else if (name == QStringLiteral("标准宽度mm") || name == QStringLiteral("标准宽度")) {
        config.fStandardWidthMm = value.toDouble();
    } else if (name == QStringLiteral("宽度容差mm") || name == QStringLiteral("宽度容差")) {
        config.fWidthToleranceMm = value.toDouble();
    } else if (name == QStringLiteral("均匀度变异系数阈值")) {
        config.fUniformityCvThreshold = value.toDouble();
    } else if (name == QStringLiteral("采样间隔mm") || name == QStringLiteral("采样间隔") || name == QStringLiteral("宽度采样间隔mm")) {
        config.fSampleIntervalMm = value.toDouble();
    } else if (name == QStringLiteral("宽度采样密度") || name == QStringLiteral("采样密度")) {
        config.fSampleDensityPerMm = value.toDouble();
    } else if (name == QStringLiteral("挂接方向外侧容许比例") || name == QStringLiteral("挂接方向错误容许比例") || name == QStringLiteral("挂接方向容差比例")) {
        config.fSideOverflowToleranceRatio = value.toDouble();
    } else if (name == QStringLiteral("挂接方向剖面长度倍率") || name == QStringLiteral("挂接方向采样剖面长度倍率")) {
        config.fSideProfileLengthFactor = value.toDouble();
    } else if (name == QStringLiteral("最小碎片面积mm2")) {
        config.fMinFragmentAreaMm2 = value.toDouble();
    } else if (name == QStringLiteral("复杂边界几何容差mm") || name == QStringLiteral("几何自相交与扭曲检测容差mm")) {
        config.fComplexGeometryToleranceMm = value.toDouble();
    } else if (name == QStringLiteral("尖角检测最小夹角deg") || name == QStringLiteral("尖角检测最小夹角") || name == QStringLiteral("尖角最小夹角deg")) {
        config.fSharpCornerMinAngleDeg = value.toDouble();
    } else if (name == QStringLiteral("尖角局部重叠容差mm") || name == QStringLiteral("尖角局部检测容差mm")) {
        config.fSharpCornerLocalOverlapToleranceMm = value.toDouble();
    } else if (name == QStringLiteral("扭曲宽度比阈值") || name == QStringLiteral("扭曲细长比阈值")) {
        config.fDistortionWidthRatioThreshold = value.toDouble();
    } else if (name == QStringLiteral("颜色容差")) {
        config.fColorTolerance = value.toDouble();
    } else if (name == QStringLiteral("检查几何合法性")) {
        config.bCheckGeometryValidity = parseBoolRule(value, config.bCheckGeometryValidity);
    } else if (name == QStringLiteral("检查颜色")) {
        config.bCheckColor = parseBoolRule(value, config.bCheckColor);
    } else if (name == QStringLiteral("检查渐变")) {
        config.bCheckGradient = parseBoolRule(value, config.bCheckGradient);
    }
}

static void applyBoundaryBandRulesFromItem(const QDomElement& itemElement, SE_BoundaryBandRenderRuleConfig& config)
{
    const QDomNodeList ruleNodes = itemElement.elementsByTagName("rule");
    for (int j = 0; j < ruleNodes.count(); ++j) {
        const QDomElement ruleElement = ruleNodes.at(j).toElement();
        applyBoundaryBandRule(
            ruleElement.firstChildElement("name").text(),
            ruleElement.firstChildElement("value").text(),
            config);
    }
}

static void applyBoundaryBandScaleFromDocument(const QDomDocument& doc, SE_BoundaryBandRenderRuleConfig& config)
{
    QString scaleText = doc.documentElement().firstChildElement("scale").firstChildElement("value").text().trimmed();
    if (scaleText.isEmpty()) {
        return;
    }
    scaleText.remove(QStringLiteral("1:"));
    bool ok = false;
    const int scale = scaleText.toInt(&ok);
    if (ok && scale > 0) {
        config.nScale = scale;
    }
}

static SE_Error loadBoundaryBandXmlDocument(const char* szSchemaXmlPath, QDomDocument& doc)
{
    if (szSchemaXmlPath == nullptr || std::strlen(szSchemaXmlPath) == 0) {
        return SE_ERROR_NONE;
    }

    QFile file(QString::fromUtf8(szSchemaXmlPath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return SE_ERROR_FILE_NOT_FOUND;
    }

    QString errorMsg;
    int errorLine = 0;
    int errorColumn = 0;
    if (!doc.setContent(&file, &errorMsg, &errorLine, &errorColumn)) {
        file.close();
        return SE_ERROR_CORRUPT_DATA;
    }
    file.close();
    return SE_ERROR_NONE;
}

static SE_Error parseXmlForBoundaryBandRenderByLevel(
    const char* szSchemaXmlPath,
    const QString& targetBoundaryLevel,
    SE_BoundaryBandRenderRuleConfig& config,
    bool& matchedLevelRule)
{
    setDefaultBoundaryBandConfig(config);
    matchedLevelRule = false;
    if (szSchemaXmlPath == nullptr || std::strlen(szSchemaXmlPath) == 0) {
        return SE_ERROR_NONE;
    }

    QDomDocument doc;
    const SE_Error loadError = loadBoundaryBandXmlDocument(szSchemaXmlPath, doc);
    if (loadError != SE_ERROR_NONE) {
        return loadError;
    }

    applyBoundaryBandScaleFromDocument(doc, config);

    const QString targetLevel = targetBoundaryLevel.trimmed();
    const QDomNodeList checkItems = doc.elementsByTagName("check_item");
    QVector<QDomElement> commonItems;
    QVector<QDomElement> matchedItems;

    for (int i = 0; i < checkItems.count(); ++i) {
        const QDomElement itemElement = checkItems.at(i).toElement();
        const QString checkName = itemElement.firstChildElement("name").text().trimmed();
        if (!isBoundaryBandCheckItemName(checkName)) {
            continue;
        }

        SE_BoundaryBandRenderRuleConfig previewConfig;
        setDefaultBoundaryBandConfig(previewConfig);
        applyBoundaryBandRulesFromItem(itemElement, previewConfig);
        const QString ruleLevel = QString::fromUtf8(previewConfig.szBoundaryLevelValue).trimmed();
        if (ruleLevel.isEmpty()) {
            commonItems.append(itemElement);
        } else if (!targetLevel.isEmpty() && ruleLevel == targetLevel) {
            matchedItems.append(itemElement);
        }
    }

    for (int i = 0; i < commonItems.size(); ++i) {
        applyBoundaryBandRulesFromItem(commonItems.at(i), config);
    }
    if (!matchedItems.isEmpty()) {
        matchedLevelRule = true;
        for (int i = 0; i < matchedItems.size(); ++i) {
            applyBoundaryBandRulesFromItem(matchedItems.at(i), config);
        }
    }

    return SE_ERROR_NONE;
}

SE_Error ParseXmlForBoundaryBandRender(const char* szSchemaXmlPath, SE_BoundaryBandRenderRuleConfig& config)
{
    bool matchedLevelRule = false;
    return parseXmlForBoundaryBandRenderByLevel(szSchemaXmlPath, QString(), config, matchedLevelRule);
}

struct ParsedColor
{
    double r;
    double g;
    double b;
    bool valid;
};

static QString normalizeColorText(const QString& colorText)
{
    QString text = colorText.trimmed();
    text.remove(QStringLiteral("rgb"), Qt::CaseInsensitive);
    text.remove(QStringLiteral("cmyk"), Qt::CaseInsensitive);
    text.remove('(');
    text.remove(')');
    text.replace(';', ',');
    text.replace('，', ',');
    return text;
}

static bool parseColorValue(const QString& colorText, ParsedColor& color)
{
    color.r = 0.0;
    color.g = 0.0;
    color.b = 0.0;
    color.valid = false;

    QString text = colorText.trimmed();
    if (text.isEmpty()) {
        return false;
    }

    if (text.startsWith('#')) {
        const QString hex = text.mid(1);
        if (hex.length() == 6) {
            bool okR = false;
            bool okG = false;
            bool okB = false;
            color.r = hex.mid(0, 2).toInt(&okR, 16);
            color.g = hex.mid(2, 2).toInt(&okG, 16);
            color.b = hex.mid(4, 2).toInt(&okB, 16);
            color.valid = okR && okG && okB;
            return color.valid;
        }
    }

    const bool isCmyk = text.contains(QStringLiteral("cmyk"), Qt::CaseInsensitive);
    text = normalizeColorText(text);
    const QStringList parts = text.split(',', QString::SkipEmptyParts);
    if (parts.size() < 3) {
        return false;
    }

    bool ok1 = false;
    bool ok2 = false;
    bool ok3 = false;
    const double v1 = QString(parts.at(0)).trimmed().remove('%').toDouble(&ok1);
    const double v2 = QString(parts.at(1)).trimmed().remove('%').toDouble(&ok2);
    const double v3 = QString(parts.at(2)).trimmed().remove('%').toDouble(&ok3);
    if (!ok1 || !ok2 || !ok3) {
        return false;
    }

    if (isCmyk && parts.size() >= 4) {
        bool ok4 = false;
        const double v4 = QString(parts.at(3)).trimmed().remove('%').toDouble(&ok4);
        if (!ok4) {
            return false;
        }
        const double c = v1 > 1.0 ? v1 / 100.0 : v1;
        const double m = v2 > 1.0 ? v2 / 100.0 : v2;
        const double y = v3 > 1.0 ? v3 / 100.0 : v3;
        const double k = v4 > 1.0 ? v4 / 100.0 : v4;
        color.r = 255.0 * (1.0 - c) * (1.0 - k);
        color.g = 255.0 * (1.0 - m) * (1.0 - k);
        color.b = 255.0 * (1.0 - y) * (1.0 - k);
        color.valid = true;
        return true;
    }

    color.r = v1;
    color.g = v2;
    color.b = v3;
    color.valid = true;
    return true;
}

static bool colorMatchesWithinTolerance(const QString& expected, const QString& actual, double tolerance)
{
    const QString expectedText = expected.trimmed();
    const QString actualText = actual.trimmed();
    if (expectedText.isEmpty()) {
        return true;
    }
    ParsedColor expectedColor;
    ParsedColor actualColor;
    if (!parseColorValue(expectedText, expectedColor) || !parseColorValue(actualText, actualColor)) {
        return expectedText == actualText;
    }
    const double dr = expectedColor.r - actualColor.r;
    const double dg = expectedColor.g - actualColor.g;
    const double db = expectedColor.b - actualColor.b;
    const double distance = std::sqrt(dr * dr + dg * dg + db * db);
    return distance <= tolerance;
}

static OGRLayer* openFirstLayer(GDALDataset*& dataset, const QString& path)
{
    dataset = nullptr;
    if (path.trimmed().isEmpty() || !QFileInfo::exists(path)) {
        return nullptr;
    }
    dataset = static_cast<GDALDataset*>(GDALOpenEx(path.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (dataset == nullptr || dataset->GetLayerCount() <= 0) {
        return nullptr;
    }
    return dataset->GetLayer(0);
}

static int countInvalidGeometries(OGRLayer* layer)
{
    if (layer == nullptr) {
        return 0;
    }
    int invalidCount = 0;
    layer->ResetReading();
    OGRFeature* feature = nullptr;
    while ((feature = layer->GetNextFeature()) != nullptr) {
        OGRGeometry* geometry = feature->GetGeometryRef();
        if (geometry == nullptr || !geometry->IsValid()) {
            ++invalidCount;
        }
        OGRFeature::DestroyFeature(feature);
    }
    layer->ResetReading();
    return invalidCount;
}

static OGRGeometry* buildUnionGeometry(OGRLayer* layer)
{
    if (layer == nullptr) {
        return nullptr;
    }
    OGRGeometry* unionGeometry = nullptr;
    layer->ResetReading();
    OGRFeature* feature = nullptr;
    while ((feature = layer->GetNextFeature()) != nullptr) {
        OGRGeometry* geometry = feature->GetGeometryRef();
        if (geometry != nullptr && !geometry->IsEmpty()) {
            OGRGeometry* cloneGeometry = geometry->clone();
            if (cloneGeometry != nullptr && !cloneGeometry->IsValid()) {
                OGRGeometry* validGeometry = cloneGeometry->MakeValid();
                if (validGeometry != nullptr && !validGeometry->IsEmpty()) {
                    delete cloneGeometry;
                    cloneGeometry = validGeometry;
                }
            }
            if (cloneGeometry != nullptr && !cloneGeometry->IsEmpty()) {
                if (unionGeometry == nullptr) {
                    unionGeometry = cloneGeometry;
                } else {
                    OGRGeometry* merged = unionGeometry->Union(cloneGeometry);
                    delete unionGeometry;
                    delete cloneGeometry;
                    unionGeometry = merged;
                }
            } else if (cloneGeometry != nullptr) {
                delete cloneGeometry;
            }
        }
        OGRFeature::DestroyFeature(feature);
    }
    layer->ResetReading();
    return unionGeometry;
}

static double calculateGeometryArea(OGRGeometry* geometry)
{
    if (geometry == nullptr || geometry->IsEmpty()) {
        return 0.0;
    }

    OGRSurface* surface = dynamic_cast<OGRSurface*>(geometry);
    if (surface != nullptr) {
        return surface->get_Area();
    }

    OGRGeometryCollection* collection = dynamic_cast<OGRGeometryCollection*>(geometry);
    if (collection != nullptr) {
        double totalArea = 0.0;
        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            totalArea += calculateGeometryArea(collection->getGeometryRef(i));
        }
        return totalArea;
    }

    return 0.0;
}

static int countOverlappingBandFeatures(OGRLayer* layer, double minOverlapArea, double& totalOverlapArea, OGRGeometry*& overlapUnion)
{
    totalOverlapArea = 0.0;
    overlapUnion = nullptr;
    if (layer == nullptr) {
        return 0;
    }

    QVector<OGRGeometry*> geometries;
    layer->ResetReading();
    OGRFeature* feature = nullptr;
    while ((feature = layer->GetNextFeature()) != nullptr) {
        OGRGeometry* geometry = feature->GetGeometryRef();
        if (geometry != nullptr && !geometry->IsEmpty()) {
            OGRGeometry* cloneGeometry = geometry->clone();
            if (cloneGeometry != nullptr && !cloneGeometry->IsValid()) {
                OGRGeometry* validGeometry = cloneGeometry->MakeValid();
                if (validGeometry != nullptr && !validGeometry->IsEmpty()) {
                    delete cloneGeometry;
                    cloneGeometry = validGeometry;
                }
            }
            if (cloneGeometry != nullptr && !cloneGeometry->IsEmpty()) {
                geometries.append(cloneGeometry);
            }
        }
        OGRFeature::DestroyFeature(feature);
    }
    layer->ResetReading();

    int overlapCount = 0;
    for (int i = 0; i < geometries.size(); ++i) {
        for (int j = i + 1; j < geometries.size(); ++j) {
            OGRGeometry* intersection = geometries[i]->Intersection(geometries[j]);
            const double area = calculateGeometryArea(intersection);
            if (area > minOverlapArea) {
                ++overlapCount;
                totalOverlapArea += area;
                if (overlapUnion == nullptr) {
                    overlapUnion = intersection ? intersection->clone() : nullptr;
                } else if (intersection != nullptr) {
                    OGRGeometry* merged = overlapUnion->Union(intersection);
                    if (merged != nullptr) {
                        delete overlapUnion;
                        overlapUnion = merged;
                    }
                }
            }
            if (intersection != nullptr) {
                delete intersection;
            }
        }
    }

    for (OGRGeometry* geometry : geometries) {
        delete geometry;
    }
    return overlapCount;
}

static int countSmallFragments(OGRGeometry* geometry, double minArea)
{
    if (geometry == nullptr || geometry->IsEmpty() || minArea <= 0.0) {
        return 0;
    }

    OGRSurface* surface = dynamic_cast<OGRSurface*>(geometry);
    if (surface != nullptr) {
        const double area = surface->get_Area();
        return area > 1e-12 && area < minArea ? 1 : 0;
    }

    OGRGeometryCollection* collection = dynamic_cast<OGRGeometryCollection*>(geometry);
    if (collection != nullptr) {
        int count = 0;
        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            count += countSmallFragments(collection->getGeometryRef(i), minArea);
        }
        return count;
    }

    return 0;
}

static void collectSmallFragmentGeometries(OGRGeometry* geometry, double minArea, QVector<OGRGeometry*>& output)
{
    if (geometry == nullptr || geometry->IsEmpty() || minArea <= 0.0) {
        return;
    }

    OGRSurface* surface = dynamic_cast<OGRSurface*>(geometry);
    if (surface != nullptr) {
        const double area = surface->get_Area();
        if (area > 1e-12 && area < minArea) {
            output.append(geometry->clone());
        }
        return;
    }

    OGRGeometryCollection* collection = dynamic_cast<OGRGeometryCollection*>(geometry);
    if (collection != nullptr) {
        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            collectSmallFragmentGeometries(collection->getGeometryRef(i), minArea, output);
        }
    }
}

static void collectInvalidFeatureGeometries(OGRLayer* layer, QVector<OGRGeometry*>& output)
{
    if (layer == nullptr) {
        return;
    }
    layer->ResetReading();
    OGRFeature* feature = nullptr;
    while ((feature = layer->GetNextFeature()) != nullptr) {
        OGRGeometry* geometry = feature->GetGeometryRef();
        if (geometry != nullptr && !geometry->IsEmpty() && !geometry->IsValid()) {
            output.append(geometry->clone());
        }
        OGRFeature::DestroyFeature(feature);
    }
    layer->ResetReading();
}

static int countSurfaceParts(OGRGeometry* geometry)
{
    if (geometry == nullptr || geometry->IsEmpty()) {
        return 0;
    }

    OGRSurface* surface = dynamic_cast<OGRSurface*>(geometry);
    if (surface != nullptr) {
        return 1;
    }

    OGRGeometryCollection* collection = dynamic_cast<OGRGeometryCollection*>(geometry);
    if (collection != nullptr) {
        int count = 0;
        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            count += countSurfaceParts(collection->getGeometryRef(i));
        }
        return count;
    }

    return 0;
}

static double calculateCornerAngleDegrees(const OGRPoint& prevPt, const OGRPoint& currPt, const OGRPoint& nextPt)
{
    const double kPi = 3.14159265358979323846;
    const double ax = prevPt.getX() - currPt.getX();
    const double ay = prevPt.getY() - currPt.getY();
    const double bx = nextPt.getX() - currPt.getX();
    const double by = nextPt.getY() - currPt.getY();
    const double lenA = std::sqrt(ax * ax + ay * ay);
    const double lenB = std::sqrt(bx * bx + by * by);
    if (lenA <= 1e-8 || lenB <= 1e-8) {
        return 180.0;
    }

    double cosValue = (ax * bx + ay * by) / (lenA * lenB);
    cosValue = std::max(-1.0, std::min(1.0, cosValue));
    return std::acos(cosValue) * 180.0 / kPi;
}

static int countSharpCornerRenderRisks(
    OGRLayer* boundaryLayer,
    OGRGeometry* bandUnion,
    double minAngleDeg,
    double localToleranceMap,
    double minFragmentAreaMapUnit,
    int& sharpCornerCount,
    QVector<OGRGeometry*>* abnormalCornerGeometries = nullptr)
{
    sharpCornerCount = 0;
    if (boundaryLayer == nullptr || bandUnion == nullptr || bandUnion->IsEmpty() || minAngleDeg <= 0.0) {
        return 0;
    }

    const double bufferRadius = std::max(localToleranceMap, 1.0);
    int abnormalCount = 0;

    std::function<void(OGRGeometry*)> inspectGeometry = [&](OGRGeometry* geometry) {
        if (geometry == nullptr || geometry->IsEmpty()) {
            return;
        }

        OGRLineString* line = dynamic_cast<OGRLineString*>(geometry);
        if (line != nullptr && line->getNumPoints() >= 3) {
            for (int i = 1; i < line->getNumPoints() - 1; ++i) {
                OGRPoint prevPt;
                OGRPoint currPt;
                OGRPoint nextPt;
                line->getPoint(i - 1, &prevPt);
                line->getPoint(i, &currPt);
                line->getPoint(i + 1, &nextPt);
                const double angleDeg = calculateCornerAngleDegrees(prevPt, currPt, nextPt);
                if (angleDeg <= 1e-6 || angleDeg >= minAngleDeg) {
                    continue;
                }

                ++sharpCornerCount;
                OGRGeometry* cornerBuffer = currPt.Buffer(bufferRadius, 8);
                OGRGeometry* localBand = (cornerBuffer != nullptr) ? bandUnion->Intersection(cornerBuffer) : nullptr;
                const int localPartCount = countSurfaceParts(localBand);
                const int localFragmentCount = countSmallFragments(localBand, minFragmentAreaMapUnit);
                if ((localPartCount > 1) || (localFragmentCount > 0) || (localBand != nullptr && !localBand->IsValid())) {
                    ++abnormalCount;
                    if (abnormalCornerGeometries != nullptr) {
                        OGRLineString* marker = new OGRLineString();
                        const double markerRadius = std::max(bufferRadius, 1.0);
                        marker->addPoint(currPt.getX() - markerRadius, currPt.getY());
                        marker->addPoint(currPt.getX() + markerRadius, currPt.getY());
                        abnormalCornerGeometries->append(marker);
                    }
                }
                if (localBand != nullptr) delete localBand;
                if (cornerBuffer != nullptr) delete cornerBuffer;
            }
            return;
        }

        OGRGeometryCollection* collection = dynamic_cast<OGRGeometryCollection*>(geometry);
        if (collection != nullptr) {
            for (int i = 0; i < collection->getNumGeometries(); ++i) {
                inspectGeometry(collection->getGeometryRef(i));
            }
        }
    };

    boundaryLayer->ResetReading();
    OGRFeature* feature = nullptr;
    while ((feature = boundaryLayer->GetNextFeature()) != nullptr) {
        inspectGeometry(feature->GetGeometryRef());
        OGRFeature::DestroyFeature(feature);
    }
    boundaryLayer->ResetReading();

    return abnormalCount;
}

static double calculateGeometryLength(OGRGeometry* geometry)
{
    if (geometry == nullptr || geometry->IsEmpty()) {
        return 0.0;
    }

    OGRCurve* curve = dynamic_cast<OGRCurve*>(geometry);
    if (curve != nullptr) {
        return curve->get_Length();
    }

    OGRGeometryCollection* collection = dynamic_cast<OGRGeometryCollection*>(geometry);
    if (collection != nullptr) {
        double totalLength = 0.0;
        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            totalLength += calculateGeometryLength(collection->getGeometryRef(i));
        }
        return totalLength;
    }

    return 0.0;
}

static QString detectBandSideByArea(OGRGeometry* adminUnion, OGRGeometry* bandUnion, double& insideArea, double& outsideArea)
{
    insideArea = 0.0;
    outsideArea = 0.0;
    if (adminUnion == nullptr || bandUnion == nullptr || adminUnion->IsEmpty() || bandUnion->IsEmpty()) {
        return QStringLiteral("unknown");
    }

    OGRGeometry* intersection = adminUnion->Intersection(bandUnion);
    if (intersection != nullptr && !intersection->IsEmpty()) {
        insideArea = calculateGeometryArea(intersection);
    }
    if (intersection != nullptr) {
        delete intersection;
    }

    OGRGeometry* outsideGeometry = bandUnion->Difference(adminUnion);
    if (outsideGeometry != nullptr && !outsideGeometry->IsEmpty()) {
        outsideArea = calculateGeometryArea(outsideGeometry);
    }
    if (outsideGeometry != nullptr) {
        delete outsideGeometry;
    }

    const double total = insideArea + outsideArea;
    if (total <= 1e-8) {
        return QStringLiteral("unknown");
    }
    const bool hasInside = insideArea > 1e-8;
    const bool hasOutside = outsideArea > 1e-8;
    if (hasInside && hasOutside) {
        return QStringLiteral("both");
    }
    if (hasInside) {
        return QStringLiteral("inside");
    }
    if (hasOutside) {
        return QStringLiteral("outside");
    }
    return QStringLiteral("unknown");
}

static QString sideCodeToText(const QString& side)
{
    const QString value = side.trimmed().toLower();
    if (value == QStringLiteral("inside") || value == QStringLiteral("inner") || value == QStringLiteral("内侧")) {
        return QStringLiteral("内侧");
    }
    if (value == QStringLiteral("outside") || value == QStringLiteral("outer") || value == QStringLiteral("外侧")) {
        return QStringLiteral("外侧");
    }
    if (value == QStringLiteral("both") || value == QStringLiteral("double") || value == QStringLiteral("双侧") || value == QStringLiteral("两侧")) {
        return QStringLiteral("双侧");
    }
    return side.trimmed().isEmpty() ? QStringLiteral("未定义") : side;
}

static double calculateTotalLineLength(OGRLayer* layer)
{
    if (layer == nullptr) {
        return 0.0;
    }
    double totalLength = 0.0;
    layer->ResetReading();
    OGRFeature* feature = nullptr;
    while ((feature = layer->GetNextFeature()) != nullptr) {
        OGRGeometry* geometry = feature->GetGeometryRef();
        if (geometry != nullptr && !geometry->IsEmpty()) {
            totalLength += calculateGeometryLength(geometry);
        }
        OGRFeature::DestroyFeature(feature);
    }
    layer->ResetReading();
    return totalLength;
}

static double calculateApproxAverageWidth(OGRGeometry* bandUnion, double totalBoundaryLength)
{
    if (bandUnion == nullptr || bandUnion->IsEmpty() || totalBoundaryLength <= 1e-8) {
        return 0.0;
    }
    return calculateGeometryArea(bandUnion) / totalBoundaryLength;
}

static void collectProjectedBoundaryHits(OGRGeometry* geometry, double ox, double oy, double nx, double ny, QVector<double>& hits)
{
    if (geometry == nullptr || geometry->IsEmpty()) {
        return;
    }

    OGRPoint* point = dynamic_cast<OGRPoint*>(geometry);
    if (point != nullptr) {
        hits.append((point->getX() - ox) * nx + (point->getY() - oy) * ny);
        return;
    }

    OGRLineString* line = dynamic_cast<OGRLineString*>(geometry);
    if (line != nullptr) {
        for (int i = 0; i < line->getNumPoints(); ++i) {
            hits.append((line->getX(i) - ox) * nx + (line->getY(i) - oy) * ny);
        }
        return;
    }

    OGRGeometryCollection* collection = dynamic_cast<OGRGeometryCollection*>(geometry);
    if (collection != nullptr) {
        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            collectProjectedBoundaryHits(collection->getGeometryRef(i), ox, oy, nx, ny, hits);
        }
    }
}

static OGRLayer* createSampleLineLayer(GDALDataset*& dataset, const QString& outputPath)
{
    dataset = nullptr;
    if (outputPath.trimmed().isEmpty()) {
        return nullptr;
    }

    const char* driverName = QFileInfo(outputPath).suffix().compare(QStringLiteral("geojson"), Qt::CaseInsensitive) == 0 ? "GeoJSON" : "ESRI Shapefile";
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName(driverName);
    if (driver == nullptr) {
        return nullptr;
    }

    if (QFileInfo::exists(outputPath)) {
        driver->Delete(outputPath.toUtf8().constData());
    }

    dataset = driver->Create(outputPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr);
    if (dataset == nullptr) {
        return nullptr;
    }

    OGRLayer* layer = dataset->CreateLayer("sample_width_lines", nullptr, wkbLineString, nullptr);
    if (layer == nullptr) {
        return nullptr;
    }

    OGRFieldDefn seqField("seq", OFTInteger);
    layer->CreateField(&seqField);
    OGRFieldDefn widthField("width", OFTReal);
    widthField.SetWidth(32);
    widthField.SetPrecision(8);
    layer->CreateField(&widthField);
    OGRFieldDefn centerXField("center_x", OFTReal);
    centerXField.SetWidth(32);
    centerXField.SetPrecision(8);
    layer->CreateField(&centerXField);
    OGRFieldDefn centerYField("center_y", OFTReal);
    centerYField.SetWidth(32);
    centerYField.SetPrecision(8);
    layer->CreateField(&centerYField);
    OGRFieldDefn errorTypeField("error_type", OFTString);
    errorTypeField.SetWidth(48);
    layer->CreateField(&errorTypeField);
    OGRFieldDefn hitCountField("hit_count", OFTInteger);
    layer->CreateField(&hitCountField);
    return layer;
}

static void writeSampleLineFeature(OGRLayer* layer, int seq, double width, double x1, double y1, double x2, double y2, const QString& errorType = QStringLiteral("OK"), int hitCount = 0)
{
    if (layer == nullptr) {
        return;
    }
    OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
    if (feature == nullptr) {
        return;
    }
    feature->SetField("seq", seq);
    feature->SetField("width", width);
    feature->SetField("center_x", (x1 + x2) / 2.0);
    feature->SetField("center_y", (y1 + y2) / 2.0);
    feature->SetField("error_type", errorType.toUtf8().constData());
    feature->SetField("hit_count", hitCount);
    OGRLineString line;
    line.addPoint(x1, y1);
    line.addPoint(x2, y2);
    feature->SetGeometry(&line);
    layer->CreateFeature(feature);
    OGRFeature::DestroyFeature(feature);
}

static bool pointInGeometry(OGRGeometry* geometry, double x, double y)
{
    if (geometry == nullptr || geometry->IsEmpty()) {
        return false;
    }
    OGRPoint point(x, y);
    return geometry->Contains(&point) || geometry->Intersects(&point);
}

static void collectProjectedOverlapSegments(OGRGeometry* geometry, double ox, double oy, double nx, double ny, QVector<QPair<double, double>>& segments)
{
    if (geometry == nullptr || geometry->IsEmpty()) {
        return;
    }

    OGRLineString* line = dynamic_cast<OGRLineString*>(geometry);
    if (line != nullptr && line->getNumPoints() >= 2) {
        double minProj = std::numeric_limits<double>::max();
        double maxProj = -std::numeric_limits<double>::max();
        for (int i = 0; i < line->getNumPoints(); ++i) {
            const double proj = (line->getX(i) - ox) * nx + (line->getY(i) - oy) * ny;
            minProj = std::min(minProj, proj);
            maxProj = std::max(maxProj, proj);
        }
        if (maxProj > minProj) {
            segments.append(qMakePair(minProj, maxProj));
        }
        return;
    }

    OGRGeometryCollection* collection = dynamic_cast<OGRGeometryCollection*>(geometry);
    if (collection != nullptr) {
        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            collectProjectedOverlapSegments(collection->getGeometryRef(i), ox, oy, nx, ny, segments);
        }
    }
}

static bool chooseLocalOverlapSegment(const QVector<QPair<double, double>>& segments, bool bothSide, double sideSign, double& leftHit, double& rightHit)
{
    if (segments.isEmpty()) {
        return false;
    }

    bool found = false;
    double bestScore = std::numeric_limits<double>::max();
    for (const QPair<double, double>& segment : segments) {
        double a = segment.first;
        double b = segment.second;
        if (a > b) std::swap(a, b);

        double score = 0.0;
        double candidateLeft = a;
        double candidateRight = b;
        if (bothSide) {
            if (a <= 0.0 && b >= 0.0) {
                score = 0.0;
            } else {
                score = std::min(std::fabs(a), std::fabs(b));
            }
        } else {
            double sa = a * sideSign;
            double sb = b * sideSign;
            if (sa > sb) std::swap(sa, sb);
            if (sb <= 0.0) {
                continue;
            }
            const double clippedNear = std::max(0.0, sa);
            const double clippedFar = sb;
            if (clippedFar - clippedNear <= 1e-8) {
                continue;
            }
            score = clippedNear;
            candidateLeft = clippedNear * sideSign;
            candidateRight = clippedFar * sideSign;
        }

        if (score < bestScore) {
            bestScore = score;
            leftHit = candidateLeft;
            rightHit = candidateRight;
            found = true;
        }
    }
    return found && std::fabs(rightHit - leftHit) > 1e-8;
}

static void addSegmentSideLength(double a, double b, bool positiveInside, double& insideLength, double& outsideLength)
{
    if (a > b) std::swap(a, b);
    if (b - a <= 1e-8) {
        return;
    }

    const double negativeLength = std::max(0.0, std::min(b, 0.0) - std::min(a, 0.0));
    const double positiveLength = std::max(0.0, std::max(b, 0.0) - std::max(a, 0.0));
    if (positiveInside) {
        insideLength += positiveLength;
        outsideLength += negativeLength;
    } else {
        insideLength += negativeLength;
        outsideLength += positiveLength;
    }
}

static QString detectBandSideByBoundarySamples(
    OGRLayer* boundaryLayer,
    OGRGeometry* adminUnion,
    OGRGeometry* bandUnion,
    double sampleIntervalMm,
    int scale,
    double standardWidthMm,
    double widthToleranceMm,
    double profileLengthFactor,
    double toleranceRatio,
    double& insideLength,
    double& outsideLength,
    int& directionSampleCount,
    int& noHitSampleCount)
{
    insideLength = 0.0;
    outsideLength = 0.0;
    directionSampleCount = 0;
    noHitSampleCount = 0;

    if (boundaryLayer == nullptr || adminUnion == nullptr || bandUnion == nullptr ||
        adminUnion->IsEmpty() || bandUnion->IsEmpty() || scale <= 0) {
        return QStringLiteral("unknown");
    }

    const double effectiveSampleIntervalMm = sampleIntervalMm > 0.0 ? sampleIntervalMm : 5.0;
    const double sampleIntervalMap = std::max(1.0, effectiveSampleIntervalMm * scale / 1000.0);
    const double widthMap = std::max(standardWidthMm + widthToleranceMm, standardWidthMm) * scale / 1000.0;
    const double halfLen = std::max(widthMap * std::max(2.0, profileLengthFactor), 100.0);
    const double probeDist = std::max(1.0, std::min(halfLen * 0.25, std::max(widthMap * 0.5, 1.0)));
    const double effectiveTolerance = std::max(0.0, std::min(0.49, toleranceRatio));

    auto sampleAtPoint = [&](const OGRPoint& pt, double dx, double dy)->void {
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len <= 1e-12) {
            return;
        }

        const double nx = -dy / len;
        const double ny = dx / len;
        const bool positiveInside = pointInGeometry(adminUnion, pt.getX() + nx * probeDist, pt.getY() + ny * probeDist);
        const bool negativeInside = pointInGeometry(adminUnion, pt.getX() - nx * probeDist, pt.getY() - ny * probeDist);
        if (positiveInside == negativeInside) {
            return;
        }

        OGRLineString cross;
        cross.addPoint(pt.getX() - nx * halfLen, pt.getY() - ny * halfLen);
        cross.addPoint(pt.getX() + nx * halfLen, pt.getY() + ny * halfLen);

        OGRGeometry* overlapGeometry = bandUnion->Intersection(&cross);
        QVector<QPair<double, double>> overlapSegments;
        collectProjectedOverlapSegments(overlapGeometry, pt.getX(), pt.getY(), nx, ny, overlapSegments);
        if (overlapGeometry != nullptr) {
            delete overlapGeometry;
        }

        if (overlapSegments.isEmpty()) {
            ++noHitSampleCount;
            return;
        }

        ++directionSampleCount;
        for (const QPair<double, double>& segment : overlapSegments) {
            addSegmentSideLength(segment.first, segment.second, positiveInside, insideLength, outsideLength);
        }
    };

    std::function<void(OGRGeometry*)> sampleGuideGeometry = [&](OGRGeometry* geometry) {
        if (geometry == nullptr || geometry->IsEmpty()) return;
        OGRLineString* line = dynamic_cast<OGRLineString*>(geometry);
        if (line != nullptr && line->getNumPoints() >= 2) {
            const double lineLength = line->get_Length();
            for (double distance = 0.0; distance <= lineLength; distance += sampleIntervalMap) {
                OGRPoint pt;
                line->Value(distance, &pt);
                OGRPoint pt2;
                const double nextDistance = std::min(distance + 1.0, lineLength);
                line->Value(nextDistance, &pt2);
                sampleAtPoint(pt, pt2.getX() - pt.getX(), pt2.getY() - pt.getY());
            }
            return;
        }
        OGRGeometryCollection* collection = dynamic_cast<OGRGeometryCollection*>(geometry);
        if (collection != nullptr) {
            for (int i = 0; i < collection->getNumGeometries(); ++i) {
                sampleGuideGeometry(collection->getGeometryRef(i));
            }
        }
    };

    boundaryLayer->ResetReading();
    OGRFeature* feature = nullptr;
    while ((feature = boundaryLayer->GetNextFeature()) != nullptr) {
        sampleGuideGeometry(feature->GetGeometryRef());
        OGRFeature::DestroyFeature(feature);
    }
    boundaryLayer->ResetReading();

    const double totalLength = insideLength + outsideLength;
    if (totalLength <= 1e-8 || directionSampleCount <= 0) {
        return QStringLiteral("unknown");
    }

    Q_UNUSED(effectiveTolerance);
    const bool hasInside = insideLength > 1e-8;
    const bool hasOutside = outsideLength > 1e-8;
    if (hasInside && hasOutside) {
        return QStringLiteral("both");
    }
    if (hasInside) {
        return QStringLiteral("inside");
    }
    if (hasOutside) {
        return QStringLiteral("outside");
    }
    return QStringLiteral("unknown");
}

static bool bandSideMatchesRule(
    const QString& expectedSide,
    double insideLength,
    double outsideLength,
    int directionSampleCount,
    double toleranceRatio)
{
    if (directionSampleCount <= 0) {
        return false;
    }
    const double totalLength = insideLength + outsideLength;
    if (totalLength <= 1e-8) {
        return false;
    }

    Q_UNUSED(toleranceRatio);
    const bool hasInside = insideLength > 1e-8;
    const bool hasOutside = outsideLength > 1e-8;
    const QString side = expectedSide.trimmed().toLower();
    if (side == QStringLiteral("inside") || side == QStringLiteral("inner") || side == QStringLiteral("内侧")) {
        return hasInside && !hasOutside;
    }
    if (side == QStringLiteral("outside") || side == QStringLiteral("outer") || side == QStringLiteral("外侧")) {
        return hasOutside && !hasInside;
    }
    if (side == QStringLiteral("both") || side == QStringLiteral("double") || side == QStringLiteral("双侧") || side == QStringLiteral("两侧")) {
        return hasInside && hasOutside;
    }
    return false;
}

static bool bandSideAreaMatchesRule(
    const QString& expectedSide,
    double insideArea,
    double outsideArea,
    double toleranceRatio)
{
    const double totalArea = insideArea + outsideArea;
    if (totalArea <= 1e-8) {
        return false;
    }

    Q_UNUSED(toleranceRatio);
    const bool hasInside = insideArea > 1e-8;
    const bool hasOutside = outsideArea > 1e-8;
    const QString side = expectedSide.trimmed().toLower();
    if (side == QStringLiteral("inside") || side == QStringLiteral("inner") || side == QStringLiteral("内侧")) {
        return hasInside && !hasOutside;
    }
    if (side == QStringLiteral("outside") || side == QStringLiteral("outer") || side == QStringLiteral("外侧")) {
        return hasOutside && !hasInside;
    }
    if (side == QStringLiteral("both") || side == QStringLiteral("double") || side == QStringLiteral("双侧") || side == QStringLiteral("两侧")) {
        return hasInside && hasOutside;
    }
    return false;
}

static void calculateSampleWidthStats(
    OGRLayer* boundaryLayer,
    OGRGeometry* adminUnion,
    OGRGeometry* bandUnion,
    double sampleIntervalMm,
    int scale,
    const QString& expectedSide,
    double standardWidthMm,
    double widthToleranceMm,
    const QString& tempSampleLinePath,
    int& sampleCount,
    double& minWidth,
    double& maxWidth,
    double& avgWidth,
    double& stddevWidth,
    double& widthCv,
    int& abnormalHitCount,
    int& widthJumpCount)
{
    sampleCount = 0;
    minWidth = 0.0;
    maxWidth = 0.0;
    avgWidth = 0.0;
    stddevWidth = 0.0;
    widthCv = 0.0;
    abnormalHitCount = 0;
    widthJumpCount = 0;
    if (bandUnion == nullptr || bandUnion->IsEmpty() || scale <= 0) {
        return;
    }

    GDALDataset* sampleDataset = nullptr;
    OGRLayer* sampleLayer = tempSampleLinePath.trimmed().isEmpty() ? nullptr : createSampleLineLayer(sampleDataset, tempSampleLinePath);

    const QString side = expectedSide.trimmed().toLower();
    const bool expectedBothSide = side == QStringLiteral("both") || side == QStringLiteral("double") ||
                                  side == QStringLiteral("双侧") || side == QStringLiteral("两侧");
    const double effectiveSampleIntervalMm = sampleIntervalMm > 0.0 ? sampleIntervalMm : 5.0;
    const double sampleIntervalMap = std::max(1.0, effectiveSampleIntervalMm * scale / 1000.0);
    const double maxWidthMap = std::max(standardWidthMm + widthToleranceMm, standardWidthMm) * scale / 1000.0;
    const double halfLen = std::max(maxWidthMap * 2.0, 100.0);

    QVector<double> widths;
    QSet<QString> sampledCenterKeys;
    double previousWidth = 0.0;
    int seq = 1;
    auto sampleAtPoint = [&](const OGRPoint& pt, double dx, double dy, bool useExpectedSide)->void {
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len <= 1e-12) {
            return;
        }

        const double nx = -dy / len;
        const double ny = dx / len;
        OGRLineString cross;
        cross.addPoint(pt.getX() - nx * halfLen, pt.getY() - ny * halfLen);
        cross.addPoint(pt.getX() + nx * halfLen, pt.getY() + ny * halfLen);

        OGRGeometry* overlapGeometry = bandUnion->Intersection(&cross);
        QVector<QPair<double, double>> overlapSegments;
        collectProjectedOverlapSegments(overlapGeometry, pt.getX(), pt.getY(), nx, ny, overlapSegments);
        if (overlapGeometry != nullptr) {
            delete overlapGeometry;
        }

        double leftHit = 0.0;
        double rightHit = 0.0;
        double sideSign = 1.0;
        bool localBothSide = !useExpectedSide || expectedBothSide || adminUnion == nullptr || adminUnion->IsEmpty();
        if (!localBothSide) {
            const bool positiveInside = pointInGeometry(adminUnion, pt.getX() + nx, pt.getY() + ny);
            sideSign = ((side == QStringLiteral("outside") || side == QStringLiteral("outer") || side == QStringLiteral("外侧")) == positiveInside) ? -1.0 : 1.0;
        }
        const int hitCount = overlapSegments.size();
        QString sampleErrorType = QStringLiteral("OK");
        if (!chooseLocalOverlapSegment(overlapSegments, localBothSide, sideSign, leftHit, rightHit)) {
            ++abnormalHitCount;
            if (sampleLayer != nullptr) {
                const double markerLen = std::max(1.0, maxWidthMap);
                const double markerLeft = localBothSide ? -markerLen : 0.0;
                const double markerRight = localBothSide ? markerLen : sideSign * markerLen;
                writeSampleLineFeature(
                    sampleLayer,
                    seq++,
                    0.0,
                    pt.getX() + nx * markerLeft,
                    pt.getY() + ny * markerLeft,
                    pt.getX() + nx * markerRight,
                    pt.getY() + ny * markerRight,
                    QStringLiteral("NO_LOCAL_OVERLAP"),
                    hitCount);
            }
            return;
        }
        if (overlapSegments.size() > 1) {
            ++abnormalHitCount;
            sampleErrorType = QStringLiteral("MULTI_INTERSECTION");
        }

        const double centerOffset = (leftHit + rightHit) * 0.5;
        const double centerX = pt.getX() + nx * centerOffset;
        const double centerY = pt.getY() + ny * centerOffset;
        const double dedupGrid = std::max(1.0, sampleIntervalMap * 0.5);
        const QString centerKey = QStringLiteral("%1_%2")
            .arg(qRound64(centerX / dedupGrid))
            .arg(qRound64(centerY / dedupGrid));
        if (sampledCenterKeys.contains(centerKey)) {
            return;
        }
        sampledCenterKeys.insert(centerKey);

        const double widthMap = std::fabs(rightHit - leftHit);
        const double widthMm = widthMap * 1000.0 / scale;
        if (widthMm <= 1e-8) {
            return;
        }
        const bool widthOutOfRange = widthMm < (standardWidthMm - widthToleranceMm) || widthMm > (standardWidthMm + widthToleranceMm);
        const bool widthJump = previousWidth > 1e-8 && std::fabs(widthMm - previousWidth) / previousWidth > 0.5;
        if (widthJump) {
            ++widthJumpCount;
            if (sampleErrorType == QStringLiteral("OK")) {
                sampleErrorType = QStringLiteral("WIDTH_JUMP");
            }
        }
        if (widthOutOfRange && sampleErrorType == QStringLiteral("OK")) {
            sampleErrorType = QStringLiteral("WIDTH_ERROR");
        }
        previousWidth = widthMm;
        widths.append(widthMm);

        if (sampleLayer != nullptr) {
            const double x1 = pt.getX() + nx * leftHit;
            const double y1 = pt.getY() + ny * leftHit;
            const double x2 = pt.getX() + nx * rightHit;
            const double y2 = pt.getY() + ny * rightHit;
            writeSampleLineFeature(sampleLayer, seq++, widthMm, x1, y1, x2, y2, sampleErrorType, hitCount);
        }
    };

    std::function<void(OGRGeometry*, bool)> sampleGuideGeometry = [&](OGRGeometry* geometry, bool useExpectedSide) {
        if (geometry == nullptr || geometry->IsEmpty()) return;
        OGRLineString* line = dynamic_cast<OGRLineString*>(geometry);
        if (line != nullptr && line->getNumPoints() >= 2) {
            const double lineLength = line->get_Length();
            for (double distance = 0.0; distance <= lineLength; distance += sampleIntervalMap) {
                OGRPoint pt;
                line->Value(distance, &pt);
                OGRPoint pt2;
                const double nextDistance = std::min(distance + 1.0, lineLength);
                line->Value(nextDistance, &pt2);
                const double dx = pt2.getX() - pt.getX();
                const double dy = pt2.getY() - pt.getY();
                sampleAtPoint(pt, dx, dy, useExpectedSide);
            }
            return;
        }
        OGRGeometryCollection* collection = dynamic_cast<OGRGeometryCollection*>(geometry);
        if (collection != nullptr) {
            for (int i = 0; i < collection->getNumGeometries(); ++i) {
                sampleGuideGeometry(collection->getGeometryRef(i), useExpectedSide);
            }
        }
    };

    OGRGeometry* bandBoundary = bandUnion->Boundary();
    sampleGuideGeometry(bandBoundary, false);
    if (bandBoundary != nullptr) {
        delete bandBoundary;
    }

    if (sampleDataset != nullptr) {
        GDALClose(sampleDataset);
    }
    if (widths.isEmpty()) {
        return;
    }

    sampleCount = widths.size();
    minWidth = widths.first();
    maxWidth = widths.first();
    double sum = 0.0;
    for (double width : widths) {
        minWidth = std::min(minWidth, width);
        maxWidth = std::max(maxWidth, width);
        sum += width;
    }
    avgWidth = sum / widths.size();

    double varianceSum = 0.0;
    for (double width : widths) {
        const double diff = width - avgWidth;
        varianceSum += diff * diff;
    }
    stddevWidth = std::sqrt(varianceSum / widths.size());
    if (avgWidth > 1e-8) {
        widthCv = stddevWidth / avgWidth;
    }
}

static QString inferBoundaryLevelFromPath(const QString& path)
{
    const QString name = QFileInfo(path).completeBaseName();
    if (name.contains(QStringLiteral("省级"))) return QStringLiteral("省级");
    if (name.contains(QStringLiteral("市州级")) || name.contains(QStringLiteral("市级"))) return QStringLiteral("市州级");
    if (name.contains(QStringLiteral("县级"))) return QStringLiteral("县级");
    return QString();
}

static bool collectDominantFieldValue(OGRLayer* layer, const char* fieldName, QString& dominantValue, int& matchedCount, int& totalCount)
{
    dominantValue.clear();
    matchedCount = 0;
    totalCount = 0;
    if (layer == nullptr || fieldName == nullptr || std::strlen(fieldName) == 0) {
        return false;
    }

    const int fieldIndex = layer->GetLayerDefn()->GetFieldIndex(fieldName);
    if (fieldIndex < 0) {
        return false;
    }

    QMap<QString, int> valueCount;
    layer->ResetReading();
    OGRFeature* feature = nullptr;
    while ((feature = layer->GetNextFeature()) != nullptr) {
        ++totalCount;
        if (feature->IsFieldSetAndNotNull(fieldIndex)) {
            const QString value = QString::fromUtf8(feature->GetFieldAsString(fieldIndex)).trimmed();
            if (!value.isEmpty()) {
                valueCount[value] += 1;
            }
        }
        OGRFeature::DestroyFeature(feature);
    }
    layer->ResetReading();

    int maxCount = 0;
    for (auto it = valueCount.constBegin(); it != valueCount.constEnd(); ++it) {
        if (it.value() > maxCount) {
            maxCount = it.value();
            dominantValue = it.key();
        }
    }
    matchedCount = maxCount;
    return !dominantValue.isEmpty();
}

static char** boundaryBandUtf8LayerOptions()
{
    char** options = nullptr;
    options = CSLSetNameValue(options, "ENCODING", "UTF-8");
    return options;
}

static void deleteBoundaryBandVectorDataset(const QString& outputPath)
{
    if (outputPath.trimmed().isEmpty() || !QFileInfo::exists(outputPath)) {
        return;
    }
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (driver != nullptr) {
        driver->Delete(outputPath.toUtf8().constData());
    }
}

static QString boundaryBandErrorTypeText(const QString& errorType)
{
    if (errorType == QStringLiteral("BAND_SIDE_ERROR")) return QStringLiteral("色带挂接方向错误");
    if (errorType == QStringLiteral("BAND_WIDTH_ERROR")) return QStringLiteral("色带宽度错误");
    if (errorType == QStringLiteral("BAND_WIDTH_UNEVEN")) return QStringLiteral("色带宽度不均匀");
    if (errorType == QStringLiteral("BAND_COMPLEX_BOUNDARY_RENDER_ERROR")) return QStringLiteral("复杂边界色带渲染异常");
    if (errorType == QStringLiteral("BAND_OVERLAP_ERROR")) return QStringLiteral("色带重叠错误");
    if (errorType == QStringLiteral("BAND_COLOR_ERROR")) return QStringLiteral("色带填充颜色错误");
    if (errorType == QStringLiteral("BAND_GRADIENT_ERROR")) return QStringLiteral("色带渐变颜色错误");
    if (errorType == QStringLiteral("BAND_GRADIENT_DIRECTION_ERROR")) return QStringLiteral("色带渐变方向错误");
    if (errorType == QStringLiteral("BAND_GRADIENT_FUNCTION_ERROR")) return QStringLiteral("色带渐变函数类型错误");
    if (errorType == QStringLiteral("WIDTH_ERROR")) return QStringLiteral("色带宽度错误");
    if (errorType == QStringLiteral("WIDTH_JUMP")) return QStringLiteral("色带宽度突变");
    if (errorType == QStringLiteral("MULTI_INTERSECTION")) return QStringLiteral("复杂边界多重交叠异常");
    if (errorType == QStringLiteral("NO_LOCAL_OVERLAP")) return QStringLiteral("复杂边界局部交叠异常");
    if (errorType == QStringLiteral("INVALID_GEOMETRY")) return QStringLiteral("几何非法");
    if (errorType == QStringLiteral("SMALL_FRAGMENT")) return QStringLiteral("碎片化");
    if (errorType == QStringLiteral("FEATURE_OVERLAP")) return QStringLiteral("面重叠");
    if (errorType == QStringLiteral("SHARP_CORNER_RISK")) return QStringLiteral("尖角局部异常");
    if (errorType == QStringLiteral("DISTORTION_RATIO_EXCEEDED")) return QStringLiteral("扭曲比超阈值");
    if (errorType == QStringLiteral("OK")) return QStringLiteral("通过");
    return errorType;
}

static QString boundaryBandSampleErrorDetail(const QString& sourceErrorType, const QString& defaultDetail)
{
    if (sourceErrorType == QStringLiteral("MULTI_INTERSECTION")) {
        return QStringLiteral("复杂边界或尖角处采样线与色带多次相交，存在重叠、扭曲或破图风险");
    }
    if (sourceErrorType == QStringLiteral("WIDTH_JUMP")) {
        return QStringLiteral("相邻采样剖面宽度突变，存在局部扭曲或宽度不连续风险");
    }
    if (sourceErrorType == QStringLiteral("WIDTH_ERROR")) {
        return QStringLiteral("色带采样宽度超出规则允许范围");
    }
    if (sourceErrorType == QStringLiteral("NO_LOCAL_OVERLAP")) {
        return QStringLiteral("复杂边界或尖角处采样线未能选出有效局部色带交段，存在断裂、扭曲或破图风险");
    }
    if (sourceErrorType == QStringLiteral("INVALID_GEOMETRY")) {
        return QStringLiteral("色带面要素存在自相交、破图或其他几何非法问题");
    }
    if (sourceErrorType == QStringLiteral("SMALL_FRAGMENT")) {
        return QStringLiteral("色带局部出现碎片化小面，存在破图或渲染残片风险");
    }
    if (sourceErrorType == QStringLiteral("FEATURE_OVERLAP")) {
        return QStringLiteral("色带原始面要素之间存在局部重叠，复杂边界处易出现渲染叠压");
    }
    if (sourceErrorType == QStringLiteral("SHARP_CORNER_RISK")) {
        return QStringLiteral("尖角附近色带局部出现碎片、断裂或几何非法风险");
    }
    if (sourceErrorType == QStringLiteral("DISTORTION_RATIO_EXCEEDED")) {
        return QStringLiteral("采样宽度最大值与最小值比值超过阈值，存在整体或局部扭曲风险");
    }
    return defaultDetail;
}

static bool writeBoundaryBandErrorLayer(
    const QString& outputPath,
    const char* driverName,
    OGRGeometry* bandUnion,
    bool hasSideError,
    bool hasWidthError,
    bool hasUnevenError,
    bool hasGeomError,
    bool hasOverlapError,
    bool hasColorError,
    bool hasGradientError,
    bool hasGradientDirectionError,
    int complexBoundaryAbnormalCount,
    int overlapFeaturePairCount,
    double overlapArea,
    double minSampleWidth,
    double maxSampleWidth,
    double widthTolerance,
    double uniformityCvThreshold,
    const QString& expectedSide,
    const QString& actualSide,
    double expectedWidth,
    double actualWidth,
    double widthCv,
    const QString& expectedFillColor,
    const QString& actualFillColor,
    const QString& expectedStartColor,
    const QString& actualStartColor,
    const QString& expectedEndColor,
    const QString& actualEndColor,
    const QString& expectedGradientDirection,
    const QString& actualGradientDirection,
    const QString& boundaryLevel,
    const QString& boundaryLayerPath,
    bool onlyCheckSide)
{
    if (outputPath.trimmed().isEmpty() || bandUnion == nullptr || bandUnion->IsEmpty()) {
        return false;
    }

    QFileInfo outInfo(outputPath);
    QDir().mkpath(outInfo.absolutePath());
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName(driverName);
    if (driver == nullptr) {
        return false;
    }
    GDALDataset* dataset = nullptr;
    OGRLayer* layer = nullptr;
    if (QFileInfo::exists(outputPath)) {
        dataset = static_cast<GDALDataset*>(GDALOpenEx(outputPath.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr, nullptr, nullptr));
        if (dataset != nullptr && dataset->GetLayerCount() > 0) {
            layer = dataset->GetLayer(0);
        }
    }
    auto ensureSummaryField = [&](OGRLayer* targetLayer, const char* name, int width) {
        if (targetLayer != nullptr && targetLayer->GetLayerDefn()->GetFieldIndex(name) < 0) {
            OGRFieldDefn field(name, OFTString);
            field.SetWidth(width);
            targetLayer->CreateField(&field);
        }
    };
    if (layer == nullptr) {
        dataset = driver->Create(outputPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr);
        if (dataset == nullptr) {
            return false;
        }

        char** layerOptions = boundaryBandUtf8LayerOptions();
        layer = dataset->CreateLayer("errors", nullptr, wkbPolygon, layerOptions);
        CSLDestroy(layerOptions);
        if (layer == nullptr) {
            GDALClose(dataset);
            return false;
        }

        OGRFieldDefn fieldCheck("check_item", OFTString);
        fieldCheck.SetWidth(48);
        layer->CreateField(&fieldCheck);
        OGRFieldDefn fieldPassed("passed", OFTString);
        fieldPassed.SetWidth(8);
        layer->CreateField(&fieldPassed);
        OGRFieldDefn fieldType("error_type", OFTString);
        fieldType.SetWidth(48);
        layer->CreateField(&fieldType);
        OGRFieldDefn fieldSummary("result_desc", OFTString);
        fieldSummary.SetWidth(200);
        layer->CreateField(&fieldSummary);
        OGRFieldDefn fieldMsg("detail", OFTString);
        fieldMsg.SetWidth(160);
        layer->CreateField(&fieldMsg);
        OGRFieldDefn fieldExpect("expect_val", OFTString);
        fieldExpect.SetWidth(64);
        layer->CreateField(&fieldExpect);
        OGRFieldDefn fieldActual("actual_val", OFTString);
        fieldActual.SetWidth(64);
        layer->CreateField(&fieldActual);
    }
    ensureSummaryField(layer, "result_desc", 200);
    ensureSummaryField(layer, "side_county", 32);
    ensureSummaryField(layer, "side_city", 32);
    ensureSummaryField(layer, "side_prov", 32);

    auto sideFieldName = [&]() -> const char* {
        const QString levelOrLayer = QString(boundaryLevel + QStringLiteral(" ") + QFileInfo(boundaryLayerPath).completeBaseName()).trimmed();
        if (levelOrLayer.contains(QStringLiteral("县"))) return "side_county";
        if (levelOrLayer.contains(QStringLiteral("市")) || levelOrLayer.contains(QStringLiteral("州"))) return "side_city";
        if (levelOrLayer.contains(QStringLiteral("省"))) return "side_prov";
        return "side_county";
    };

    auto writeOne = [&](const QString& checkItem, bool failed, const QString& errorType, const QString& detail, const QString& expectVal, const QString& actualVal) {
        const bool boundaryRelated = checkItem == QStringLiteral("side");
        const QString boundaryName = QFileInfo(boundaryLayerPath).completeBaseName();
        const QString levelText = boundaryRelated ? (boundaryLevel.isEmpty() ? boundaryName : boundaryLevel) : QStringLiteral("通用");
        const QString displayCheckItem = boundaryRelated ? boundaryName + QStringLiteral("挂接方向") : checkItem;
        const QString resultDesc = QStringLiteral("【%1】%2-%3：%4；期望=%5；实际=%6")
            .arg(levelText,
                 boundaryRelated ? boundaryName : QStringLiteral("色带自身"),
                 displayCheckItem,
                 failed ? boundaryBandErrorTypeText(errorType) : QStringLiteral("通过"),
                 expectVal,
                 actualVal);
        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        feature->SetField("check_item", displayCheckItem.toUtf8().constData());
        feature->SetField("passed", failed ? "false" : "true");
        feature->SetField("error_type", failed ? boundaryBandErrorTypeText(errorType).toUtf8().constData() : boundaryBandErrorTypeText(QStringLiteral("OK")).toUtf8().constData());
        feature->SetField("result_desc", resultDesc.toUtf8().constData());
        feature->SetField("detail", detail.toUtf8().constData());
        feature->SetField("expect_val", expectVal.toUtf8().constData());
        feature->SetField("actual_val", actualVal.toUtf8().constData());
        if (boundaryRelated) {
            feature->SetField(sideFieldName(), actualVal.toUtf8().constData());
        }
        feature->SetGeometry(bandUnion);
        layer->CreateFeature(feature);
        OGRFeature::DestroyFeature(feature);
    };

    writeOne("side", hasSideError, "BAND_SIDE_ERROR", hasSideError ? QStringLiteral("色带挂接方向与规则要求不一致") : QStringLiteral("色带挂接方向检查通过"), sideCodeToText(expectedSide), actualSide);
    if (onlyCheckSide) {
        GDALClose(dataset);
        return true;
    }
    writeOne("width", hasWidthError, "BAND_WIDTH_ERROR", hasWidthError ? QStringLiteral("色带采样宽度超出规则允许范围") : QStringLiteral("色带宽度检查通过"), QString::number(expectedWidth) + QStringLiteral("±") + QString::number(widthTolerance), QStringLiteral("平均=") + QString::number(actualWidth) + QStringLiteral(",最小=") + QString::number(minSampleWidth) + QStringLiteral(",最大=") + QString::number(maxSampleWidth));
    writeOne("uniformity", hasUnevenError, "BAND_WIDTH_UNEVEN", hasUnevenError ? QStringLiteral("色带宽度变化或局部突变超出允许阈值") : QStringLiteral("色带宽度均匀度检查通过"), QStringLiteral("变异系数<=") + QString::number(uniformityCvThreshold), QString::number(widthCv));
    writeOne("complex_geom", hasGeomError, "BAND_COMPLEX_BOUNDARY_RENDER_ERROR", hasGeomError ? QStringLiteral("复杂边界或尖角处存在重叠、扭曲、碎片或破图风险") : QStringLiteral("复杂边界几何渲染检查通过"), "0", QString::number(complexBoundaryAbnormalCount));
    writeOne("overlap", hasOverlapError, "BAND_OVERLAP_ERROR", hasOverlapError ? QStringLiteral("色带原始面要素之间存在面积重叠") : QStringLiteral("色带重叠检查通过"), "0", QStringLiteral("重叠对数=") + QString::number(overlapFeaturePairCount) + QStringLiteral(",重叠面积=") + QString::number(overlapArea));
    writeOne("color", hasColorError, "BAND_COLOR_ERROR", hasColorError ? QStringLiteral("色带填充颜色与规则要求不一致") : QStringLiteral("色带颜色检查通过或未启用"), expectedFillColor, actualFillColor);
    writeOne("gradient", hasGradientError, "BAND_GRADIENT_ERROR", hasGradientError ? QStringLiteral("色带渐变起止色与规则要求不一致") : QStringLiteral("色带渐变颜色检查通过或未启用"), expectedStartColor + " -> " + expectedEndColor, actualStartColor + " -> " + actualEndColor);
    writeOne("gradient_dir", hasGradientDirectionError, "BAND_GRADIENT_DIRECTION_ERROR", hasGradientDirectionError ? QStringLiteral("色带渐变方向与规则要求不一致") : QStringLiteral("色带渐变方向检查通过或未启用"), expectedGradientDirection, actualGradientDirection);

    GDALClose(dataset);
    return true;
}

static bool writeBoundaryBandSingleCheckLayer(
    const QString& outputPath,
    const char* driverName,
    OGRGeometry* geometry,
    const QString& checkItem,
    const QString& errorType,
    const QString& detail,
    const QString& expectVal,
    const QString& actualVal,
    const QString& boundaryLevel,
    const QString& boundaryLayerPath,
    const QString& bandPart)
{
    if (outputPath.trimmed().isEmpty() || geometry == nullptr || geometry->IsEmpty()) {
        return false;
    }

    QFileInfo outInfo(outputPath);
    QDir().mkpath(outInfo.absolutePath());
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName(driverName);
    if (driver == nullptr) {
        return false;
    }
    GDALDataset* dataset = nullptr;
    OGRLayer* layer = nullptr;
    if (QFileInfo::exists(outputPath)) {
        dataset = static_cast<GDALDataset*>(GDALOpenEx(outputPath.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr, nullptr, nullptr));
        if (dataset != nullptr && dataset->GetLayerCount() > 0) {
            layer = dataset->GetLayer(0);
        }
    }
    if (layer == nullptr) {
        dataset = driver->Create(outputPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr);
        if (dataset == nullptr) {
            return false;
        }

        OGRwkbGeometryType geometryType = wkbFlatten(geometry->getGeometryType());
        if (geometryType != wkbPolygon && geometryType != wkbMultiPolygon) {
            geometryType = wkbUnknown;
        }
        char** layerOptions = boundaryBandUtf8LayerOptions();
        layer = dataset->CreateLayer(outInfo.completeBaseName().toUtf8().constData(), nullptr, geometryType, layerOptions);
        CSLDestroy(layerOptions);
        if (layer == nullptr) {
            GDALClose(dataset);
            return false;
        }

        OGRFieldDefn fieldCheck("check_item", OFTString);
        fieldCheck.SetWidth(48);
        layer->CreateField(&fieldCheck);
        OGRFieldDefn fieldType("error_type", OFTString);
        fieldType.SetWidth(48);
        layer->CreateField(&fieldType);
        OGRFieldDefn fieldLevel("boundary_lv", OFTString);
        fieldLevel.SetWidth(32);
        layer->CreateField(&fieldLevel);
        OGRFieldDefn fieldLayer("boundary_ly", OFTString);
        fieldLayer.SetWidth(96);
        layer->CreateField(&fieldLayer);
        OGRFieldDefn fieldPart("band_part", OFTString);
        fieldPart.SetWidth(16);
        layer->CreateField(&fieldPart);
        OGRFieldDefn fieldMsg("detail", OFTString);
        fieldMsg.SetWidth(160);
        layer->CreateField(&fieldMsg);
        OGRFieldDefn fieldExpect("expect_val", OFTString);
        fieldExpect.SetWidth(64);
        layer->CreateField(&fieldExpect);
        OGRFieldDefn fieldActual("actual_val", OFTString);
        fieldActual.SetWidth(64);
        layer->CreateField(&fieldActual);
    }
    auto ensureField = [&](const char* name, int width) {
        if (layer->GetLayerDefn()->GetFieldIndex(name) < 0) {
            OGRFieldDefn field(name, OFTString);
            field.SetWidth(width);
            layer->CreateField(&field);
        }
    };
    ensureField("boundary_lv", 32);
    ensureField("boundary_ly", 96);
    ensureField("band_part", 16);

    OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
    const bool boundaryRelated = checkItem == QStringLiteral("side");
    feature->SetField("check_item", checkItem.toUtf8().constData());
    feature->SetField("error_type", boundaryBandErrorTypeText(errorType).toUtf8().constData());
    feature->SetField("boundary_lv", boundaryRelated ? boundaryLevel.toUtf8().constData() : "");
    feature->SetField("boundary_ly", boundaryRelated ? QFileInfo(boundaryLayerPath).completeBaseName().toUtf8().constData() : "");
    feature->SetField("band_part", bandPart.toUtf8().constData());
    feature->SetField("detail", detail.toUtf8().constData());
    feature->SetField("expect_val", expectVal.toUtf8().constData());
    feature->SetField("actual_val", actualVal.toUtf8().constData());
    feature->SetGeometry(geometry);
    const bool ok = layer->CreateFeature(feature) == OGRERR_NONE;
    OGRFeature::DestroyFeature(feature);
    GDALClose(dataset);
    return ok;
}

static int writeFilteredSampleLineErrorLayer(
    const QString& outputPath,
    const QString& sampleLinePath,
    const QStringList& errorTypes,
    const QString& checkItem,
    const QString& detail,
    const QString& boundaryLevel,
    const QString& boundaryLayerPath,
    const QString& bandPart)
{
    if (outputPath.trimmed().isEmpty() || sampleLinePath.trimmed().isEmpty() || errorTypes.isEmpty()) {
        return 0;
    }

    GDALDataset* sourceDataset = nullptr;
    OGRLayer* sourceLayer = openFirstLayer(sourceDataset, sampleLinePath);
    if (sourceLayer == nullptr) {
        return 0;
    }

    QFileInfo outInfo(outputPath);
    QDir().mkpath(outInfo.absolutePath());
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (driver == nullptr) {
        GDALClose(sourceDataset);
        return 0;
    }
    GDALDataset* outputDataset = nullptr;
    OGRLayer* outputLayer = nullptr;
    if (QFileInfo::exists(outputPath)) {
        outputDataset = static_cast<GDALDataset*>(GDALOpenEx(outputPath.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr, nullptr, nullptr));
        if (outputDataset != nullptr && outputDataset->GetLayerCount() > 0) {
            outputLayer = outputDataset->GetLayer(0);
        }
    }
    if (outputLayer == nullptr) {
        outputDataset = driver->Create(outputPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr);
        if (outputDataset == nullptr) {
            GDALClose(sourceDataset);
            return 0;
        }

        char** layerOptions = boundaryBandUtf8LayerOptions();
        outputLayer = outputDataset->CreateLayer(outInfo.completeBaseName().toUtf8().constData(), nullptr, wkbLineString, layerOptions);
        CSLDestroy(layerOptions);
        if (outputLayer == nullptr) {
            GDALClose(outputDataset);
            GDALClose(sourceDataset);
            return 0;
        }

        OGRFieldDefn seqField("seq", OFTInteger);
        outputLayer->CreateField(&seqField);
        OGRFieldDefn widthField("width", OFTReal);
        widthField.SetWidth(32);
        widthField.SetPrecision(8);
        outputLayer->CreateField(&widthField);
        OGRFieldDefn errorTypeField("error_type", OFTString);
        errorTypeField.SetWidth(48);
        outputLayer->CreateField(&errorTypeField);
        OGRFieldDefn hitCountField("hit_count", OFTInteger);
        outputLayer->CreateField(&hitCountField);
        OGRFieldDefn checkField("check_item", OFTString);
        checkField.SetWidth(48);
        outputLayer->CreateField(&checkField);
        OGRFieldDefn levelField("boundary_lv", OFTString);
        levelField.SetWidth(32);
        outputLayer->CreateField(&levelField);
        OGRFieldDefn layerField("boundary_ly", OFTString);
        layerField.SetWidth(96);
        outputLayer->CreateField(&layerField);
        OGRFieldDefn partField("band_part", OFTString);
        partField.SetWidth(16);
        outputLayer->CreateField(&partField);
        OGRFieldDefn detailField("detail", OFTString);
        detailField.SetWidth(160);
        outputLayer->CreateField(&detailField);
    }
    auto ensureOutputField = [&](const char* name, int width) {
        if (outputLayer->GetLayerDefn()->GetFieldIndex(name) < 0) {
            OGRFieldDefn field(name, OFTString);
            field.SetWidth(width);
            outputLayer->CreateField(&field);
        }
    };
    ensureOutputField("boundary_lv", 32);
    ensureOutputField("boundary_ly", 96);
    ensureOutputField("band_part", 16);

    const int seqIndex = sourceLayer->GetLayerDefn()->GetFieldIndex("seq");
    const int widthIndex = sourceLayer->GetLayerDefn()->GetFieldIndex("width");
    const int errorTypeIndex = sourceLayer->GetLayerDefn()->GetFieldIndex("error_type");
    const int hitCountIndex = sourceLayer->GetLayerDefn()->GetFieldIndex("hit_count");
    if (errorTypeIndex < 0) {
        GDALClose(outputDataset);
        GDALClose(sourceDataset);
        driver->Delete(outputPath.toUtf8().constData());
        return 0;
    }

    int writtenCount = 0;
    sourceLayer->ResetReading();
    OGRFeature* sourceFeature = nullptr;
    while ((sourceFeature = sourceLayer->GetNextFeature()) != nullptr) {
        const QString sourceErrorType = QString::fromUtf8(sourceFeature->GetFieldAsString(errorTypeIndex)).trimmed();
        if (errorTypes.contains(sourceErrorType)) {
            OGRGeometry* geometry = sourceFeature->GetGeometryRef();
            if (geometry != nullptr && !geometry->IsEmpty()) {
                OGRFeature* outputFeature = OGRFeature::CreateFeature(outputLayer->GetLayerDefn());
                if (seqIndex >= 0) {
                    outputFeature->SetField("seq", sourceFeature->GetFieldAsInteger(seqIndex));
                }
                if (widthIndex >= 0) {
                    outputFeature->SetField("width", sourceFeature->GetFieldAsDouble(widthIndex));
                }
                outputFeature->SetField("error_type", boundaryBandErrorTypeText(sourceErrorType).toUtf8().constData());
                if (hitCountIndex >= 0) {
                    outputFeature->SetField("hit_count", sourceFeature->GetFieldAsInteger(hitCountIndex));
                }
                outputFeature->SetField("check_item", checkItem.toUtf8().constData());
                outputFeature->SetField("boundary_lv", boundaryLevel.toUtf8().constData());
                outputFeature->SetField("boundary_ly", QFileInfo(boundaryLayerPath).completeBaseName().toUtf8().constData());
                outputFeature->SetField("band_part", bandPart.toUtf8().constData());
                outputFeature->SetField("detail", boundaryBandSampleErrorDetail(sourceErrorType, detail).toUtf8().constData());
                outputFeature->SetGeometry(geometry);
                if (outputLayer->CreateFeature(outputFeature) == OGRERR_NONE) {
                    ++writtenCount;
                }
                OGRFeature::DestroyFeature(outputFeature);
            }
        }
        OGRFeature::DestroyFeature(sourceFeature);
    }

    GDALClose(outputDataset);
    GDALClose(sourceDataset);
    if (writtenCount == 0) {
        driver->Delete(outputPath.toUtf8().constData());
    }
    return writtenCount;
}

static OGRLayer* openOrCreateComplexLineErrorLayer(GDALDataset*& dataset, const QString& outputPath)
{
    dataset = nullptr;
    QFileInfo outInfo(outputPath);
    QDir().mkpath(outInfo.absolutePath());
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (driver == nullptr) {
        return nullptr;
    }

    OGRLayer* layer = nullptr;
    if (QFileInfo::exists(outputPath)) {
        dataset = static_cast<GDALDataset*>(GDALOpenEx(outputPath.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr, nullptr, nullptr));
        if (dataset != nullptr && dataset->GetLayerCount() > 0) {
            layer = dataset->GetLayer(0);
        }
    }
    if (layer == nullptr) {
        dataset = driver->Create(outputPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr);
        if (dataset == nullptr) {
            return nullptr;
        }
        char** layerOptions = boundaryBandUtf8LayerOptions();
        layer = dataset->CreateLayer(outInfo.completeBaseName().toUtf8().constData(), nullptr, wkbLineString, layerOptions);
        CSLDestroy(layerOptions);
        if (layer == nullptr) {
            GDALClose(dataset);
            dataset = nullptr;
            return nullptr;
        }
    }

    auto ensureIntField = [&](const char* name) {
        if (layer->GetLayerDefn()->GetFieldIndex(name) < 0) {
            OGRFieldDefn field(name, OFTInteger);
            layer->CreateField(&field);
        }
    };
    auto ensureRealField = [&](const char* name, int width, int precision) {
        if (layer->GetLayerDefn()->GetFieldIndex(name) < 0) {
            OGRFieldDefn field(name, OFTReal);
            field.SetWidth(width);
            field.SetPrecision(precision);
            layer->CreateField(&field);
        }
    };
    auto ensureStringField = [&](const char* name, int width) {
        if (layer->GetLayerDefn()->GetFieldIndex(name) < 0) {
            OGRFieldDefn field(name, OFTString);
            field.SetWidth(width);
            layer->CreateField(&field);
        }
    };

    ensureIntField("seq");
    ensureRealField("width", 32, 8);
    ensureStringField("error_type", 48);
    ensureIntField("hit_count");
    ensureStringField("check_item", 48);
    ensureStringField("boundary_lv", 32);
    ensureStringField("boundary_ly", 96);
    ensureStringField("band_part", 16);
    ensureStringField("detail", 160);
    ensureStringField("actual_val", 64);
    return layer;
}

static OGRGeometry* createLineGeometryForComplexError(OGRGeometry* geometry)
{
    if (geometry == nullptr || geometry->IsEmpty()) {
        return nullptr;
    }
    const OGRwkbGeometryType flatType = wkbFlatten(geometry->getGeometryType());
    if (flatType == wkbLineString || flatType == wkbMultiLineString) {
        return geometry->clone();
    }
    OGRGeometry* boundary = geometry->Boundary();
    if (boundary != nullptr && !boundary->IsEmpty()) {
        return boundary;
    }
    if (boundary != nullptr) {
        delete boundary;
    }
    return nullptr;
}

static int appendDerivedComplexErrorFeatures(
    const QString& outputPath,
    const QVector<OGRGeometry*>& geometries,
    const QString& errorType,
    const QString& checkItem,
    const QString& detail,
    const QString& boundaryLevel,
    const QString& boundaryLayerPath,
    const QString& bandPart,
    const QString& actualVal)
{
    if (outputPath.trimmed().isEmpty() || geometries.isEmpty()) {
        return 0;
    }

    GDALDataset* dataset = nullptr;
    OGRLayer* layer = openOrCreateComplexLineErrorLayer(dataset, outputPath);
    if (layer == nullptr) {
        return 0;
    }

    int writtenCount = 0;
    for (OGRGeometry* geometry : geometries) {
        OGRGeometry* lineGeometry = createLineGeometryForComplexError(geometry);
        if (lineGeometry == nullptr || lineGeometry->IsEmpty()) {
            if (lineGeometry != nullptr) delete lineGeometry;
            continue;
        }
        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        feature->SetField("seq", 0);
        feature->SetField("width", 0.0);
        feature->SetField("error_type", boundaryBandErrorTypeText(errorType).toUtf8().constData());
        feature->SetField("hit_count", 0);
        feature->SetField("check_item", checkItem.toUtf8().constData());
        feature->SetField("boundary_lv", boundaryLevel.toUtf8().constData());
        feature->SetField("boundary_ly", QFileInfo(boundaryLayerPath).completeBaseName().toUtf8().constData());
        feature->SetField("band_part", bandPart.toUtf8().constData());
        feature->SetField("detail", boundaryBandSampleErrorDetail(errorType, detail).toUtf8().constData());
        feature->SetField("actual_val", actualVal.toUtf8().constData());
        feature->SetGeometry(lineGeometry);
        if (layer->CreateFeature(feature) == OGRERR_NONE) {
            ++writtenCount;
        }
        OGRFeature::DestroyFeature(feature);
        delete lineGeometry;
    }

    GDALClose(dataset);
    return writtenCount;
}

static int appendExtremeWidthSampleErrorFeatures(
    const QString& outputPath,
    const QString& sampleLinePath,
    const QString& errorType,
    const QString& checkItem,
    const QString& detail,
    const QString& boundaryLevel,
    const QString& boundaryLayerPath,
    const QString& bandPart,
    const QString& actualVal)
{
    if (outputPath.trimmed().isEmpty() || sampleLinePath.trimmed().isEmpty()) {
        return 0;
    }

    GDALDataset* sourceDataset = nullptr;
    OGRLayer* sourceLayer = openFirstLayer(sourceDataset, sampleLinePath);
    if (sourceLayer == nullptr) {
        return 0;
    }

    const int widthIndex = sourceLayer->GetLayerDefn()->GetFieldIndex("width");
    const int seqIndex = sourceLayer->GetLayerDefn()->GetFieldIndex("seq");
    if (widthIndex < 0) {
        GDALClose(sourceDataset);
        return 0;
    }

    OGRFeature* minFeature = nullptr;
    OGRFeature* maxFeature = nullptr;
    double minWidth = std::numeric_limits<double>::max();
    double maxWidth = -std::numeric_limits<double>::max();

    sourceLayer->ResetReading();
    OGRFeature* sourceFeature = nullptr;
    while ((sourceFeature = sourceLayer->GetNextFeature()) != nullptr) {
        OGRGeometry* geometry = sourceFeature->GetGeometryRef();
        const double width = sourceFeature->GetFieldAsDouble(widthIndex);
        if (geometry != nullptr && !geometry->IsEmpty() && width > 0.0) {
            if (width < minWidth) {
                if (minFeature != nullptr) OGRFeature::DestroyFeature(minFeature);
                minFeature = sourceFeature->Clone();
                minWidth = width;
            }
            if (width > maxWidth) {
                if (maxFeature != nullptr) OGRFeature::DestroyFeature(maxFeature);
                maxFeature = sourceFeature->Clone();
                maxWidth = width;
            }
        }
        OGRFeature::DestroyFeature(sourceFeature);
    }
    GDALClose(sourceDataset);

    QVector<OGRFeature*> selectedFeatures;
    if (minFeature != nullptr) selectedFeatures.append(minFeature);
    if (maxFeature != nullptr && maxFeature != minFeature) selectedFeatures.append(maxFeature);
    if (selectedFeatures.isEmpty()) {
        return 0;
    }

    GDALDataset* dataset = nullptr;
    OGRLayer* layer = openOrCreateComplexLineErrorLayer(dataset, outputPath);
    if (layer == nullptr) {
        for (OGRFeature* feature : selectedFeatures) OGRFeature::DestroyFeature(feature);
        return 0;
    }

    int writtenCount = 0;
    for (OGRFeature* selectedFeature : selectedFeatures) {
        OGRGeometry* sourceGeometry = selectedFeature->GetGeometryRef();
        if (sourceGeometry == nullptr || sourceGeometry->IsEmpty()) {
            continue;
        }
        OGRFeature* outputFeature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        if (seqIndex >= 0) {
            outputFeature->SetField("seq", selectedFeature->GetFieldAsInteger(seqIndex));
        }
        outputFeature->SetField("width", selectedFeature->GetFieldAsDouble(widthIndex));
        outputFeature->SetField("error_type", boundaryBandErrorTypeText(errorType).toUtf8().constData());
        outputFeature->SetField("hit_count", 0);
        outputFeature->SetField("check_item", checkItem.toUtf8().constData());
        outputFeature->SetField("boundary_lv", boundaryLevel.toUtf8().constData());
        outputFeature->SetField("boundary_ly", QFileInfo(boundaryLayerPath).completeBaseName().toUtf8().constData());
        outputFeature->SetField("band_part", bandPart.toUtf8().constData());
        outputFeature->SetField("detail", boundaryBandSampleErrorDetail(errorType, detail).toUtf8().constData());
        outputFeature->SetField("actual_val", actualVal.toUtf8().constData());
        outputFeature->SetGeometry(sourceGeometry);
        if (layer->CreateFeature(outputFeature) == OGRERR_NONE) {
            ++writtenCount;
        }
        OGRFeature::DestroyFeature(outputFeature);
    }

    GDALClose(dataset);
    for (OGRFeature* feature : selectedFeatures) OGRFeature::DestroyFeature(feature);
    return writtenCount;
}

static bool writeBoundaryBandJson(
    const QString& outputJsonPath,
    bool success,
    bool passed,
    const QString& boundaryLinePath,
    const QString& adminPolygonPath,
    const QString& bandPolygonPath,
    const QString& schemaXmlPath,
    const SE_BoundaryBandRenderRuleConfig& config,
    const QString& dominantBoundaryLevel,
    int boundaryFeatureCount,
    int adminFeatureCount,
    int bandFeatureCount,
    int invalidBandGeometryCount,
    const QString& actualSide,
    double sideInsideLength,
    double sideOutsideLength,
    int directionSampleCount,
    int directionNoHitSampleCount,
    double insideArea,
    double outsideArea,
    double totalBoundaryLength,
    double approxAverageWidth,
    int sampleCount,
    double minSampleWidth,
    double maxSampleWidth,
    double avgSampleWidth,
    double stddevSampleWidth,
    double widthCv,
    int abnormalSampleHitCount,
    int widthJumpCount,
    int smallFragmentCount,
    int overlapFeaturePairCount,
    double overlapArea,
    int sharpCornerCount,
    int sharpCornerAbnormalCount,
    double distortionWidthRatio,
    int distortionAbnormalCount,
    const QString& dominantFillColor,
    const QString& dominantStartColor,
    const QString& dominantEndColor,
    const QString& dominantGradientDirection,
    const QString& dominantGradientFunction,
    bool errorLayerWritten,
    const QString& outputShpPath,
    const QString& outputGeoJsonPath,
    const QString& tempSampleLinePath,
    bool matchedLevelRule,
    int scale,
    bool onlyCheckSide)
{
    QJsonObject item;
    item["id"] = "214";
    item["name"] = QStringLiteral("境界线色带渲染合规性检查");
    item["status"] = success ? QStringLiteral("质检成功") : QStringLiteral("质检失败");
    item["err_exist"] = passed ? QStringLiteral("false") : QStringLiteral("true");

    QJsonObject errors;
    int errorIndex = 1;
    if (!success) {
        QJsonObject error;
        error["type"] = "BAND_INPUT_ERROR";
        error["description"] = QStringLiteral("境界线色带检查输入图层不完整或无法读取");
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    if (success && bandFeatureCount == 0) {
        QJsonObject error;
        error["type"] = "BAND_MISSING";
        error["description"] = QStringLiteral("未读取到色带面要素");
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    if (success && invalidBandGeometryCount > 0) {
        QJsonObject error;
        error["type"] = "BAND_GEOM_INVALID";
        error["description"] = QStringLiteral("色带面存在几何非法、自相交或破图风险");
        error["expected_value"] = 0;
        error["actual_value"] = invalidBandGeometryCount;
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    const QString jsonExpectedSideText = sideCodeToText(QString::fromUtf8(config.szExpectedSide));
    const bool jsonSidePassed = directionSampleCount > 0
        ? bandSideMatchesRule(
              QString::fromUtf8(config.szExpectedSide),
              sideInsideLength,
              sideOutsideLength,
              directionSampleCount,
              config.fSideOverflowToleranceRatio)
        : bandSideAreaMatchesRule(
              QString::fromUtf8(config.szExpectedSide),
              insideArea,
              outsideArea,
              config.fSideOverflowToleranceRatio);
    if (success && !jsonSidePassed) {
        QJsonObject error;
        error["type"] = "BAND_SIDE_ERROR";
        error["description"] = QStringLiteral("色带挂接方向与规则要求不一致");
        error["expected_value"] = jsonExpectedSideText;
        error["actual_value"] = actualSide;
        error["inside_length"] = sideInsideLength;
        error["outside_length"] = sideOutsideLength;
        error["sample_count"] = directionSampleCount;
        error["tolerance_ratio"] = config.fSideOverflowToleranceRatio;
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    const bool hasSampleWidth = !onlyCheckSide && success && sampleCount > 0 && avgSampleWidth > 0.0;
    const double minAllowedWidth = config.fStandardWidthMm - config.fWidthToleranceMm;
    const double maxAllowedWidth = config.fStandardWidthMm + config.fWidthToleranceMm;
    if (hasSampleWidth &&
        (avgSampleWidth < minAllowedWidth || avgSampleWidth > maxAllowedWidth ||
         minSampleWidth < minAllowedWidth || maxSampleWidth > maxAllowedWidth)) {
        QJsonObject error;
        error["type"] = "BAND_WIDTH_ERROR";
        error["description"] = QStringLiteral("色带采样宽度超出规则允许范围");
        error["expected_value"] = config.fStandardWidthMm;
        error["actual_value"] = avgSampleWidth;
        error["min_sample_width"] = minSampleWidth;
        error["max_sample_width"] = maxSampleWidth;
        error["tolerance"] = config.fWidthToleranceMm;
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    if (!onlyCheckSide && success && sampleCount > 0 && widthCv > config.fUniformityCvThreshold) {
        QJsonObject error;
        error["type"] = "BAND_WIDTH_UNEVEN";
        error["description"] = QStringLiteral("色带采样宽度变异系数超出允许阈值");
        error["expected_value"] = config.fUniformityCvThreshold;
        error["actual_value"] = widthCv;
        error["sample_count"] = sampleCount;
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    if (success && abnormalSampleHitCount > 0) {
        QJsonObject error;
        error["type"] = "BAND_GEOM_INVALID";
        error["description"] = QStringLiteral("复杂边界或尖角处出现多重交点，存在重叠、扭曲或破图风险");
        error["expected_value"] = 0;
        error["actual_value"] = abnormalSampleHitCount;
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    if (!onlyCheckSide && success && widthJumpCount > 0) {
        QJsonObject error;
        error["type"] = "BAND_WIDTH_UNEVEN";
        error["description"] = QStringLiteral("相邻采样剖面宽度突变，存在局部扭曲或宽度不连续风险");
        error["expected_value"] = 0;
        error["actual_value"] = widthJumpCount;
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    if (!onlyCheckSide && success && smallFragmentCount > 0) {
        QJsonObject error;
        error["type"] = "BAND_GEOM_INVALID";
        error["description"] = QStringLiteral("色带存在小碎片面，存在碎片化或破图风险");
        error["expected_value"] = 0;
        error["actual_value"] = smallFragmentCount;
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    if (success && overlapFeaturePairCount > 0) {
        QJsonObject error;
        error["type"] = "BAND_OVERLAP_ERROR";
        error["description"] = QStringLiteral("色带原始面要素之间存在面积重叠，存在重叠渲染风险");
        error["expected_value"] = 0;
        error["actual_value"] = overlapFeaturePairCount;
        error["overlap_area"] = overlapArea;
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    if (success && sharpCornerAbnormalCount > 0) {
        QJsonObject error;
        error["type"] = "SHARP_CORNER_RISK";
        error["description"] = QStringLiteral("尖角附近色带局部出现碎片、断裂或几何非法风险");
        error["expected_value"] = 0;
        error["actual_value"] = sharpCornerAbnormalCount;
        error["sharp_corner_count"] = sharpCornerCount;
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    if (success && distortionAbnormalCount > 0) {
        QJsonObject error;
        error["type"] = "DISTORTION_RATIO_EXCEEDED";
        error["description"] = QStringLiteral("采样宽度最大值与最小值比值超过阈值，存在整体或局部扭曲风险");
        error["expected_value"] = config.fDistortionWidthRatioThreshold;
        error["actual_value"] = distortionWidthRatio;
        error["min_sample_width"] = minSampleWidth;
        error["max_sample_width"] = maxSampleWidth;
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    const int complexBoundaryAbnormalCount = invalidBandGeometryCount + smallFragmentCount + overlapFeaturePairCount +
        abnormalSampleHitCount + widthJumpCount + sharpCornerAbnormalCount + distortionAbnormalCount;
    if (success && complexBoundaryAbnormalCount > 0) {
        QJsonObject error;
        error["type"] = "BAND_COMPLEX_BOUNDARY_RENDER_ERROR";
        error["description"] = QStringLiteral("复杂边界或尖角处存在重叠、扭曲、破图或宽度突变等异常渲染风险");
        error["expected_value"] = 0;
        error["actual_value"] = complexBoundaryAbnormalCount;
        error["invalid_geometry_count"] = invalidBandGeometryCount;
        error["small_fragment_count"] = smallFragmentCount;
        error["overlap_feature_pair_count"] = overlapFeaturePairCount;
        error["abnormal_sample_hit_count"] = abnormalSampleHitCount;
        error["width_jump_count"] = widthJumpCount;
        error["sharp_corner_count"] = sharpCornerCount;
        error["sharp_corner_abnormal_count"] = sharpCornerAbnormalCount;
        error["distortion_width_ratio"] = distortionWidthRatio;
        error["distortion_abnormal_count"] = distortionAbnormalCount;
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    if (success && config.bCheckColor && !colorMatchesWithinTolerance(QString::fromUtf8(config.szStandardFillColor), dominantFillColor, config.fColorTolerance)) {
        QJsonObject error;
        error["type"] = "BAND_COLOR_ERROR";
        error["description"] = QStringLiteral("色带填充颜色与规则要求不一致");
        error["expected_value"] = QString::fromUtf8(config.szStandardFillColor);
        error["actual_value"] = dominantFillColor;
        error["tolerance"] = config.fColorTolerance;
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    if (!onlyCheckSide && success && config.bCheckGradient &&
        (!colorMatchesWithinTolerance(QString::fromUtf8(config.szStandardGradientStartColor), dominantStartColor, config.fColorTolerance) ||
         !colorMatchesWithinTolerance(QString::fromUtf8(config.szStandardGradientEndColor), dominantEndColor, config.fColorTolerance))) {
        QJsonObject error;
        error["type"] = "BAND_GRADIENT_ERROR";
        error["description"] = QStringLiteral("色带渐变起止色与规则要求不一致");
        error["expected_value"] = QString(QString::fromUtf8(config.szStandardGradientStartColor) + QStringLiteral(" -> ") + QString::fromUtf8(config.szStandardGradientEndColor));
        error["actual_value"] = QString(dominantStartColor + QStringLiteral(" -> ") + dominantEndColor);
        error["tolerance"] = config.fColorTolerance;
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    if (success && config.bCheckGradient && !QString::fromUtf8(config.szStandardGradientDirection).trimmed().isEmpty() && dominantGradientDirection != QString::fromUtf8(config.szStandardGradientDirection)) {
        QJsonObject error;
        error["type"] = "BAND_GRADIENT_DIRECTION_ERROR";
        error["description"] = QStringLiteral("色带渐变方向与规则要求不一致");
        error["expected_value"] = QString::fromUtf8(config.szStandardGradientDirection);
        error["actual_value"] = dominantGradientDirection;
        errors[QString("error%1").arg(errorIndex++)] = error;
    }
    if (success && config.bCheckGradient && !QString::fromUtf8(config.szStandardGradientFunction).trimmed().isEmpty() && dominantGradientFunction != QString::fromUtf8(config.szStandardGradientFunction)) {
        QJsonObject error;
        error["type"] = "BAND_GRADIENT_FUNCTION_ERROR";
        error["description"] = QStringLiteral("色带渐变函数类型与规则要求不一致");
        error["expected_value"] = QString::fromUtf8(config.szStandardGradientFunction);
        error["actual_value"] = dominantGradientFunction;
        errors[QString("error%1").arg(errorIndex++)] = error;
    }

    QJsonObject info;
    info["boundary_line_layer"] = boundaryLinePath;
    info["admin_polygon_layer"] = adminPolygonPath;
    info["band_polygon_layer"] = bandPolygonPath;
    info["rule_xml"] = schemaXmlPath;
    info["scale"] = scale;
    info["standard_width_map_unit"] = scale > 0 ? config.fStandardWidthMm * scale / 1000.0 : 0.0;
    info["min_width_map_unit"] = scale > 0 ? std::max(0.0, config.fStandardWidthMm - config.fWidthToleranceMm) * scale / 1000.0 : 0.0;
    info["max_width_map_unit"] = scale > 0 ? (config.fStandardWidthMm + config.fWidthToleranceMm) * scale / 1000.0 : 0.0;
    info["expected_side"] = sideCodeToText(QString::fromUtf8(config.szExpectedSide));
    info["expected_side_rule"] = QString::fromUtf8(config.szExpectedSide);
    info["actual_side"] = actualSide;
    info["side_inside_length"] = sideInsideLength;
    info["side_outside_length"] = sideOutsideLength;
    info["side_direction_sample_count"] = directionSampleCount;
    info["side_no_hit_sample_count"] = directionNoHitSampleCount;
    info["side_overflow_tolerance_ratio"] = config.fSideOverflowToleranceRatio;
    info["inside_area"] = insideArea;
    info["outside_area"] = outsideArea;
    info["total_boundary_length"] = totalBoundaryLength;
    info["approx_average_width_mm"] = approxAverageWidth;
    info["sample_count"] = sampleCount;
    info["min_sample_width_mm"] = minSampleWidth;
    info["max_sample_width_mm"] = maxSampleWidth;
    info["avg_sample_width_mm"] = avgSampleWidth;
    info["stddev_sample_width_mm"] = stddevSampleWidth;
    info["width_cv"] = widthCv;
    info["abnormal_sample_hit_count"] = abnormalSampleHitCount;
    info["width_jump_count"] = widthJumpCount;
    info["small_fragment_count"] = smallFragmentCount;
    info["overlap_feature_pair_count"] = overlapFeaturePairCount;
    info["overlap_area"] = overlapArea;
    info["complex_boundary_check_status"] = "done";
    info["complex_boundary_abnormal_count"] = complexBoundaryAbnormalCount;
    info["standard_width_mm"] = config.fStandardWidthMm;
    info["width_tolerance_mm"] = config.fWidthToleranceMm;
    info["uniformity_cv_threshold"] = config.fUniformityCvThreshold;
    info["sample_interval_mm"] = config.fSampleIntervalMm;
    info["sample_density_per_mm"] = config.fSampleDensityPerMm;
    info["boundary_level_field"] = QString::fromUtf8(config.szBoundaryLevelField);
    info["rule_boundary_level"] = QString::fromUtf8(config.szBoundaryLevelValue);
    info["dominant_boundary_level"] = dominantBoundaryLevel;
    info["matched_level_rule"] = matchedLevelRule;
    info["boundary_level_matched"] = QString::fromUtf8(config.szBoundaryLevelValue).trimmed().isEmpty() || dominantBoundaryLevel.trimmed().isEmpty() || QString::fromUtf8(config.szBoundaryLevelValue) == dominantBoundaryLevel;
    info["color_field"] = QString::fromUtf8(config.szColorField);
    info["gradient_start_color_field"] = QString::fromUtf8(config.szGradientStartColorField);
    info["gradient_end_color_field"] = QString::fromUtf8(config.szGradientEndColorField);
    info["gradient_direction_field"] = QString::fromUtf8(config.szGradientDirectionField);
    info["gradient_function_field"] = QString::fromUtf8(config.szGradientFunctionField);
    info["standard_fill_color"] = QString::fromUtf8(config.szStandardFillColor);
    info["standard_gradient_start_color"] = QString::fromUtf8(config.szStandardGradientStartColor);
    info["standard_gradient_end_color"] = QString::fromUtf8(config.szStandardGradientEndColor);
    info["standard_gradient_direction"] = QString::fromUtf8(config.szStandardGradientDirection);
    info["standard_gradient_function"] = QString::fromUtf8(config.szStandardGradientFunction);
    info["color_tolerance"] = config.fColorTolerance;
    info["complex_geometry_tolerance_mm"] = config.fComplexGeometryToleranceMm;
    info["sharp_corner_min_angle_deg"] = config.fSharpCornerMinAngleDeg;
    info["sharp_corner_local_overlap_tolerance_mm"] = config.fSharpCornerLocalOverlapToleranceMm;
    info["distortion_width_ratio_threshold"] = config.fDistortionWidthRatioThreshold;
    info["dominant_fill_color"] = dominantFillColor;
    info["dominant_gradient_start_color"] = dominantStartColor;
    info["dominant_gradient_end_color"] = dominantEndColor;
    info["dominant_gradient_direction"] = dominantGradientDirection;
    info["dominant_gradient_function"] = dominantGradientFunction;
    info["check_geometry_validity"] = config.bCheckGeometryValidity;
    info["check_color"] = config.bCheckColor;
    info["check_gradient"] = config.bCheckGradient;
    info["boundary_feature_count"] = boundaryFeatureCount;
    info["admin_polygon_feature_count"] = adminFeatureCount;
    info["band_feature_count"] = bandFeatureCount;
    info["invalid_band_geometry_count"] = invalidBandGeometryCount;
    info["sharp_corner_count"] = sharpCornerCount;
    info["sharp_corner_abnormal_count"] = sharpCornerAbnormalCount;
    info["distortion_width_ratio"] = distortionWidthRatio;
    info["distortion_abnormal_count"] = distortionAbnormalCount;
    info["side_check_status"] = success ? QStringLiteral("done") : QStringLiteral("skipped");
    info["width_check_status"] = QStringLiteral("done");
    info["geometry_check_status"] = config.bCheckGeometryValidity ? QStringLiteral("done") : QStringLiteral("skipped");
    info["color_check_status"] = config.bCheckColor ? QStringLiteral("done") : QStringLiteral("skipped");
    info["gradient_check_status"] = config.bCheckGradient ? QStringLiteral("done") : QStringLiteral("skipped");
    info["error_result_shp"] = outputShpPath;
    info["error_result_geojson"] = outputGeoJsonPath;
    info["error_layer_written"] = errorLayerWritten;
    info["temp_sample_line_layer"] = tempSampleLinePath;
    info["description"] = QStringLiteral("境界线色带渲染合规性检查稳定版第一版：已完成方向、宽度、均匀度、几何、颜色与渐变字段检查");

    item["info"] = info;
    item["errors"] = errors;

    QJsonObject root;
    root["check_item"] = item;
    root["ok"] = success;
    root["passed"] = passed;

    QFileInfo outInfo(outputJsonPath);
    QDir().mkpath(outInfo.absolutePath());
    QFile jsonFile(outputJsonPath);
    if (!jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    jsonFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    jsonFile.close();
    return true;
}

SE_Error BoundaryBandRenderCheck(
    const char* szBoundaryLineShpPath,
    const char* szAdminPolygonShpPath,
    const char* szBandPolygonShpPath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    const char* szOutputShpPath,
    const char* szOutputGeoJsonPath,
    const char* szTempSampleLineShpPath,
    int nScale,
    bool& bPassed,
    bool bOnlyCheckSide)
{
    bPassed = false;
    GDALAllRegister();

    SE_BoundaryBandRenderRuleConfig config;
    bool matchedLevelRule = false;
    ParseXmlForBoundaryBandRender(szSchemaXmlPath, config);

    const QString boundaryPath = QString::fromUtf8(szBoundaryLineShpPath ? szBoundaryLineShpPath : "");
    const QString adminPath = QString::fromUtf8(szAdminPolygonShpPath ? szAdminPolygonShpPath : "");
    const QString bandPath = QString::fromUtf8(szBandPolygonShpPath ? szBandPolygonShpPath : "");
    const QString xmlPath = QString::fromUtf8(szSchemaXmlPath ? szSchemaXmlPath : "");
    const QString jsonPath = QString::fromUtf8(szOutputJsonPath ? szOutputJsonPath : "");
    const QString outputShpPath = QString::fromUtf8(szOutputShpPath ? szOutputShpPath : "");
    const QString outputGeoJsonPath = QString::fromUtf8(szOutputGeoJsonPath ? szOutputGeoJsonPath : "");
    const QString tempSamplePath = QString::fromUtf8(szTempSampleLineShpPath ? szTempSampleLineShpPath : "");

    GDALDataset* boundaryDataset = nullptr;
    GDALDataset* adminDataset = nullptr;
    GDALDataset* bandDataset = nullptr;
    OGRLayer* boundaryLayer = openFirstLayer(boundaryDataset, boundaryPath);
    OGRLayer* adminLayer = openFirstLayer(adminDataset, adminPath);
    OGRLayer* bandLayer = openFirstLayer(bandDataset, bandPath);

    const bool success = boundaryLayer != nullptr && adminLayer != nullptr && bandLayer != nullptr;
    const int boundaryFeatureCount = boundaryLayer ? static_cast<int>(boundaryLayer->GetFeatureCount()) : 0;
    const int adminFeatureCount = adminLayer ? static_cast<int>(adminLayer->GetFeatureCount()) : 0;
    const int bandFeatureCount = bandLayer ? static_cast<int>(bandLayer->GetFeatureCount()) : 0;
    const int invalidBandGeometryCount = (success && config.bCheckGeometryValidity) ? countInvalidGeometries(bandLayer) : 0;

    QString dominantBoundaryLevel;
    QString dominantFillColor;
    QString dominantStartColor;
    QString dominantEndColor;
    QString dominantGradientDirection;
    QString dominantGradientFunction;
    QString actualSide = QStringLiteral("unknown");
    double insideArea = 0.0;
    double outsideArea = 0.0;
    double sideInsideLength = 0.0;
    double sideOutsideLength = 0.0;
    int directionSampleCount = 0;
    int directionNoHitSampleCount = 0;
    double totalBoundaryLength = 0.0;
    double approxAverageWidth = 0.0;
    int sampleCount = 0;
    double minSampleWidth = 0.0;
    double maxSampleWidth = 0.0;
    double avgSampleWidth = 0.0;
    double stddevSampleWidth = 0.0;
    double widthCv = 0.0;
    int abnormalSampleHitCount = 0;
    int widthJumpCount = 0;
    int smallFragmentCount = 0;
    int overlapFeaturePairCount = 0;
    double overlapArea = 0.0;
    double distortionWidthRatio = 0.0;
    OGRGeometry* overlapUnion = nullptr;
    int effectiveScale = 0;
    int sharpCornerCount = 0;
    int sharpCornerAbnormalCount = 0;
    int distortionAbnormalCount = 0;
    QVector<OGRGeometry*> invalidBandGeometries;
    QVector<OGRGeometry*> smallFragmentGeometries;
    QVector<OGRGeometry*> sharpCornerGeometries;

    OGRGeometry* adminUnion = nullptr;
    OGRGeometry* bandUnion = nullptr;

    if (success) {
        if (config.bCheckGeometryValidity) {
            collectInvalidFeatureGeometries(bandLayer, invalidBandGeometries);
        }
        int matchedCount = 0;
        int totalCount = 0;
        collectDominantFieldValue(boundaryLayer, config.szBoundaryLevelField, dominantBoundaryLevel, matchedCount, totalCount);
        if (dominantBoundaryLevel.trimmed().isEmpty()) {
            dominantBoundaryLevel = inferBoundaryLevelFromPath(boundaryPath);
        }
        parseXmlForBoundaryBandRenderByLevel(szSchemaXmlPath, dominantBoundaryLevel, config, matchedLevelRule);
        collectDominantFieldValue(boundaryLayer, config.szBoundaryLevelField, dominantBoundaryLevel, matchedCount, totalCount);
        if (dominantBoundaryLevel.trimmed().isEmpty()) {
            dominantBoundaryLevel = inferBoundaryLevelFromPath(boundaryPath);
        }
        collectDominantFieldValue(bandLayer, config.szColorField, dominantFillColor, matchedCount, totalCount);
        collectDominantFieldValue(bandLayer, config.szGradientStartColorField, dominantStartColor, matchedCount, totalCount);
        collectDominantFieldValue(bandLayer, config.szGradientEndColorField, dominantEndColor, matchedCount, totalCount);
        collectDominantFieldValue(bandLayer, config.szGradientDirectionField, dominantGradientDirection, matchedCount, totalCount);
        collectDominantFieldValue(bandLayer, config.szGradientFunctionField, dominantGradientFunction, matchedCount, totalCount);
        effectiveScale = nScale > 0 ? nScale : config.nScale;

        adminUnion = buildUnionGeometry(adminLayer);
        bandUnion = buildUnionGeometry(bandLayer);
        const QString areaSideCode = detectBandSideByArea(adminUnion, bandUnion, insideArea, outsideArea);
        const double configuredSideSampleIntervalMm = config.fSampleDensityPerMm > 0.0
            ? (1.0 / config.fSampleDensityPerMm)
            : config.fSampleIntervalMm;
        QString sampledSideCode = detectBandSideByBoundarySamples(
            boundaryLayer,
            adminUnion,
            bandUnion,
            configuredSideSampleIntervalMm,
            effectiveScale,
            config.fStandardWidthMm,
            config.fWidthToleranceMm,
            config.fSideProfileLengthFactor,
            config.fSideOverflowToleranceRatio,
            sideInsideLength,
            sideOutsideLength,
            directionSampleCount,
            directionNoHitSampleCount);
        if (sampledSideCode == QStringLiteral("unknown")) {
            sampledSideCode = areaSideCode;
        }
        actualSide = sideCodeToText(sampledSideCode);
        OGRGeometry* bandBoundaryForLength = bandUnion != nullptr ? bandUnion->Boundary() : nullptr;
        totalBoundaryLength = calculateGeometryLength(bandBoundaryForLength);
        if (bandBoundaryForLength != nullptr) {
            delete bandBoundaryForLength;
        }
        approxAverageWidth = calculateApproxAverageWidth(bandUnion, totalBoundaryLength);
        if (effectiveScale > 0) {
            approxAverageWidth = approxAverageWidth * 1000.0 / effectiveScale;
        }
        if (!bOnlyCheckSide) {
            const double configuredSampleIntervalMm = config.fSampleDensityPerMm > 0.0
                ? (1.0 / config.fSampleDensityPerMm)
                : config.fSampleIntervalMm;
            calculateSampleWidthStats(
                boundaryLayer,
                adminUnion,
                bandUnion,
                configuredSampleIntervalMm,
                effectiveScale,
                QString::fromUtf8(config.szExpectedSide),
                config.fStandardWidthMm,
                config.fWidthToleranceMm,
                tempSamplePath,
                sampleCount,
                minSampleWidth,
                maxSampleWidth,
                avgSampleWidth,
                stddevSampleWidth,
                widthCv,
                abnormalSampleHitCount,
                widthJumpCount);
            const double minFragmentAreaMapUnit = effectiveScale > 0 ? config.fMinFragmentAreaMm2 * effectiveScale * effectiveScale / 1000000.0 : config.fMinFragmentAreaMm2;
            const double geometryToleranceMapUnit = effectiveScale > 0 ? config.fComplexGeometryToleranceMm * effectiveScale / 1000.0 : config.fComplexGeometryToleranceMm;
            const double minAreaMapUnit = geometryToleranceMapUnit > 0.0
                ? std::max(minFragmentAreaMapUnit, geometryToleranceMapUnit * geometryToleranceMapUnit)
                : minFragmentAreaMapUnit;
            const double sharpCornerToleranceMapUnit = effectiveScale > 0
                ? config.fSharpCornerLocalOverlapToleranceMm * effectiveScale / 1000.0
                : config.fSharpCornerLocalOverlapToleranceMm;
            const double overlapThresholdMapUnit = sharpCornerToleranceMapUnit > 0.0
                ? std::max(minAreaMapUnit, sharpCornerToleranceMapUnit * sharpCornerToleranceMapUnit)
                : minAreaMapUnit;
            smallFragmentCount = countSmallFragments(bandUnion, minAreaMapUnit);
            collectSmallFragmentGeometries(bandUnion, minAreaMapUnit, smallFragmentGeometries);
            overlapFeaturePairCount = countOverlappingBandFeatures(bandLayer, overlapThresholdMapUnit, overlapArea, overlapUnion);
            sharpCornerAbnormalCount = countSharpCornerRenderRisks(
                boundaryLayer,
                bandUnion,
                config.fSharpCornerMinAngleDeg,
                std::max(sharpCornerToleranceMapUnit, geometryToleranceMapUnit),
                minAreaMapUnit,
                sharpCornerCount,
                &sharpCornerGeometries);
        }
    }

    const QString expectedSideText = sideCodeToText(QString::fromUtf8(config.szExpectedSide));
    const bool sidePassed = directionSampleCount > 0
        ? bandSideMatchesRule(
              QString::fromUtf8(config.szExpectedSide),
              sideInsideLength,
              sideOutsideLength,
              directionSampleCount,
              config.fSideOverflowToleranceRatio)
        : bandSideAreaMatchesRule(
              QString::fromUtf8(config.szExpectedSide),
              insideArea,
              outsideArea,
              config.fSideOverflowToleranceRatio);
    const bool hasSampleWidth = sampleCount > 0 && avgSampleWidth > 0.0;
    const double minAllowedWidth = config.fStandardWidthMm - config.fWidthToleranceMm;
    const double maxAllowedWidth = config.fStandardWidthMm + config.fWidthToleranceMm;
    const bool widthPassed = !hasSampleWidth ||
        (avgSampleWidth >= minAllowedWidth && avgSampleWidth <= maxAllowedWidth &&
         minSampleWidth >= minAllowedWidth && maxSampleWidth <= maxAllowedWidth);
    const bool uniformityPassed = (sampleCount <= 0) || (widthCv <= config.fUniformityCvThreshold);
    const bool colorPassed = !config.bCheckColor || colorMatchesWithinTolerance(QString::fromUtf8(config.szStandardFillColor), dominantFillColor, config.fColorTolerance);
    const bool gradientColorPassed = !config.bCheckGradient ||
        (colorMatchesWithinTolerance(QString::fromUtf8(config.szStandardGradientStartColor), dominantStartColor, config.fColorTolerance) &&
         colorMatchesWithinTolerance(QString::fromUtf8(config.szStandardGradientEndColor), dominantEndColor, config.fColorTolerance));
    const bool gradientDirectionPassed = !config.bCheckGradient || QString::fromUtf8(config.szStandardGradientDirection).trimmed().isEmpty() || dominantGradientDirection == QString::fromUtf8(config.szStandardGradientDirection);
    const bool gradientFunctionPassed = !config.bCheckGradient || QString::fromUtf8(config.szStandardGradientFunction).trimmed().isEmpty() || dominantGradientFunction == QString::fromUtf8(config.szStandardGradientFunction);
    distortionWidthRatio = (minSampleWidth > 1e-8) ? (maxSampleWidth / minSampleWidth) : 0.0;
    distortionAbnormalCount = (!bOnlyCheckSide && sampleCount > 1 && config.fDistortionWidthRatioThreshold > 0.0 &&
        distortionWidthRatio > config.fDistortionWidthRatioThreshold) ? 1 : 0;

    const bool missingSampleWidth = success && !bOnlyCheckSide && bandFeatureCount > 0 && sampleCount == 0;
    if (missingSampleWidth) {
        abnormalSampleHitCount = std::max(abnormalSampleHitCount, 1);
    }
    const int complexBoundaryAbnormalCount = invalidBandGeometryCount + smallFragmentCount + overlapFeaturePairCount + abnormalSampleHitCount + widthJumpCount + sharpCornerAbnormalCount + distortionAbnormalCount;
    const bool hasSideError = success && !sidePassed;
    const bool hasWidthError = success && !bOnlyCheckSide && !widthPassed && hasSampleWidth;
    const bool hasUnevenError = success && !bOnlyCheckSide && (!uniformityPassed || widthJumpCount > 0 || distortionAbnormalCount > 0) && sampleCount > 0;
    const bool hasOverlapError = success && !bOnlyCheckSide && overlapFeaturePairCount > 0;
    const bool hasGeomError = success && !bOnlyCheckSide && complexBoundaryAbnormalCount > 0;
    const bool hasColorError = success && !bOnlyCheckSide && !colorPassed;
    const bool hasGradientError = success && !bOnlyCheckSide && !gradientColorPassed;
    const bool hasGradientDirectionError = success && !bOnlyCheckSide && !gradientDirectionPassed;
    const bool hasGradientFunctionError = success && !bOnlyCheckSide && !gradientFunctionPassed;

    bPassed = success && bandFeatureCount > 0 && sidePassed &&
              (bOnlyCheckSide ||
               (invalidBandGeometryCount == 0 && abnormalSampleHitCount == 0 && smallFragmentCount == 0 &&
                overlapFeaturePairCount == 0 && widthJumpCount == 0 && sharpCornerAbnormalCount == 0 && distortionAbnormalCount == 0 && widthPassed && uniformityPassed &&
                 colorPassed && gradientColorPassed && gradientDirectionPassed && gradientFunctionPassed));

    bool errorLayerWritten = false;
    if (success && bandUnion != nullptr) {
        const bool shpOk = writeBoundaryBandErrorLayer(
            outputShpPath,
            "ESRI Shapefile",
            bandUnion,
            hasSideError,
            hasWidthError,
            hasUnevenError,
            hasGeomError,
            hasOverlapError,
            hasColorError,
            hasGradientError,
            hasGradientDirectionError,
            complexBoundaryAbnormalCount,
            overlapFeaturePairCount,
            overlapArea,
            minSampleWidth,
            maxSampleWidth,
            config.fWidthToleranceMm,
            config.fUniformityCvThreshold,
            QString::fromUtf8(config.szExpectedSide),
            actualSide,
            config.fStandardWidthMm,
            avgSampleWidth,
            widthCv,
            QString::fromUtf8(config.szStandardFillColor),
            dominantFillColor,
            QString::fromUtf8(config.szStandardGradientStartColor),
            dominantStartColor,
            QString::fromUtf8(config.szStandardGradientEndColor),
            dominantEndColor,
            QString::fromUtf8(config.szStandardGradientDirection),
            dominantGradientDirection,
            dominantBoundaryLevel,
            boundaryPath,
            bOnlyCheckSide);
        const bool geojsonOk = writeBoundaryBandErrorLayer(
            outputGeoJsonPath,
            "GeoJSON",
            bandUnion,
            hasSideError,
            hasWidthError,
            hasUnevenError,
            hasGeomError,
            hasOverlapError,
            hasColorError,
            hasGradientError,
            hasGradientDirectionError,
            complexBoundaryAbnormalCount,
            overlapFeaturePairCount,
            overlapArea,
            minSampleWidth,
            maxSampleWidth,
            config.fWidthToleranceMm,
            config.fUniformityCvThreshold,
            QString::fromUtf8(config.szExpectedSide),
            actualSide,
            config.fStandardWidthMm,
            avgSampleWidth,
            widthCv,
            QString::fromUtf8(config.szStandardFillColor),
            dominantFillColor,
            QString::fromUtf8(config.szStandardGradientStartColor),
            dominantStartColor,
            QString::fromUtf8(config.szStandardGradientEndColor),
            dominantEndColor,
            QString::fromUtf8(config.szStandardGradientDirection),
            dominantGradientDirection,
            dominantBoundaryLevel,
            boundaryPath,
            bOnlyCheckSide);
        errorLayerWritten = shpOk || geojsonOk;

        QFileInfo errorShpInfo(outputShpPath);
        QString singleBaseName = errorShpInfo.completeBaseName();
        if (singleBaseName.endsWith(QStringLiteral("_errors"))) {
            singleBaseName.chop(QStringLiteral("_errors").length());
        }
        const QString singleDir = errorShpInfo.absolutePath();
        QString bandPart = singleBaseName;
        const int lastUnderscore = bandPart.lastIndexOf('_');
        if (lastUnderscore >= 0) {
            bandPart = bandPart.mid(lastUnderscore + 1);
        }
        auto singlePath = [&](const QString& suffix) {
            return QDir(singleDir).filePath(singleBaseName + QStringLiteral("_") + suffix + QStringLiteral(".shp"));
        };
        auto writeSingle = [&](bool failed, OGRGeometry* geometry, const QString& suffix, const QString& checkItem, const QString& errorType, const QString& detail, const QString& expectVal, const QString& actualVal) {
            if (failed && geometry != nullptr && !geometry->IsEmpty()) {
                if (writeBoundaryBandSingleCheckLayer(singlePath(suffix), "ESRI Shapefile", geometry, checkItem, errorType, detail, expectVal, actualVal, dominantBoundaryLevel, boundaryPath, bandPart)) {
                    errorLayerWritten = true;
                }
            }
        };

        writeSingle(hasSideError, bandUnion, "side_error", "side", "BAND_SIDE_ERROR", QStringLiteral("色带挂接方向与规则要求不一致"), sideCodeToText(QString::fromUtf8(config.szExpectedSide)), actualSide);
        if (hasWidthError && writeFilteredSampleLineErrorLayer(singlePath("width_error"), tempSamplePath, QStringList() << QStringLiteral("WIDTH_ERROR"), "width", QStringLiteral("色带采样宽度超出规则允许范围"), dominantBoundaryLevel, boundaryPath, bandPart) > 0) {
            errorLayerWritten = true;
        }
        if (hasUnevenError && writeFilteredSampleLineErrorLayer(singlePath("uniformity_error"), tempSamplePath, QStringList() << QStringLiteral("WIDTH_JUMP"), "uniformity", QStringLiteral("相邻采样线宽度变化过大，存在局部宽度突变"), dominantBoundaryLevel, boundaryPath, bandPart) > 0) {
            errorLayerWritten = true;
        }
        if (hasGeomError) {
            const QString complexPath = singlePath("complex_geom_error");
            deleteBoundaryBandVectorDataset(complexPath);
            int complexDetailCount = writeFilteredSampleLineErrorLayer(
                complexPath,
                tempSamplePath,
                QStringList() << QStringLiteral("MULTI_INTERSECTION") << QStringLiteral("WIDTH_JUMP") << QStringLiteral("NO_LOCAL_OVERLAP"),
                "complex_geom",
                QStringLiteral("复杂边界或尖角处存在重叠、扭曲、破图或宽度突变等异常"),
                dominantBoundaryLevel,
                boundaryPath,
                bandPart);
            if (invalidBandGeometryCount > 0) {
                complexDetailCount += appendDerivedComplexErrorFeatures(
                    complexPath,
                    invalidBandGeometries,
                    "INVALID_GEOMETRY",
                    "complex_geom",
                    QStringLiteral("色带面要素存在自相交、破图或其他几何非法问题"),
                    dominantBoundaryLevel,
                    boundaryPath,
                    bandPart,
                    QStringLiteral("异常数=") + QString::number(invalidBandGeometryCount));
            }
            if (smallFragmentCount > 0) {
                complexDetailCount += appendDerivedComplexErrorFeatures(
                    complexPath,
                    smallFragmentGeometries,
                    "SMALL_FRAGMENT",
                    "complex_geom",
                    QStringLiteral("色带局部出现碎片化小面，存在破图或渲染残片风险"),
                    dominantBoundaryLevel,
                    boundaryPath,
                    bandPart,
                    QStringLiteral("碎片数=") + QString::number(smallFragmentCount));
            }
            if (overlapFeaturePairCount > 0 && overlapUnion != nullptr) {
                QVector<OGRGeometry*> overlapGeometries;
                overlapGeometries.append(overlapUnion);
                complexDetailCount += appendDerivedComplexErrorFeatures(
                    complexPath,
                    overlapGeometries,
                    "FEATURE_OVERLAP",
                    "complex_geom",
                    QStringLiteral("色带原始面要素之间存在局部重叠，复杂边界处易出现渲染叠压"),
                    dominantBoundaryLevel,
                    boundaryPath,
                    bandPart,
                    QStringLiteral("重叠对数=") + QString::number(overlapFeaturePairCount) + QStringLiteral(",面积=") + QString::number(overlapArea));
            }
            if (sharpCornerAbnormalCount > 0) {
                complexDetailCount += appendDerivedComplexErrorFeatures(
                    complexPath,
                    sharpCornerGeometries,
                    "SHARP_CORNER_RISK",
                    "complex_geom",
                    QStringLiteral("尖角附近色带局部出现碎片、断裂或几何非法风险"),
                    dominantBoundaryLevel,
                    boundaryPath,
                    bandPart,
                    QStringLiteral("尖角异常数=") + QString::number(sharpCornerAbnormalCount));
            }
            if (distortionAbnormalCount > 0) {
                complexDetailCount += appendExtremeWidthSampleErrorFeatures(
                    complexPath,
                    tempSamplePath,
                    "DISTORTION_RATIO_EXCEEDED",
                    "complex_geom",
                    QStringLiteral("采样宽度最大值与最小值比值超过阈值，存在整体或局部扭曲风险"),
                    dominantBoundaryLevel,
                    boundaryPath,
                    bandPart,
                    QStringLiteral("比值=") + QString::number(distortionWidthRatio) + QStringLiteral(",阈值=") + QString::number(config.fDistortionWidthRatioThreshold));
            }
            if (complexDetailCount > 0) {
                errorLayerWritten = true;
            }
        }
        writeSingle(hasOverlapError, overlapUnion, "overlap_error", "overlap", "BAND_OVERLAP_ERROR", QStringLiteral("色带原始面要素之间存在面积重叠"), "0", QStringLiteral("重叠对数=") + QString::number(overlapFeaturePairCount) + QStringLiteral(",重叠面积=") + QString::number(overlapArea));
        writeSingle(hasColorError, bandUnion, "color_error", "color", "BAND_COLOR_ERROR", QStringLiteral("色带填充颜色与规则要求不一致"), QString::fromUtf8(config.szStandardFillColor), dominantFillColor);
        writeSingle(hasGradientError, bandUnion, "gradient_error", "gradient", "BAND_GRADIENT_ERROR", QStringLiteral("色带渐变起止色与规则要求不一致"), QString::fromUtf8(config.szStandardGradientStartColor) + " -> " + QString::fromUtf8(config.szStandardGradientEndColor), dominantStartColor + " -> " + dominantEndColor);
        writeSingle(hasGradientDirectionError, bandUnion, "gradient_dir_error", "gradient_dir", "BAND_GRADIENT_DIRECTION_ERROR", QStringLiteral("色带渐变方向与规则要求不一致"), QString::fromUtf8(config.szStandardGradientDirection), dominantGradientDirection);
        writeSingle(hasGradientFunctionError, bandUnion, "gradient_func_error", "gradient_func", "BAND_GRADIENT_FUNCTION_ERROR", QStringLiteral("色带渐变函数类型与规则要求不一致"), QString::fromUtf8(config.szStandardGradientFunction), dominantGradientFunction);
    }

    writeBoundaryBandJson(
        jsonPath,
        success,
        bPassed,
        boundaryPath,
        adminPath,
        bandPath,
        xmlPath,
        config,
        dominantBoundaryLevel,
        boundaryFeatureCount,
        adminFeatureCount,
        bandFeatureCount,
        invalidBandGeometryCount,
        actualSide,
        sideInsideLength,
        sideOutsideLength,
        directionSampleCount,
        directionNoHitSampleCount,
        insideArea,
        outsideArea,
        totalBoundaryLength,
        approxAverageWidth,
        sampleCount,
        minSampleWidth,
        maxSampleWidth,
        avgSampleWidth,
        stddevSampleWidth,
        widthCv,
        abnormalSampleHitCount,
        widthJumpCount,
        smallFragmentCount,
        overlapFeaturePairCount,
        overlapArea,
        sharpCornerCount,
        sharpCornerAbnormalCount,
        distortionWidthRatio,
        distortionAbnormalCount,
        dominantFillColor,
        dominantStartColor,
        dominantEndColor,
        dominantGradientDirection,
        dominantGradientFunction,
        errorLayerWritten,
        outputShpPath,
        outputGeoJsonPath,
        tempSamplePath,
        matchedLevelRule,
        effectiveScale,
        bOnlyCheckSide);

    if (adminUnion != nullptr) {
        delete adminUnion;
    }
    if (bandUnion != nullptr) {
        delete bandUnion;
    }
    if (overlapUnion != nullptr) {
        delete overlapUnion;
    }
    for (OGRGeometry* geometry : invalidBandGeometries) {
        delete geometry;
    }
    for (OGRGeometry* geometry : smallFragmentGeometries) {
        delete geometry;
    }
    for (OGRGeometry* geometry : sharpCornerGeometries) {
        delete geometry;
    }

    if (boundaryDataset != nullptr) {
        GDALClose(boundaryDataset);
    }
    if (adminDataset) {
        GDALClose(adminDataset);
    }
    if (bandDataset) {
        GDALClose(bandDataset);
    }

    return success ? SE_ERROR_NONE : SE_ERROR_FILE_NOT_FOUND;
}
