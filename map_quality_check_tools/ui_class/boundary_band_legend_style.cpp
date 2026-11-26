#define _HAS_STD_BYTE 0
#include "boundary_band_legend_style.h"
#include "boundary_band_color.h"
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <cpl_string.h>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <memory>
#include <vector>
#include <map>

namespace {
struct Issue {
    QJsonObject detail;
    struct FeatureDeleter { void operator()(OGRFeature* feature) const { OGRFeature::DestroyFeature(feature); } };
    std::unique_ptr<OGRFeature, FeatureDeleter> source;
};

QString opacityText(double opacity)
{
    return QString::number(opacity * 100, 'g', 8) + '%';
}

QString errorTypeText(const QString& type)
{
    if (type == "BAND_COLOR_ERROR") return QStringLiteral("色带填充颜色错误");
    if (type == "BAND_COLOR_UNVERIFIABLE") return QStringLiteral("色带颜色无法验证");
    if (type == "BAND_OPACITY_ERROR") return QStringLiteral("色带不透明度错误");
    if (type == "BAND_OPACITY_UNVERIFIABLE") return QStringLiteral("色带不透明度无法验证");
    if (type == "BAND_GRADIENT_ERROR") return QStringLiteral("色带渐变颜色错误");
    if (type == "BAND_GRADIENT_DIRECTION_ERROR") return QStringLiteral("色带渐变方向错误");
    if (type == "BAND_GRADIENT_FUNCTION_ERROR") return QStringLiteral("色带渐变函数类型错误");
    return type;
}

QString checkItemForType(const QString& type)
{
    if (type.startsWith("BAND_OPACITY")) return "opacity";
    if (type == "BAND_GRADIENT_DIRECTION_ERROR") return "gradient_dir";
    if (type == "BAND_GRADIENT_FUNCTION_ERROR") return "gradient_func";
    if (type == "BAND_GRADIENT_ERROR") return "gradient";
    return "color";
}

bool writeIssues(const QString& path, const char* driverName, OGRLayer* source,
                 const std::vector<Issue>& issues)
{
    if (path.isEmpty()) return true;
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName(driverName);
    if (!driver) return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QByteArray bytes = path.toUtf8();
    if (QFileInfo::exists(path) && driver->Delete(bytes.constData()) != CE_None) return false;
    GDALDataset* dataset = driver->Create(bytes.constData(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!dataset) return false;
    char** options = nullptr;
    if (QString::fromLatin1(driverName) == "ESRI Shapefile")
        options = CSLSetNameValue(options, "ENCODING", "UTF-8");
    OGRLayer* layer = dataset->CreateLayer(QFileInfo(path).completeBaseName().toUtf8().constData(), source->GetSpatialRef(), wkbPolygon, options);
    CSLDestroy(options);
    bool ok = layer != nullptr;
    if (layer) {
        // Match writeBoundaryBandSingleCheckLayer: one feature for each issue.
        struct Field { const char* name; int width; };
        const Field fields[] = {
            {"check_item", 48}, {"error_type", 48}, {"boundary_lv", 32},
            {"boundary_ly", 96}, {"band_part", 16}, {"detail", 160},
            {"expect_val", 64}, {"actual_val", 64}
        };
        for (const Field& field : fields) {
            OGRFieldDefn definition(field.name, OFTString);
            definition.SetWidth(field.width);
            ok = layer->CreateField(&definition) == OGRERR_NONE && ok;
        }
        if (ok) for (const Issue& issue : issues) {
            if (!issue.source) continue;
            OGRGeometry* geometry = issue.source->GetGeometryRef();
            if (!geometry || geometry->IsEmpty()) continue;
            const QString type = issue.detail["type"].toString();
            const int partIndex = issue.source->GetFieldIndex("band_part");
            OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
            // Index assignment also preserves original field names after SHP laundering.
            feature->SetField(0, checkItemForType(type).toUtf8().constData());
            feature->SetField(1, errorTypeText(type).toUtf8().constData());
            feature->SetField(2, "");
            feature->SetField(3, "");
            feature->SetField(4, partIndex >= 0 ? issue.source->GetFieldAsString(partIndex) : "");
            feature->SetField(5, issue.detail["description"].toString().toUtf8().constData());
            feature->SetField(6, issue.detail["expected_value"].toString().toUtf8().constData());
            feature->SetField(7, issue.detail["actual_value"].toString().toUtf8().constData());
            ok = feature->SetGeometry(geometry) == OGRERR_NONE && ok;
            ok = layer->CreateFeature(feature) == OGRERR_NONE && ok;
            OGRFeature::DestroyFeature(feature);
        }
    }
    GDALClose(dataset);
    return ok;
}
}

SE_Error InspectBoundaryBandLegendStyles(
    OGRLayer* layer, const SE_BoundaryBandRenderRuleConfig& config,
    const QString& outputShp, const QString& outputGeoJson, QJsonObject& result)
{
    result = QJsonObject();
    std::vector<Issue> issues;
    QJsonObject errors;
    auto addIssue = [&](OGRFeature* feature, const QString& type, const QString& description,
                        const QString& expected, const QString& actual, const QString& source) {
        Issue issue;
        issue.detail["type"] = type;
        issue.detail["description"] = description;
        issue.detail["expected_value"] = expected;
        issue.detail["actual_value"] = actual;
        issue.detail["value_source"] = source;
        if (feature) {
            issue.detail["feature_id"] = QString::number(feature->GetFID());
            issue.source.reset(feature->Clone());
            OGRGeometry* geometry = feature->GetGeometryRef();
            OGRPoint point;
            if (geometry && !geometry->IsEmpty() && geometry->Centroid(&point) == OGRERR_NONE && !point.IsEmpty()) {
                issue.detail["position_x"] = point.getX();
                issue.detail["position_y"] = point.getY();
            }
        }
        errors[QString("error%1").arg(issues.size() + 1)] = issue.detail;
        issues.push_back(std::move(issue));
    };
    QStringList palette;
    const QString singleColor = QString::fromUtf8(config.szStandardFillColor).trimmed();
    const QString innerColor = QString::fromUtf8(config.szStandardInnerColor).trimmed();
    const QString outerColor = QString::fromUtf8(config.szStandardOuterColor).trimmed();
    if (!singleColor.isEmpty()) palette << singleColor;
    else {
        if (!innerColor.isEmpty()) palette << innerColor;
        if (!outerColor.isEmpty()) palette << outerColor;
    }
    bool configValid = true;
    auto invalidRule = [&](const QString& description) {
        configValid = false;
        addIssue(nullptr, "BAND_STYLE_RULE_ERROR", description, "有效的图例规则", "", "XML");
    };
    if (config.bCheckColor) {
        if (palette.isEmpty()) invalidRule(QStringLiteral("已启用颜色检查，但未配置标准颜色或内外层颜色"));
        for (const QString& expected : palette) {
            BoundaryBandColor::Value parsed;
            if (!BoundaryBandColor::parse(expected, parsed)) invalidRule(QStringLiteral("图例标准颜色无法解析：") + expected);
        }
        if (!std::isfinite(config.fCmykColorTolerance) || config.fCmykColorTolerance < 0)
            invalidRule(QStringLiteral("CMYK颜色容差必须为非负数"));
    }
    if (config.bCheckGradient) {
        for (const QString& expected : {QString::fromUtf8(config.szStandardGradientStartColor), QString::fromUtf8(config.szStandardGradientEndColor)}) {
            BoundaryBandColor::Value parsed;
            if (!expected.trimmed().isEmpty() && !BoundaryBandColor::parse(expected, parsed))
                invalidRule(QStringLiteral("图例标准渐变颜色无法解析：") + expected);
        }
        if (!std::isfinite(config.fCmykColorTolerance) || config.fCmykColorTolerance < 0)
            invalidRule(QStringLiteral("渐变颜色容差必须为非负数"));
    }
    if (config.bCheckOpacity && (!std::isfinite(config.fStandardOpacity) || config.fStandardOpacity < 0 || config.fStandardOpacity > 1 ||
        !std::isfinite(config.fOpacityTolerance) || config.fOpacityTolerance < 0 || config.fOpacityTolerance > 1))
        invalidRule(QStringLiteral("不透明度规则必须在0%至100%之间"));
    const QStringList opacityUnits = {"auto", "percent", "%", "0-100", "fraction", "0-1", "alpha", "byte"};
    if (config.bCheckOpacity && !opacityUnits.contains(QString::fromUtf8(config.szOpacityUnit).trimmed().toLower()))
        invalidRule(QStringLiteral("不透明度字段单位应为auto、percent、fraction或alpha"));
    if (!layer) addIssue(nullptr, "BAND_INPUT_ERROR", QStringLiteral("无法读取晕带图层"), "晕带面图层", "", "input");

    int total = 0, colorErrors = 0, opacityErrors = 0, opacityUnknown = 0;
    int colorChecked = 0, opacityChecked = 0, opacitySkipped = 0;
    int gradientErrors = 0, gradientDirectionErrors = 0, gradientFunctionErrors = 0;
    bool hasOpacityMetadata = false;
    QStringList opacitySources;
    double minOpacity = 1, maxOpacity = 0;
    if (layer && configValid) {
        OGRFeatureDefn* definition = layer->GetLayerDefn();
        const int colorIndex = definition->GetFieldIndex(config.szColorField);
        const int partIndex = definition->GetFieldIndex("band_part");
        const int startIndex = definition->GetFieldIndex(config.szGradientStartColorField);
        const int endIndex = definition->GetFieldIndex(config.szGradientEndColorField);
        const int directionIndex = definition->GetFieldIndex(config.szGradientDirectionField);
        const int functionIndex = definition->GetFieldIndex(config.szGradientFunctionField);
        int opacityIndex = definition->GetFieldIndex(config.szOpacityField);
        QString opacityField = QString::fromUtf8(config.szOpacityField);
        QString opacityUnit = QString::fromUtf8(config.szOpacityUnit);
        if (opacityUnit == "auto" && opacityField.toLower().contains("alpha")) opacityUnit = "alpha";
        // Default auto lookup accommodates the ten-character Shapefile field limit.
        if (opacityIndex < 0 && (opacityField.isEmpty() || opacityField == "opacity")) {
            const QStringList aliases = {"fill_opac", "fill_opacity", "fill_alpha", "alpha"};
            for (const QString& alias : aliases) {
                opacityIndex = definition->GetFieldIndex(alias.toUtf8().constData());
                if (opacityIndex >= 0) {
                    opacityField = alias;
                    if (alias.contains("alpha")) opacityUnit = "alpha";
                    break;
                }
            }
        }
        // Whole layers without opacity metadata are legacy data. A malformed
        // explicit value or a partially populated layer still needs validation.
        layer->ResetReading();
        OGRFeature* opacityProbe = nullptr;
        while ((opacityProbe = layer->GetNextFeature()) != nullptr) {
            if (opacityIndex >= 0 && opacityProbe->IsFieldSetAndNotNull(opacityIndex) &&
                !QString::fromUtf8(opacityProbe->GetFieldAsString(opacityIndex)).trimmed().isEmpty()) hasOpacityMetadata = true;
            BoundaryBandColor::Value probeColor;
            if (colorIndex >= 0 && BoundaryBandColor::parse(QString::fromUtf8(opacityProbe->GetFieldAsString(colorIndex)), probeColor) && probeColor.hasOpacity)
                hasOpacityMetadata = true;
            OGRFeature::DestroyFeature(opacityProbe);
        }
        layer->ResetReading();
        OGRFeature* feature = nullptr;
        while ((feature = layer->GetNextFeature()) != nullptr) {
            ++total;
            const QString actualColor = colorIndex >= 0 && feature->IsFieldSetAndNotNull(colorIndex)
                ? QString::fromUtf8(feature->GetFieldAsString(colorIndex)).trimmed() : QString();
            BoundaryBandColor::Value color;
            const bool colorValid = BoundaryBandColor::parse(actualColor, color);
            if (config.bCheckColor) {
                ++colorChecked;
                QStringList allowed = palette;
                // When a layer identity is available, do not accept the other layer's color.
                if (singleColor.isEmpty() && partIndex >= 0) {
                    const QString part = QString::fromUtf8(feature->GetFieldAsString(partIndex)).trimmed().toLower();
                    if (part == "inner" && !innerColor.isEmpty()) allowed = QStringList() << innerColor;
                    if (part == "outer" && !outerColor.isEmpty()) allowed = QStringList() << outerColor;
                }
                bool matched = false;
                for (const QString& expected : allowed)
                    matched = BoundaryBandColor::matches(expected, actualColor, 0.0, config.fCmykColorTolerance) || matched;
                if (!matched) {
                    ++colorErrors;
                    addIssue(feature, colorValid ? "BAND_COLOR_ERROR" : "BAND_COLOR_UNVERIFIABLE",
                        colorValid ? QStringLiteral("晕带颜色不符合图例规定") : QStringLiteral("晕带颜色缺失或无法解析，不能验证图例颜色"),
                        allowed.join(" / "), actualColor, QString::fromUtf8(config.szColorField));
                }
            }
            if (config.bCheckOpacity && !hasOpacityMetadata && !config.bRequireOpacity) {
                ++opacityUnknown;
                ++opacitySkipped;
            } else if (config.bCheckOpacity) {
                double opacity = 0;
                QString source, actual;
                bool known = false;
                if (opacityIndex >= 0 && feature->IsFieldSetAndNotNull(opacityIndex)) {
                    source = opacityField;
                    actual = QString::fromUtf8(feature->GetFieldAsString(opacityIndex)).trimmed();
                    known = BoundaryBandColor::parseOpacity(actual, opacityUnit, opacity);
                } else if (colorValid && color.hasOpacity) {
                    opacity = color.opacity;
                    source = QString::fromUtf8(config.szColorField) + ":alpha";
                    actual = opacityText(opacity);
                    known = true;
                }
                if (!known) {
                    ++opacityUnknown;
                    addIssue(feature, "BAND_OPACITY_UNVERIFIABLE", QStringLiteral("不透明度缺失或无法解析，不能确认是否符合图例；需要有效不透明度字段或RGBA颜色"),
                        opacityText(config.fStandardOpacity), actual, source);
                } else {
                    ++opacityChecked;
                    minOpacity = std::min(minOpacity, opacity);
                    maxOpacity = std::max(maxOpacity, opacity);
                    if (!opacitySources.contains(source)) opacitySources << source;
                    if (std::fabs(opacity - config.fStandardOpacity) > config.fOpacityTolerance + 1e-12) {
                        ++opacityErrors;
                        addIssue(feature, "BAND_OPACITY_ERROR", QStringLiteral("晕带不透明度不符合图例规定"),
                            opacityText(config.fStandardOpacity), opacityText(opacity), source);
                    }
                }
            }
            if (config.bCheckGradient) {
                auto fieldValue = [&](int index) {
                    return index >= 0 && feature->IsFieldSetAndNotNull(index)
                        ? QString::fromUtf8(feature->GetFieldAsString(index)).trimmed() : QString();
                };
                const QString start = fieldValue(startIndex), end = fieldValue(endIndex);
                const QString expectedStart = QString::fromUtf8(config.szStandardGradientStartColor).trimmed();
                const QString expectedEnd = QString::fromUtf8(config.szStandardGradientEndColor).trimmed();
                auto matches = [&](const QString& expected, const QString& actual) {
                    return expected.isEmpty() || BoundaryBandColor::matches(expected, actual, 0.0, config.fCmykColorTolerance);
                };
                if (!matches(expectedStart, start) || !matches(expectedEnd, end)) {
                    ++gradientErrors;
                    addIssue(feature, "BAND_GRADIENT_ERROR", QStringLiteral("色带渐变起止色与规则要求不一致"),
                        expectedStart + " -> " + expectedEnd, start + " -> " + end,
                        QString::fromUtf8(config.szGradientStartColorField) + " / " + QString::fromUtf8(config.szGradientEndColorField));
                }
                const QString direction = fieldValue(directionIndex);
                const QString expectedDirection = QString::fromUtf8(config.szStandardGradientDirection).trimmed();
                if (!expectedDirection.isEmpty() && direction != expectedDirection) {
                    ++gradientDirectionErrors;
                    addIssue(feature, "BAND_GRADIENT_DIRECTION_ERROR", QStringLiteral("色带渐变方向与规则要求不一致"),
                        expectedDirection, direction, QString::fromUtf8(config.szGradientDirectionField));
                }
                const QString function = fieldValue(functionIndex);
                const QString expectedFunction = QString::fromUtf8(config.szStandardGradientFunction).trimmed();
                if (!expectedFunction.isEmpty() && function != expectedFunction) {
                    ++gradientFunctionErrors;
                    addIssue(feature, "BAND_GRADIENT_FUNCTION_ERROR", QStringLiteral("色带渐变函数类型与规则要求不一致"),
                        expectedFunction, function, QString::fromUtf8(config.szGradientFunctionField));
                }
            }
            OGRFeature::DestroyFeature(feature);
        }
        layer->ResetReading();
        if (total == 0) addIssue(nullptr, "BAND_MISSING", QStringLiteral("未读取到晕带面要素"), "至少一个晕带面要素", "0", "input");
    }
    bool written = true;
    if (layer) {
        written = writeIssues(outputShp, "ESRI Shapefile", layer, issues) && written;
        written = writeIssues(outputGeoJson, "GeoJSON", layer, issues) && written;
    }
    if (!written) addIssue(nullptr, "BAND_STYLE_OUTPUT_ERROR", QStringLiteral("图例样式错误图层写出失败"), "可写输出路径", outputShp + " / " + outputGeoJson, "output");
    const bool success = layer && configValid && written;
    QJsonObject info;
    info["feature_count"] = total;
    info["color_checked_count"] = colorChecked;
    info["color_error_count"] = colorErrors;
    info["gradient_error_count"] = gradientErrors;
    info["gradient_direction_error_count"] = gradientDirectionErrors;
    info["gradient_function_error_count"] = gradientFunctionErrors;
    info["check_gradient"] = config.bCheckGradient;
    info["opacity_checked_count"] = opacityChecked;
    info["opacity_error_count"] = opacityErrors;
    info["opacity_unverifiable_count"] = opacityUnknown;
    info["opacity_skipped_count"] = opacitySkipped;
    info["require_opacity"] = config.bRequireOpacity;
    info["opacity_check_status"] = !config.bCheckOpacity ? "disabled" : opacitySkipped > 0 ? "legacy_no_metadata" : "done";
    if (opacitySkipped > 0) info["opacity_note"] = QStringLiteral("老色带未保存不透明度信息，本次未核验不透明度，继续检查其他项目");
    std::map<GIntBig, bool> errorFeatures;
    for (const Issue& issue : issues) if (issue.source) errorFeatures[issue.source->GetFID()] = true;
    info["error_feature_count"] = static_cast<int>(errorFeatures.size());
    info["check_color"] = config.bCheckColor;
    info["check_opacity"] = config.bCheckOpacity;
    info["standard_colors"] = QJsonArray::fromStringList(palette);
    info["standard_opacity"] = config.fStandardOpacity;
    info["opacity_tolerance"] = config.fOpacityTolerance;
    info["opacity_sources"] = QJsonArray::fromStringList(opacitySources);
    if (opacityChecked > 0) { info["min_opacity"] = minOpacity; info["max_opacity"] = maxOpacity; }
    info["color_comparison"] = QStringLiteral("CMYK对CMYK按各通道百分比比较；涉及RGB时使用简化CMYK转RGB和RGB距离，不等同ICC色彩管理");
    info["result_shp"] = outputShp;
    info["result_geojson"] = outputGeoJson;
    info["error_result_shp"] = outputShp;
    info["error_result_geojson"] = outputGeoJson;
    info["description"] = issues.empty()
        ? (opacitySkipped > 0 ? QStringLiteral("适用的图例样式检查通过；老色带无不透明度信息，本次未核验不透明度")
                              : QStringLiteral("已启用的图例样式检查通过"))
        : QStringLiteral("发现%1个图例样式问题").arg(issues.size());
    result["id"] = "214";
    result["name"] = QStringLiteral("境界线色带图例样式检查");
    result["status"] = success ? QStringLiteral("质检成功") : QStringLiteral("质检失败");
    result["passed"] = success && issues.empty();
    result["err_exist"] = issues.empty() ? QStringLiteral("false") : QStringLiteral("true");
    result["errors"] = errors;
    result["info"] = info;
    return !layer ? SE_ERROR_FILE_NOT_FOUND : !configValid ? SE_ERROR_INVALID_PARAMETER : !written ? SE_ERROR_WRITE_FILE_FAILED : SE_ERROR_NONE;
}

SE_Error BoundaryBandLegendStyleCheck(const char* bandPolygonPath, const char* schemaXmlPath,
    const char* outputJsonPath, const char* outputShpPath, const char* outputGeoJsonPath,
    bool& passed, QJsonObject& result)
{
    passed = false;
    result = QJsonObject();
    SE_BoundaryBandRenderRuleConfig config;
    SE_Error error = ParseXmlForBoundaryBandRender(schemaXmlPath, config);
    if (error != SE_ERROR_NONE) {
        result["passed"] = false;
        result["status"] = QStringLiteral("质检失败");
        result["description"] = QStringLiteral("图例规则XML无法读取或解析");
        return error;
    }
    GDALAllRegister();
    GDALDataset* dataset = bandPolygonPath && *bandPolygonPath
        ? static_cast<GDALDataset*>(GDALOpenEx(bandPolygonPath, GDAL_OF_VECTOR, nullptr, nullptr, nullptr)) : nullptr;
    OGRLayer* layer = dataset && dataset->GetLayerCount() ? dataset->GetLayer(0) : nullptr;
    error = InspectBoundaryBandLegendStyles(layer, config,
        QString::fromUtf8(outputShpPath ? outputShpPath : ""), QString::fromUtf8(outputGeoJsonPath ? outputGeoJsonPath : ""), result);
    if (dataset) GDALClose(dataset);
    if (outputJsonPath && *outputJsonPath) {
        const QString path = QString::fromUtf8(outputJsonPath);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QJsonObject item = result;
        item.remove("passed");
        QJsonObject report;
        report["check_item"] = item;
        report["ok"] = error == SE_ERROR_NONE;
        report["passed"] = error == SE_ERROR_NONE && result["passed"].toBool();
        const QByteArray bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size()) {
            error = SE_ERROR_WRITE_FILE_FAILED;
            result["passed"] = false;
            result["status"] = QStringLiteral("质检失败");
            result["description"] = QStringLiteral("图例检查报告写出失败");
        }
    }
    passed = error == SE_ERROR_NONE && result["passed"].toBool();
    return error;
}
