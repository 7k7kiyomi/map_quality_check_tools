#define _HAS_STD_BYTE 0
#include "se_map_annotation_check_algorithm.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <QtXml/QDomDocument>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QString>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <cmath>
#include <cstring>

// 简单的中文路径支持：统一用UTF-8
static QString qFromUtf8(const char* s) { return QString::fromUtf8(s); }

// JSON输出（采用标准 check_item4 结构）
static void WriteJsonResult(
    const char* szOutputJsonPath,
    bool success,
    bool passed,
    const QJsonArray& errorDetails = QJsonArray(),
    const QString& infoText = QString())
{
    QJsonObject item;
    item["id"] = "305";  // 地图落款检查ID
    item["name"] = QStringLiteral("制图说明检查");
    item["status"] = success ? QStringLiteral("质检成功") : QStringLiteral("质检失败");
    item["err_exist"] = passed ? QStringLiteral("false") : QStringLiteral("true");

    if (!infoText.isEmpty())
        item["info"] = infoText;
    else
        item["info"] = passed ? QStringLiteral("综合后地图不存在地图落款错误")
                               : QStringLiteral("综合后地图存在地图落款错误");

    if (!passed && !errorDetails.isEmpty()) {
        QJsonObject errors;
        for (int i = 0; i < errorDetails.size(); ++i) {
            QJsonObject d = errorDetails[i].toObject();
            // 只保留description作为错误信息
            errors[QString("error%1").arg(i + 1)] = d.value("description").toString();
        }
        item["errors"] = errors;
    }

    QJsonObject root;
    root["check_item"] = item;

    QJsonDocument doc(root);
    QFile jsonFile(szOutputJsonPath);
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        jsonFile.write(doc.toJson(QJsonDocument::Indented));
        jsonFile.close();
    }
}

// 解析XML：支持 check_item -> rule -> (name,value)
SE_Error ParseXmlForMapAnnotation(const char* szSchemaXmlPath, MapAnnotationRules* pRules)
{
    if (!pRules) return SE_ERROR_FAILURE;

    QFile file(szSchemaXmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return SE_ERROR_FILE_NOT_FOUND;

    QDomDocument doc;
    QString errorMsg; int errorLine = 0, errorColumn = 0;
    if (!doc.setContent(&file, &errorMsg, &errorLine, &errorColumn)) {
        file.close();
        return SE_ERROR_XML_PARSE_FAILED;
    }
    file.close();

    QDomElement root = doc.documentElement();
    QDomNodeList checkItems = root.elementsByTagName("check_item");

    for (int i = 0; i < checkItems.count(); ++i) {
        QDomElement checkItem = checkItems.at(i).toElement();
        QDomElement itemNameEl = checkItem.firstChildElement("name");
        if (itemNameEl.isNull()) continue;
        const QString itemName = itemNameEl.text();

        // 仅处理与落款/图名样式相关的项
        if (!itemName.contains(QStringLiteral("图名内容准确性")) &&
            !itemName.contains(QStringLiteral("地图落款")) &&
            !itemName.contains(QStringLiteral("图名样式")) &&
            !itemName.contains(QStringLiteral("制图说明检查")))
            continue;

        // 遍历 rule
        QDomNodeList rules = checkItem.elementsByTagName("rule");
        for (int j = 0; j < rules.count(); ++j) {
            QDomElement rule = rules.at(j).toElement();
            QDomElement ruleName = rule.firstChildElement("name");
            QDomElement ruleValue = rule.firstChildElement("value");
            if (ruleName.isNull() || ruleValue.isNull()) continue;

            const QString name = ruleName.text();
            const QString value = ruleValue.text();

            if (name.contains(QStringLiteral("字体"))) {
                strncpy(pRules->szFontName, value.toUtf8().constData(), sizeof(pRules->szFontName)-1);
                pRules->szFontName[sizeof(pRules->szFontName)-1] = '\0';
            } else if (name.contains(QStringLiteral("字大")) || name.contains(QStringLiteral("字号"))) {
                pRules->dFontSizeMM = value.toDouble();
            } else if (name.contains(QStringLiteral("图廓颜色")) || name.contains(QStringLiteral("颜色"))) {
                strncpy(pRules->szFrameColor, value.toUtf8().constData(), sizeof(pRules->szFrameColor)-1);
                pRules->szFrameColor[sizeof(pRules->szFrameColor)-1] = '\0';
            } else if (name.contains(QStringLiteral("距离"))) {
                pRules->dDistance = value.toDouble();
            } else if (name.contains(QStringLiteral("对齐"))) {
                strncpy(pRules->szAlignment, value.toUtf8().constData(), sizeof(pRules->szAlignment)-1);
                pRules->szAlignment[sizeof(pRules->szAlignment)-1] = '\0';
            } else if (name.contains(QStringLiteral("排布"))) {
                strncpy(pRules->szLayout, value.toUtf8().constData(), sizeof(pRules->szLayout)-1);
                pRules->szLayout[sizeof(pRules->szLayout)-1] = '\0';
            }
        }
    }

    return SE_ERROR_NONE;
}

// 正则判断日期（中文数字年月）简化：只检测"年"和"月"字符是否都出现
static bool looksLikeChineseDate(const QString& s)
{
    return s.contains(QStringLiteral("年")) && s.contains(QStringLiteral("月"));
}

// 规范化颜色名：去空格、小写
static QString normalizeColor(const QString& s)
{
    QString t = s;
    t.replace(" ", "");
    return t.toLower();
}

// 字体匹配（支持常见别名映射）
static bool isFontMatch(const QString& expected, const QString& actual)
{
    // 直接相等（不区分大小写）
    if (expected.compare(actual, Qt::CaseInsensitive) == 0)
        return true;

    // 黑体 ≈ SimHei
    if (expected.contains(QStringLiteral("黑体"), Qt::CaseInsensitive) &&
        actual.compare(QStringLiteral("SimHei"), Qt::CaseInsensitive) == 0)
        return true;

    // 可扩展更多映射
    return false;
}

SE_Error MapAnnotationCheck(
    const char* szDecorationLinePath,
    const char* szDecorationPolyPath,
    const char* szInnerFramePath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    bool& bPassed)
{
    bPassed = false;

    // 解析XML
    MapAnnotationRules rules;
    {
        SE_Error perr = ParseXmlForMapAnnotation(szSchemaXmlPath, &rules);
        if (perr != SE_ERROR_NONE) {
            WriteJsonResult(szOutputJsonPath, false, false, QJsonArray(), QStringLiteral("XML解析失败或文件不存在"));
            return perr;
        }
    }

    // 注册GDAL并配置中文路径
    GDALAllRegister();
#ifdef _WIN32
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");
#else
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "NO");
    CPLSetConfigOption("SHAPE_ENCODING", "");
#endif

    // 打开数据集
    auto openVector = [](const char* p) -> GDALDataset* {
        if (!p) return nullptr;
        QString q = qFromUtf8(p);
#ifdef _WIN32
        std::string local = q.toUtf8().constData();
#else
        std::string local = q.toLocal8Bit().constData();
#endif
        GDALDataset* ds = (GDALDataset*)GDALOpenEx(local.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (!ds) ds = (GDALDataset*)GDALOpenEx(p, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        return ds;
    };

    GDALDataset* dsPoly = openVector(szDecorationPolyPath);
    GDALDataset* dsLine = openVector(szDecorationLinePath); // 目前未用，但保留参数位
    GDALDataset* dsInner = openVector(szInnerFramePath);

    if (!dsPoly || !dsInner) {
        if (dsPoly) GDALClose(dsPoly);
        if (dsLine) GDALClose(dsLine);
        if (dsInner) GDALClose(dsInner);
        WriteJsonResult(szOutputJsonPath, false, false, QJsonArray(), QStringLiteral("图层无效或无法打开"));
        return SE_ERROR_LAYER_INVALID;
    }

    OGRLayer* layerPoly = dsPoly->GetLayer(0);
    OGRLayer* layerInner = dsInner->GetLayer(0);
    if (!layerPoly || !layerInner) {
        if (dsPoly) GDALClose(dsPoly);
        if (dsLine) GDALClose(dsLine);
        if (dsInner) GDALClose(dsInner);
        WriteJsonResult(szOutputJsonPath, false, false, QJsonArray(), QStringLiteral("图层为空"));
        return SE_ERROR_LAYER_INVALID;
    }

    // 在整饰面图层中：
    // 1) 找到外图廓（最大面积要素）
    // 2) 找到包含“监制”或“编制”的落款文字要素（通过 text_conte 字段）
    QJsonArray errorDetails;
    OGRFeature* outerFrameFeat = nullptr;
    OGRFeature* annoFeat = nullptr;
    double maxArea = -1.0;

    layerPoly->ResetReading();
    OGRFeature* f;
    while ((f = layerPoly->GetNextFeature()) != nullptr) {
        OGRGeometry* g = f->GetGeometryRef();
        if (g) {
            OGRwkbGeometryType gType = wkbFlatten(g->getGeometryType());
            double area = 0.0;
            if (gType == wkbPolygon || gType == wkbMultiPolygon) {
                area = static_cast<OGRPolygon*>(g)->get_Area();
            }
            if (area > maxArea) { maxArea = area; if (outerFrameFeat) OGRFeature::DestroyFeature(outerFrameFeat); outerFrameFeat = f->Clone(); }
        }
        // 找落款文字
        int textIdx = f->GetFieldIndex("text_conte");
        if (textIdx >= 0 && f->IsFieldSetAndNotNull(textIdx)) {
            QString txt = QString::fromUtf8(f->GetFieldAsString(textIdx));
            if (txt.contains(QStringLiteral("监制")) || txt.contains(QStringLiteral("编制"))) {
                if (!annoFeat) annoFeat = f->Clone();
            }
        }
        OGRFeature::DestroyFeature(f);
    }

    if (!outerFrameFeat) {
        QJsonObject e; e["description"] = QStringLiteral("未找到外图廓"); e["expected"] = QStringLiteral("存在外图廓"); e["actual"] = QStringLiteral("未找到"); errorDetails.append(e);
    }

    // 检查内容项
    if (annoFeat) {
        int textIdx = annoFeat->GetFieldIndex("text_conte");
        QString txt = (textIdx >= 0) ? QString::fromUtf8(annoFeat->GetFieldAsString(textIdx)) : QString();
        bool hasSupervision = txt.contains(QStringLiteral("监制"));
        bool hasCompilation = txt.contains(QStringLiteral("编制"));
        bool hasDate = looksLikeChineseDate(txt);
        if (!hasSupervision) { QJsonObject e; e["description"] = QStringLiteral("缺少监制信息"); e["expected"] = QStringLiteral("存在"); e["actual"] = QStringLiteral("缺失"); errorDetails.append(e); }
        if (!hasCompilation) { QJsonObject e; e["description"] = QStringLiteral("缺少编制信息"); e["expected"] = QStringLiteral("存在"); e["actual"] = QStringLiteral("缺失"); errorDetails.append(e); }
        if (!hasDate) { QJsonObject e; e["description"] = QStringLiteral("缺少时间信息"); e["expected"] = QStringLiteral("存在中文年月"); e["actual"] = QStringLiteral("缺失"); errorDetails.append(e); }
    } else {
        QJsonObject e; e["description"] = QStringLiteral("未找到落款文字要素"); e["expected"] = QStringLiteral("存在"); e["actual"] = QStringLiteral("缺失"); errorDetails.append(e);
    }

    // 样式检查与位置检查（仅在找到落款文字时）
    if (annoFeat) {
        OGRFeatureDefn* defn = annoFeat->GetDefnRef();
        int fontFamIdx = defn->GetFieldIndex("font_famil");
        int fontSizeIdx = defn->GetFieldIndex("font_size");
        int colorIdx = defn->GetFieldIndex("font_color");

        // 字体
        if (fontFamIdx >= 0 && annoFeat->IsFieldSetAndNotNull(fontFamIdx) && std::strlen(rules.szFontName) > 0) {
            QString actualFont = QString::fromUtf8(annoFeat->GetFieldAsString(fontFamIdx));
            QString expectedFont = QString::fromUtf8(rules.szFontName);
            if (!isFontMatch(expectedFont, actualFont)) {
                QJsonObject e; e["description"] = QStringLiteral("字体不匹配"); e["expected"] = expectedFont; e["actual"] = actualFont; errorDetails.append(e); }
        }

        // 字号（pt->mm）
        if (fontSizeIdx >= 0 && std::fabs(rules.dFontSizeMM) > 1e-9) {
            double fontSizePt = annoFeat->GetFieldAsDouble(fontSizeIdx);
            double fontSizeMm = fontSizePt * 0.3528;
            if (std::fabs(fontSizeMm - rules.dFontSizeMM) > 0.1) {
                QJsonObject e; e["description"] = QStringLiteral("字号不匹配");
                e["expected"] = QString(QString::number(rules.dFontSizeMM, 'f', 3) + QStringLiteral(" mm"));
                e["actual"] = QString(QString::number(fontSizeMm, 'f', 3) + QStringLiteral(" mm"));
                errorDetails.append(e);
            }
        }

        // 颜色
        if (colorIdx >= 0 && annoFeat->IsFieldSetAndNotNull(colorIdx) && std::strlen(rules.szFrameColor) > 0) {
            QString actualColor = normalizeColor(QString::fromUtf8(annoFeat->GetFieldAsString(colorIdx)));
            QString expectedColor = normalizeColor(QString::fromUtf8(rules.szFrameColor));
            if (actualColor != expectedColor) {
                QJsonObject e; e["description"] = QStringLiteral("颜色不匹配"); e["expected"] = expectedColor; e["actual"] = actualColor; errorDetails.append(e); }
        }

        // 位置与对齐（需要外图廓与内图廓）
        if (outerFrameFeat && layerInner) {
            OGRGeometry* gOuter = outerFrameFeat->GetGeometryRef();
            OGRGeometry* gAnno = annoFeat->GetGeometryRef();
            layerInner->ResetReading();
            OGRFeature* innerFeat = layerInner->GetNextFeature();
            OGRGeometry* gInner = innerFeat ? innerFeat->GetGeometryRef() : nullptr;

            if (gOuter && gAnno && gInner) {
                OGREnvelope envOuter, envAnno, envInner;
                gOuter->getEnvelope(&envOuter);
                gAnno->getEnvelope(&envAnno);
                gInner->getEnvelope(&envInner);

                // 比例尺（和插件保持一致，固定 1:145000）
                const double mapScale = 145000.0;
                auto toMM = [mapScale](double meters) { return meters / mapScale * 1000.0; };

                // 与外图廓上边界距离（参考原实现：yMin 差）
                double distanceToTop_m = std::fabs(envAnno.MinY - envOuter.MinY);
                double distanceToTop_mm = toMM(distanceToTop_m);
                if (rules.dDistance > 0.0) {
                    if (std::fabs(distanceToTop_mm - rules.dDistance) > 0.5) {
                        QJsonObject e; e["description"] = QStringLiteral("与外图廓上边界距离不匹配");
                        e["expected"] = QString(QString::number(rules.dDistance, 'f', 2) + QStringLiteral(" mm ±0.5"));
                        e["actual"] = QString(QString::number(distanceToTop_mm, 'f', 2) + QStringLiteral(" mm"));
                        errorDetails.append(e);
                    }
                }

                // 按配置对齐方式判定（默认仅在配置提供时检查），容差±2mm
                if (std::strlen(rules.szAlignment) > 0) {
                    const QString align = QString::fromUtf8(rules.szAlignment);
                    if (align.contains(QStringLiteral("右对齐"))) {
                        double offsetRight_m = (envAnno.MaxX - envInner.MaxX);
                        double offsetRight_mm = toMM(offsetRight_m);
                        if (std::fabs(offsetRight_mm) > 2.0) {
                            QJsonObject e; e["description"] = QStringLiteral("与内图廓右对齐不满足");
                            e["expected"] = QStringLiteral("右对齐，偏差≤2.00 mm");
                            e["actual"] = QString(QString::number(offsetRight_mm, 'f', 2) + QStringLiteral(" mm"));
                            errorDetails.append(e);
                        }
                    } else if (align.contains(QStringLiteral("左对齐"))) {
                        double offsetLeft_m = (envAnno.MinX - envInner.MinX);
                        double offsetLeft_mm = toMM(offsetLeft_m);
                        if (std::fabs(offsetLeft_mm) > 2.0) {
                            QJsonObject e; e["description"] = QStringLiteral("与内图廓左对齐不满足");
                            e["expected"] = QStringLiteral("左对齐，偏差≤2.00 mm");
                            e["actual"] = QString(QString::number(offsetLeft_mm, 'f', 2) + QStringLiteral(" mm"));
                            errorDetails.append(e);
                        }
                    } else if (align.contains(QStringLiteral("居中"))) {
                        double centerAnno_m = (envAnno.MinX + envAnno.MaxX) * 0.5;
                        double centerInner_m = (envInner.MinX + envInner.MaxX) * 0.5;
                        double offsetCenter_m = (centerAnno_m - centerInner_m);
                        double offsetCenter_mm = toMM(offsetCenter_m);
                        if (std::fabs(offsetCenter_mm) > 2.0) {
                            QJsonObject e; e["description"] = QStringLiteral("与内图廓居中不满足");
                            e["expected"] = QStringLiteral("居中，偏差≤2.00 mm");
                            e["actual"] = QString(QString::number(offsetCenter_mm, 'f', 2) + QStringLiteral(" mm"));
                            errorDetails.append(e);
                        }
                    }
                }
            } else {
                QJsonObject e; e["description"] = QStringLiteral("内外图廓或文字几何无效"); e["expected"] = QStringLiteral("有效几何"); e["actual"] = QStringLiteral("无效"); errorDetails.append(e);
            }

            if (innerFeat) OGRFeature::DestroyFeature(innerFeat);
        }

        // 排布：是否一行
        if (std::strlen(rules.szLayout) > 0) {
            int textIdx2 = annoFeat->GetFieldIndex("text_conte");
            QString txt = (textIdx2 >= 0) ? QString::fromUtf8(annoFeat->GetFieldAsString(textIdx2)) : QString();
            bool singleLine = !txt.contains("\n") && !txt.contains("\r");
            const QString expectedLayout = QString::fromUtf8(rules.szLayout);
            if ((expectedLayout == QStringLiteral("一行") && !singleLine) ||
                (expectedLayout != QStringLiteral("一行") && singleLine)) {
                QJsonObject e; e["description"] = QStringLiteral("排布方式不匹配");
                e["expected"] = expectedLayout;
                e["actual"] = singleLine ? QStringLiteral("一行") : QStringLiteral("多行");
                errorDetails.append(e);
            }
        }
    }

    // 释放资源
    if (outerFrameFeat) OGRFeature::DestroyFeature(outerFrameFeat);
    if (annoFeat) OGRFeature::DestroyFeature(annoFeat);
    if (dsPoly) GDALClose(dsPoly);
    if (dsLine) GDALClose(dsLine);
    if (dsInner) GDALClose(dsInner);

    // 通过判定
    bPassed = errorDetails.isEmpty();
    WriteJsonResult(szOutputJsonPath, true, bPassed, errorDetails);
    return SE_ERROR_NONE;
}
