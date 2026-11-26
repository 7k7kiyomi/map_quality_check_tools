#define _HAS_STD_BYTE 0
#include "boundary_band_geometry.h"
#include "boundary_band_color.h"
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <cpl_string.h>
#include <QDir>
#include <QFileInfo>
#include <vector>
#include <QtXml/QDomDocument>
#include <QFile>
#include <QTextStream>

static double boundaryBandArea(OGRGeometry* geometry)
{
    if (geometry == nullptr || geometry->IsEmpty()) return 0.0;
    OGRSurface* surface = dynamic_cast<OGRSurface*>(geometry);
    if (surface != nullptr) return surface->get_Area();
    OGRMultiSurface* multiSurface = dynamic_cast<OGRMultiSurface*>(geometry);
    if (multiSurface != nullptr) return multiSurface->get_Area();
    OGRGeometryCollection* collection = dynamic_cast<OGRGeometryCollection*>(geometry);
    if (collection != nullptr) {
        double area = 0.0;
        for (int i = 0; i < collection->getNumGeometries(); ++i) {
            area += boundaryBandArea(collection->getGeometryRef(i));
        }
        return area;
    }
    return 0.0;
}

static OGRGeometry* boundaryBandValidClone(OGRGeometry* geometry)
{
    if (geometry == nullptr) return nullptr;
    OGRGeometry* valid = geometry->MakeValid();
    if (valid != nullptr && !valid->IsEmpty()) return valid;
    if (valid != nullptr) delete valid;
    return geometry->clone();
}

bool PrepareBoundaryBandGeometry(const QString& sourcePath, const QString& outputPath,
    const QString& colorFieldName, const QString& partName, QJsonObject& diagnostic)
{
    diagnostic = QJsonObject();
    diagnostic["identification"] = QStringLiteral("按band_part属性或几何嵌套与原始颜色分组识别，不按标准颜色过滤");
    GDALDataset* sourceDataset = static_cast<GDALDataset*>(GDALOpenEx(sourcePath.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (sourceDataset == nullptr || sourceDataset->GetLayerCount() <= 0) {
        if (sourceDataset != nullptr) GDALClose(sourceDataset);
        return false;
    }
    OGRLayer* sourceLayer = sourceDataset->GetLayer(0);
    if (sourceLayer == nullptr) {
        GDALClose(sourceDataset);
        return false;
    }

    struct BandSourceFeature
    {
        OGRFeature* feature;
        OGRGeometry* geometry;
        double area;
        QString color;
        QString part;
    };
    std::vector<BandSourceFeature> outerFeatures;
    std::vector<BandSourceFeature> innerFeatures;
    const QByteArray colorFieldBytes = colorFieldName.toUtf8();
    const int colorFieldIndex = sourceLayer->GetLayerDefn()->GetFieldIndex(colorFieldBytes.constData());
    const int partIndex = sourceLayer->GetLayerDefn()->GetFieldIndex("band_part");
    std::vector<BandSourceFeature> sources;
    sourceLayer->ResetReading();
    OGRFeature* feature = nullptr;
    while ((feature = sourceLayer->GetNextFeature()) != nullptr) {
        OGRGeometry* geometry = boundaryBandValidClone(feature->GetGeometryRef());
        const double area = boundaryBandArea(geometry);
        if (geometry && !geometry->IsEmpty() && area > 0) {
            const QString color = colorFieldIndex >= 0 ? QString::fromUtf8(feature->GetFieldAsString(colorFieldIndex)).trimmed() : QString();
            const QString part = partIndex >= 0 ? QString::fromUtf8(feature->GetFieldAsString(partIndex)).trimmed().toLower() : QString();
            sources.push_back({feature, geometry, area, color, part});
        } else {
            delete geometry;
            OGRFeature::DestroyFeature(feature);
        }
    }
    auto sameColor = [](const QString& a, const QString& b) {
        return a.compare(b, Qt::CaseInsensitive) == 0 || BoundaryBandColor::matches(a, b, 0, 0);
    };
    bool singleColor = !sources.empty();
    for (const auto& item : sources)
        if (!sameColor(item.color, sources.front().color)) singleColor = false;
    int innerCount = 0, outerCount = 0, explicitCount = 0;
    for (size_t i = 0; i < sources.size(); ++i) {
        auto& item = sources[i];
        if (item.part != "inner" && item.part != "outer") {
            size_t enclosing = i;
            OGREnvelope box; item.geometry->getEnvelope(&box);
            for (size_t j = 0; j < sources.size(); ++j) {
                const auto& candidate = sources[j];
                if (candidate.area <= sources[enclosing].area * (1 + 1e-9)) continue;
                OGREnvelope parentBox; candidate.geometry->getEnvelope(&parentBox);
                if (parentBox.MinX > box.MinX || parentBox.MinY > box.MinY || parentBox.MaxX < box.MaxX || parentBox.MaxY < box.MaxY) continue;
                OGRGeometry* overlap = candidate.geometry->Intersection(item.geometry);
                const double ratio = boundaryBandArea(overlap) / item.area;
                delete overlap;
                if (ratio >= 0.98) enclosing = j;
            }
            // A same-colour nested stack has an outer source and inner cutters.
            item.part = singleColor ? (enclosing == i ? "outer" : "inner")
                : (sameColor(item.color, sources[enclosing].color) ? "outer" : "inner");
        } else ++explicitCount;
        if (item.part == "inner") ++innerCount; else ++outerCount;
        if (item.part == partName) outerFeatures.push_back(item);
        else innerFeatures.push_back(item);
    }
    diagnostic["source_count"] = static_cast<int>(sources.size());
    diagnostic["identified_inner_count"] = innerCount;
    diagnostic["identified_outer_count"] = outerCount;
    diagnostic["explicit_part_count"] = explicitCount;

    if (outerFeatures.empty()) {
        for (BandSourceFeature& item : outerFeatures) { delete item.geometry; OGRFeature::DestroyFeature(item.feature); }
        for (BandSourceFeature& item : innerFeatures) { delete item.geometry; OGRFeature::DestroyFeature(item.feature); }
        GDALClose(sourceDataset);
        return false;
    }

    QFileInfo outInfo(outputPath);
    QDir().mkpath(outInfo.absolutePath());
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (driver == nullptr) {
        for (BandSourceFeature& item : outerFeatures) { delete item.geometry; OGRFeature::DestroyFeature(item.feature); }
        for (BandSourceFeature& item : innerFeatures) { delete item.geometry; OGRFeature::DestroyFeature(item.feature); }
        GDALClose(sourceDataset);
        return false;
    }
    if (QFileInfo::exists(outputPath)) {
        driver->Delete(outputPath.toUtf8().constData());
    }

    char** layerOptions = nullptr;
    layerOptions = CSLSetNameValue(layerOptions, "ENCODING", "UTF-8");
    GDALDataset* outputDataset = driver->Create(outputPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr);
    if (outputDataset == nullptr) {
        CSLDestroy(layerOptions);
        for (BandSourceFeature& item : outerFeatures) { delete item.geometry; OGRFeature::DestroyFeature(item.feature); }
        for (BandSourceFeature& item : innerFeatures) { delete item.geometry; OGRFeature::DestroyFeature(item.feature); }
        GDALClose(sourceDataset);
        return false;
    }
    OGRLayer* outputLayer = outputDataset->CreateLayer(outInfo.completeBaseName().toUtf8().constData(), sourceLayer->GetSpatialRef(), wkbPolygon, layerOptions);
    CSLDestroy(layerOptions);
    if (outputLayer == nullptr) {
        GDALClose(outputDataset);
        for (BandSourceFeature& item : outerFeatures) { delete item.geometry; OGRFeature::DestroyFeature(item.feature); }
        for (BandSourceFeature& item : innerFeatures) { delete item.geometry; OGRFeature::DestroyFeature(item.feature); }
        GDALClose(sourceDataset);
        return false;
    }

    OGRFeatureDefn* sourceDefn = sourceLayer->GetLayerDefn();
    for (int i = 0; i < sourceDefn->GetFieldCount(); ++i) {
        OGRFieldDefn fieldDefn(sourceDefn->GetFieldDefn(i));
        outputLayer->CreateField(&fieldDefn);
    }
    OGRFieldDefn partField("band_part", OFTString);
    partField.SetWidth(16);
    if (outputLayer->GetLayerDefn()->GetFieldIndex("band_part") < 0) outputLayer->CreateField(&partField);
    OGRFieldDefn srcAreaField("src_area", OFTReal);
    srcAreaField.SetWidth(32);
    srcAreaField.SetPrecision(3);
    if (outputLayer->GetLayerDefn()->GetFieldIndex("src_area") < 0) outputLayer->CreateField(&srcAreaField);
    OGRFieldDefn resAreaField("res_area", OFTReal);
    resAreaField.SetWidth(32);
    resAreaField.SetPrecision(3);
    if (outputLayer->GetLayerDefn()->GetFieldIndex("res_area") < 0) outputLayer->CreateField(&resAreaField);

    double maxTargetArea = 0.0;
    for (const BandSourceFeature& item : outerFeatures) {
        if (item.area > maxTargetArea) maxTargetArea = item.area;
    }

    int written = 0;
    const bool isInnerPart = partName == QStringLiteral("inner");
    for (BandSourceFeature& outer : outerFeatures) {
        const double smallTargetLimit = maxTargetArea * 0.01;
        const bool isSmallTargetFragment = maxTargetArea > 0.0 && outer.area < smallTargetLimit;
        bool smallTargetUsedByLargerBand = false;

        if (!isInnerPart && isSmallTargetFragment) {
            for (BandSourceFeature& largerTarget : outerFeatures) {
                if (&largerTarget == &outer || largerTarget.area <= outer.area) continue;
                OGRGeometry* overlap = largerTarget.geometry->Intersection(outer.geometry);
                const double overlapArea = boundaryBandArea(overlap);
                if (overlap != nullptr) delete overlap;
                if (overlapArea > 0.0 && overlapArea >= outer.area * 0.20) {
                    smallTargetUsedByLargerBand = true;
                    break;
                }
            }
            if (partName != QStringLiteral("inner")) {
                for (BandSourceFeature& largerCutter : innerFeatures) {
                    if (largerCutter.area <= outer.area) continue;
                    OGRGeometry* overlap = largerCutter.geometry->Intersection(outer.geometry);
                    const double overlapArea = boundaryBandArea(overlap);
                    if (overlap != nullptr) delete overlap;
                    if (overlapArea > 0.0 && overlapArea >= outer.area * 0.20) {
                        smallTargetUsedByLargerBand = true;
                        break;
                    }
                }
            }
            if (smallTargetUsedByLargerBand) {
                continue;
            }
        }

        OGRGeometry* subtractGeometry = nullptr;
        auto appendSubtractGeometry = [&](OGRGeometry* cutterGeometry) {
            if (cutterGeometry == nullptr || cutterGeometry->IsEmpty()) return;
            if (subtractGeometry == nullptr) {
                subtractGeometry = cutterGeometry->clone();
            } else {
                OGRGeometry* merged = subtractGeometry->Union(cutterGeometry);
                if (merged != nullptr) {
                    delete subtractGeometry;
                    subtractGeometry = merged;
                }
            }
        };

        auto shouldUseAsCutter = [&](BandSourceFeature& cutter, bool sameColor)->bool {
            if (cutter.geometry == nullptr || cutter.geometry->IsEmpty()) return false;
            const double smallCutterLimit = isInnerPart && sameColor ? outer.area * 1.20 : (sameColor ? smallTargetLimit : outer.area * 1.20);
            if (cutter.area > smallCutterLimit) return false;
            OGRGeometry* overlap = outer.geometry->Intersection(cutter.geometry);
            const double overlapArea = boundaryBandArea(overlap);
            if (overlap != nullptr) delete overlap;
            if (overlapArea <= 0.0) return false;
            const double cutterOverlapRatio = cutter.area > 0.0 ? overlapArea / cutter.area : 0.0;
            const double outerOverlapRatio = outer.area > 0.0 ? overlapArea / outer.area : 0.0;
            if (cutter.area < outer.area) {
                if (cutterOverlapRatio >= 0.20) return true;
                if (outerOverlapRatio >= 0.000001) return true;
            }
            return cutterOverlapRatio >= 0.80 && cutter.area <= outer.area * 1.20;
        };

        if (!isInnerPart) {
            for (BandSourceFeature& inner : innerFeatures) {
                if (shouldUseAsCutter(inner, false)) {
                    appendSubtractGeometry(inner.geometry);
                }
            }
        }

        bool hasSameColorCutter = false;
        for (BandSourceFeature& sameColorFragment : outerFeatures) {
            if (&sameColorFragment == &outer) continue;
            const bool possibleFragment = isInnerPart
                ? sameColorFragment.area < outer.area && sameColorFragment.area >= outer.area * 0.80
                : (sameColorFragment.area < outer.area * 0.20 || sameColorFragment.area < smallTargetLimit);
            if (!possibleFragment) continue;
            if (shouldUseAsCutter(sameColorFragment, true)) {
                appendSubtractGeometry(sameColorFragment.geometry);
                hasSameColorCutter = true;
            }
        }

        if (isInnerPart && !hasSameColorCutter) {
            if (subtractGeometry != nullptr) delete subtractGeometry;
            continue;
        }

        OGRGeometry* validDiff = nullptr;
        if (subtractGeometry == nullptr) {
            validDiff = boundaryBandValidClone(outer.geometry);
        } else {
            OGRGeometry* diffGeometry = outer.geometry->Difference(subtractGeometry);
            delete subtractGeometry;
            validDiff = boundaryBandValidClone(diffGeometry);
            if (diffGeometry != nullptr) delete diffGeometry;
        }
        const double resultArea = boundaryBandArea(validDiff);
        const double minFragmentArea = maxTargetArea * 0.00001;
        if (validDiff == nullptr || validDiff->IsEmpty() || resultArea <= minFragmentArea) {
            if (validDiff != nullptr) delete validDiff;
            continue;
        }
        if (partName != QStringLiteral("inner") && isSmallTargetFragment && resultArea <= smallTargetLimit) {
            delete validDiff;
            continue;
        }

        OGRFeature* outputFeature = OGRFeature::CreateFeature(outputLayer->GetLayerDefn());
        outputFeature->SetFrom(outer.feature, TRUE);
        outputFeature->SetField("band_part", partName.toUtf8().constData());
        outputFeature->SetField("src_area", outer.area);
        outputFeature->SetField("res_area", resultArea);
        outputFeature->SetGeometry(validDiff);
        if (outputLayer->CreateFeature(outputFeature) == OGRERR_NONE) ++written;
        OGRFeature::DestroyFeature(outputFeature);
        delete validDiff;
    }

    GDALClose(outputDataset);
    for (BandSourceFeature& item : outerFeatures) { delete item.geometry; OGRFeature::DestroyFeature(item.feature); }
    for (BandSourceFeature& item : innerFeatures) { delete item.geometry; OGRFeature::DestroyFeature(item.feature); }
    GDALClose(sourceDataset);
    diagnostic["written_count"] = written;
    return written > 0;
}


QString CreateBoundaryBandPartRuleXml(const QString& sourceXmlPath, const QString& outputXmlPath, const QString& colorFieldName, const QString& standardColor, double standardWidthMm, double widthToleranceMm)
{
    // 内外圈检查需要不同的标准颜色和宽度，因此运行时临时派生一份规则 XML。
    // 调用处会把它放到系统临时目录，避免在“质检json文件”目录产生 outer_rule/inner_rule 中间文件。
    QFile file(sourceXmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return sourceXmlPath;
    QDomDocument doc;
    if (!doc.setContent(&file)) {
        file.close();
        return sourceXmlPath;
    }
    file.close();

    auto isBoundaryBandItem = [](const QString& checkName) {
        const QString name = checkName.trimmed();
        return name.isEmpty() || name == QStringLiteral("境界线色带渲染合规性检查") ||
               name == QStringLiteral("境界线色带检查") || name == QStringLiteral("色带渲染合规性检查");
    };
    auto setRule = [&](QDomElement& checkItem, const QString& name, const QString& value, const QString& unit = QString()) {
        QDomNodeList ruleNodes = checkItem.elementsByTagName("rule");
        for (int i = 0; i < ruleNodes.count(); ++i) {
            QDomElement rule = ruleNodes.at(i).toElement();
            if (rule.firstChildElement("name").text().trimmed() == name) {
                QDomElement valueElement = rule.firstChildElement("value");
                if (valueElement.isNull()) {
                    valueElement = doc.createElement("value");
                    rule.appendChild(valueElement);
                }
                while (!valueElement.firstChild().isNull()) valueElement.removeChild(valueElement.firstChild());
                valueElement.appendChild(doc.createTextNode(value));
                if (!unit.isNull()) {
                    QDomElement unitElement = rule.firstChildElement("unit");
                    if (unitElement.isNull()) {
                        unitElement = doc.createElement("unit");
                        rule.appendChild(unitElement);
                    }
                    while (!unitElement.firstChild().isNull()) unitElement.removeChild(unitElement.firstChild());
                    unitElement.appendChild(doc.createTextNode(unit));
                }
                return;
            }
        }
        QDomElement rule = doc.createElement("rule");
        QDomElement nameElement = doc.createElement("name");
        nameElement.appendChild(doc.createTextNode(name));
        QDomElement valueElement = doc.createElement("value");
        valueElement.appendChild(doc.createTextNode(value));
        QDomElement unitElement = doc.createElement("unit");
        unitElement.appendChild(doc.createTextNode(unit));
        rule.appendChild(nameElement);
        rule.appendChild(valueElement);
        rule.appendChild(unitElement);
        checkItem.appendChild(rule);
    };

    QDomNodeList checkItems = doc.elementsByTagName("check_item");
    for (int i = 0; i < checkItems.count(); ++i) {
        QDomElement checkItem = checkItems.at(i).toElement();
        if (!isBoundaryBandItem(checkItem.firstChildElement("name").text())) continue;
        setRule(checkItem, QStringLiteral("颜色字段"), colorFieldName);
        setRule(checkItem, QStringLiteral("检查颜色"), QStringLiteral("true"));
        setRule(checkItem, QStringLiteral("标准填充颜色"), standardColor);
        setRule(checkItem, QStringLiteral("标准宽度mm"), QString::number(standardWidthMm), QStringLiteral("mm"));
        setRule(checkItem, QStringLiteral("宽度容差mm"), QString::number(widthToleranceMm), QStringLiteral("mm"));
        // Preserve the configured direction, including boundary-level overrides.
    }

    QFileInfo outInfo(outputXmlPath);
    QDir().mkpath(outInfo.absolutePath());
    QFile outFile(outputXmlPath);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) return sourceXmlPath;
    QTextStream stream(&outFile);
    stream.setCodec("UTF-8");
    doc.save(stream, 4);
    outFile.close();
    return outputXmlPath;
}

