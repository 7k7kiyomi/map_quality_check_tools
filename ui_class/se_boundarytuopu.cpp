#include "se_boundarytuopu.h"
#include "ui_se_boundarytuopu.h"

#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>

#include <cmath>
#include <utility>

#include "cpl_string.h"

namespace
{
const double kNodeTolerance = 1e-8;

BoundaryTopologyDatasetPtr openVectorDataset(const QString& path,
    int flags = GDAL_OF_VECTOR | GDAL_OF_READONLY)
{
    return BoundaryTopologyDatasetPtr(static_cast<GDALDataset*>(
        GDALOpenEx(path.toUtf8().constData(), flags, nullptr, nullptr, nullptr)));
}

bool envelopesIntersect(const OGREnvelope& a, const OGREnvelope& b)
{
    return a.MinX <= b.MaxX && a.MaxX >= b.MinX
        && a.MinY <= b.MaxY && a.MaxY >= b.MinY;
}

bool isLineGeometry(const OGRGeometry* geometry)
{
    if (geometry == nullptr)
        return false;

    const OGRwkbGeometryType type = wkbFlatten(geometry->getGeometryType());
    if (type == wkbLineString || type == wkbMultiLineString)
        return true;

    const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
    if (collection == nullptr)
        return false;

    for (int index = 0; index < collection->getNumGeometries(); ++index) {
        if (isLineGeometry(collection->getGeometryRef(index)))
            return true;
    }
    return false;
}

void appendIntersectionPoints(const OGRGeometry* geometry,
    std::vector<OGRPoint>& points)
{
    if (geometry == nullptr || geometry->IsEmpty())
        return;

    if (const auto* point = dynamic_cast<const OGRPoint*>(geometry)) {
        for (const OGRPoint& existing : points) {
            if (std::hypot(existing.getX() - point->getX(),
                    existing.getY() - point->getY()) <= kNodeTolerance)
                return;
        }
        points.push_back(*point);
        return;
    }

    const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
    if (collection == nullptr)
        return; // A line overlap is not a point intersection for this check.

    for (int index = 0; index < collection->getNumGeometries(); ++index)
        appendIntersectionPoints(collection->getGeometryRef(index), points);
}

bool hasVertexAt(const OGRGeometry* geometry, const OGRPoint& point)
{
    if (geometry == nullptr)
        return false;

    if (const auto* line = dynamic_cast<const OGRSimpleCurve*>(geometry)) {
        for (int index = 0; index < line->getNumPoints(); ++index) {
            OGRPoint vertex;
            line->getPoint(index, &vertex);
            if (std::hypot(vertex.getX() - point.getX(),
                    vertex.getY() - point.getY()) <= kNodeTolerance)
                return true;
        }
        return false;
    }

    const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
    if (collection == nullptr)
        return false;

    for (int index = 0; index < collection->getNumGeometries(); ++index) {
        if (hasVertexAt(collection->getGeometryRef(index), point))
            return true;
    }
    return false;
}

bool createField(OGRLayer* layer, const char* name, OGRFieldType type,
    int width = 0, int precision = 0)
{
    OGRFieldDefn field(name, type);
    if (width > 0)
        field.SetWidth(width);
    if (precision > 0)
        field.SetPrecision(precision);
    return layer->CreateField(&field) == OGRERR_NONE;
}

bool writeUtf8Cpg(const QString& shpPath, QString& error)
{
    const QFileInfo shpInfo(shpPath);
    const QString cpgPath = shpInfo.absolutePath() + "/"
        + shpInfo.completeBaseName() + ".cpg";
    QFile cpgFile(cpgPath);
    if (!cpgFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        error = QStringLiteral("无法写入 Shapefile 编码声明文件：%1").arg(cpgPath);
        return false;
    }

    // 65001 is the Windows code-page identifier for UTF-8 and is recognized
    // by both ArcGIS desktop and GDAL/QGIS readers.
    if (cpgFile.write("65001\n") != 6) {
        error = QStringLiteral("无法写入 Shapefile UTF-8 编码声明：%1").arg(cpgPath);
        return false;
    }
    return true;
}

std::unique_ptr<OGRSpatialReference> readSpatialReference(const QStringList& paths)
{
    for (const QString& path : paths) {
        BoundaryTopologyDatasetPtr dataset = openVectorDataset(path);
        OGRLayer* layer = dataset && dataset->GetLayerCount() > 0
            ? dataset->GetLayer(0) : nullptr;
        if (layer != nullptr && layer->GetSpatialRef() != nullptr)
            return std::unique_ptr<OGRSpatialReference>(layer->GetSpatialRef()->Clone());
    }
    return std::unique_ptr<OGRSpatialReference>();
}
}

se_boundarytuopu::se_boundarytuopu(const QString& shpPath, const QString& gdbShpPath, QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
    , ui(new Ui::se_boundarytuopu)
    , m_boundaryPaths(shpPath)
{
    ui->setupUi(this);
    GDALAllRegister();
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    CPLSetConfigOption("SHAPE_ENCODING", "");

    setWindowTitle(QStringLiteral("境界线交点实拓扑自动检查"));
    ui->lineEdit_filepath->setReadOnly(true);
    ui->textEdit_log->setReadOnly(true);
    appendLog(QStringLiteral("境界线交点实拓扑检查模块已初始化。"));
}

se_boundarytuopu::~se_boundarytuopu()
{
    delete ui;
}

void se_boundarytuopu::on_pushButton_boundary_clicked()
{
    const QStringList selected = QFileDialog::getOpenFileNames(this,
        QStringLiteral("选择境界线数据（可多选）"), QString(),
        QStringLiteral("SHP 文件 (*.shp);;所有文件 (*.*)"));
    if (selected.isEmpty())
        return;

    m_boundaryPaths = normalizeInputPaths(selected);
    if (m_boundaryPaths.isEmpty())
        return;

    ui->lineEdit_filepath->setText(m_boundaryPaths.join(QStringLiteral(";")));
    resetCheckState(QStringLiteral("已加载 %1 个境界线 SHP 文件。")
        .arg(m_boundaryPaths.size()));
}

void se_boundarytuopu::on_pushButton_start_clicked()
{
    m_boundaryPaths = normalizeInputPaths(
        ui->lineEdit_filepath->text().split(';', QString::SkipEmptyParts));
    if (m_boundaryPaths.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("请先加载一个或多个境界线 SHP 文件。"));
        return;
    }

    appendLog(QStringLiteral("开始检查 %1 个境界线数据文件。")
        .arg(m_boundaryPaths.size()));
    int problemCount = 0;
    QString message;
    if (!runCheck(QString(), false, problemCount, message)) {
        m_lastCheckFinished = false;
        appendLog(QStringLiteral("检查失败：") + message);
        QMessageBox::critical(this, QStringLiteral("检查失败"), message);
        return;
    }

    m_lastCheckedPaths = m_boundaryPaths;
    m_lastCheckFinished = true;
    m_lastProblemCount = problemCount;
    appendLog(message);
    QMessageBox::information(this, QStringLiteral("检查完成"), message);
}

void se_boundarytuopu::on_pushButton_shuchu_clicked()
{
    m_boundaryPaths = normalizeInputPaths(
        ui->lineEdit_filepath->text().split(';', QString::SkipEmptyParts));
    if (m_boundaryPaths.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("请先加载一个或多个境界线 SHP 文件。"));
        return;
    }

    if (!m_lastCheckFinished || m_lastCheckedPaths != m_boundaryPaths) {
        appendLog(QStringLiteral("当前数据尚未检查，输出前先执行检查。"));
        int count = 0;
        QString checkMessage;
        if (!runCheck(QString(), false, count, checkMessage)) {
            m_lastCheckFinished = false;
            appendLog(QStringLiteral("检查失败：") + checkMessage);
            QMessageBox::critical(this, QStringLiteral("检查失败"), checkMessage);
            return;
        }
        m_lastCheckedPaths = m_boundaryPaths;
        m_lastCheckFinished = true;
        m_lastProblemCount = count;
        appendLog(checkMessage);
    }

    if (m_lastProblemCount == 0) {
        const QString message = QStringLiteral("未发现非实拓扑交点，无需输出错误图层。");
        appendLog(message);
        QMessageBox::information(this, QStringLiteral("提示"), message);
        return;
    }

    const QString defaultPath = QFileInfo(m_boundaryPaths.first()).absolutePath()
        + QStringLiteral("/boundary_real_topology_error.shp");
    QString outputPath = QFileDialog::getSaveFileName(this,
        QStringLiteral("保存境界线实拓扑错误图层"), defaultPath,
        QStringLiteral("SHP 文件 (*.shp)"));
    if (outputPath.isEmpty())
        return;
    if (QFileInfo(outputPath).suffix().compare(QStringLiteral("shp"), Qt::CaseInsensitive) != 0)
        outputPath += QStringLiteral(".shp");

    int outputCount = 0;
    QString message;
    if (!runCheck(outputPath, true, outputCount, message)) {
        appendLog(QStringLiteral("输出失败：") + message);
        QMessageBox::critical(this, QStringLiteral("输出失败"), message);
        return;
    }

    m_lastProblemCount = outputCount;
    appendLog(message);
    QMessageBox::information(this, QStringLiteral("输出完成"), message);
}

void se_boundarytuopu::appendLog(const QString& message)
{
    ui->textEdit_log->append(QDateTime::currentDateTime().toString(
        QStringLiteral("[yyyy-MM-dd HH:mm:ss] ")) + message);
    QApplication::processEvents();
}

void se_boundarytuopu::resetCheckState(const QString& reloadMessage)
{
    ui->textEdit_log->clear();
    m_lastCheckedPaths.clear();
    m_lastCheckFinished = false;
    m_lastProblemCount = 0;
    appendLog(reloadMessage);
}

bool se_boundarytuopu::runCheck(const QString& outputShpPath, bool writeOutput,
    int& problemCount, QString& message)
{
    std::vector<BoundaryTopologyIssue> issues;
    QString error;
    if (!collectTopologyIssues(issues, error)) {
        message = error;
        return false;
    }

    problemCount = static_cast<int>(issues.size());
    if (writeOutput && !writeProblemLayer(outputShpPath, issues, error)) {
        message = error;
        return false;
    }

    message = writeOutput
        ? QStringLiteral("检查完成：发现 %1 个非实拓扑交点，错误图层已输出至：%2")
            .arg(problemCount).arg(outputShpPath)
        : QStringLiteral("检查完成：发现 %1 个非实拓扑交点。")
            .arg(problemCount);
    return true;
}

bool se_boundarytuopu::collectTopologyIssues(
    std::vector<BoundaryTopologyIssue>& issues, QString& error)
{
    std::vector<BoundaryTopologyLine> lines;
    std::unique_ptr<OGRSpatialReference> referenceSrs;

    for (const QString& path : m_boundaryPaths) {
        BoundaryTopologyDatasetPtr dataset = openVectorDataset(path);
        if (!dataset || dataset->GetLayerCount() <= 0) {
            error = QStringLiteral("无法打开境界线数据：%1").arg(path);
            return false;
        }

        OGRLayer* layer = dataset->GetLayer(0);
        if (layer == nullptr) {
            error = QStringLiteral("无法读取境界线图层：%1").arg(path);
            return false;
        }
        OGRSpatialReference* currentSrs = layer->GetSpatialRef();
        if (referenceSrs && currentSrs && !referenceSrs->IsSame(currentSrs)) {
            error = QStringLiteral("输入数据空间参考不一致，无法直接进行相交分析：%1").arg(path);
            return false;
        }
        if (!referenceSrs && currentSrs)
            referenceSrs.reset(currentSrs->Clone());

        const QString layerName = QFileInfo(path).completeBaseName();
        layer->ResetReading();
        OGRFeature* rawFeature = nullptr;
        while ((rawFeature = layer->GetNextFeature()) != nullptr) {
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                feature(rawFeature, OGRFeature::DestroyFeature);
            OGRGeometry* geometry = feature->GetGeometryRef();
            if (!isLineGeometry(geometry))
                continue;

            BoundaryTopologyLine line;
            line.layerName = layerName;
            line.fid = feature->GetFID();
            line.geometry.reset(geometry->clone());
            line.geometry->getEnvelope(&line.envelope);
            lines.push_back(std::move(line));
        }
    }

    if (lines.size() < 2) {
        error = QStringLiteral("至少需要读取到两条有效的线状境界线要素。请确认输入数据为线 Shapefile。");
        return false;
    }

    int candidatePairs = 0;
    for (size_t first = 0; first < lines.size(); ++first) {
        for (size_t second = first + 1; second < lines.size(); ++second) {
            if (!envelopesIntersect(lines[first].envelope, lines[second].envelope))
                continue;
            ++candidatePairs;

            BoundaryTopologyGeometryPtr intersection(
                lines[first].geometry->Intersection(lines[second].geometry.get()));
            if (!intersection || intersection->IsEmpty())
                continue;

            std::vector<OGRPoint> points;
            appendIntersectionPoints(intersection.get(), points);
            for (const OGRPoint& point : points) {
                const bool firstHasNode = hasVertexAt(lines[first].geometry.get(), point);
                const bool secondHasNode = hasVertexAt(lines[second].geometry.get(), point);
                if (firstHasNode && secondHasNode)
                    continue;

                BoundaryTopologyIssue issue;
                issue.x = point.getX();
                issue.y = point.getY();
                issue.layerA = lines[first].layerName;
                issue.fidA = lines[first].fid;
                issue.layerB = lines[second].layerName;
                issue.fidB = lines[second].fid;
                if (!firstHasNode && !secondHasNode) {
                    issue.type = QStringLiteral("双方未节点化");
                    issue.message = QStringLiteral("交点位于两条境界线内部，双方均不存在对应节点。");
                } else if (!firstHasNode) {
                    issue.type = QStringLiteral("境界线 A 未节点化");
                    issue.message = QStringLiteral("交点位于境界线 A 内部，境界线 B 存在对应节点。");
                } else {
                    issue.type = QStringLiteral("境界线 B 未节点化");
                    issue.message = QStringLiteral("交点位于境界线 B 内部，境界线 A 存在对应节点。");
                }
                issues.push_back(issue);
            }
        }
    }

    appendLog(QStringLiteral("读取有效境界线要素 %1 条，完成 %2 对候选线要素的相交分析。")
        .arg(lines.size()).arg(candidatePairs));
    return true;
}

bool se_boundarytuopu::writeProblemLayer(const QString& outputShpPath,
    const std::vector<BoundaryTopologyIssue>& issues, QString& error) const
{
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (driver == nullptr) {
        error = QStringLiteral("当前 GDAL 环境不支持 ESRI Shapefile 驱动。");
        return false;
    }

    const QByteArray outputPath = toGdalPath(outputShpPath);
    if (QFileInfo::exists(outputShpPath)
        && driver->Delete(outputPath.constData()) != CE_None) {
        error = QStringLiteral("无法覆盖已有错误图层：%1").arg(outputShpPath);
        return false;
    }

    BoundaryTopologyDatasetPtr outputDataset(driver->Create(
        outputPath.constData(), 0, 0, 0, GDT_Unknown, nullptr));
    if (!outputDataset) {
        error = QStringLiteral("无法创建错误图层：%1").arg(outputShpPath);
        return false;
    }

    std::unique_ptr<OGRSpatialReference> spatialReference =
        readSpatialReference(m_boundaryPaths);
    char** layerOptions = nullptr;
    layerOptions = CSLSetNameValue(layerOptions, "ENCODING", "UTF-8");
    OGRLayer* outputLayer = outputDataset->CreateLayer("topology_error",
        spatialReference.get(), wkbPoint, layerOptions);
    CSLDestroy(layerOptions);
    if (outputLayer == nullptr) {
        error = QStringLiteral("无法创建错误点图层。");
        return false;
    }

    if (!createField(outputLayer, "ERR_TYPE", OFTString, 40)
        || !createField(outputLayer, "X_COORD", OFTReal, 24, 8)
        || !createField(outputLayer, "Y_COORD", OFTReal, 24, 8)
        || !createField(outputLayer, "LAYER_A", OFTString, 80)
        || !createField(outputLayer, "FID_A", OFTInteger64)
        || !createField(outputLayer, "LAYER_B", OFTString, 80)
        || !createField(outputLayer, "FID_B", OFTInteger64)
        || !createField(outputLayer, "MESSAGE", OFTString, 160)) {
        error = QStringLiteral("无法创建错误图层属性字段。");
        return false;
    }

    OGRFeatureDefn* definition = outputLayer->GetLayerDefn();
    for (const BoundaryTopologyIssue& issue : issues) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            OGRFeature::CreateFeature(definition), OGRFeature::DestroyFeature);
        OGRPoint point(issue.x, issue.y);
        feature->SetGeometry(&point);
        feature->SetField("ERR_TYPE", issue.type.toUtf8().constData());
        feature->SetField("X_COORD", issue.x);
        feature->SetField("Y_COORD", issue.y);
        feature->SetField("LAYER_A", issue.layerA.toUtf8().constData());
        feature->SetField("FID_A", issue.fidA);
        feature->SetField("LAYER_B", issue.layerB.toUtf8().constData());
        feature->SetField("FID_B", issue.fidB);
        feature->SetField("MESSAGE", issue.message.toUtf8().constData());
        if (outputLayer->CreateFeature(feature.get()) != OGRERR_NONE) {
            error = QStringLiteral("写入错误图层要素失败。");
            return false;
        }
    }
    // Close the dataset first, then create a .cpg sidecar for applications
    // that do not honor the GDAL layer-creation encoding option.
    outputDataset.reset();
    return writeUtf8Cpg(outputShpPath, error);
}

QStringList se_boundarytuopu::normalizeInputPaths(const QStringList& paths)
{
    QStringList result;
    for (const QString& rawPath : paths) {
        const QString path = rawPath.trimmed();
        if (path.isEmpty())
            continue;
        if (QFileInfo(path).suffix().compare(QStringLiteral("shp"), Qt::CaseInsensitive) != 0)
            continue;
        if (!result.contains(path, Qt::CaseInsensitive))
            result.append(path);
    }
    return result;
}

QByteArray se_boundarytuopu::toGdalPath(const QString& path)
{
    return QFileInfo(path).absoluteFilePath().toUtf8();
}
