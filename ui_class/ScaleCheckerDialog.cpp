#include "ScaleCheckerDialog.h"
#include "ui_ScaleCheckerDialog.h"
#include <QRegExp>
#include <QXmlStreamReader>
#include <QTextStream>
#include <qfiledialog.h>
#include <qmessagebox.h>
#include <QDateTime>
#include <gdal_priv.h>
#include <ogr_api.h>
#include "ogrsf_frmts.h"
#include "ogr_feature.h"
#include "ogr_geometry.h"

// --- 添加 Qt JSON 模块 ---
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile> // (已包含在 .h 中，但显式包含更安全)

ScaleCheckerDialog::ScaleCheckerDialog(QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
    , ui(new Ui::ScaleCheckerDialogClass)
    , m_shpFilePath("")              // 比例尺源SHP路径
    , m_mapFrameShpPath("")          // 内图廓SHP路径
    , m_shpScaleValue("")            // 从SHP提取的比例尺
    , m_calculatedScale("")          // 计算的比例尺
    , m_jsonOutputPath("")           // 初始化JSON输出路径
{
    ui->setupUi(this);
    m_logTextEdit = ui->logTextEdit;
    connect(this, &ScaleCheckerDialog::updateLog, this, &ScaleCheckerDialog::updateLog);
}

ScaleCheckerDialog::~ScaleCheckerDialog()
{
    delete ui;
}

//加载图例SHP
void ScaleCheckerDialog::on_loadShpButton_clicked()
{
    QString filePath = QFileDialog::getOpenFileName(
        this,
        tr("选择SHP文件"),
        "",
        tr("Shapefiles (*.shp);;所有文件 (*)")
    );

    if (!filePath.isEmpty())
    {
        m_shpFilePath = filePath;
        ui->shpFilePathEdit->setText(m_shpFilePath);
        ui->statusLabel->setText(tr("已选择比例尺源SHP文件"));
    }
}

//加载内图廓SHP
void ScaleCheckerDialog::on_loadGeoPdfButton_clicked()
{
    m_mapFrameShpPath.clear();
    m_mapFramePoints.clear();
    m_mapFrameAttrPixelWidth = 0.0;
    m_mapFrameAttrPixelHeight = 0.0;

    QString filePath = QFileDialog::getOpenFileName(
        this,
        tr("选择内图廓SHP文件"),
        "",
        tr("Shapefiles (*.shp);;所有文件 (*)")
    );

    if (filePath.isEmpty()) {
        ui->statusLabel->setText(tr("已取消选择内图廓SHP文件"));
        return;
    }

    m_mapFrameShpPath = filePath;
    ui->pdfFilePathEdit->setText(m_mapFrameShpPath);

    if (!extractMapFramePointsFromShp(filePath, m_mapFramePoints)) {
        cleanMapFrameData();
        return;
    }

    // 计算内图廓比例尺
    m_calculatedScale = calculateActualMapScale();
    if (m_calculatedScale.isEmpty()) {
        cleanMapFrameData();
        QMessageBox::critical(this, tr("错误"), tr("内图廓比例尺计算失败"));
        return;
    }

    QString coordInfo = tr("内图廓信息：\n");
    coordInfo += tr("内图廓宽度：%1\n").arg(m_mapFrameWidth, 0, 'f', 6);
    coordInfo += tr("内图廓顶点坐标：\n");
    for (int i = 0; i < m_mapFramePoints.size(); ++i) {
        coordInfo += tr("顶点 %1：X = %2，Y = %3\n")
            .arg(i + 1)
            .arg(m_mapFramePoints[i].x(), 0, 'f', 6)
            .arg(m_mapFramePoints[i].y(), 0, 'f', 6);
    }
    coordInfo += tr("\n内图廓像素大小（属性表）：宽度 = %1 px，高度 = %2 px\n")
        .arg(m_mapFrameAttrPixelWidth, 0, 'f', 2)
        .arg(m_mapFrameAttrPixelHeight, 0, 'f', 2);
    coordInfo += tr("内图廓计算的比例尺：%1").arg(m_calculatedScale);

    emit updateLog(coordInfo);
    ui->statusLabel->setText(tr("内图廓SHP选择成功！已计算比例尺"));
}

void ScaleCheckerDialog::updateLog(const QString& log)
{
    if (m_logTextEdit) {
        m_logTextEdit->append(log);
    }
}

void ScaleCheckerDialog::on_loadjsonButton_clicked()
{
    QString filePath = QFileDialog::getSaveFileName(
        this,
        tr("选择JSON日志输出路径"),
        "",
        tr("JSON文件 (*.json);;所有文件 (*)")
    );

    if (!filePath.isEmpty()) {
        m_jsonOutputPath = filePath;
        ui->jsonFilePathEdit->setText(m_jsonOutputPath);

        // 检查是否已完成检查（确保有足够的数据生成JSON）
        if (m_shpScaleValue.isEmpty() || m_calculatedScale.isEmpty()) {
            emit updateLog(tr("警告：请先执行检查，再导出JSON日志"));
            return;
        }

        // 生成检查结果文本（与on_checkButton_clicked中一致）
        QString resultText = tr(
            "===== 比例尺对比报告 =====\n"
            "生成时间：%1\n\n"
            "1. 数据来源\n"
            "- 图例SHP：%2\n"
            "- 内图廓SHP：%3\n\n"
            "2. 比例尺信息\n"
            "- 图例SHP提取的比例尺：%4\n"
            "- 内图廓计算的比例尺：%5\n\n"
            "3. 对比结果\n"
            "- 图例SHP与内图廓：%6\n\n"
            "4. 结论：%7"
        ).arg(
            QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"),
            m_shpFilePath,
            m_mapFrameShpPath,
            m_shpScaleValue,
            m_calculatedScale,
            (m_shpScaleValue == m_calculatedScale) ? tr("一致") : tr("不一致"),
            (m_shpScaleValue == m_calculatedScale) ? tr("两者比例尺一致") : tr("两者比例尺存在不一致")
        );

        exportResultToJson(resultText, m_shpScaleValue, "", "");
    }
}

//执行检查
void ScaleCheckerDialog::on_checkButton_clicked()
{
    // 检查必要文件是否已选择
    if (m_shpFilePath.isEmpty()) {
        emit updateLog(tr("错误：请先选择有效的图例SHP文件！"));
        return;
    }
    if (m_mapFrameShpPath.isEmpty() || m_mapFramePoints.size() != 4 || m_calculatedScale.isEmpty()) {
        emit updateLog(tr("错误：请先选择有效的内图廓SHP文件（需包含4个顶点）！"));
        return;
    }

    // 从图例SHP提取比例尺
    GDALAllRegister();
    GDALDataset* dataset = (GDALDataset*)GDALOpenEx(
        m_shpFilePath.toStdString().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    if (!dataset) {
        emit updateLog(tr("错误：无法打开图例SHP文件！"));
        return;
    }

    OGRLayer* layer = dataset->GetLayer(0);
    if (!layer) {
        GDALClose(dataset);
        emit updateLog(tr("错误：图例SHP文件图层获取失败！"));
        return;
    }

    int textConteIndex = layer->GetLayerDefn()->GetFieldIndex("text_conte");
    if (textConteIndex == -1) {
        GDALClose(dataset);
        QString resultMsg = tr("图例SHP文件中未找到\"text_conte\"字段，无法提取比例尺");
        emit updateLog(resultMsg);
        ui->statusLabel->setText(resultMsg);
        return;
    }

    OGRFeature* feature;
    bool hasValidScale = false;
    int errorFeatureId = -1; // 错误要素ID（用于JSON导出）
    layer->ResetReading();
    while ((feature = layer->GetNextFeature()) != nullptr) {
        const char* textValue = feature->GetFieldAsString(textConteIndex);
        QString currentShpScale;
        if (isValidScaleText(QString::fromUtf8(textValue), &currentShpScale)) {
            m_shpScaleValue = currentShpScale;
            hasValidScale = true;
            OGRFeature::DestroyFeature(feature);
            break;
        }
        else {
            errorFeatureId = feature->GetFID(); // 记录无效要素ID
        }
        OGRFeature::DestroyFeature(feature);
    }

    GDALClose(dataset);

    if (!hasValidScale) {
        QString resultMsg = tr("图例SHP文件中未找到有效的比例尺");
        emit updateLog(resultMsg);
        ui->statusLabel->setText(resultMsg);
        return;
    }

    // 对比比例尺
    bool isScaleMatch = (m_shpScaleValue == m_calculatedScale);
    QString resultText = tr(
        "===== 比例尺对比报告 =====\n"
        "生成时间：%1\n\n"
        "1. 数据来源\n"
        "- 图例SHP：%2\n"
        "- 内图廓SHP：%3\n\n"
        "2. 比例尺信息\n"
        "- 图例SHP提取的比例尺：%4\n"
        "- 内图廓计算的比例尺：%5\n\n"
        "3. 对比结果\n"
        "- 图例SHP与内图廓：%6\n\n"
        "4. 结论：%7"
    ).arg(
        QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"),
        m_shpFilePath,
        m_mapFrameShpPath,
        m_shpScaleValue,
        m_calculatedScale,
        isScaleMatch ? tr("一致") : tr("不一致"),
        isScaleMatch ? tr("两者比例尺一致") : tr("两者比例尺存在不一致")
    );

    emit updateLog(resultText);

    // 结果不一致时，自动保存SHP到JSON相同目录
    if (!isScaleMatch) {
        if (!m_jsonOutputPath.isEmpty()) {
            QString targetDir = QFileInfo(m_jsonOutputPath).absolutePath();
            saveShpAs(m_shpFilePath, targetDir);
        }
        else {
            emit updateLog(tr("警告：未选择JSON日志输出路径，无法自动保存SHP文件"));
        }
    }

    ui->statusLabel->setText(tr("检查完成: 图例SHP与内图廓比例尺对比已完成"));
}

//从内图廓SHP获取点信息
bool ScaleCheckerDialog::extractMapFramePointsFromShp(const QString& shpPath, QList<QgsPointXY>& points)
{
    GDALAllRegister();
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        shpPath.toStdString().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (!dataset) {
        emit updateLog(tr("错误：无法打开内图廓SHP文件"));
        return false;
    }

    OGRLayer* layer = dataset->GetLayer(0);
    if (!layer) {
        GDALClose(dataset);
        emit updateLog(tr("错误：内图廓SHP文件图层获取失败"));
        return false;
    }

    OGRwkbGeometryType geomType = layer->GetGeomType();
    if (!(wkbFlatten(geomType) == wkbPolygon || wkbFlatten(geomType) == wkbLineString)) {
        GDALClose(dataset);
        emit updateLog(tr("错误：内图廓SHP几何类型无效，需为Polygon或LineString"));
        return false;
    }

    int widthFieldIdx = layer->GetLayerDefn()->GetFieldIndex("width");
    int heightFieldIdx = layer->GetLayerDefn()->GetFieldIndex("height");
    if (widthFieldIdx == -1 || heightFieldIdx == -1) {
        emit updateLog(tr("错误：内图廓SHP属性表中未找到width或height字段"));
        GDALClose(dataset);
        return false;
    }

    OGRFeature* feature = layer->GetNextFeature();
    if (!feature) {
        GDALClose(dataset);
        emit updateLog(tr("错误：内图廓SHP中无要素"));
        return false;
    }

    m_mapFrameAttrPixelWidth = feature->GetFieldAsDouble(widthFieldIdx);
    m_mapFrameAttrPixelHeight = feature->GetFieldAsDouble(heightFieldIdx);

    if (m_mapFrameAttrPixelWidth <= 0 || m_mapFrameAttrPixelHeight <= 0) {
        emit updateLog(tr("错误：内图廓SHP属性表中width或height值无效（<=0）"));
        OGRFeature::DestroyFeature(feature);
        GDALClose(dataset);
        return false;
    }

    OGRGeometry* geom = feature->GetGeometryRef();
    if (!geom) {
        OGRFeature::DestroyFeature(feature);
        GDALClose(dataset);
        emit updateLog(tr("错误：内图廓SHP要素无几何信息"));
        return false;
    }

    points.clear();
    if (wkbFlatten(geom->getGeometryType()) == wkbPolygon) {
        OGRPolygon* polygon = dynamic_cast<OGRPolygon*>(geom);
        if (polygon && polygon->getExteriorRing()) {
            OGRLinearRing* ring = polygon->getExteriorRing();
            // 创建OGRPoint对象接收坐标
            OGRPoint point;
            for (int i = 0; i < ring->getNumPoints() - 1; ++i) {
                // 调用getPoint：参数为索引和OGRPoint指针
                ring->getPoint(i, &point);
                // 从OGRPoint对象中提取X、Y坐标
                double x = point.getX();
                double y = point.getY();
                points.append(QgsPointXY(x, y));
            }
        }
    }
    else if (wkbFlatten(geom->getGeometryType()) == wkbLineString) {
        OGRLineString* line = dynamic_cast<OGRLineString*>(geom);
        if (line) {
            // 创建OGRPoint对象接收坐标
            OGRPoint point;
            for (int i = 0; i < line->getNumPoints(); ++i) {
                // 调用getPoint：参数为索引和OGRPoint指针
                line->getPoint(i, &point);
                // 从OGRPoint对象中提取X、Y坐标
                double x = point.getX();
                double y = point.getY();
                points.append(QgsPointXY(x, y));
            }
        }
    }

    if (points.size() > 4) {
        QList<QgsPointXY> uniquePoints;
        for (const auto& p : points) {
            bool isDuplicate = false;
            for (const auto& up : uniquePoints) {
                if (qAbs(p.x() - up.x()) < 1e-6 && qAbs(p.y() - up.y()) < 1e-6) {
                    isDuplicate = true;
                    break;
                }
            }
            if (!isDuplicate) {
                uniquePoints.append(p);
                if (uniquePoints.size() == 4) break;
            }
        }
        points = uniquePoints;
    }

    m_mapFrameWidth = calculateMapFrameWidth(points);

    OGRFeature::DestroyFeature(feature);
    GDALClose(dataset);
    return points.size() == 4;
}

//计算内图廓宽度
double ScaleCheckerDialog::calculateMapFrameWidth(const QList<QgsPointXY>& points)
{
    if (points.size() != 4) return 0.0;

    double minX = points[0].x(), maxX = points[0].x();
    for (const auto& p : points) {
        minX = qMin(minX, p.x());
        maxX = qMax(maxX, p.x());
    }

    return maxX - minX;
}

bool ScaleCheckerDialog::isValidScaleText(const QString& text, QString* scaleValue)
{
    // 允许数字间有空格（如“1:145 000”）
    QRegExp scalePattern("^\\s*比例尺?\\s*\\d+[:：]\\s*(\\d+\\s*)+\\s*$");
    if (scalePattern.exactMatch(text)) {
        if (scaleValue) {
            QString cleanedText = text;
            cleanedText = cleanedText.trimmed()
                .remove(QRegExp("\\s+")) // 移除所有空格
                .remove("比例尺")
                .replace("：", ":");
            *scaleValue = cleanedText; // 最终得到“1:145000”
        }
        return true;
    }
    return false;
}

//计算内图廓比例尺数值
QString ScaleCheckerDialog::calculateActualMapScale()
{
    const double EPS = 1e-6;
    const double DPI = 72.0;
    const double METER_PER_INCH = 0.0254;

    if (m_mapFrameWidth <= EPS || m_mapFrameAttrPixelWidth <= EPS) {
        emit updateLog(tr("错误：内图廓宽度或像素宽度无效（<=0）"));
        return "";
    }

    double scaleDenominator = m_mapFrameWidth / (m_mapFrameAttrPixelWidth * (METER_PER_INCH / DPI));

    if (scaleDenominator <= 0) {
        emit updateLog(tr("错误：比例尺计算结果无效（<=0）"));
        return "";
    }

    if (scaleDenominator > 1e12) {
        scaleDenominator = 1e12;
        emit updateLog(tr("警告：比例尺过大，已限制为1:1e12"));
    }
    else if (scaleDenominator < 1) {
        scaleDenominator = 1;
        emit updateLog(tr("警告：比例尺过小，已限制为1:1"));
    }

    return QString("1:%1").arg(qRound(scaleDenominator));
}

void ScaleCheckerDialog::cleanMapFrameData()
{
    m_mapFrameShpPath.clear();
    ui->pdfFilePathEdit->clear();
    m_mapFramePoints.clear();
    m_mapFrameAttrPixelWidth = 0.0;
    m_mapFrameAttrPixelHeight = 0.0;
    m_mapFrameWidth = 0.0;
    ui->statusLabel->setText(tr("内图廓SHP加载失败"));
}

bool ScaleCheckerDialog::exportResultToJson(const QString& result, const QString& shpValue, const QString& xmlValue, const QString& pdfValue)
{
    if (m_jsonOutputPath.isEmpty()) {
        emit updateLog(tr("错误：未选择JSON日志输出路径，无法导出"));
        return false;
    }
    if (m_shpFilePath.isEmpty() || m_mapFrameShpPath.isEmpty() || m_calculatedScale.isEmpty() || m_shpScaleValue.isEmpty()) {
        emit updateLog(tr("错误：缺少必要的SHP数据或计算结果，无法导出JSON"));
        return false;
    }

    // 构建 QJsonObject 结构
    QJsonObject root;    // <--- 修改
    QJsonObject task1;   // <--- 修改

    // 任务基础信息
    task1["layer_name"] = "比例尺";
    task1["status"] = "完成";

    // 时间信息
    QDateTime startTime = QDateTime::currentDateTime();
    QDateTime endTime = QDateTime::currentDateTime();
    QJsonObject timing; // <--- 修改
    timing["start_time"] = startTime.toString("yyyy-MM-dd HH:mm:ss");
    timing["end_time"] = endTime.toString("yyyy-MM-dd HH:mm:ss");
    task1["timing"] = timing;

    // 检查项
    QJsonObject check_item1; // <--- 修改
    check_item1["id"] = errorFeatureId != -1 ? errorFeatureId : 0;
    check_item1["name"] = "比例尺数值";
    check_item1["status"] = "完成";

    // 错误信息
    QJsonObject errors; // <--- 修改
    bool isMatch = (m_shpScaleValue == m_calculatedScale);
    if (!isMatch) {
        QJsonObject error1; // <--- 修改
        error1["feature_id"] = (errorFeatureId != -1)
            ? QString::number(errorFeatureId)
            : QString("N/A"); // <--- 修改 (直接使用 QString)
        QString errorDesc = tr("图例SHP提取的比例尺：%1，内图廓计算的比例尺：%2，存在不一致")
            .arg(m_shpScaleValue, m_calculatedScale);
        error1["description"] = errorDesc; // <--- 修改 (直接使用 QString)
        errors["error1"] = error1;
    }
    check_item1["errors"] = errors;

    // 组装JSON
    task1["check_item1"] = check_item1;
    root["task1"] = task1;

    // 写入文件
    QFile file(m_jsonOutputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit updateLog(tr("错误：无法打开JSON文件进行写入"));
        return false;
    }

    // --- 使用 QJsonDocument 写入 ---
    QJsonDocument doc(root); // <--- 修改
    file.write(doc.toJson(QJsonDocument::Indented)); // <--- 修改
    file.close();

    emit updateLog(tr("JSON结果已成功导出至：%1").arg(m_jsonOutputPath));
    return true;
}

bool ScaleCheckerDialog::saveShpAs(const QString& originalPath, const QString& targetDir)
{
    if (targetDir.isEmpty()) {
        emit updateLog(tr("错误：目标保存目录为空"));
        return false;
    }

    QString fileName = QFileInfo(originalPath).fileName();
    QString newPath = QString("%1/%2").arg(targetDir, fileName);

    if (QFile::exists(newPath)) {
        QFile::remove(newPath);
    }

    QString baseName = originalPath.left(originalPath.lastIndexOf('.'));
    QDir dir(QFileInfo(originalPath).path());
    QStringList filters;
    filters << QString("%1.*").arg(QFileInfo(originalPath).baseName());
    QStringList files = dir.entryList(filters);

    foreach(QString file, files) {
        QString src = QString("%1/%2").arg(dir.path(), file);
        QString dst = QString("%1/%2").arg(targetDir, file);
        if (!QFile::copy(src, dst)) {
            emit updateLog(tr("错误：无法复制文件: %1").arg(file));
            return false;
        }
    }

    emit updateLog(tr("SHP文件已保存至:\n%1").arg(targetDir));
    return true;
}