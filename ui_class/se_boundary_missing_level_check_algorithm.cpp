#include "se_boundary_missing_level_check_algorithm.h"

// 让 VS/MSVC 按 UTF-8 解释本文件中的中文字符串，避免任务日志文字乱码。
#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QStringList>

#include <cmath>

#include "cpl_conv.h"
#include "cpl_string.h"

namespace
{
    // 线宽属性表单位是 pt，1 pt = 1/72 inch = 25.4/72 mm。
    const double kPointToMillimeter = 25.4 / 72.0;

    // 线宽换算后允许 0.01mm 的浮点和字段取值误差。
    const double kWidthToleranceMm = 0.01;
}

bool se_boundary_missing_level_check_algorithm::RunBoundaryMissingLevelCheck(
    const QString& shpPath,
    const QString& xmlPath,
    const QString& outputShpPath,
    BoundaryMissingLevelCheckResult& result)
{
    GDALAllRegister();
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    CPLSetConfigOption("SHAPE_ENCODING", "");
    return runCore(shpPath, xmlPath, outputShpPath, !outputShpPath.trimmed().isEmpty(), result, {});
}

bool se_boundary_missing_level_check_algorithm::runCore(
    const QString& shpPath,
    const QString& xmlPath,
    const QString& outputShpPath,
    bool writeOutput,
    BoundaryMissingLevelCheckResult& result,
    const std::function<void(const QString&)>& log)
{
    result.problemCount = 0;
    result.message.clear();
    result.problemDetails = QJsonArray();

    int& problemCount = result.problemCount;
    QString& message = result.message;

    const auto writeLog = [&log](const QString& logMessage) {
        if (log)
            log(logMessage);
    };

    BoundarySymbolRule rule;
    QString error;

    // 总控流程：
    // 1. 从 XML 读取“线宽字段、颜色字段、标准线宽、标准颜色”；
    // 2. 打开解析后的 SHP；
    // 3. 遍历每条要素，统计线宽或颜色不一致的问题；
    // 4. 只有 writeOutput 为 true 时，才创建并写出问题 SHP
    writeLog("正在读取 XML 规则。");
    if (!readRuleFromXml(xmlPath, rule, error)) {
        message = error;
        return false;
    }

    writeLog(QString("规则读取完成：线宽字段=%1，颜色字段=%2，标准线宽=%3mm，标准颜色=%4。")
        .arg(rule.widthFieldName)
        .arg(rule.colorFieldName)
        .arg(rule.expectedWidthMm)
        .arg(rule.expectedColor));

    writeLog("正在打开解析后的 SHP 数据。");
    GdalDatasetPtr inputDataset(static_cast<GDALDataset*>(
        GDALOpenEx(
            toGdalPath(shpPath).constData(),
            GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr,
            nullptr,
            nullptr)));

    if (!inputDataset) {
        message = "SHP 数据打开失败：" + shpPath;
        return false;
    }

    if (inputDataset->GetLayerCount() <= 0) {
        message = "SHP 中没有可检查的图层。";
        return false;
    }

    GDALDriver* driver = nullptr;
    QByteArray outputBytes;
    GdalDatasetPtr outputDataset;
    OGRLayer* outputLayer = nullptr;

    if (writeOutput) {
        if (outputShpPath.trimmed().isEmpty()) {
            message = "输出路径为空，无法生成问题 SHP。";
            return false;
        }

        OGRLayer* firstLayer = inputDataset->GetLayer(0);
        if (firstLayer == nullptr) {
            message = "SHP 中没有可检查的图层。";
            return false;
        }

        driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
        if (driver == nullptr) {
            message = "当前 GDAL 环境不支持 ESRI Shapefile 驱动。";
            return false;
        }

        outputBytes = toGdalPath(outputShpPath);

    // 如果结果文件已存在，让 GDAL 删除 shp/dbf/shx/prj 等整组文件。
        if (QFileInfo(outputShpPath).exists())
            driver->Delete(outputBytes.constData());

        outputDataset.reset(
            driver->Create(outputBytes.constData(), 0, 0, 0, GDT_Unknown, nullptr));

        if (!outputDataset) {
            message = "结果 SHP 创建失败：" + outputShpPath;
            return false;
        }

        // 明确声明 DBF 使用 UTF-8。ESRI Shapefile 驱动会同时生成 .cpg，
        // 供 GIS 软件按正确编码读取中文问题说明和图层名。
        char** layerOptions = nullptr;
        layerOptions = CSLSetNameValue(layerOptions, "ENCODING", "UTF-8");

        outputLayer = outputDataset->CreateLayer(
            "symbol_err",
            firstLayer->GetSpatialRef(),
            firstLayer->GetGeomType(),
            layerOptions);

        CSLDestroy(layerOptions);

        if (outputLayer == nullptr) {
            message = "结果图层创建失败。";
            return false;
        }

        // 结果字段记录原要素来源、实际符号参数、标准符号参数和问题说明。
            // 后续在 GIS 软件里打开结果 SHP 时，可以直接定位是哪条原始要素出了问题。
        OGRFieldDefn layerField("src_layer", OFTString);
        layerField.SetWidth(64);
        outputLayer->CreateField(&layerField);

        OGRFieldDefn fidField("src_fid", OFTInteger64);
        outputLayer->CreateField(&fidField);

        OGRFieldDefn widthPtField("width_pt", OFTString);
        widthPtField.SetWidth(32);
        outputLayer->CreateField(&widthPtField);

        OGRFieldDefn widthMmField("width_mm", OFTReal);
        widthMmField.SetWidth(18);
        widthMmField.SetPrecision(6);
        outputLayer->CreateField(&widthMmField);

        OGRFieldDefn expectedWidthField("exp_width", OFTReal);
        expectedWidthField.SetWidth(18);
        expectedWidthField.SetPrecision(6);
        outputLayer->CreateField(&expectedWidthField);

        OGRFieldDefn colorField("color", OFTString);
        colorField.SetWidth(64);
        outputLayer->CreateField(&colorField);

        OGRFieldDefn expectedColorField("exp_color", OFTString);
        expectedColorField.SetWidth(64);
        outputLayer->CreateField(&expectedColorField);

        OGRFieldDefn issueField("issue", OFTString);
        issueField.SetWidth(128);
        outputLayer->CreateField(&issueField);
    }
    else {
        writeLog("本次只执行检查统计，不生成结果 SHP。");
    }
    for (int i = 0; i < inputDataset->GetLayerCount(); ++i) {
        OGRLayer* layer = inputDataset->GetLayer(i);
        if (layer == nullptr)
            continue;

        // 一个 SHP 通常只有一个图层，这里仍按 GDAL 数据集的通用方式逐图层处理。
        writeLog(QString("检查 SHP 图层：%1").arg(layer->GetName()));
        if (!checkLayerSymbol(layer, outputLayer, rule, problemCount, error, result.problemDetails, writeLog)) {
            message = error;
            return false;
        }
    }

    if (outputLayer != nullptr)
        outputLayer->SyncToDisk();

    if (problemCount == 0) {
        if (writeOutput) {
            // 没有发现问题时，不保留空的结果 SHP，避免用户误以为生成了有效问题数据。
            outputDataset.reset();
            if (driver != nullptr && driver->Delete(outputBytes.constData()) != CE_None)
                writeLog("未发现符号等级问题，但删除空结果 SHP 失败，请手动检查输出目录。");
        }

        message = "检查完成。未发现符号等级不合规要素，未生成结果 SHP。";
        return true;
    }

    if (!writeOutput) {
        message = QString("检查完成。符号等级不合规要素数量：%1。未指定输出路径，未生成问题 SHP。")
            .arg(problemCount);
        return true;
    }

    message = QString("检查完成。符号等级不合规要素数量：%1。结果文件：%2")
        .arg(problemCount)
        .arg(outputShpPath);
    return true;
}

bool se_boundary_missing_level_check_algorithm::readRuleFromXml(
    const QString& xmlPath,
    BoundarySymbolRule& rule,
    QString& error)
{
    // XML 规则文件决定“读取 SHP 哪两个属性字段”和“标准符号值是多少”。
    // 本功能只处理 ID=114 的“境界线越级检查”。
    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = "XML 文件打开失败：" + xmlPath;
        return false;
    }

    QDomDocument document;
    QString parseError;
    int errorLine = 0;
    int errorColumn = 0;

    if (!document.setContent(&file, &parseError, &errorLine, &errorColumn)) {
        error = QString("XML 解析失败：%1，第 %2 行，第 %3 列")
            .arg(parseError)
            .arg(errorLine)
            .arg(errorColumn);
        return false;
    }

    const QDomElement root = document.documentElement();
    const QDomNodeList itemNodes = root.elementsByTagName("check_item");

    for (int i = 0; i < itemNodes.count(); ++i) {
        const QDomElement itemElement = itemNodes.at(i).toElement();
        const QString idText = itemElement.firstChildElement("id").text().trimmed();

        if (idText != "114")
            continue;

        // 按 rule/name 区分规则含义：
        // “线宽属性项、颜色属性项”给出 SHP 字段名；
        // “线宽、颜色”给出要对比的标准值。
        const QDomNodeList ruleNodes = itemElement.elementsByTagName("rule");
        for (int j = 0; j < ruleNodes.count(); ++j) {
            const QDomElement ruleElement = ruleNodes.at(j).toElement();
            const QString ruleName = ruleElement.firstChildElement("name").text().trimmed();
            const QString valueText = ruleElement.firstChildElement("value").text().trimmed();

            if (ruleName == "线宽属性项") {
                rule.widthFieldName = valueText;
                continue;
            }

            if (ruleName == "颜色属性项") {
                rule.colorFieldName = valueText;
                continue;
            }

            if (ruleName == "线宽") {
                rule.expectedWidthMm = valueText.toDouble();
                rule.widthUnit = ruleElement.firstChildElement("unit").text().trimmed();
                continue;
            }

            if (ruleName == "颜色") {
                rule.expectedColor = valueText;
                continue;
            }
        }

        // 四个关键规则缺一不可，否则无法知道该读哪个字段、该按什么标准比对。
        if (rule.widthFieldName.isEmpty()) {
            error = "XML 中 ID=114 缺少“线宽属性项”规则。";
            return false;
        }

        if (rule.colorFieldName.isEmpty()) {
            error = "XML 中 ID=114 缺少“颜色属性项”规则。";
            return false;
        }

        if (rule.expectedWidthMm <= 0.0) {
            error = "XML 中 ID=114 的“线宽”必须大于 0。";
            return false;
        }

        if (rule.expectedColor.isEmpty()) {
            error = "XML 中 ID=114 缺少“颜色”规则。";
            return false;
        }

        if (rule.widthUnit.isEmpty())
            rule.widthUnit = "mm";

        return true;
    }

    error = "XML 中没有找到 ID=114 的境界线越级检查规则。";
    return false;
}

bool se_boundary_missing_level_check_algorithm::checkLayerSymbol(
    OGRLayer* inputLayer,
    OGRLayer* outputLayer,
    const BoundarySymbolRule& rule,
    int& problemCount,
    QString& error,
    QJsonArray& problemDetails,
    const std::function<void(const QString&)>& log)
{
    if (inputLayer == nullptr) {
        error = "输入图层为空。";
        return false;
    }

    OGRFeatureDefn* definition = inputLayer->GetLayerDefn();
    if (definition == nullptr) {
        error = QString("图层 %1 的字段定义为空。").arg(inputLayer->GetName());
        return false;
    }

    // XML 中保存的是字段名称，这里先转换成字段下标，后面逐要素读取会更直接。
    const QByteArray widthFieldBytes = rule.widthFieldName.toUtf8();
    const QByteArray colorFieldBytes = rule.colorFieldName.toUtf8();

    const int widthFieldIndex = definition->GetFieldIndex(widthFieldBytes.constData());
    const int colorFieldIndex = definition->GetFieldIndex(colorFieldBytes.constData());

    if (widthFieldIndex < 0) {
        error = QString("图层 %1 没有线宽属性字段：%2。")
            .arg(inputLayer->GetName())
            .arg(rule.widthFieldName);
        return false;
    }

    if (colorFieldIndex < 0) {
        error = QString("图层 %1 没有颜色属性字段：%2。")
            .arg(inputLayer->GetName())
            .arg(rule.colorFieldName);
        return false;
    }

    const QString expectedColorNorm = normalizeColor(rule.expectedColor);

    // 从第一条要素重新开始读，逐条检查线宽和颜色是否符合 XML 标准。
    inputLayer->ResetReading();

    const int problemCountBeforeLayer = problemCount;
    int checkedCount = 0;
    int emptyGeometryCount = 0;
    int validWidthCount = 0;
    int problemLogCount = 0;
    double minWidthMm = 0.0;
    double maxWidthMm = 0.0;
    QString sampleWidthText;
    QString sampleColorText;
    QString sampleColorNorm;

    OGRFeature* rawFeature = nullptr;
    while ((rawFeature = inputLayer->GetNextFeature()) != nullptr) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
            feature(rawFeature, OGRFeature::DestroyFeature);

        OGRGeometry* geometry = feature->GetGeometryRef();
        if (geometry == nullptr || geometry->IsEmpty()) {
            ++emptyGeometryCount;
            continue;

        }

        ++checkedCount;

        const QString actualWidthText =
            QString::fromUtf8(feature->GetFieldAsString(widthFieldIndex)).trimmed();
        const QString actualColor =
            QString::fromUtf8(feature->GetFieldAsString(colorFieldIndex)).trimmed();
        const QString actualColorNorm = normalizeColor(actualColor);

        if (sampleColorText.isEmpty()) {
            sampleColorText = actualColor;
            sampleColorNorm = actualColorNorm;
        }

        double actualWidthPoint = 0.0;
        double actualWidthMm = 0.0;
        QStringList issues;

        // SHP 属性表里的线宽单位是 pt，先换算成 mm，再和 XML 中的标准 mm 值比较。
        if (!parseWidthPointToMm(actualWidthText, actualWidthPoint, actualWidthMm)) {
            issues << QString("线宽值无法解析：%1").arg(actualWidthText);
        }
        else {
            if (validWidthCount == 0) {
                minWidthMm = actualWidthMm;
                maxWidthMm = actualWidthMm;
                sampleWidthText = actualWidthText;
            }
            else {
                if (actualWidthMm < minWidthMm)
                    minWidthMm = actualWidthMm;
                if (actualWidthMm > maxWidthMm)
                    maxWidthMm = actualWidthMm;
            }

            ++validWidthCount;

            if (std::fabs(actualWidthMm - rule.expectedWidthMm) > kWidthToleranceMm) {
                issues << QString("线宽不一致，实际%1mm，标准%2mm")
                    .arg(actualWidthMm, 0, 'f', 6)
                    .arg(rule.expectedWidthMm, 0, 'f', 6);
            }
        }

        // SHP 颜色是十六进制，XML 颜色是 CMYK；比较前统一换算成 CMYK 文本
        if (actualColorNorm != expectedColorNorm) {
            issues << QString("颜色不一致，实际%1，标准%2")
                .arg(actualColor)
                .arg(rule.expectedColor);
        }

        if (issues.isEmpty())
            continue;

        // outputLayer 为空表示“只检查不输出”；不为空时才写入问题 SHP。
        if (outputLayer != nullptr) {
            if (!writeProblemFeature(
                outputLayer,
                QString::fromUtf8(inputLayer->GetName()),
                feature->GetFID(),
                geometry,
                actualWidthText,
                actualWidthMm,
                rule.expectedWidthMm,
                actualColor,
                rule.expectedColor,
                issues.join("; "))) {
                error = "写入问题要素失败。";
                return false;
            }
        }

        ++problemCount;

        QJsonObject problemDetail;
        problemDetail["layer"] = QString::fromUtf8(inputLayer->GetName());
        problemDetail["fid"] = static_cast<qint64>(feature->GetFID());
        problemDetail["actual_width_pt"] = actualWidthText;
        problemDetail["actual_width_mm"] = actualWidthMm;
        problemDetail["expected_width_mm"] = rule.expectedWidthMm;
        problemDetail["actual_color"] = actualColor;
        problemDetail["expected_color"] = rule.expectedColor;
        problemDetail["issue"] = issues.join("; ");
        problemDetails.append(problemDetail);

        // 日志中最多展开前 20 条问题明细，避免问题很多时刷屏。
        if (problemLogCount < 20) {
            if (log) log(QString("发现问题：图层=%1，FID=%2，线宽=%3pt/%4mm，标准线宽=%5mm，颜色=%6->%7，标准颜色=%8->%9，问题=%10")
                .arg(inputLayer->GetName())
                .arg(feature->GetFID())
                .arg(actualWidthText)
                .arg(actualWidthMm, 0, 'f', 6)
                .arg(rule.expectedWidthMm, 0, 'f', 6)
                .arg(actualColor)
                .arg(actualColorNorm)
                .arg(rule.expectedColor)
                .arg(expectedColorNorm)
                .arg(issues.join("; ")));
            ++problemLogCount;
        }
    }

    const int layerProblemCount = problemCount - problemCountBeforeLayer;
    if (log) log(QString("图层比对完成：%1。检查要素=%2，合格要素=%3，问题要素=%4，空几何跳过=%5。")
        .arg(inputLayer->GetName())
        .arg(checkedCount)
        .arg(checkedCount - layerProblemCount)
        .arg(layerProblemCount)
        .arg(emptyGeometryCount));

    if (validWidthCount > 0) {
        if (log) log(QString("线宽比对结果：字段=%1，标准=%2mm，实际范围=%3~%4mm，示例原值=%5pt。")
            .arg(rule.widthFieldName)
            .arg(rule.expectedWidthMm, 0, 'f', 6)
            .arg(minWidthMm, 0, 'f', 6)
            .arg(maxWidthMm, 0, 'f', 6)
            .arg(sampleWidthText));
    }
    else {
        if (log) log(QString("线宽比对结果：字段=%1，没有解析到有效线宽值。")
            .arg(rule.widthFieldName));
    }

    if (!sampleColorText.isEmpty()) {
        if (log) log(QString("颜色比对结果：字段=%1，标准=%2->%3，示例实际=%4->%5。")
            .arg(rule.colorFieldName)
            .arg(rule.expectedColor)
            .arg(expectedColorNorm)
            .arg(sampleColorText)
            .arg(sampleColorNorm));
    }
    else {
        if (log) log(QString("颜色比对结果：字段=%1，没有读取到有效颜色值。")
            .arg(rule.colorFieldName));
    }

    if (layerProblemCount > problemLogCount) {
        if (log) log(QString("问题明细较多，仅在日志中展开前 %1 条，其余 %2 条可通过输出 SHP 查看。")
            .arg(problemLogCount)
            .arg(layerProblemCount - problemLogCount));

    }

    return true;
}

bool se_boundary_missing_level_check_algorithm::writeProblemFeature(
    OGRLayer* outputLayer,
    const QString& sourceLayerName,
    GIntBig sourceFid,
    OGRGeometry* geometry,
    const QString& actualWidthText,
    double actualWidthMm,
    double expectedWidthMm,
    const QString& actualColor,
    const QString& expectedColor,
    const QString& issueText)
{
    // 结果要素沿用原始几何，只额外记录问题说明和比对用的属性值。
    OGRFeatureDefn* definition = outputLayer->GetLayerDefn();
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
        outputFeature(OGRFeature::CreateFeature(definition), OGRFeature::DestroyFeature);

    const QByteArray layerNameBytes = sourceLayerName.toUtf8();
    const QByteArray widthTextBytes = actualWidthText.toUtf8();
    const QByteArray colorBytes = actualColor.toUtf8();
    const QByteArray expectedColorBytes = expectedColor.toUtf8();
    const QByteArray issueBytes = issueText.toUtf8();

    outputFeature->SetField("src_layer", layerNameBytes.constData());
    outputFeature->SetField("src_fid", sourceFid);
    outputFeature->SetField("width_pt", widthTextBytes.constData());
    outputFeature->SetField("width_mm", actualWidthMm);
    outputFeature->SetField("exp_width", expectedWidthMm);
    outputFeature->SetField("color", colorBytes.constData());
    outputFeature->SetField("exp_color", expectedColorBytes.constData());
    outputFeature->SetField("issue", issueBytes.constData());
    outputFeature->SetGeometry(geometry);

    return outputLayer->CreateFeature(outputFeature.get()) == OGRERR_NONE;
}

bool se_boundary_missing_level_check_algorithm::parseWidthPointToMm(
    const QString& widthText,
    double& widthPoint,
    double& widthMm)
{
    // 兼容 “0.5pt”、“0.5 point”、“0.5点”、“0.5” 这类常见写法。
    QString text = widthText.trimmed();
    text.remove("pt", Qt::CaseInsensitive);
    text.remove("point", Qt::CaseInsensitive);
    text.remove("points", Qt::CaseInsensitive);
    text.remove("点");
    text.remove(" ");

    bool ok = false;
    widthPoint = text.toDouble(&ok);
    if (!ok)
        return false;

    // pt 转 mm：1 pt = 1/72 inch，1 inch = 25.4 mm。
    widthMm = widthPoint * kPointToMillimeter;
    return true;
}

QString se_boundary_missing_level_check_algorithm::normalizeColor(
    const QString& colorText)
{
    QString text = colorText.trimmed().toLower();
    text.remove(" ");
    text.remove("\t");
    text.remove("\r");
    text.remove("\n");
    text.remove("%");

    auto toPercentText = [](double value) -> QString {
        if (value < 0.0)
            value = 0.0;
        if (value > 100.0)
            value = 100.0;

        return QString::number(static_cast<int>(std::floor(value + 0.5)));
    };

    auto makeCmykText = [&](double c, double m, double y, double k) -> QString {
        return QString("c%1m%2y%3k%4")
            .arg(toPercentText(c))
            .arg(toPercentText(m))
            .arg(toPercentText(y))
            .arg(toPercentText(k));
    };

    // SHP 里的十六进制颜色：#RRGGBB 或 RRGGBB。
    QString hexText = text;
    if (hexText.startsWith("#"))
        hexText.remove(0, 1);

    if (hexText.length() == 6) {
        bool rOk = false;
        bool gOk = false;
        bool bOk = false;

        const int rValue = hexText.mid(0, 2).toInt(&rOk, 16);
        const int gValue = hexText.mid(2, 2).toInt(&gOk, 16);
        const int bValue = hexText.mid(4, 2).toInt(&bOk, 16);

        if (rOk && gOk && bOk) {
            const double r = rValue / 255.0;
            const double g = gValue / 255.0;
            const double b = bValue / 255.0;

            double maxRgb = r;
            if (g > maxRgb)
                maxRgb = g;
            if (b > maxRgb)
                maxRgb = b;

            const double k = 1.0 - maxRgb;

            if (k >= 1.0)
                return makeCmykText(0.0, 0.0, 0.0, 100.0);

            const double c = (1.0 - r - k) / (1.0 - k);
            const double m = (1.0 - g - k) / (1.0 - k);
            const double y = (1.0 - b - k) / (1.0 - k);

            return makeCmykText(c * 100.0, m * 100.0, y * 100.0, k * 100.0);
        }
    }

    // XML 里的 k100，表示 c0m0y0k100。
    if (text.startsWith("k")) {
        bool ok = false;
        const double k = text.mid(1).toDouble(&ok);
        if (ok)
            return makeCmykText(0.0, 0.0, 0.0, k);
    }

    // XML 里的 cmyk(0,0,0,100)。
    QString cmykText = text;
    if (cmykText.startsWith("cmyk(") && cmykText.endsWith(")")) {
        cmykText = cmykText.mid(5, cmykText.length() - 6);

        const QStringList values = cmykText.split(",");
        if (values.size() == 4) {
            bool cOk = false;
            bool mOk = false;
            bool yOk = false;
            bool kOk = false;

            const double c = values.at(0).toDouble(&cOk);
            const double m = values.at(1).toDouble(&mOk);
            const double y = values.at(2).toDouble(&yOk);
            const double k = values.at(3).toDouble(&kOk);

            if (cOk && mOk && yOk && kOk)
                return makeCmykText(c, m, y, k);
        }
    }

    // XML 里的 c0m0y0k100，已经是目标格式。
    return text;
}

QByteArray se_boundary_missing_level_check_algorithm::toGdalPath(
    const QString& path)
{
    // GDALOpenEx/Create 使用 char* 路径，这里统一按 UTF-8 传入。
    return path.toUtf8();
}
