#include "ScaleCheckerNormativeDialog.h"
#include "ui_ScaleCheckerNormativeDialog.h"
#include <QRegExp>
#include <QXmlStreamReader>
#include <QTextStream>
#include <QFileDialog>
#include <QMessageBox>
#include <QDateTime>
#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgisinterface.h>
#include <qgsdistancearea.h>
#include <qgsunittypes.h>
#include "gdal_priv.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include <qdesktopwidget.h>
#include <QScreen>  // 添加这个头文件

ScaleCheckerNormativeDialog::ScaleCheckerNormativeDialog(QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
    , ui(new Ui::ScaleCheckerNormativeDialogClass)
    , mIface(nullptr) // 初始化QGIS接口指针
{
    ui->setupUi(this);
}

ScaleCheckerNormativeDialog::~ScaleCheckerNormativeDialog()
{
    delete ui;
}

// 设置QGIS接口（用于后续地图画布操作）
void ScaleCheckerNormativeDialog::setQgisInterface(QgisInterface* iface)
{
   mIface = iface;
}

//加载shp文件
void ScaleCheckerNormativeDialog::on_loadGeoPdfButton_clicked()
{
    // 重置状态
    m_geoPdfPath.clear();
    m_pdfCrs = QgsCoordinateReferenceSystem();
    m_pdfGeoExtent = QgsRectangle();
    m_pdfPageSize = QSize();

    // 选择文件
    QString filePath = QFileDialog::getOpenFileName(
        this,
        tr("选择GeoPDF文件"),
        "",
        tr("GeoPDF文件 (*.pdf);;所有文件 (*)")
    );
    if (filePath.isEmpty())
    {
        ui->statusLabel->setText(tr("已取消选择GeoPDF文件"));
        return;
    }

    // 显示文件路径
    ui->geoPdfFilePathEdit->setText(filePath);

    // 解析GeoPDF信息
    if (readGeoPdfInfo(filePath))
    {
        m_geoPdfPath = filePath;

        // 显示坐标系信息（优先用authid，否则用描述）
        QString crsInfo;
        if (!m_pdfCrs.authid().isEmpty())
            crsInfo = m_pdfCrs.authid(); // 如EPSG:4326
        else if (!m_pdfCrs.description().isEmpty())
            crsInfo = m_pdfCrs.description(); // 自定义坐标系描述
        else
            crsInfo = tr("未知坐标系（但包含地理信息）");

        ui->statusLabel->setText(tr("GeoPDF加载成功！坐标系：%1").arg(crsInfo));
        QMessageBox::information(this, tr("成功"),
            tr("GeoPDF信息：\n文件路径：%1\n地理范围：%2\n页面尺寸：%3×%4像素\n坐标系：%5")
            .arg(filePath)
            .arg(m_pdfGeoExtent.toString())
            .arg(m_pdfPageSize.width())
            .arg(m_pdfPageSize.height())
            .arg(crsInfo)); // 显示坐标系信息
    }
    else
    {
        QMessageBox::critical(this, tr("错误"), tr("无法解析该文件为GeoPDF！请确认文件包含地理信息。"));
        ui->statusLabel->setText(tr("GeoPDF加载失败"));
        ui->geoPdfFilePathEdit->clear();
    }
}


void ScaleCheckerNormativeDialog::on_loadXmlButton_clicked()
{
    // 1. 选择XML文件
    QString filePath = QFileDialog::getOpenFileName(
        this,
        tr("选择XML文件"),
        "",
        tr("XML文件 (*.xml);;所有文件 (*)")
    );
    if (filePath.isEmpty())
    {
        ui->statusLabel->setText(tr("XML文件选择已取消，请重新加载"));
        return;
    }

    // 2. 读取XML中的比例尺（value节点内容）
    QString rawScale;
    if (readScaleFromXml(filePath, rawScale))
    {
        // 3. 校验比例尺格式（必须是“1:N”或“比例尺 1:N”格式）
        if (isValidScaleFormat(rawScale, m_xmlTargetScale))
        {
            m_xmlFilePath = filePath;
            ui->xmlFilePathEdit->setText(m_xmlFilePath);
            ui->statusLabel->setText(tr("XML加载成功！目标比例尺：%1").arg(m_xmlTargetScale));
            QMessageBox::information(this, tr("成功"), tr("XML中提取的目标比例尺：%1").arg(m_xmlTargetScale));
        }
        else
        {
            QMessageBox::warning(this, tr("格式错误"), tr("XML中value节点内容无效！仅支持\"1:N\"或\"比例尺 1:N\"格式（如1:10000）"));
            ui->statusLabel->setText(tr("XML比例尺格式无效，请重新选择"));
        }
    }
    else
    {
        QMessageBox::warning(this, tr("读取失败"), tr("XML文件中未找到value节点，或文件无法打开！"));
        ui->statusLabel->setText(tr("XML读取失败，请重新选择"));
    }
}

void ScaleCheckerNormativeDialog::on_checkButton_clicked()
{
    // 前置检查
    if (m_geoPdfPath.isEmpty() || !m_pdfCrs.isValid() || m_pdfGeoExtent.isEmpty())
    {
        QMessageBox::warning(this, tr("错误"), tr("请先加载有效的GeoPDF文件！"));
        return;
    }
    if (m_xmlFilePath.isEmpty() || m_xmlTargetScale.isEmpty())
    {
        QMessageBox::warning(this, tr("错误"), tr("请先加载有效的XML文件！"));
        return;
    }

    ui->statusLabel->setText(tr("正在计算实际比例尺..."));
    qApp->processEvents();

    // 计算实际比例尺
    m_calculatedScale = calculateActualMapScale();
    if (m_calculatedScale.isEmpty())
    {
        ui->statusLabel->setText(tr("计算失败"));
        return;
    }

    // 对比结果
    bool isMatch = (m_calculatedScale == m_xmlTargetScale);
    QString resultText = tr(
        "===== 比例尺对比报告 =====\n"
        "生成时间：%1\n\n"
        "1. 数据来源\n"
        "- GeoPDF：%2\n"
        "- XML：%3\n\n"
        "2. 比例尺信息\n"
        "- 目标比例尺（XML）：%4\n"
        "- 实际比例尺（计算）：%5\n\n"
        "3. 结论：%6"
    ).arg(
        QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"),
        m_geoPdfPath,
        m_xmlFilePath,
        m_xmlTargetScale,
        m_calculatedScale,
        isMatch ? tr("一致") : tr("不一致")
    );

    // 显示结果
    QMessageBox::information(this, tr("检查完成"), resultText);

    // 导出结果
    if (exportResultToTxt(resultText, m_calculatedScale, m_xmlTargetScale))
    {
        ui->statusLabel->setText(tr("结果已导出，实际比例尺：%1").arg(m_calculatedScale));
    }
    else
    {
        ui->statusLabel->setText(tr("检查完成，实际比例尺：%1").arg(m_calculatedScale));
    }
}
// 解析GeoPDF的核心信息
bool ScaleCheckerNormativeDialog::readGeoPdfInfo(const QString& pdfPath)
{
    // 初始化GDAL
    GDALAllRegister();

    // 打开PDF文件
    GDALDataset* poDataset = (GDALDataset*)GDALOpenEx(
        pdfPath.toUtf8().constData(),
        GDAL_OF_RASTER | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr
    );

    if (poDataset == nullptr)
    {
        QMessageBox::critical(this, tr("错误"), tr("GDAL无法打开该PDF文件！"));
        return false;
    }

    // 获取地理范围
    double adfGeoTransform[6];
    if (poDataset->GetGeoTransform(adfGeoTransform) != CE_None)
    {
        GDALClose(poDataset);
        return false;
    }

    int nXSize = poDataset->GetRasterXSize();
    int nYSize = poDataset->GetRasterYSize();

    // 计算地理范围
    double minX = adfGeoTransform[0];
    double maxY = adfGeoTransform[3];
    double maxX = adfGeoTransform[0] + adfGeoTransform[1] * nXSize;
    double minY = adfGeoTransform[3] + adfGeoTransform[5] * nYSize;

    m_pdfGeoExtent = QgsRectangle(minX, minY, maxX, maxY);

    // 获取坐标系
    const char* pszProjection = poDataset->GetProjectionRef();
    if (pszProjection == nullptr || *pszProjection == '\0')
    {
        GDALClose(poDataset);
        return false;
    }
    m_pdfCrs = QgsCoordinateReferenceSystem(QString(pszProjection));

    // 获取页面尺寸
    m_pdfPageSize = QSize(nXSize, nYSize);

    // 关闭数据集
    GDALClose(poDataset);

    return m_pdfCrs.isValid() && !m_pdfGeoExtent.isEmpty() &&
        m_pdfPageSize.width() > 0 && m_pdfPageSize.height() > 0;
}


QString ScaleCheckerNormativeDialog::calculateActualMapScale()
{
    // 1. 基础校验（依赖解析的GeoPDF信息）
    if (!mIface || !mIface->mapCanvas())
    {
        QMessageBox::critical(this, tr("错误"), tr("无法获取地图画布！"));
        return "";
    }

    // 校验解析的核心数据是否有效（必须在readGeoPdfInfo成功后调用）
    if (!m_pdfCrs.isValid())
    {
        QMessageBox::critical(this, tr("错误"), tr("GeoPDF坐标系无效！"));
        return "";
    }
    if (m_pdfPageSize.width() <= 0 || m_pdfPageSize.height() <= 0)
    {
        QMessageBox::critical(this, tr("错误"), tr("GeoPDF页面尺寸无效！"));
        return "";
    }

    // 2. 地理范围有效性校验（基于GDAL解析的范围）
    if (m_pdfGeoExtent.xMinimum() >= m_pdfGeoExtent.xMaximum() ||
        m_pdfGeoExtent.yMinimum() >= m_pdfGeoExtent.yMaximum())
    {
        // 尝试交换坐标（GDAL可能返回min > max的情况）
        double xMin = qMin(m_pdfGeoExtent.xMinimum(), m_pdfGeoExtent.xMaximum());
        double xMax = qMax(m_pdfGeoExtent.xMinimum(), m_pdfGeoExtent.xMaximum());
        double yMin = qMin(m_pdfGeoExtent.yMinimum(), m_pdfGeoExtent.yMaximum());
        double yMax = qMax(m_pdfGeoExtent.yMinimum(), m_pdfGeoExtent.yMaximum());
        m_pdfGeoExtent = QgsRectangle(xMin, yMin, xMax, yMax);

        // 交换后仍无效则报错
        if (m_pdfGeoExtent.xMinimum() >= m_pdfGeoExtent.xMaximum() ||
            m_pdfGeoExtent.yMinimum() >= m_pdfGeoExtent.yMaximum())
        {
            QMessageBox::critical(this, tr("错误"), tr("地理范围无效，无法计算比例尺！"));
            return "";
        }
    }

    // 3. 计算实际地理距离（仅使用解析出的坐标差值）
    const double EPS = 1e-6; // 放宽阈值，适应解析精度
    double geographicDistance = 0.0;

    // 优先使用水平距离
    geographicDistance = m_pdfGeoExtent.xMaximum() - m_pdfGeoExtent.xMinimum();

    // 若水平距离过小，尝试垂直距离
    if (geographicDistance <= EPS)
    {
        geographicDistance = m_pdfGeoExtent.height();

        // 若仍过小，尝试对角线距离
        if (geographicDistance <= EPS)
        {
            double dx = m_pdfGeoExtent.width();
            double dy = m_pdfGeoExtent.height();
            geographicDistance = sqrt(dx * dx + dy * dy);
        }
    }

    // 4. 单位转换（确保结果为米）
    if (m_pdfCrs.mapUnits() != QgsUnitTypes::DistanceMeters)
    {
        QgsDistanceArea converter;
        converter.setSourceCrs(m_pdfCrs, QgsProject::instance()->transformContext());
        geographicDistance = converter.convertLengthMeasurement(geographicDistance, QgsUnitTypes::DistanceMeters);
    }

    // 校验距离有效性
    if (geographicDistance <= EPS)
    {
        QMessageBox::critical(this, tr("错误"), tr("实际地理距离计算无效（<=0）！请检查GeoPDF地理信息。"));
        return "";
    }

    // 5. 计算像素物理长度（基于页面像素宽度和DPI）
    double dpi = 96.0;
    QWidget* mainWindow = QApplication::activeWindow();
    if (mainWindow)
    {
        QScreen* screen = QGuiApplication::screenAt(mainWindow->geometry().center());
        if (screen)
            dpi = screen->physicalDotsPerInchX(); // 优先使用物理DPI，更准确
    }
    if (dpi <= 0) dpi = 96.0;

    double meterPerPixel = 0.0254 / dpi; // 1英寸=0.0254米
    double pixelTotalLength = m_pdfPageSize.width() * meterPerPixel;

    if (pixelTotalLength <= EPS)
    {
        QMessageBox::critical(this, tr("错误"), tr("页面物理长度计算无效！"));
        return "";
    }

    // 6. 计算比例尺并处理异常
    double scaleDenominator = geographicDistance / pixelTotalLength;

    if (scaleDenominator <= 0)
    {
        QMessageBox::critical(this, tr("错误"), tr("比例尺计算错误，结果为负数或零！"));
        return "";
    }

    // 限制极值
    if (scaleDenominator > 1e12)
    {
        if (geographicDistance < 1e-3) // 小于1毫米
        {
            QMessageBox::critical(this, tr("错误"), tr("地理范围过小，无法计算有效比例尺！"));
            return "";
        }
        else
        {
            scaleDenominator = 1e12;
            QMessageBox::warning(this, tr("警告"), tr("比例尺过大，已限制为1:1e12"));
        }
    }
    else if (scaleDenominator < 1) // 比例尺过小（1:1以下）
    {
        scaleDenominator = 1;
        QMessageBox::warning(this, tr("警告"), tr("比例尺过小，已限制为1:1"));
    }

    // 四舍五入为整数
    scaleDenominator = qRound(scaleDenominator);
    return QString("1:%1").arg(scaleDenominator);
}



// 核心功能3：校验比例尺格式（支持“1:N”或“比例尺 1:N”，清理格式为标准“1:N”）
bool ScaleCheckerNormativeDialog::isValidScaleFormat(const QString& rawText, QString& cleanedScale)
{
    // 采用与isValidScaleText一致的正则模式，同时确保是"1:N"格式
    // 支持带前缀、带空格的格式，如"比例尺 1:145 000"、"1:10000"、"比例尺1:20 000"
    QRegExp scalePattern("^(比例尺\\s+)?1[:：]\\s*(\\d+\\s*)*\\d+$");

    if (!scalePattern.exactMatch(rawText))
    {
        return false;
    }

    // 清理格式
    QString cleaned = rawText;
    cleaned.remove("比例尺").remove(" ");
    cleaned.replace("：", ":");

    QStringList parts = cleaned.split(":");
    if (parts.size() != 2 || parts[0] != "1")
    {
        return false;
    }

    bool isNumber;
    qlonglong denominator = parts[1].toLongLong(&isNumber);
    if (!isNumber || denominator <= 0)
    {
        return false;
    }

    cleanedScale = QString("%1:%2").arg(parts[0], parts[1]);
    return true;
}

// 核心功能2：从XML的value节点读取比例尺原始值
bool ScaleCheckerNormativeDialog::readScaleFromXml(const QString& xmlPath, QString& rawScale)
{
    QFile xmlFile(xmlPath);
    // 1. 尝试打开XML文件
    if (!xmlFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return false;
    }

    // 2. 解析XML，查找value节点
    QXmlStreamReader reader(&xmlFile);
    while (!reader.atEnd())
    {
        if (reader.hasError())
        {
            xmlFile.close();
            return false;
        }

        // 找到value开始节点，读取其文本内容
        if (reader.tokenType() == QXmlStreamReader::StartElement && reader.name() == "value")
        {
            rawScale = reader.readElementText().trimmed();
            xmlFile.close();
            return true;
        }

        reader.readNext(); // 继续遍历下一个节点
    }

    // 遍历结束未找到value节点
    xmlFile.close();
    return false;
}

// 导出质检报告到TXT（用户自定义路径）
bool ScaleCheckerNormativeDialog::exportResultToTxt(const QString& result, const QString& actualScale, const QString& targetScale)
{
    // 生成默认文件名（含时间戳，避免重复）
    QString defaultFileName = QString("比例尺对比结果_%1.txt")
        .arg(QDateTime::currentDateTime().toString("yyyyMMddHHmmss"));

    // 选择保存路径
    QString savePath = QFileDialog::getSaveFileName(
        this,
        tr("保存对比结果"),
        defaultFileName,
        tr("文本文件 (*.txt);;所有文件 (*)")
    );
    if (savePath.isEmpty())
    {
        return false; // 用户取消保存
    }

    // 写入文件
    QFile resultFile(savePath);
    if (!resultFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return false; // 文件打开失败
    }

    QTextStream out(&resultFile);
    out << tr("地图比例尺对比报告\n");
    out << tr("生成时间：%1\n\n").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    out << result << "\n";
    out << tr("\n备注：实际比例尺基于当前地图画布范围计算，若调整画布大小需重新计算。");

    resultFile.close();
    return true;
}

// 高亮函数无需修改，保持原逻辑（仅接收m_invalidFeatureIds中的ID，即参与提取的要素ID）
//void ScaleCheckerNormativeDialog::highlightInvalidFeatures(const QgsVectorLayer* layer, const QList<QgsFeatureId>& invalidFeatureIds)
//{
    // 前置检查增强
    //if (!layer)
    //{
        //QMessageBox::warning(nullptr, tr("错误"), tr("图层无效，无法高亮要素！"));
        //return;
   // }
    //if (invalidFeatureIds.isEmpty())
    //{
       // QMessageBox::warning(nullptr, tr("错误"), tr("没有需要高亮的不规范要素ID！"));
        //return;
    //}
    //if (!mIface || !mIface->mapCanvas())
    //{
       // QMessageBox::warning(nullptr, tr("错误"), tr("地图画布无效，无法高亮要素！"));
        //return;
   // }

    // 清理旧的高亮
    //if (m_highlight) {
       // delete m_highlight;
       // m_highlight = nullptr;
    //}

    // 构造要素请求（仅过滤参与提取的要素ID）
    //QgsFeatureRequest request;
    //request.setFilterFids(QgsFeatureIds(invalidFeatureIds.begin(), invalidFeatureIds.end()));

    // 遍历要素，创建高亮（仅处理参与提取的要素）
    //QgsFeature feature;
    //QgsFeatureIterator it = layer->getFeatures(request);
    //while (it.nextFeature(feature)) {
        //if (!feature.hasGeometry() || feature.geometry().isEmpty())
            //continue;

        //m_highlight = new QgsHighlight(
            //mIface->mapCanvas(),
            //feature.geometry(),
            //const_cast<QgsVectorLayer*>(layer)
        //);
        //m_highlight->setColor(Qt::red);
        //m_highlight->setFillColor(QColor(255, 0, 0, 100));
        //m_highlight->show();
    //}

    // 缩放到参与提取的要素范围
   // if (!invalidFeatureIds.isEmpty()) {
       //QgsFeature firstFeature;
        //QgsFeatureRequest firstRequest;
       // firstRequest.setFilterFids(QgsFeatureIds(invalidFeatureIds.begin(), invalidFeatureIds.begin() + 1));

       // QgsFeatureIterator firstIt = layer->getFeatures(firstRequest);
       // if (firstIt.nextFeature(firstFeature) && firstFeature.hasGeometry()) {
            //mIface->mapCanvas()->setExtent(firstFeature.geometry().boundingBox());
           // mIface->mapCanvas()->refresh();
       // }
    //}
//}




// 另存SHP文件（仅在结果不规范时调用）
bool ScaleCheckerNormativeDialog::saveShpAs(const QString& originalPath)
{
    // 选择保存目录（而非直接指定文件名，避免覆盖风险）
    QString saveDir = QFileDialog::getExistingDirectory(
        this,
        tr("选择SHP保存目录"),
        QFileInfo(originalPath).path() // 默认目录为原文件所在目录
    );

    if (saveDir.isEmpty()) // 用户取消选择目录
        return false;

    // 构造新文件名（原文件名+_modified后缀，避免覆盖）
    QFileInfo originalFile(originalPath);
    QString newBaseName = originalFile.baseName() + "_modified";
    QString newShpPath = QString("%1/%2.shp").arg(saveDir, newBaseName);

    // 复制SHP相关所有附属文件（.shp/.shx/.dbf/.prj/.qix等）
    QDir originalDir(originalFile.path());
    QStringList fileFilters = {
        originalFile.baseName() + ".*" // 匹配原文件所有后缀的附属文件
    };
    QStringList relatedFiles = originalDir.entryList(fileFilters, QDir::Files);

    if (relatedFiles.isEmpty()) // 无相关文件（异常情况）
    {
        QMessageBox::critical(this, tr("错误"), tr("未找到SHP相关的附属文件！"));
        return false;
    }

    // 遍历复制所有文件
    bool allCopied = true;
    foreach(QString fileName, relatedFiles)
    {
        QString srcPath = originalDir.filePath(fileName);
        // 替换文件名中的原基底名为新基底名（如xxx.shp → xxx_modified.shp）
        QString newFileName = fileName.replace(originalFile.baseName(), newBaseName);
        QString dstPath = QString("%1/%2").arg(saveDir, newFileName);

        // 若目标文件已存在，先删除（避免复制失败）
        if (QFile::exists(dstPath))
            QFile::remove(dstPath);

        // 执行复制
        if (!QFile::copy(srcPath, dstPath))
        {
            QMessageBox::warning(this, tr("复制失败"),
                tr("文件复制失败：%1").arg(fileName));
            allCopied = false;
            break;
        }
    }

    if (allCopied)
    {
        QMessageBox::information(this, tr("成功"),
            tr("SHP文件已成功另存至：\n%1\n新文件名为：%2.shp").arg(saveDir, newBaseName));
        return true;
    }
    else
    {
        QMessageBox::critical(this, tr("失败"), tr("部分文件复制失败，SHP另存未完成！"));
        return false;
    }
}
