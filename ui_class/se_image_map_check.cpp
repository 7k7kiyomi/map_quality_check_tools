#include "se_image_map_check.h"
#include "se_boundary_anomaly_check_algorithm.h"

/*------------Qt--------------------*/
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QComboBox>
#include <QProcess>
#include <QFileDialog>
#include <QInputDialog>
#include <QListWidget>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QApplication>
#include <QGroupBox>
#include <QDir>
#include <QDebug>
#include <QFileInfo>
#include <QProgressBar>
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QColorDialog>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QThread>
#include <QElapsedTimer>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <functional>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

/*------------QGIS------------------*/
#include "qgisinterface.h"
#include "qgsmapcanvas.h"
#include "qgsrasterlayer.h"
#include "qgsvectorlayer.h"
#include "qgsproject.h"
#include "qgsfeature.h"
#include "qgsfeatureiterator.h"
#include "qgswkbtypes.h"
#include "qgsfields.h"
#include "qgsfield.h"
#include "qgsgeometry.h"
#include "qgslinestring.h"
#include "qgspoint.h"
#include "qgspointxy.h"
#include "qgsrectangle.h"
#include "qgsmaplayer.h"
#include "qgsmapsettings.h"
#include "qgscoordinatereferencesystem.h"
#include "qgsvectorlayerlabeling.h"
#include "qgspallabeling.h"
#include "qgstextformat.h"
#include "qgsproperty.h"
#include "qgssinglesymbolrenderer.h"
#include "qgssymbol.h"
#include "qgslinesymbol.h"
#include "qgsfillsymbol.h"
#include "qgssymbollayer.h"
#include "qgsvectorfilewriter.h"
#include "qgssettings.h"

/*------------OpenCV----------------*/
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>

/*------------LittleCMS (ICC 色彩管理 / CMYK->RGB) ----------*/
#include <lcms2.h>
#include <cstring>
#include <cstdint>

/*------------Tesseract OCR---------*/
#include <tesseract/baseapi.h>
#include <tesseract/resultiterator.h>

// ======================================================================
//  ICC 色彩管理: CMYK -> BGR
//  地图扫描图常为 CMYK 色彩模式(JPEG/TIFF)。OpenCV 的 IMREAD_COLOR 会做
//  朴素的 CMYK->BGR 转换,忽略内嵌 ICC profile,导致取色/配准/预览窗口中
//  颜色与实际不符。此处从文件中提取内嵌 ICC profile,交由 LittleCMS 按
//  profile 做受管转换,使显示颜色与原图一致。
// ======================================================================

// 从 JPEG 的 APP2(0xFFE2) 标记中提取分块存放的 ICC profile
static std::vector<uchar> extractIccFromJpeg(const uchar* data, size_t size)
{
    std::vector<uchar> profile;
    if (size < 2 || data[0] != 0xFF || data[1] != 0xD8) return profile; // 非 JPEG

    std::map<int, std::vector<uchar>> chunks;
    size_t i = 2;
    while (i + 4 <= size) {
        if (data[i] != 0xFF) break;
        uchar marker = data[i + 1];
        if (marker == 0xFF) { ++i; continue; }                       // 填充字节
        if (marker == 0xD8) { i += 2; continue; }                   // SOI
        if (marker == 0xD9) break;                                   // EOI
        if (marker >= 0xD0 && marker <= 0xD7) { i += 2; continue; }  // RSTn(无段长度)
        if (marker == 0xDA) break;                                    // SOS: 后续为压缩数据

        ushort segLen = (static_cast<ushort>(data[i + 2]) << 8) | data[i + 3];
        if (segLen < 2) break;
        size_t segEnd = i + 2 + segLen;
        if (segEnd > size) break;
        const uchar* seg = data + i + 4;
        size_t segDataLen = static_cast<size_t>(segLen) - 2;

        if (marker == 0xE2 && segDataLen >= 14) {                    // APP2: ICC_PROFILE
            static const char ICC_TAG[] = "ICC_PROFILE";
            if (std::memcmp(seg, ICC_TAG, 12) == 0) {
                int seq = seg[12];
                chunks[seq].assign(seg + 14, seg + segDataLen);
            }
        }
        i = segEnd;
    }
    for (const auto& kv : chunks)
        profile.insert(profile.end(), kv.second.begin(), kv.second.end());
    return profile;
}

// 从 TIFF IFD 中提取 ICC profile(tag 34675)
static std::vector<uchar> extractIccFromTiff(const uchar* data, size_t size)
{
    std::vector<uchar> profile;
    if (size < 8) return profile;
    bool le = (data[0] == 'I' && data[1] == 'I');                   // 否则 'MM'(大端)
    auto rd16 = [&](size_t off) -> ushort {
        return le ? (ushort(data[off]) | (ushort(data[off + 1]) << 8))
                  : ((ushort(data[off]) << 8) | ushort(data[off + 1]));
    };
    auto rd32 = [&](size_t off) -> uint32_t {
        return le ? (uint32_t(data[off]) | (uint32_t(data[off + 1]) << 8) |
                     (uint32_t(data[off + 2]) << 16) | (uint32_t(data[off + 3]) << 24))
                  : ((uint32_t(data[off]) << 24) | (uint32_t(data[off + 1]) << 16) |
                     (uint32_t(data[off + 2]) << 8) | uint32_t(data[off + 3]));
    };
    uint32_t ifdOff = rd32(4);
    if (ifdOff == 0 || static_cast<size_t>(ifdOff) + 2 > size) return profile;
    size_t p = ifdOff;
    ushort numEntries = rd16(p);
    p += 2;
    for (int e = 0; e < numEntries; ++e) {
        if (p + 12 > size) break;
        ushort tag = rd16(p);
        uint32_t count = rd32(p + 4);
        if (tag == 34675) {                                          // ICC Profile
            size_t total = count;
            if (total <= 4) {
                profile.assign(data + p + 8, data + p + 8 + total);
            } else {
                uint32_t off = rd32(p + 8);
                if (off != 0 && static_cast<size_t>(off) + total <= size)
                    profile.assign(data + off, data + off + total);
            }
            return profile;
        }
        p += 12;
    }
    return profile;
}

// 根据文件头判断格式并提取内嵌 ICC profile
static std::vector<uchar> extractIccProfile(const uchar* data, size_t size)
{
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8)
        return extractIccFromJpeg(data, size);
    if (size >= 2 && ((data[0] == 'I' && data[1] == 'I') ||
                      (data[0] == 'M' && data[1] == 'M')))
        return extractIccFromTiff(data, size);
    return {};
}

// 使用 LittleCMS 将 4 通道 CMYK 图转为 3 通道 BGR(OpenCV 约定)
// iccProfile: 内嵌 ICC(可为空)
// 注意: OpenCV imdecode(IMREAD_UNCHANGED) 对 CMYK 图像的输出有两个特征:
//   1) 通道顺序为 Y, M, C, K(前 3 通道反转, 类似 RGB→BGR 的重排);
//   2) 像素为 Adobe 反相风格(255=无墨, 0=满墨)。
// LittleCMS 的 TYPE_CMYK_8_REV 期望 C, M, Y, K + 反相, 故需先交换 ch0<->ch2
// 把 Y,M,C,K 还原为 C,M,Y,K, 再用 REV 让 LittleCMS 在转换时反相。
// 返回空 Mat 表示无法做受管转换,调用方可退回默认处理
static cv::Mat cmykToBgrViaLcms(const cv::Mat& cmyk,
                                const std::vector<uchar>& iccProfile)
{
    if (cmyk.empty() || cmyk.type() != CV_8UC4) return cv::Mat();

    cmsHPROFILE hIn = nullptr;
    if (!iccProfile.empty())
        hIn = cmsOpenProfileFromMem(iccProfile.data(),
                                    static_cast<cmsUInt32Number>(iccProfile.size()));
    // 校验输入 profile 确为 CMYK, 否则放弃以免误转(如 RGBA 的 RGB profile)
    if (hIn && cmsGetColorSpace(hIn) != cmsSigCmykData) {
        cmsCloseProfile(hIn);
        hIn = nullptr;
    }
    if (!hIn) return cv::Mat();  // 无可用 CMYK profile

    cmsHPROFILE hOut = cmsCreate_sRGBProfile();
    if (!hOut) { cmsCloseProfile(hIn); return cv::Mat(); }

    // 相对色度测量 + 黑点补偿: 精确保持中性灰与黑点(图像查看器标准配置)
    // 感知意图(INTENT_PERCEPTUAL)会压缩整个色域, 导致黑白灰轻微偏色
    cmsHTRANSFORM hXform = cmsCreateTransform(
        hIn, TYPE_CMYK_8_REV, hOut, TYPE_BGR_8,
        INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION);
    cmsCloseProfile(hIn);
    cmsCloseProfile(hOut);
    if (!hXform) return cv::Mat();

    // OpenCV 输出顺序为 Y, M, C, K; 交换 ch0<->ch2 还原为 C, M, Y, K
    cv::Mat ordered(cmyk.size(), cmyk.type());
    int fromTo[] = { 2, 0, 1, 1, 0, 2, 3, 3 };
    cv::mixChannels(&cmyk, 1, &ordered, 1, fromTo, 4);

    // LittleCMS 要求输入为连续内存
    if (!ordered.isContinuous()) ordered = ordered.clone();

    cv::Mat bgr(cmyk.rows, cmyk.cols, CV_8UC3);
    cmsDoTransform(hXform, ordered.data, bgr.data,
                   static_cast<cmsUInt32Number>(cmyk.rows) *
                   static_cast<cmsUInt32Number>(cmyk.cols));
    cmsDeleteTransform(hXform);
    return bgr;
}

// 支持中文路径的图片读取（cv::imread 不支持非 ASCII 路径）
// 对 CMYK(JPEG/TIFF) 图像使用 LittleCMS 做受管 CMYK->BGR 转换,
// 其余格式退化为 cv::IMREAD_COLOR(与原实现一致)
static cv::Mat imreadCN(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray data = file.readAll();
    file.close();
    std::vector<uchar> buf(data.begin(), data.end());

    // 先按原样解码, 判断通道数以识别 CMYK(4 通道)
    cv::Mat raw = cv::imdecode(buf, cv::IMREAD_UNCHANGED);
    if (!raw.empty() && raw.channels() == 4) {
        const uchar* p = reinterpret_cast<const uchar*>(data.constData());
        size_t n = static_cast<size_t>(data.size());
        std::vector<uchar> icc = extractIccProfile(p, n);
        cv::Mat bgr = cmykToBgrViaLcms(raw, icc);
        if (!bgr.empty()) return bgr;
        // 无可用 CMYK ICC: 退化为 OpenCV 内置转换(等价原 IMREAD_COLOR 行为)
    }

    return cv::imdecode(buf, cv::IMREAD_COLOR);
}

// 支持中文路径的图片写入（cv::imwrite 不支持非 ASCII 路径）
// ext: 文件扩展名（含点），如 ".png"
static bool imwriteCN(const QString& path, const cv::Mat& img, const QString& ext = ".png")
{
    std::vector<uchar> buf;
    std::string extStd = ext.toUtf8().toStdString();
    if (!cv::imencode(extStd, img, buf)) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    qint64 n = file.write(reinterpret_cast<const char*>(buf.data()), (qint64)buf.size());
    file.close();
    return n == (qint64)buf.size();
}

// cv::Mat 转 QImage
static QImage cvMatToQImage(const cv::Mat& mat)
{
    if (mat.empty()) return QImage();
    if (mat.type() == CV_8UC1) {
        return QImage(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step),
                      QImage::Format_Grayscale8).copy();
    } else if (mat.type() == CV_8UC3) {
        cv::Mat rgb;
        cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
        return QImage(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step),
                      QImage::Format_RGB888).copy();
    } else if (mat.type() == CV_8UC4) {
        return QImage(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step),
                      QImage::Format_RGBA8888).copy();
    }
    return QImage();
}

// 在指定 QLabel 中显示 cv::Mat（自动缩放）
static void showMatInLabel(QLabel* label, const cv::Mat& mat, const QString& title)
{
    if (mat.empty()) {
        label->setText(title + "\n[空图像]");
        return;
    }
    QImage img = cvMatToQImage(mat);
    if (img.isNull()) {
        label->setText(title + "\n[转换失败]");
        return;
    }
    label->setPixmap(QPixmap::fromImage(img).scaled(
        480, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    label->setAlignment(Qt::AlignCenter);
}

// ========== 按 id 读取 XML 质检规则的辅助读取器 ==========
// 用 QXmlStreamReader 流式解析，构建 id → (ruleName → value) 缓存
// 与 XmlConfigReader（按名称查询）并存，提供按 check_item <id> 查询的能力
// 用法: configByIdReader().load(xmlPath); ... configByIdReader().getRuleValue(601, "颜色容差", 5.0);
class XmlConfigByIdReader
{
public:
    // 加载 XML 文件，构建 id → rules 映射；返回是否成功
    bool load(const QString& xmlPath)
    {
        QFile file(xmlPath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "[XmlConfigByIdReader] 无法打开文件:" << xmlPath;
            m_loaded = false;
            return false;
        }
        QXmlStreamReader xml(&file);
        m_data.clear();

        int  curId = -1;       // 当前 check_item 的 id（-1 表示未读到 id）
        bool inRule = false;   // 是否在 <rule> 内部
        QString curRuleName;
        QString curRuleValue;

        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.hasError()) break;
            if (xml.isStartElement()) {
                const QStringRef tag = xml.name();
                if (tag == QStringLiteral("check_item")) {
                    curId = -1;  // 重置，等读到 <id> 时填入
                } else if (tag == QStringLiteral("id")) {
                    bool ok = false;
                    int id = xml.readElementText().trimmed().toInt(&ok);
                    if (ok) curId = id;
                } else if (tag == QStringLiteral("rule")) {
                    inRule = true;
                    curRuleName.clear();
                    curRuleValue.clear();
                } else if (inRule && tag == QStringLiteral("name")) {
                    curRuleName = xml.readElementText().trimmed();
                } else if (inRule && tag == QStringLiteral("value")) {
                    curRuleValue = xml.readElementText().trimmed();
                }
            } else if (xml.isEndElement()) {
                if (xml.name() == QStringLiteral("rule") && inRule) {
                    inRule = false;
                    if (curId > 0 && !curRuleName.isEmpty()) {
                        m_data[curId][curRuleName] = curRuleValue;
                    }
                }
            }
        }

        m_loaded = !xml.hasError() && !m_data.isEmpty();
        if (!m_loaded) {
            qWarning() << "[XmlConfigByIdReader] 解析失败或为空:" << xmlPath
                       << "错误:" << xml.errorString();
        } else {
            qDebug() << "[XmlConfigByIdReader] 加载成功, check_item 数:" << m_data.size();
        }
        return m_loaded;
    }

    bool isLoaded() const { return m_loaded; }

    // 按 id + ruleName 读取数值，失败返回 defaultValue
    double getRuleValue(int itemId, const QString& ruleName, double defaultValue) const
    {
        auto itItem = m_data.constFind(itemId);
        if (itItem == m_data.constEnd()) {
            qDebug("[XmlConfigById] getRuleValue: id=%d 未找到, rule='%s', 使用默认值=%g",
                   itemId, qPrintable(ruleName), defaultValue);
            return defaultValue;
        }
        auto itRule = itItem->constFind(ruleName);
        if (itRule == itItem->constEnd()) {
            qDebug("[XmlConfigById] getRuleValue: id=%d rule='%s' 未找到, 使用默认值=%g",
                   itemId, qPrintable(ruleName), defaultValue);
            return defaultValue;
        }
        bool ok = false;
        double v = itRule->toDouble(&ok);
        if (!ok) {
            qDebug("[XmlConfigById] getRuleValue: id=%d rule='%s' 值='%s' 无法转数值, 使用默认值=%g",
                   itemId, qPrintable(ruleName), qPrintable(*itRule), defaultValue);
            return defaultValue;
        }
        qDebug("[XmlConfigById] getRuleValue: id=%d rule='%s' = %g", itemId, qPrintable(ruleName), v);
        return v;
    }

    // 按 id + ruleName 读取字符串，失败返回 defaultValue
    QString getRuleStringValue(int itemId, const QString& ruleName, const QString& defaultValue) const
    {
        auto itItem = m_data.constFind(itemId);
        if (itItem == m_data.constEnd()) {
            qDebug("[XmlConfigById] getRuleStringValue: id=%d 未找到, rule='%s', 使用默认值='%s'",
                   itemId, qPrintable(ruleName), qPrintable(defaultValue));
            return defaultValue;
        }
        auto itRule = itItem->constFind(ruleName);
        if (itRule == itItem->constEnd()) {
            qDebug("[XmlConfigById] getRuleStringValue: id=%d rule='%s' 未找到, 使用默认值='%s'",
                   itemId, qPrintable(ruleName), qPrintable(defaultValue));
            return defaultValue;
        }
        qDebug("[XmlConfigById] getRuleStringValue: id=%d rule='%s' = '%s'",
               itemId, qPrintable(ruleName), qPrintable(*itRule));
        return *itRule;
    }

    // 按 id 读取该检查项下所有 rule（ruleName → value），无则返回空 map
    // 用于需要遍历某个检查项全部参数的场景（如敏感词分类词表）
    QMap<QString, QString> getRulesByItemId(int itemId) const
    {
        auto itItem = m_data.constFind(itemId);
        if (itItem == m_data.constEnd()) {
            qDebug("[XmlConfigById] getRulesByItemId: id=%d 未找到, 返回空map", itemId);
            return {};
        }
        qDebug("[XmlConfigById] getRulesByItemId: id=%d 返回 %d 条rule",
               itemId, itItem->size());
        return *itItem;
    }

private:
    // id → (ruleName → value 字符串)
    QMap<int, QMap<QString, QString>> m_data;
    bool m_loaded = false;
};

// 全局单例访问（Meyers Singleton），生命周期与进程一致
// 在 loadAllConfigFromXml 中加载，业务代码各处直接调用查询
static XmlConfigByIdReader& configByIdReader()
{
    static XmlConfigByIdReader instance;
    return instance;
}

// check_item id 常量（对应 图片地图质检.xml 中的 <id> 值）
constexpr int ITEM_ID_BOUNDARY_COLOR   = 601;  // 境界线颜色提取
constexpr int ITEM_ID_BOUNDARY_ANOMALY = 602;  // 境界线异常检测
constexpr int ITEM_ID_ANNOTATION_OCR   = 603;  // 注记文字提取
constexpr int ITEM_ID_SENSITIVE_WORDS  = 604;  // 敏感文字合规性检查
constexpr int ITEM_ID_GAZETTEER_MATCH  = 605;  // 地名库匹配

// ========== 标签状态辅助函数 ==========
// 通过 qss 属性选择器 QLabel[status="xxx"] 切换标签颜色, 避免内联 setStyleSheet
static void setLabelStatus(QLabel* lbl, const char* status)
{
    if (!lbl) return;
    lbl->setProperty("status", status);
    lbl->style()->unpolish(lbl);
    lbl->style()->polish(lbl);
}

// 插件追加样式: 平台 qss 未定义的状态标签/提示框选择器
// 在顶层窗口 setStyleSheet(qApp->styleSheet() + kPluginExtraQss) 中追加, 不修改平台 qss 文件
static const char* kPluginExtraQss = R"(
QLabel[status="unloaded"] { color: #9ca8b5; }
QLabel[status="loaded"] { color: #7eff9e; font-weight: bold; }
QLabel[status="error"] { color: #ff7a7a; }
QLabel[status="progress"] { color: #5bceff; }
QLabel[status="warning"] { color: #ffb238; font-weight: bold; }
QLabel[status="muted"] { color: #b0bccc; }
QLabel[hintBox="true"] { padding: 8px; background: rgba(5, 30, 58, 215); border: 1px solid rgba(27, 201, 255, 150); border-radius: 4px; color: #e9fbff; }
QLabel[hintBox="large"] { padding: 10px; font-size: 20px; }
)";

// 为插件顶层窗口拼接平台样式 + 插件追加样式
static void applyPluginStyle(QWidget* w)
{
    if (!w) return;
    w->setStyleSheet(qApp->styleSheet() + QString::fromLatin1(kPluginExtraQss));
}

// ========== 可缩放取色视图 ==========
class PickableGraphicsView : public QGraphicsView
{
public:
    std::function<void(int img_x, int img_y)> onImageClicked;

    // 同步伙伴视图：设置后，缩放和平移会按场景比例同步到伙伴（双向同步需两边互相设置）
    void setSyncView(QGraphicsView* peer) { m_syncView = peer; }

    // 同步缩放（按钮调用）：本视图按 factor 缩放后，伙伴也按相同 factor 缩放，并同步中心点
    void syncZoom(qreal factor)
    {
        scale(factor, factor);
        syncPeerZoom(factor);
    }

    // 同步适应窗口：本视图 fitInView 后，伙伴也 fitInView
    void syncFit(QGraphicsItem* item)
    {
        fitInView(item, Qt::KeepAspectRatio);
        if (m_syncView && !m_syncing) {
            m_syncing = true;
            // 伙伴 fit 自己的场景矩形
            m_syncView->fitInView(m_syncView->scene()->sceneRect(), Qt::KeepAspectRatio);
            m_syncing = false;
        }
    }

    explicit PickableGraphicsView(QWidget* parent = nullptr) : QGraphicsView(parent)
    {
        setRenderHint(QPainter::Antialiasing);
        setRenderHint(QPainter::SmoothPixmapTransform);
        setDragMode(QGraphicsView::NoDrag);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    }

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        const qreal factor = (event->angleDelta().y() > 0) ? 1.2 : 0.83;
        scale(factor, factor);
        syncPeerZoom(factor);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::RightButton) {
            m_dragging = true;
            m_lastPos = event->pos();
            setCursor(Qt::ClosedHandCursor);
            return;
        }
        if (event->button() == Qt::LeftButton && onImageClicked) {
            QPointF scenePos = mapToScene(event->pos());
            // 遍历场景中该位置的所有 item，找到最底层（Z 值最小）的 pixmap item
            // 避免被后添加的标记（ellipse/text/path）遮挡导致拾取失败
            const QList<QGraphicsItem*> itemsAtPos = scene()->items(scenePos);
            QGraphicsPixmapItem* pixItem = nullptr;
            qreal minZ = std::numeric_limits<qreal>::max();
            for (QGraphicsItem* it : itemsAtPos) {
                if (it->type() == QGraphicsPixmapItem::Type) {
                    qreal z = it->zValue();
                    if (z < minZ) { minZ = z; pixItem = static_cast<QGraphicsPixmapItem*>(it); }
                }
            }
            if (pixItem) {
                QPointF itemPos = pixItem->mapFromScene(scenePos);
                onImageClicked(static_cast<int>(itemPos.x()), static_cast<int>(itemPos.y()));
                return;
            }
        }
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_dragging) {
            QPoint delta = event->pos() - m_lastPos;
            m_lastPos = event->pos();
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
            verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
            syncPeerPan();
            return;
        }
        QGraphicsView::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::RightButton && m_dragging) {
            m_dragging = false;
            unsetCursor();
            return;
        }
        QGraphicsView::mouseReleaseEvent(event);
    }

private:
    bool m_dragging = false;
    QPoint m_lastPos;
    QGraphicsView* m_syncView = nullptr;  // 同步伙伴视图（nullptr=不同步）
    bool m_syncing = false;               // 正在同步中，防止递归

    // 缩放同步：让伙伴按相同 factor 缩放（相对缩放，避免初始缩放基准不同导致反向），并同步中心点
    void syncPeerZoom(qreal factor)
    {
        if (!m_syncView || m_syncing) return;
        m_syncing = true;
        m_syncView->scale(factor, factor);  // 相对缩放，保证两边同向放大/缩小
        syncPeerCenter();
        m_syncing = false;
    }

    // 平移同步：仅同步中心点（按场景比例映射），不改变缩放
    void syncPeerPan()
    {
        if (!m_syncView || m_syncing) return;
        m_syncing = true;
        syncPeerCenter();
        m_syncing = false;
    }

    // 将本视图中心点按场景比例映射到伙伴视图中心
    void syncPeerCenter()
    {
        QRectF myRect = scene()->sceneRect();
        QRectF peerRect = m_syncView->scene()->sceneRect();
        QPointF center = mapToScene(viewport()->rect().center());
        if (myRect.width() > 0 && myRect.height() > 0) {
            qreal rx = (center.x() - myRect.left()) / myRect.width();
            qreal ry = (center.y() - myRect.top()) / myRect.height();
            QPointF peerCenter(
                peerRect.left() + rx * peerRect.width(),
                peerRect.top() + ry * peerRect.height());
            m_syncView->centerOn(peerCenter);
        }
    }
};

// ========== 取色对话框 ==========
class ImagePickDialog : public QDialog
{
public:
    cv::Scalar m_pickedColor;
    bool m_hasPicked = false;
    cv::Mat m_img;
    PickableGraphicsView* m_view = nullptr;
    QGraphicsScene* m_scene = nullptr;
    QGraphicsPixmapItem* m_pixItem = nullptr;

    explicit ImagePickDialog(const cv::Mat& img, QWidget* parent = nullptr)
        : QDialog(parent), m_img(img.clone())
    {
        applyPluginStyle(this);
        setWindowTitle("取色窗口 - 点击图片选择境界线颜色");
        resize(1000, 750);

        QVBoxLayout* layout = new QVBoxLayout(this);

        QLabel* hint = new QLabel(
            "操作说明:\n"
            "  - 左键点击: 选取颜色 (点击后自动关闭并处理)\n"
            "  - 右键按住拖动: 平移图片\n"
            "  - 滚轮: 缩放图片\n"
            "  - 滚动条: 平移图片");
        hint->setProperty("hintBox", "large");

        m_view = new PickableGraphicsView();
        m_scene = new QGraphicsScene(m_view);
        m_view->setScene(m_scene);

        QImage qimg = cvMatToQImage(m_img);
        m_pixItem = m_scene->addPixmap(QPixmap::fromImage(qimg));
        m_scene->setSceneRect(m_pixItem->boundingRect());
        m_view->fitInView(m_pixItem, Qt::KeepAspectRatio);

        m_view->onImageClicked = [this](int x, int y) {
            if (x < 0 || y < 0 || x >= m_img.cols || y >= m_img.rows) return;
            cv::Vec3b bgr = m_img.at<cv::Vec3b>(y, x);
            m_pickedColor = cv::Scalar(bgr[0], bgr[1], bgr[2]);
            m_hasPicked = true;
            accept();
        };

        QHBoxLayout* btnLayout = new QHBoxLayout();
        QPushButton* btnZoomIn = new QPushButton("放大");
        QPushButton* btnZoomOut = new QPushButton("缩小");
        QPushButton* btnFit = new QPushButton("适应窗口");
        QPushButton* btnActual = new QPushButton("原始大小 1:1");
        QPushButton* btnCancel = new QPushButton("取消");
        btnLayout->addWidget(btnZoomIn);
        btnLayout->addWidget(btnZoomOut);
        btnLayout->addWidget(btnFit);
        btnLayout->addWidget(btnActual);
        btnLayout->addStretch();
        btnLayout->addWidget(btnCancel);

        layout->addWidget(hint);
        layout->addWidget(m_view, 1);
        layout->addLayout(btnLayout);

        QObject::connect(btnZoomIn, &QPushButton::clicked, [this]() { m_view->scale(1.25, 1.25); });
        QObject::connect(btnZoomOut, &QPushButton::clicked, [this]() { m_view->scale(0.8, 0.8); });
        QObject::connect(btnFit, &QPushButton::clicked, [this]() { m_view->fitInView(m_pixItem, Qt::KeepAspectRatio); });
        QObject::connect(btnActual, &QPushButton::clicked, [this]() { m_view->resetTransform(); });
        QObject::connect(btnCancel, &QPushButton::clicked, [this]() { reject(); });
    }
};

// ============================================================
// GeoreferenceDialog: 独立地理配准窗口
// 左侧图片视图 + 右侧境界线视图 + 底部控制点表格
// 采集流程：交替在左右视图点击，先图片后境界线，自动配对
// ============================================================
class GeoreferenceDialog : public QDialog
{
public:
    cv::Mat m_affine;               // 计算结果：仿射变换矩阵 2x3 CV_64F：img = M * geo
    bool    m_affineValid = false;  // 是否成功计算
    std::vector<cv::Point2f> m_gcpImgPts;
    std::vector<cv::Point2d> m_gcpGeoPts;

    GeoreferenceDialog(const cv::Mat& image,
                       const std::vector<std::vector<cv::Point2d>>& boundaryFeatures,
                       const cv::Rect2d& boundaryExtent,
                       QWidget* parent = nullptr)
        : QDialog(parent)
        , m_image(image.clone())
        , m_boundaryFeatures(boundaryFeatures)
        , m_boundaryExtent(boundaryExtent)
    {
        applyPluginStyle(this);
        setWindowTitle("地理配准 - 控制点采集");
        resize(1400, 850);

        // 计算境界线范围（若传入无效则重新计算）
        if (m_boundaryExtent.width < 1e-9 || m_boundaryExtent.height < 1e-9) {
            double minX = std::numeric_limits<double>::max();
            double minY = std::numeric_limits<double>::max();
            double maxX = -std::numeric_limits<double>::max();
            double maxY = -std::numeric_limits<double>::max();
            for (auto& feat : m_boundaryFeatures) {
                for (auto& p : feat) {
                    if (p.x < minX) minX = p.x;
                    if (p.y < minY) minY = p.y;
                    if (p.x > maxX) maxX = p.x;
                    if (p.y > maxY) maxY = p.y;
                }
            }
            if (maxX > minX && maxY > minY) {
                m_boundaryExtent = cv::Rect2d(minX, minY, maxX - minX, maxY - minY);
            }
        }

        buildUi();
    }

private:
    cv::Mat m_image;
    const std::vector<std::vector<cv::Point2d>>& m_boundaryFeatures;
    cv::Rect2d m_boundaryExtent;

    // 视图
    PickableGraphicsView* m_imgView = nullptr;
    QGraphicsScene*       m_imgScene = nullptr;
    QGraphicsPixmapItem*  m_imgPixItem = nullptr;
    PickableGraphicsView* m_geoView = nullptr;
    QGraphicsScene*       m_geoScene = nullptr;
    QGraphicsPixmapItem*  m_geoPixItem = nullptr;  // 白色底图（用于点击拾取与 fitInView）

    // 地理视图的范围参数（用于视图坐标<->地理坐标转换）
    double m_geoMinX = 0, m_geoMinY = 0, m_geoExtW = 1, m_geoExtH = 1;
    int m_geoViewW = 800, m_geoViewH = 600;

    // 控件
    QLabel*       m_lblHint = nullptr;
    QTableWidget* m_tblGcp = nullptr;
    QPushButton*  m_btnRemove = nullptr;
    QPushButton*  m_btnClear = nullptr;
    QPushButton*  m_btnFinish = nullptr;
    QPushButton*  m_btnCancel = nullptr;
    QLabel*       m_lblStatus = nullptr;

    // 采集状态
    // 0 = 等待图片点, 1 = 等待境界线点
    int m_pickState = 0;
    cv::Point2f m_pendingImgPt = cv::Point2f(-1, -1);
    cv::Point2d m_pendingGeoPt = cv::Point2d(-1, -1);

    // 境界线视图上的图元（控制点标记）
    QList<QGraphicsItem*> m_imgMarkers;
    QList<QGraphicsItem*> m_geoMarkers;

    void buildUi()
    {
        QVBoxLayout* mainLayout = new QVBoxLayout(this);

        // 顶部提示
        m_lblHint = new QLabel(
            "操作流程: 依次在【左侧图片】和【右侧境界线】上点击同名控制点，自动配对。\n"
            "  左键点击: 选点    右键拖动: 平移    滚轮: 缩放\n"
            "  境界线视图上点击对应位置即可选点。\n"
            "  控制点 ≥ 3 对后，【完成配准】按钮将亮起。");
        m_lblHint->setProperty("hintBox", "true");
        m_lblHint->setWordWrap(true);
        mainLayout->addWidget(m_lblHint);

        // 左右双视图
        QHBoxLayout* viewLayout = new QHBoxLayout();

        // 左：图片视图
        QVBoxLayout* leftLayout = new QVBoxLayout();
        leftLayout->addWidget(new QLabel("【图片】点击选择控制点:"));
        m_imgView = new PickableGraphicsView();
        m_imgScene = new QGraphicsScene(m_imgView);
        m_imgView->setScene(m_imgScene);
        QImage qimg = cvMatToQImage(m_image);
        m_imgPixItem = m_imgScene->addPixmap(QPixmap::fromImage(qimg));
        m_imgPixItem->setZValue(-1000);  // 最底层，便于点击拾取与标记叠加
        m_imgScene->setSceneRect(m_imgPixItem->boundingRect());
        m_imgView->fitInView(m_imgPixItem, Qt::KeepAspectRatio);
        leftLayout->addWidget(m_imgView, 1);

        QHBoxLayout* leftBtnRow = new QHBoxLayout();
        QPushButton* btnImgZoomIn = new QPushButton("放大");
        QPushButton* btnImgZoomOut = new QPushButton("缩小");
        QPushButton* btnImgFit = new QPushButton("适应窗口");
        leftBtnRow->addWidget(btnImgZoomIn);
        leftBtnRow->addWidget(btnImgZoomOut);
        leftBtnRow->addWidget(btnImgFit);
        leftBtnRow->addStretch();
        leftLayout->addLayout(leftBtnRow);

        QObject::connect(btnImgZoomIn, &QPushButton::clicked, [this]() { m_imgView->syncZoom(1.25); });
        QObject::connect(btnImgZoomOut, &QPushButton::clicked, [this]() { m_imgView->syncZoom(0.8); });
        QObject::connect(btnImgFit, &QPushButton::clicked, [this]() { m_imgView->syncFit(m_imgPixItem); });

        // 右：境界线视图
        QVBoxLayout* rightLayout = new QVBoxLayout();
        rightLayout->addWidget(new QLabel("【境界线】点击选择对应控制点:"));
        m_geoView = new PickableGraphicsView();
        m_geoScene = new QGraphicsScene(m_geoView);
        m_geoView->setScene(m_geoScene);
        renderBoundaryToView();
        rightLayout->addWidget(m_geoView, 1);

        QHBoxLayout* rightBtnRow = new QHBoxLayout();
        QPushButton* btnGeoZoomIn = new QPushButton("放大");
        QPushButton* btnGeoZoomOut = new QPushButton("缩小");
        QPushButton* btnGeoFit = new QPushButton("适应窗口");
        rightBtnRow->addWidget(btnGeoZoomIn);
        rightBtnRow->addWidget(btnGeoZoomOut);
        rightBtnRow->addWidget(btnGeoFit);
        rightBtnRow->addStretch();
        rightLayout->addLayout(rightBtnRow);

        QObject::connect(btnGeoZoomIn, &QPushButton::clicked, [this]() { m_geoView->syncZoom(1.25); });
        QObject::connect(btnGeoZoomOut, &QPushButton::clicked, [this]() { m_geoView->syncZoom(0.8); });
        QObject::connect(btnGeoFit, &QPushButton::clicked, [this]() { m_geoView->syncFit(m_geoPixItem); });

        // 双向同步：左右视图的缩放和平移互相同步（按场景比例映射中心点）
        m_imgView->setSyncView(m_geoView);
        m_geoView->setSyncView(m_imgView);

        viewLayout->addLayout(leftLayout, 1);
        viewLayout->addLayout(rightLayout, 1);
        mainLayout->addLayout(viewLayout, 5);

        // 底部：控制点表格 + 状态 + 按钮
        m_tblGcp = new QTableWidget(0, 5);
        m_tblGcp->setHorizontalHeaderLabels({ "#", "图片 X", "图片 Y", "地理 X", "地理 Y" });
        m_tblGcp->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        for (int i = 1; i < 5; ++i)
            m_tblGcp->horizontalHeader()->setSectionResizeMode(i, QHeaderView::Stretch);
        m_tblGcp->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_tblGcp->setSelectionMode(QAbstractItemView::SingleSelection);
        m_tblGcp->setMaximumHeight(160);
        mainLayout->addWidget(m_tblGcp);

        // 状态行
        m_lblStatus = new QLabel("请先在左侧图片上点击控制点");
        setLabelStatus(m_lblStatus, "progress");
        mainLayout->addWidget(m_lblStatus);

        // 按钮行
        QHBoxLayout* btnRow = new QHBoxLayout();
        m_btnRemove = new QPushButton("移除选中");
        m_btnClear = new QPushButton("清空全部");
        m_btnFinish = new QPushButton("完成配准");
        m_btnCancel = new QPushButton("取消");
        m_btnFinish->setEnabled(false);
        // qss 已为 QPushButton 设置 font-weight: 650, 无需内联样式

        btnRow->addWidget(m_btnRemove);
        btnRow->addWidget(m_btnClear);
        btnRow->addStretch();
        btnRow->addWidget(m_lblStatus);
        btnRow->addStretch();
        btnRow->addWidget(m_btnFinish);
        btnRow->addWidget(m_btnCancel);
        mainLayout->addLayout(btnRow);

        // 连接信号
        m_imgView->onImageClicked = [this](int x, int y) {
            if (x < 0 || y < 0 || x >= m_image.cols || y >= m_image.rows) return;
            onImagePointPicked(x, y);
        };
        m_geoView->onImageClicked = [this](int x, int y) {
            if (x < 0 || y < 0 || x >= m_geoViewW || y >= m_geoViewH) return;
            onGeoPointPicked(x, y);
        };

        QObject::connect(m_btnRemove, &QPushButton::clicked, [this]() {
            int row = m_tblGcp->currentRow();
            if (row < 0 || row >= (int)m_gcpImgPts.size()) return;
            m_gcpImgPts.erase(m_gcpImgPts.begin() + row);
            m_gcpGeoPts.erase(m_gcpGeoPts.begin() + row);
            refreshTable();
            redrawMarkers();
            updateStatus();
        });

        QObject::connect(m_btnClear, &QPushButton::clicked, [this]() {
            m_gcpImgPts.clear();
            m_gcpGeoPts.clear();
            m_pendingImgPt = cv::Point2f(-1, -1);
            m_pendingGeoPt = cv::Point2d(-1, -1);
            m_pickState = 0;
            refreshTable();
            redrawMarkers();
            updateStatus();
        });

        QObject::connect(m_btnFinish, &QPushButton::clicked, [this]() {
            if (finishGeoreference()) {
                m_affineValid = true;
                accept();
            }
        });

        QObject::connect(m_btnCancel, &QPushButton::clicked, [this]() { reject(); });

        updateStatus();
    }

    // 渲染境界线（矢量形式）
    void renderBoundaryToView()
    {
        m_geoViewW = 800;
        m_geoViewH = 600;

        // 白色底图（不透明，作为 PickableGraphicsView 的点击拾取源）
        QPixmap bgPixmap(m_geoViewW, m_geoViewH);
        bgPixmap.fill(Qt::white);
        m_geoPixItem = m_geoScene->addPixmap(bgPixmap);
        m_geoPixItem->setZValue(-1000);  // 最底层
        m_geoScene->setSceneRect(0, 0, m_geoViewW, m_geoViewH);

        if (m_boundaryFeatures.empty()) {
            m_geoView->fitInView(m_geoPixItem, Qt::KeepAspectRatio);
            return;
        }

        m_geoMinX = m_boundaryExtent.x;
        m_geoMinY = m_boundaryExtent.y;
        m_geoExtW = m_boundaryExtent.width;
        m_geoExtH = m_boundaryExtent.height;

        double margin = 0.05;
        m_geoMinX -= m_geoExtW * margin;
        m_geoMinY -= m_geoExtH * margin;
        m_geoExtW *= (1 + 2 * margin);
        m_geoExtH *= (1 + 2 * margin);

        // 视图坐标 -> 地理坐标的反向：viewX = (gx - minX) / extW * W
        //                              viewY = H - (gy - minY) / extH * H
        auto toView = [&](double gx, double gy) -> QPointF {
            double vx = (gx - m_geoMinX) / m_geoExtW * m_geoViewW;
            double vy = m_geoViewH - (gy - m_geoMinY) / m_geoExtH * m_geoViewH;
            return QPointF(vx, vy);
        };

        // 用 QPainterPath 构建境界线矢量路径（蓝色），分段绘制
        // 不使用 cosmetic，线宽随缩放变化，放大时境界线变粗，便于看清细节
        QPen boundaryPen(QColor(0, 100, 255), 1.5);
        for (auto& feat : m_boundaryFeatures) {
            if (feat.size() < 2) continue;
            QPainterPath path;
            QPointF p0 = toView(feat.front().x, feat.front().y);
            path.moveTo(p0);
            for (size_t i = 1; i < feat.size(); ++i) {
                path.lineTo(toView(feat[i].x, feat[i].y));
            }
            auto* pathItem = m_geoScene->addPath(path, boundaryPen);
            pathItem->setZValue(0);
        }
        // 顶点不预先绘制

        m_geoView->fitInView(m_geoPixItem, Qt::KeepAspectRatio);
    }

    // 图片点击回调
    void onImagePointPicked(int x, int y)
    {
        if (m_pickState != 0) return;
        m_pendingImgPt = cv::Point2f((float)x, (float)y);
        m_pickState = 1;
        updateStatus();
        redrawMarkers();
    }

    // 境界线点击回调
    void onGeoPointPicked(int viewX, int viewY)
    {
        if (m_pickState != 1) return;

        // 视图坐标 -> 地理坐标
        double gx = m_geoMinX + (double)viewX / m_geoViewW * m_geoExtW;
        double gy = m_geoMinY + (m_geoViewH - (double)viewY) / m_geoViewH * m_geoExtH;

        m_pendingGeoPt = cv::Point2d(gx, gy);
        // 配对完成，加入列表
        m_gcpImgPts.push_back(m_pendingImgPt);
        m_gcpGeoPts.push_back(m_pendingGeoPt);

        // 重置待配对点
        m_pendingImgPt = cv::Point2f(-1, -1);
        m_pendingGeoPt = cv::Point2d(-1, -1);
        m_pickState = 0;

        refreshTable();
        redrawMarkers();
        updateStatus();
    }

    void refreshTable()
    {
        m_tblGcp->setRowCount(0);
        for (size_t i = 0; i < m_gcpImgPts.size() && i < m_gcpGeoPts.size(); ++i) {
            int row = m_tblGcp->rowCount();
            m_tblGcp->insertRow(row);
            m_tblGcp->setItem(row, 0, new QTableWidgetItem(QString::number(i+1)));
            m_tblGcp->setItem(row, 1, new QTableWidgetItem(QString::number(m_gcpImgPts[i].x, 'f', 1)));
            m_tblGcp->setItem(row, 2, new QTableWidgetItem(QString::number(m_gcpImgPts[i].y, 'f', 1)));
            m_tblGcp->setItem(row, 3, new QTableWidgetItem(QString::number(m_gcpGeoPts[i].x, 'f', 2)));
            m_tblGcp->setItem(row, 4, new QTableWidgetItem(QString::number(m_gcpGeoPts[i].y, 'f', 2)));
        }
    }

    // 在两个视图上绘制控制点标记
    void redrawMarkers()
    {
        // 清除旧标记
        for (auto* item : m_imgMarkers) m_imgScene->removeItem(item);
        for (auto* item : m_geoMarkers) m_geoScene->removeItem(item);
        m_imgMarkers.clear();
        m_geoMarkers.clear();

        const qreal MARKER_Z = 20;  // 标记层级，确保在境界线（z=0）和底图（z=-1000）之上

        // 已配对的点（红色编号）
        for (size_t i = 0; i < m_gcpImgPts.size() && i < m_gcpGeoPts.size(); ++i) {
            // 图片视图
            auto* ellImg = m_imgScene->addEllipse(m_gcpImgPts[i].x - 6, m_gcpImgPts[i].y - 6, 12, 12,
                QPen(Qt::red, 2), QBrush(Qt::red));
            ellImg->setZValue(MARKER_Z);
            auto* txtImg = m_imgScene->addText(QString::number(i+1));
            txtImg->setDefaultTextColor(Qt::yellow);
            txtImg->setPos(m_gcpImgPts[i].x + 8, m_gcpImgPts[i].y - 18);
            txtImg->setZValue(MARKER_Z);
            m_imgMarkers.append(ellImg);
            m_imgMarkers.append(txtImg);

            // 境界线视图
            double vx = (m_gcpGeoPts[i].x - m_geoMinX) / m_geoExtW * m_geoViewW;
            double vy = m_geoViewH - (m_gcpGeoPts[i].y - m_geoMinY) / m_geoExtH * m_geoViewH;
            auto* ellGeo = m_geoScene->addEllipse(vx - 6, vy - 6, 12, 12,
                QPen(Qt::red, 2), QBrush(Qt::red));
            ellGeo->setZValue(MARKER_Z);
            auto* txtGeo = m_geoScene->addText(QString::number(i+1));
            txtGeo->setDefaultTextColor(Qt::yellow);
            txtGeo->setPos(vx + 8, vy - 18);
            txtGeo->setZValue(MARKER_Z);
            m_geoMarkers.append(ellGeo);
            m_geoMarkers.append(txtGeo);
        }

        // 待配对的图片点（绿色）
        if (m_pickState == 1 && m_pendingImgPt.x >= 0) {
            auto* ell = m_imgScene->addEllipse(m_pendingImgPt.x - 6, m_pendingImgPt.y - 6, 12, 12,
                QPen(Qt::green, 2), QBrush(Qt::green));
            ell->setZValue(MARKER_Z);
            m_imgMarkers.append(ell);
        }
    }

    void updateStatus()
    {
        int n = (int)m_gcpImgPts.size();
        if (m_pickState == 0) {
            m_lblStatus->setText(QString("已采集 %1 对控制点 | 请在左侧图片上点击下一个控制点").arg(n));
            setLabelStatus(m_lblStatus, "progress");
        } else {
            m_lblStatus->setText(QString("已采集 %1 对控制点 | 请在右侧境界线上点击对应位置（带吸附）").arg(n));
            setLabelStatus(m_lblStatus, "loaded");
        }
        m_btnFinish->setEnabled(n >= 3);
    }

    // 计算仿射变换：用最小二乘法拟合 (px,py) = M*(gx,gy,1)
    bool finishGeoreference()
    {
        int n = (int)m_gcpImgPts.size();
        if (n < 3 || n != (int)m_gcpGeoPts.size()) return false;

        std::vector<cv::Point2f> geoF;
        geoF.reserve(n);
        for (auto& p : m_gcpGeoPts) geoF.emplace_back((float)p.x, (float)p.y);

        cv::Mat A(2*n, 6, CV_64F), B(2*n, 1, CV_64F);
        for (int i = 0; i < n; ++i) {
            A.at<double>(2*i, 0) = geoF[i].x;
            A.at<double>(2*i, 1) = geoF[i].y;
            A.at<double>(2*i, 2) = 1.0;
            A.at<double>(2*i, 3) = 0; A.at<double>(2*i, 4) = 0; A.at<double>(2*i, 5) = 0;
            B.at<double>(2*i, 0) = m_gcpImgPts[i].x;

            A.at<double>(2*i+1, 0) = 0; A.at<double>(2*i+1, 1) = 0; A.at<double>(2*i+1, 2) = 0;
            A.at<double>(2*i+1, 3) = geoF[i].x;
            A.at<double>(2*i+1, 4) = geoF[i].y;
            A.at<double>(2*i+1, 5) = 1.0;
            B.at<double>(2*i+1, 0) = m_gcpImgPts[i].y;
        }
        cv::Mat x;
        if (!cv::solve(A, B, x, cv::DECOMP_SVD)) return false;
        m_affine = x.reshape(1, 2).clone();  // 2x3 CV_64F

        // 计算残差并显示
        double totalErr = 0, maxErr = 0;
        QString detail;
        for (int i = 0; i < n; ++i) {
            double gx = m_gcpGeoPts[i].x, gy = m_gcpGeoPts[i].y;
            double px = m_affine.at<double>(0,0)*gx + m_affine.at<double>(0,1)*gy + m_affine.at<double>(0,2);
            double py = m_affine.at<double>(1,0)*gx + m_affine.at<double>(1,1)*gy + m_affine.at<double>(1,2);
            double dx = px - m_gcpImgPts[i].x;
            double dy = py - m_gcpImgPts[i].y;
            double err = std::sqrt(dx*dx + dy*dy);
            totalErr += err;
            if (err > maxErr) maxErr = err;
            detail += QString("#%1 误差=%2 px\n").arg(i+1).arg(err, 0, 'f', 2);
        }
        double avgErr = totalErr / n;

        QMessageBox::information(this, "配准完成",
            QString("控制点数: %1\n平均误差: %2 px\n最大误差: %3 px\n\n%4")
                .arg(n).arg(avgErr, 0, 'f', 2).arg(maxErr, 0, 'f', 2).arg(detail));
        return true;
    }
};

// ============================================================
// SE_ImageMapCheckDialog
// ============================================================
SE_ImageMapCheckDialog::SE_ImageMapCheckDialog(QgisInterface* iface, QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
    , mIface(iface)
{
    // 复用平台全局 QSS 并追加插件自定义选择器(状态标签/提示框), 不修改平台 qss 文件
    applyPluginStyle(this);
    setWindowTitle("图片地图质检");
    resize(1600, 800);
    buildUi();
    
    // ===== 自动加载 XML 质检规则文件中的所有参数 =====
    // 路径优先级: QgsSettings 中上次使用的路径 -> 默认路径
    // 参考 xml_editor.cpp 的 QgsSettings 持久化做法
    const QString defaultXmlPath = QStringLiteral("");
    QgsSettings settings;
    QString xmlPath = settings.value(
        QStringLiteral("SE_IMAGE_MAP_CHECK/XmlConfigPath"), defaultXmlPath,
        QgsSettings::Section::Plugins).toString();

    // 若记录的路径不存在，回退到默认路径
    if (!QFile::exists(xmlPath)) {
        xmlPath = defaultXmlPath;
    }

    if (QFile::exists(xmlPath) && loadAllConfigFromXml(xmlPath)) {
        addCheckLog(QString("已加载 XML 质检规则: %1").arg(xmlPath));
        if (m_lblXmlConfig) {
            m_lblXmlConfig->setText(QString("XML 规则: %1").arg(QFileInfo(xmlPath).fileName()));
            setLabelStatus(m_lblXmlConfig, "loaded");
        }
    } else {
        addCheckLog(QString("XML 质检规则文件未加载（路径: %1），请点击\"加载 XML 质检规则\"按钮选择").arg(xmlPath));
        if (m_lblXmlConfig) {
            m_lblXmlConfig->setText("XML 规则: 未加载，请点击上方按钮选择");
            setLabelStatus(m_lblXmlConfig, "unloaded");
        }
    }

    // 恢复上次使用的输出目录（QgsSettings 持久化）
    {
        QString lastOutputDir = settings.value(
            QStringLiteral("SE_IMAGE_MAP_CHECK/OutputDir"), QString(),
            QgsSettings::Section::Plugins).toString();
        if (!lastOutputDir.isEmpty() && QDir(lastOutputDir).exists()) {
            m_outputDir = lastOutputDir;
            if (m_editOutputDir) m_editOutputDir->setText(lastOutputDir);
        }
    }
}

SE_ImageMapCheckDialog::~SE_ImageMapCheckDialog()
{
    // 保存当前 XML 质检规则文件路径到 QgsSettings（下次启动时自动恢复）
    if (!m_xmlConfigPath.isEmpty()) {
        QgsSettings settings;
        settings.setValue(QStringLiteral("SE_IMAGE_MAP_CHECK/XmlConfigPath"),
            m_xmlConfigPath, QgsSettings::Section::Plugins);
    }
    // 保存输出目录到 QgsSettings
    {
        QgsSettings settings;
        settings.setValue(QStringLiteral("SE_IMAGE_MAP_CHECK/OutputDir"),
            m_outputDir, QgsSettings::Section::Plugins);
    }
}

void SE_ImageMapCheckDialog::buildUi()
{
    // ===== 通用控件 =====
    m_btnImage = new QPushButton("选择图片");
    m_editImage = new QLineEdit();
    m_editImage->setPlaceholderText("待处理的图片地图路径 (*.tif *.png *.jpg)");

    m_btnAccept = new QPushButton("将结果加入图层");
    m_btnAccept->setEnabled(false);

    m_btnExportReport = new QPushButton("输出质检文件");
    m_btnExportReport->setEnabled(false);

    // 输出路径设置：浏览按钮 + 路径显示框（为空则使用图片同目录默认规则）
    m_btnBrowseOutput = new QPushButton("输出目录...");
    m_btnBrowseOutput->setToolTip("设置质检文件的输出目录（不设置则默认输出到图片同目录）");
    m_editOutputDir = new QLineEdit();
    m_editOutputDir->setPlaceholderText("默认: 图片同目录/图片名_质检输出");
    m_editOutputDir->setReadOnly(true);

    // 一键质检：自动串联境界线检测→注记检测
    m_btnOneClickCheck = new QPushButton("一键质检");
    m_btnOneClickCheck->setToolTip("自动执行境界线检测与注记检测全流程");
    m_progressBar = new QProgressBar();
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFormat("%p%");  // 只显示百分比
    // 样式由 qss 全局接管(科幻风: 青色→橙色渐变进度块)
    // 进度条下方文字标签
    m_progressLabel = new QLabel("待执行");
    m_progressLabel->setAlignment(Qt::AlignCenter);

    // ===== 数据加载板块（图片 + gdb 数据源）=====
    QGroupBox* dataBox = new QGroupBox("数据加载");
    QVBoxLayout* dataLayout = new QVBoxLayout(dataBox);

    m_btnLoadGdb = new QPushButton("加载 gdb 数据源");
    m_lblGdbPath = new QLabel("gdb: 未加载");
    m_lblGdbPath->setWordWrap(true);
    setLabelStatus(m_lblGdbPath, "unloaded");

    // XML 质检规则加载按钮（参考 ScaleCheckerNormativeDialog 的做法）
    // 放在 gdb 数据源下方，作为整体质检方案配置入口
    m_btnLoadXmlConfig = new QPushButton("质检方案配置");
    m_lblXmlConfig = new QLabel("XML 规则: 未加载");
    m_lblXmlConfig->setWordWrap(true);
    setLabelStatus(m_lblXmlConfig, "unloaded");

    dataLayout->addWidget(new QLabel("地图图片:"));
    dataLayout->addWidget(m_btnImage);
    dataLayout->addWidget(m_editImage);
    dataLayout->addSpacing(8);
    dataLayout->addWidget(m_btnLoadXmlConfig);
    dataLayout->addWidget(m_lblXmlConfig);
    dataLayout->addSpacing(8);
    dataLayout->addWidget(m_btnLoadGdb);
    dataLayout->addWidget(m_lblGdbPath);

    // ===== 边界提取板块 =====
    QGroupBox* boundaryBox = new QGroupBox("境界线检测");
    QVBoxLayout* boundaryLayout = new QVBoxLayout(boundaryBox);

    // 境界线图层选择按钮（从已加载 gdb 中选择）
    m_btnSelectBoundaryLayer = new QPushButton("选择境界线图层");
    m_btnSelectBoundaryLayer->setEnabled(false);  // 需先加载 gdb
    m_btnSelectBoundaryLayer->setToolTip("从已加载的 gdb 数据源中选择境界线图层");
    m_lblBoundaryLayer = new QLabel("境界线图层: 未选择");
    m_lblBoundaryLayer->setWordWrap(true);
    setLabelStatus(m_lblBoundaryLayer, "unloaded");

    // 地理配准按钮
    m_btnGeoref = new QPushButton("地理配准...");
    m_btnGeoref->setEnabled(false);  // 需先选择境界线图层

    // 地理配准状态标签（显示控制点数量等配准信息）
    m_lblGeoref = new QLabel("配准: 未配准");
    setLabelStatus(m_lblGeoref, "unloaded");

    boundaryLayout->addWidget(m_btnSelectBoundaryLayer);
    boundaryLayout->addWidget(m_lblBoundaryLayer);
    boundaryLayout->addSpacing(4);
    boundaryLayout->addWidget(m_btnGeoref);
    boundaryLayout->addWidget(m_lblGeoref);
    boundaryLayout->addSpacing(8);

    // 默认颜色不再硬编码，由 XML 质检规则（境界线颜色提取）提供
    // loadAllConfigFromXml 中仅在颜色未设置时(m_currentColor.isValid()==false)加载默认值

    m_btnPickHaloColor = new QPushButton("取内晕线");
    m_btnHaloColor = new QPushButton();
    m_btnHaloColor->setFixedWidth(40);
    m_btnHaloColor->setToolTip("点击选择内晕线颜色（境界线紧邻的晕带）");
    m_editHaloColor = new QLineEdit();
    m_editHaloColor->setPlaceholderText("#RRGGBB");
    m_editHaloColor->setFixedWidth(90);

    m_btnPickOuterHaloColor = new QPushButton("取外晕线");
    m_btnOuterHaloColor = new QPushButton();
    m_btnOuterHaloColor->setFixedWidth(40);
    m_btnOuterHaloColor->setToolTip("点击选择外晕线颜色（内晕带外面的晕带）");
    m_editOuterHaloColor = new QLineEdit();
    m_editOuterHaloColor->setPlaceholderText("#RRGGBB");
    m_editOuterHaloColor->setFixedWidth(90);

    m_lblTolerance = new QLabel("颜色容差: 5");
    m_sliderTolerance = new QSlider(Qt::Horizontal);
    m_sliderTolerance->setRange(5, 100);
    m_sliderTolerance->setValue(5);

    m_btnExtractColor = new QPushButton("检测境界线");
    m_btnExtractColor->setEnabled(false);  // 需先完成地理配准
    m_btnExtractColor->setToolTip("请先完成地理配准后再进行境界线检测");

    QHBoxLayout* haloColorRow = new QHBoxLayout();
    haloColorRow->addWidget(new QLabel("内晕线:"));
    haloColorRow->addWidget(m_btnHaloColor);
    haloColorRow->addWidget(m_editHaloColor);
    haloColorRow->addWidget(m_btnPickHaloColor);

    QHBoxLayout* outerHaloColorRow = new QHBoxLayout();
    outerHaloColorRow->addWidget(new QLabel("外晕线:"));
    outerHaloColorRow->addWidget(m_btnOuterHaloColor);
    outerHaloColorRow->addWidget(m_editOuterHaloColor);
    outerHaloColorRow->addWidget(m_btnPickOuterHaloColor);

    boundaryLayout->addLayout(haloColorRow);
    boundaryLayout->addLayout(outerHaloColorRow);
    boundaryLayout->addWidget(m_lblTolerance);
    boundaryLayout->addWidget(m_sliderTolerance);
    // boundaryLayout->addWidget(m_btnExtractColor);  // 独立执行按钮已移除，仅支持一键质检

    // 境界线异常检测状态（检测在提取后自动执行，无需按钮）
    m_lblAnomalyStatus = new QLabel("");
    m_lblAnomalyStatus->setWordWrap(true);
    setLabelStatus(m_lblAnomalyStatus, "unloaded");
    boundaryLayout->addWidget(m_lblAnomalyStatus);

    // ===== 注记提取板块 =====
    QGroupBox* textBox = new QGroupBox("注记检测");
    QVBoxLayout* textLayout = new QVBoxLayout(textBox);

    m_cbColorChannel = new QCheckBox("分颜色通道识别（识别更全，速度慢）");
    m_cbColorChannel->setToolTip("开启后会把图中黑色、淡蓝、品红、绿色文字分别处理，\n能更完整地识别出浅色文字（如蓝色水系名），但识别速度会变慢");
    m_cbColorChannel->setChecked(true);  // 默认开启, 与原 XML 默认值一致

    m_btnSelectGazetteerLayers = new QPushButton("选择地名库图层");
    m_btnSelectGazetteerLayers->setEnabled(false);  // 需先加载 gdb
    m_btnSelectGazetteerLayers->setToolTip("从已加载的 gdb 中选择图层，提取 NAME 字段构建地名库");
    m_lblGazetteer = new QLabel("地名库: 未加载");
    m_lblGazetteer->setWordWrap(true);
    setLabelStatus(m_lblGazetteer, "unloaded");

    m_btnExtractText = new QPushButton("检测注记");
    m_btnExtractText->setEnabled(false);  // 需先完成地理配准
    m_btnExtractText->setToolTip("请先完成地理配准后再进行注记检测");

    textLayout->addWidget(m_btnSelectGazetteerLayers);
    textLayout->addWidget(m_lblGazetteer);
    textLayout->addWidget(m_cbColorChannel);
    textLayout->addSpacing(6);
    // textLayout->addWidget(m_btnExtractText);  // 独立执行按钮已移除，仅支持一键质检

    // ===== 主布局：左侧控制 + 右侧标签页 =====
    QVBoxLayout* ctrlLayout = new QVBoxLayout();
    ctrlLayout->addWidget(dataBox);
    ctrlLayout->addSpacing(8);
    ctrlLayout->addWidget(boundaryBox);
    ctrlLayout->addSpacing(8);
    ctrlLayout->addWidget(textBox);
    ctrlLayout->addSpacing(8);
    // 输出目录设置栏（按钮 + 路径显示）
    {
        QHBoxLayout* outDirLayout = new QHBoxLayout();
        outDirLayout->setSpacing(4);
        outDirLayout->addWidget(m_btnBrowseOutput);
        outDirLayout->addWidget(m_editOutputDir, 1);
        ctrlLayout->addLayout(outDirLayout);
    }
    // ctrlLayout->addWidget(m_btnExportReport);  // 独立执行按钮已移除，仅支持一键质检
    // ctrlLayout->addWidget(m_btnAccept);
    ctrlLayout->addStretch();
    ctrlLayout->addWidget(m_btnOneClickCheck);
    ctrlLayout->addWidget(m_progressBar);
    ctrlLayout->addWidget(m_progressLabel);

    QWidget* ctrlWidget = new QWidget();
    ctrlWidget->setLayout(ctrlLayout);
    // 左侧控制面板包裹滚动条: 控件较多时压缩高度, 可上下滚动使用全部控件
    QScrollArea* ctrlScroll = new QScrollArea();
    ctrlScroll->setWidgetResizable(true);
    ctrlScroll->setWidget(ctrlWidget);
    ctrlScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ctrlScroll->setFrameShape(QFrame::NoFrame);
    // 控件与滚动条之间留出空隙, 避免视觉拥挤(用 contentsMargins 实现, 不覆盖 qss)
    ctrlWidget->setContentsMargins(0, 0, 8, 0);

    // 右侧预览区（单画面：原图 + 境界线叠加显示）
    m_lblOriginal = new QLabel("原图\n[未加载]");
    m_lblOriginal->setAlignment(Qt::AlignCenter);
    m_lblOriginal->setObjectName("previewLabel");  // 由 qss 定向设置深色背景
    m_lblOriginal->setCursor(Qt::OpenHandCursor);  // 提示可拖拽
    m_lblOriginal->setFocusPolicy(Qt::StrongFocus);  // 接收滚轮事件
    m_lblOriginal->setMouseTracking(true);  // 接收鼠标移动事件
    m_lblOriginal->installEventFilter(this);  // 滚轮缩放 + 左键拖拽平移

    // QLabel 放在 QScrollArea 中：放大后 QLabel sizeHint 增大，滚动条自动出现
    m_scrollOriginal = new QScrollArea();
    m_scrollOriginal->setWidgetResizable(true);
    m_scrollOriginal->setWidget(m_lblOriginal);
    m_scrollOriginal->setAlignment(Qt::AlignCenter);
    m_scrollOriginal->setMinimumSize(360, 300);
    m_scrollOriginal->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scrollOriginal->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    QVBoxLayout* previewLayout = new QVBoxLayout();
    previewLayout->setSpacing(6);
    previewLayout->addWidget(m_scrollOriginal, 1);

    // 预览缩放改为滚轮操作, 移除缩放控制按钮(放大/缩小/1:1/适应窗口)
    QWidget* previewContainer = new QWidget();
    previewContainer->setLayout(previewLayout);

    QScrollArea* scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setWidget(previewContainer);

    // 底部表格：展示未匹配项与纠错项
    m_tblText = new QTableWidget(0, 4);
    m_tblText->setHorizontalHeaderLabels({ "OCR 识别文字", "匹配/纠错结果", "置信度", "状态" });
    m_tblText->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tblText->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tblText->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tblText->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_tblText->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tblText->setSelectionMode(QAbstractItemView::SingleSelection);

    // 标签页切换：预览窗口 / 注记表格
    QTabWidget* tabWidget = new QTabWidget();
    tabWidget->addTab(scrollArea, "处理预览");
    tabWidget->addTab(m_tblText, "异常注记");

    // 主布局：左侧控制(滚动) + 右侧标签页
    QHBoxLayout* mainLayout = new QHBoxLayout(this);
    mainLayout->addWidget(ctrlScroll, 0);
    mainLayout->addWidget(tabWidget, 1);

    // 信号连接
    connect(m_btnImage, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onSelectImage);
    // connect(m_btnExtractText, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onExtractText);  // 独立执行已移除
    connect(m_btnPickHaloColor, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onPickHaloColor);
    connect(m_btnHaloColor, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onHaloColorBtnClicked);
    connect(m_editHaloColor, &QLineEdit::editingFinished, this, &SE_ImageMapCheckDialog::onHaloColorEdited);
    connect(m_btnPickOuterHaloColor, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onPickOuterHaloColor);
    connect(m_btnOuterHaloColor, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onOuterHaloColorBtnClicked);
    connect(m_editOuterHaloColor, &QLineEdit::editingFinished, this, &SE_ImageMapCheckDialog::onOuterHaloColorEdited);
    // connect(m_btnExtractColor, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onExtractColor);  // 独立执行已移除
    // connect(m_btnAccept, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onAccept);
    // connect(m_btnExportReport, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onExportReport);
    connect(m_btnBrowseOutput, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onBrowseOutputDir);
    connect(m_btnSelectGazetteerLayers, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onSelectGazetteerLayers);
    connect(m_btnSelectBoundaryLayer, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onSelectBoundaryLayer);
    connect(m_btnLoadXmlConfig, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onLoadXmlConfig);
    connect(m_btnOneClickCheck, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onOneClickCheck);
    connect(m_btnLoadGdb, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onLoadGdbData);
    connect(m_btnGeoref, &QPushButton::clicked, this, &SE_ImageMapCheckDialog::onOpenGeoreference);
    connect(m_sliderTolerance, &QSlider::valueChanged, this, [this](int v) {
        m_lblTolerance->setText(QString("颜色容差: %1").arg(v));
    });

    // 预览缩放按钮已移除, 缩放改为滚轮操作(见 eventFilter)
}

// 设置预览图：保存原始 QImage 到 m_origImg，并按当前缩放比例渲染
void SE_ImageMapCheckDialog::setPreviewImage(bool isOriginal, const cv::Mat& mat, const QString& title)
{
    // 单画面预览：无论 isOriginal 与否都写入 m_origImg（兼容旧调用点）
    Q_UNUSED(isOriginal);

    if (mat.empty()) {
        m_origImg = QImage();
        m_lblOriginal->setText(title + "\n[空图像]");
        return;
    }
    QImage img = cvMatToQImage(mat);
    if (img.isNull()) {
        m_origImg = QImage();
        m_lblOriginal->setText(title + "\n[转换失败]");
        return;
    }
    m_origImg = img;
    // 新图片加载后自动适应窗口大小
    fitPreviewToWindow();
}

// 按 m_previewScale 重新渲染预览（单画面，保持原始比例）
void SE_ImageMapCheckDialog::applyPreviewScale()
{
    auto render = [](QLabel* label, const QImage& img, double scale) {
        if (img.isNull()) return;
        int w = std::max(1, (int)(img.width()  * scale));
        int h = std::max(1, (int)(img.height() * scale));
        QPixmap pm = QPixmap::fromImage(img)
            .scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        label->setPixmap(pm);
    };
    render(m_lblOriginal, m_origImg, m_previewScale);
}

// 计算"适应窗口"的缩放比例：基于原图尺寸和 scrollArea 可视区域
void SE_ImageMapCheckDialog::fitPreviewToWindow()
{
    // 取原图尺寸作为基准
    int refW = 0, refH = 0;
    if (!m_origImg.isNull()) { refW = m_origImg.width(); refH = m_origImg.height(); }
    if (refW == 0 || refH == 0) return;

    // 用 m_scrollOriginal 的可视区域作为可用空间（单画面占满）
    int availW = 480, availH = 360;
    if (m_scrollOriginal) {
        availW = m_scrollOriginal->viewport()->width()  - 20;
        availH = m_scrollOriginal->viewport()->height() - 20;
    }
    availW = std::max(100, availW);
    availH = std::max(100, availH);

    double sx = (double)availW / refW;
    double sy = (double)availH / refH;
    m_previewScale = std::min(sx, sy);
    if (m_previewScale > 1.0) m_previewScale = 1.0;  // 不超过原始大小
    applyPreviewScale();
}

// 预览标签事件过滤器：滚轮缩放（与按钮同步）+ 左键拖拽平移
bool SE_ImageMapCheckDialog::eventFilter(QObject* watched, QEvent* event)
{
    QLabel* lbl = qobject_cast<QLabel*>(watched);
    if (!lbl || lbl != m_lblOriginal) {
        return QDialog::eventFilter(watched, event);
    }

    // 滚轮缩放（以鼠标位置为中心）
    if (event->type() == QEvent::Wheel) {
        QWheelEvent* we = static_cast<QWheelEvent*>(event);
        const double factor = (we->angleDelta().y() > 0) ? 1.15 : (1.0 / 1.15);
        double newScale = std::clamp(m_previewScale * factor, 0.05, 8.0);
        if (std::abs(newScale - m_previewScale) < 1e-6) return true;

        // 找到所在 QScrollArea 及其滚动条
        QScrollArea* area = nullptr;
        QWidget* p = lbl->parentWidget();
        while (p) {
            if ((area = qobject_cast<QScrollArea*>(p))) break;
            p = p->parentWidget();
        }
        if (!area) return true;

        QScrollBar* hBar = area->horizontalScrollBar();
        QScrollBar* vBar = area->verticalScrollBar();

        // we->pos() 是相对于 QLabel（内容）的坐标，即内容坐标
        // 鼠标在 viewport 中的位置 = 内容坐标 - 滚动条值（缩放前后保持不变）
        const int mx = we->pos().x();
        const int my = we->pos().y();
        const int viewportMouseX = mx - hBar->value();
        const int viewportMouseY = my - vBar->value();
        // 缩放比 = 新缩放比例 / 旧缩放比例
        const double zoomFactor = newScale / m_previewScale;

        m_previewScale = newScale;
        applyPreviewScale();

        // 缩放后鼠标所指内容点的新坐标 = 原坐标 × 缩放比
        const int newMx = (int)(mx * zoomFactor);
        const int newMy = (int)(my * zoomFactor);
        // 调整滚动条使该内容点保持在鼠标的 viewport 位置
        hBar->setValue(newMx - viewportMouseX);
        vBar->setValue(newMy - viewportMouseY);
        return true;  // 拦截事件，不传递给 QScrollArea
    }

    // 左键拖拽平移：通过操作所在 QScrollArea 的滚动条实现
    if (event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            m_dragLabel = lbl;
            m_dragLastPos = me->globalPos();
            lbl->setCursor(Qt::ClosedHandCursor);
            return true;
        }
    } else if (event->type() == QEvent::MouseMove && m_dragLabel == lbl) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        QPoint delta = me->globalPos() - m_dragLastPos;
        m_dragLastPos = me->globalPos();
        // 向上查找最近的 QScrollArea 并调整其滚动条位置
        QWidget* p = lbl->parentWidget();
        while (p) {
            if (QScrollArea* area = qobject_cast<QScrollArea*>(p)) {
                area->horizontalScrollBar()->setValue(
                    area->horizontalScrollBar()->value() - delta.x());
                area->verticalScrollBar()->setValue(
                    area->verticalScrollBar()->value() - delta.y());
                break;
            }
            p = p->parentWidget();
        }
        return true;
    } else if (event->type() == QEvent::MouseButtonRelease && m_dragLabel == lbl) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            m_dragLabel = nullptr;
            lbl->setCursor(Qt::OpenHandCursor);
            return true;
        }
    }

    return QDialog::eventFilter(watched, event);
}

void SE_ImageMapCheckDialog::onSelectImage()
{
    QString path = QFileDialog::getOpenFileName(
        this, "选择图片", "", "图像文件 (*.tif *.tiff *.png *.jpg *.jpeg *.img *.bil);;所有文件 (*.*)");
    if (!path.isEmpty()) {
        m_imagePath = path;
        m_editImage->setText(path);
        m_checkLog.clear(); // 重新选择图片时清空历史日志
        addCheckLog(QString("选择图片: %1").arg(path));

        // 重置配准状态，尝试从缓存恢复
        m_affineValid = false;
        m_gcpImgPts.clear();
        m_gcpGeoPts.clear();
        m_boundaryLoaded = false;
        m_boundaryFeatures.clear();
        m_btnGeoref->setEnabled(false);
        m_btnExtractColor->setEnabled(false);
        m_btnExtractText->setEnabled(false);
        m_lblGdbPath->setText("gdb: 未加载");
        setLabelStatus(m_lblGdbPath, "unloaded");
        m_lblGeoref->setText("配准: 未配准");
        setLabelStatus(m_lblGeoref, "unloaded");
        m_lblBoundaryLayer->setText("境界线图层: 未选择");
        setLabelStatus(m_lblBoundaryLayer, "unloaded");
        m_btnSelectBoundaryLayer->setEnabled(false);

        // 立即加载并预览所选图片
        m_previewScale = 1.0;
        cv::Mat img = imreadCN(m_imagePath);
        if (!img.empty()) {
            setPreviewImage(true, img, "原图");
        } else {
            m_origImg = QImage();
            m_lblOriginal->setText("原图\n[加载失败]");
            addCheckLog("警告: 无法读取所选图片，预览不可用");
        }
        // 重置质检时间（避免上一张图片的时间戳残留）
        m_checkStartTime = QDateTime();
        m_checkEndTime = QDateTime();

        if (loadGeorefCache()) {
            addCheckLog(QString("从缓存恢复地理配准: %1 对控制点").arg(m_gcpImgPts.size()));
            QMessageBox::information(this, "地理配准",
                QString("已从缓存恢复配准结果（%1 对控制点），可直接进行提取。")
                .arg(m_gcpImgPts.size()));
        }

    }
}

// ============================================================
// 坐标转换 & QGIS 图层辅助函数
// ============================================================

QgsCoordinateReferenceSystem SE_ImageMapCheckDialog::getOutputCrs() const
{
    if (m_boundaryCrs.isValid()) return m_boundaryCrs;
    // 回退: 若尚未加载境界线, probe gdb 图层 CRS
    if (!m_gdbPath.isEmpty() && m_boundaryLayerId >= 0) {
        QString uri = m_gdbPath + "|layerid=" + QString::number(m_boundaryLayerId);
        QgsVectorLayer probe(uri, "probe", "ogr");
        if (probe.isValid() && probe.crs().isValid()) {
            return probe.crs();
        }
    }
    return QgsCoordinateReferenceSystem(QStringLiteral("EPSG:4326"));
}

bool SE_ImageMapCheckDialog::imgToGeo(const cv::Point2f& imgPt, cv::Point2d& geoPt) const
{
    if (!m_affineValid) return false;
    // affine: img = M * geo  =>  geo = M^-1 * img
    cv::Mat M3 = cv::Mat::eye(3, 3, CV_64F);
    m_affine.copyTo(M3(cv::Rect(0, 0, 3, 2)));
    cv::Mat M3inv;
    if (!cv::invert(M3, M3inv)) return false;
    geoPt.x = M3inv.at<double>(0,0)*imgPt.x + M3inv.at<double>(0,1)*imgPt.y + M3inv.at<double>(0,2);
    geoPt.y = M3inv.at<double>(1,0)*imgPt.x + M3inv.at<double>(1,1)*imgPt.y + M3inv.at<double>(1,2);
    return true;
}

bool SE_ImageMapCheckDialog::imgToGeo(const std::vector<cv::Point2f>& imgPts,
                                       std::vector<cv::Point2d>& geoPts) const
{
    if (!m_affineValid) return false;
    cv::Mat M3 = cv::Mat::eye(3, 3, CV_64F);
    m_affine.copyTo(M3(cv::Rect(0, 0, 3, 2)));
    cv::Mat M3inv;
    if (!cv::invert(M3, M3inv)) return false;
    double ia = M3inv.at<double>(0,0), ib = M3inv.at<double>(0,1), ic = M3inv.at<double>(0,2);
    double id = M3inv.at<double>(1,0), ie = M3inv.at<double>(1,1), if_ = M3inv.at<double>(1,2);
    geoPts.clear();
    geoPts.reserve(imgPts.size());
    for (auto& p : imgPts)
        geoPts.emplace_back(ia*p.x + ib*p.y + ic, id*p.x + ie*p.y + if_);
    return true;
}

std::vector<std::vector<cv::Point2d>> SE_ImageMapCheckDialog::imgContoursToGeo(
    const std::vector<std::vector<cv::Point2f>>& contours) const
{
    std::vector<std::vector<cv::Point2d>> result;
    if (!m_affineValid) return result;
    result.reserve(contours.size());
    for (auto& line : contours) {
        std::vector<cv::Point2d> geoLine;
        if (!line.empty() && imgToGeo(line, geoLine)) {
            result.push_back(std::move(geoLine));
        }
    }
    return result;
}

QgsVectorLayer* SE_ImageMapCheckDialog::addGeoLineLayer(
    const QString& name, const QColor& color,
    const std::vector<std::vector<cv::Point2d>>& lines, double lineWidth,
    bool addToProject)
{
    QgsCoordinateReferenceSystem crs = getOutputCrs();
    QString uri = QStringLiteral("LineString");
    QgsVectorLayer* layer = new QgsVectorLayer(uri, name, "memory");
    if (!layer->isValid() || lines.empty()) return layer;
    layer->setCrs(crs);

    // 添加属性字段: 序号
    QgsFields f;
    f.append(QgsField("序号", QVariant::Int));
    layer->dataProvider()->addAttributes(f.toList());
    layer->updateFields();

    QgsFeatureList features;
    int seq = 0;
    for (auto& line : lines) {
        if (line.size() < 2) continue;
        QgsPointSequence points;
        for (auto& pt : line)
            points.append(QgsPoint(pt.x, pt.y));
        QgsFeature feat;
        feat.setGeometry(QgsGeometry::fromPolyline(points));
        // 计算线段总长度(地理坐标, 单位与 CRS 一致)
        double totalLen = 0.0;
        for (int k = 1; k < points.size(); ++k) {
            double dx = points[k].x() - points[k-1].x();
            double dy = points[k].y() - points[k-1].y();
            totalLen += std::sqrt(dx * dx + dy * dy);
        }
        feat.setFields(layer->fields());
        feat.setAttribute("序号", ++seq);
        feat.setAttribute("长度", totalLen);
        features << feat;
    }
    layer->dataProvider()->addFeatures(features);
    layer->commitChanges();
    layer->updateExtents();

    // 设置线条样式
    QgsLineSymbol* lineSym = new QgsLineSymbol();
    lineSym->setColor(color);
    lineSym->setWidth(lineWidth);
    QgsSingleSymbolRenderer* renderer = new QgsSingleSymbolRenderer(lineSym);
    layer->setRenderer(renderer);

    if (addToProject)
        QgsProject::instance()->addMapLayer(layer);
    return layer;
}

QgsVectorLayer* SE_ImageMapCheckDialog::createTextLayer(
    const QString& name, const QColor& bufColor,
    std::function<bool(const TextResult&)> filter,
    bool addToProject,
    const std::vector<TextResult>* dataSource)
{
    const std::vector<TextResult>& src = dataSource ? *dataSource : m_textResults;
    if (!m_affineValid || src.empty()) return nullptr;
    QgsCoordinateReferenceSystem crs = getOutputCrs();

    cv::Mat M3 = cv::Mat::eye(3, 3, CV_64F);
    m_affine.copyTo(M3(cv::Rect(0, 0, 3, 2)));
    cv::Mat M3inv;
    cv::invert(M3, M3inv);
    double ia = M3inv.at<double>(0,0), ib = M3inv.at<double>(0,1), ic = M3inv.at<double>(0,2);
    double id = M3inv.at<double>(1,0), ie = M3inv.at<double>(1,1), if_ = M3inv.at<double>(1,2);

    auto pixelToGeo = [&](double cx, double cy) -> QgsPoint {
        return QgsPoint(ia*cx + ib*cy + ic, id*cx + ie*cy + if_);
    };

    QgsFields f;
    f.append(QgsField("text", QVariant::String));
    f.append(QgsField("confidence", QVariant::Double));
    f.append(QgsField("group", QVariant::Int));
    f.append(QgsField("fontsize", QVariant::Int));
    f.append(QgsField("color", QVariant::String));
    f.append(QgsField("verified", QVariant::Int));
    f.append(QgsField("matchedname", QVariant::String));
    f.append(QgsField("originaltext", QVariant::String));
    f.append(QgsField("corrected", QVariant::Int));
    f.append(QgsField("sensitive", QVariant::Int));
    f.append(QgsField("senscat", QVariant::String));
    f.append(QgsField("sensword", QVariant::String));

    QgsVectorLayer* lyr = new QgsVectorLayer(QStringLiteral("Point"), name, "memory");
    lyr->setCrs(crs);
    lyr->dataProvider()->addAttributes(f.toList());
    lyr->updateFields();
    QgsFields pf = lyr->dataProvider()->fields();
    QgsFeatureList feats;
    for (auto& tr : src) {
        if (!filter(tr)) continue;
        double cx = tr.bbox.x + tr.bbox.width / 2.0;
        double cy = tr.bbox.y + tr.bbox.height / 2.0;
        QgsFeature tf(pf);
        tf.setGeometry(QgsGeometry(new QgsPoint(pixelToGeo(cx, cy))));
        tf.setAttribute("text", (tr.verified && !tr.matchedName.isEmpty()) ? tr.matchedName : tr.text);
        tf.setAttribute("confidence", tr.confidence);
        tf.setAttribute("group", tr.group);
        tf.setAttribute("fontsize", tr.fontSize);
        tf.setAttribute("color", QColor((int)tr.avgColor[2], (int)tr.avgColor[1], (int)tr.avgColor[0]).name());
        tf.setAttribute("verified", tr.verified ? 1 : 0);
        tf.setAttribute("matchedname", tr.matchedName);
        tf.setAttribute("originaltext", tr.originalText);
        tf.setAttribute("corrected", tr.corrected ? 1 : 0);
        tf.setAttribute("sensitive", tr.sensitive ? 1 : 0);
        tf.setAttribute("senscat", tr.sensitiveCategory);
        tf.setAttribute("sensword", tr.sensitiveWord);
        feats.append(tf);
    }
    if (feats.isEmpty()) { delete lyr; return nullptr; }
    lyr->dataProvider()->addFeatures(feats);
    lyr->commitChanges(); lyr->updateExtents();
    QgsPalLayerSettings ls;
    ls.fieldName = "text";
    QgsTextFormat fmt;
    fmt.setSize(10); fmt.setColor(QColor(0, 0, 0));
    fmt.buffer().setEnabled(true); fmt.buffer().setSize(1.0); fmt.buffer().setColor(bufColor);
    ls.setFormat(fmt);
    lyr->setLabeling(new QgsVectorLayerSimpleLabeling(ls));
    lyr->setLabelsEnabled(true);
    if (addToProject)
        QgsProject::instance()->addMapLayer(lyr);
    return lyr;
}

void SE_ImageMapCheckDialog::addCheckLog(const QString& msg)
{
    QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    m_checkLog << QString("%1 - %2").arg(ts).arg(msg);
}

// 从 XML 质检规则文件加载全部参数（系统启动时自动调用）
// 使用 configByIdReader 按 id 解析所有质检项（601-605），同时联动加载敏感词表
// 路径持久化于 QgsSettings（key: SE_IMAGE_MAP_CHECK/XmlConfigPath）
bool SE_ImageMapCheckDialog::loadAllConfigFromXml(const QString& xmlPath)
{
    if (xmlPath.isEmpty() || !QFile::exists(xmlPath)) {
        qWarning() << "XML 质检规则文件不存在:" << xmlPath;
        return false;
    }

    // 创建配置读取器（如尚未创建），parent=this 由 Qt 自动管理生命周期
    if (!m_configReader) {
        m_configReader = new XmlConfigReader(this);
    }

    // 加载 XML 中所有质检项参数
    if (!m_configReader->loadConfig(xmlPath)) {
        qWarning() << "XML 质检规则文件加载失败:" << xmlPath;
        return false;
    }

    m_xmlConfigPath = xmlPath;

    // 同步加载按 id 查询的缓存（与 m_configReader 并存，互不影响）
    // 失败不阻断流程，业务侧查询失败时各自走默认值
    configByIdReader().load(xmlPath);

    // 联动加载敏感词词表（检查项 604，从 configByIdReader 缓存读取）
    // 同时更新 m_sensitiveRulesPath，与 m_xmlConfigPath 保持一致
    loadSensitiveWordsFromXml(xmlPath);

    // 加载默认颜色（质检项 101：境界线颜色提取）
    // 仅当当前颜色未设置时才应用 XML 中的默认值，避免覆盖取色按钮或缓存中的颜色
    // 流程: buildUi(不设色) → loadAllConfigFromXml(设默认色) → onLoadGdbData(缓存色覆盖，如有)
    if (!m_currentColor.isValid()) {
        QString hex = configByIdReader().getRuleStringValue(ITEM_ID_BOUNDARY_COLOR, "境界线默认颜色", "");
        QColor c(hex);
        if (c.isValid()) {
            setCurrentColor(c);
            qDebug() << "[XML] 加载境界线默认颜色:" << hex;
        }
    }
    if (!m_currentHaloColor.isValid()) {
        QString hex = configByIdReader().getRuleStringValue(ITEM_ID_BOUNDARY_COLOR, "内晕线默认颜色", "");
        QColor c(hex);
        if (c.isValid()) {
            setCurrentHaloColor(c);
            qDebug() << "[XML] 加载内晕线默认颜色:" << hex;
        }
    }
    if (!m_currentOuterHaloColor.isValid()) {
        QString hex = configByIdReader().getRuleStringValue(ITEM_ID_BOUNDARY_COLOR, "外晕线默认颜色", "");
        QColor c(hex);
        if (c.isValid()) {
            setCurrentOuterHaloColor(c);
            qDebug() << "[XML] 加载外晕线默认颜色:" << hex;
        }
    }

    qDebug() << "XML 质检规则加载完成:" << xmlPath
             << "可用比例尺数:" << m_configReader->getAvailableScales().size()
             << "敏感词分类数:" << m_sensitiveWords.size();
    return true;
}

// 从 XML 质检规则文件加载敏感词词表
// 直接从已加载的 id 缓存（configByIdReader）读取检查项 604 的全部 rule
// 每个 rule: name=分类名, value=逗号分隔的词列表
// 注意：调用前需确保 configByIdReader().load(xmlPath) 已执行（由 loadAllConfigFromXml 保证）
bool SE_ImageMapCheckDialog::loadSensitiveWordsFromXml(const QString& xmlPath)
{
    const QMap<QString, QString> rules =
        configByIdReader().getRulesByItemId(ITEM_ID_SENSITIVE_WORDS);

    QList<QPair<QString, QStringList>> result;
    for (auto it = rules.constBegin(); it != rules.constEnd(); ++it) {
        const QString& categoryName = it.key();
        QStringList words = it.value().split(',', Qt::SkipEmptyParts);
        QStringList trimmedWords;
        for (const QString& w : words) {
            QString tw = w.trimmed();
            if (!tw.isEmpty()) trimmedWords << tw;
        }
        if (!trimmedWords.isEmpty()) {
            result << qMakePair(categoryName, trimmedWords);
        }
    }

    if (result.isEmpty()) {
        return false;
    }
    m_sensitiveWords = result;
    m_sensitiveRulesPath = xmlPath;
    return true;
}

// 对所有 OCR 提取文本执行敏感文字合规性检查
// 命中者标记 tr.sensitive=true 并记录分类和命中词，不参与后续地名匹配
int SE_ImageMapCheckDialog::checkSensitiveWordsInResults(std::vector<TextResult>& results)
{
    if (m_sensitiveWords.isEmpty()) return 0;
    int hitCount = 0;
    for (auto& tr : results) {
        if (tr.text.isEmpty()) continue;
        for (const auto& cat : m_sensitiveWords) {
            const QString& categoryName = cat.first;
            const QStringList& words = cat.second;
            for (const QString& w : words) {
                if (w.isEmpty()) continue;
                if (tr.text.contains(w, Qt::CaseSensitive)) {
                    tr.sensitive = true;
                    tr.sensitiveCategory = categoryName;
                    tr.sensitiveWord = w;
                    ++hitCount;
                    break; // 命中一个即标记，记录第一个命中的分类和词
                }
            }
            if (tr.sensitive) break;
        }
    }
    return hitCount;
}

void SE_ImageMapCheckDialog::checkSensitiveWords()
{
    m_sensitiveHitCount = checkSensitiveWordsInResults(m_textResults);
}

void SE_ImageMapCheckDialog::cacheResult(const cv::Mat& img,
                                         const std::vector<std::vector<cv::Point2f>>& contours,
                                         const QString& layerName)
{
    m_lastImg = img.clone();
    m_lastContours = contours;
    m_lastLayerName = layerName;
    m_hasResult = true;
    m_btnAccept->setEnabled(true);
    m_btnExportReport->setEnabled(true);
    addCheckLog(QString("境界线提取完成: %1 条轮廓, 图层名: %2").arg(contours.size()).arg(layerName));
    // 提取后自动触发境界线异常检测（若已地理配准且加载了标准境界线）
    runBoundaryAnomalyCheck();
}

// ===== 地理配准实现 =====

// 几何类型枚举转字符串
static QString geomTypeToString(QgsWkbTypes::GeometryType t)
{
    switch (t) {
    case QgsWkbTypes::PointGeometry:    return "Point";
    case QgsWkbTypes::LineGeometry:     return "Line";
    case QgsWkbTypes::PolygonGeometry:  return "Polygon";
    default:                            return "Unknown";
    }
}

bool SE_ImageMapCheckDialog::listGdbSubLayers(const QString& gdbPath,
                                              QStringList& layerNames,
                                              QStringList& layerTypes)
{
    layerNames.clear();
    layerTypes.clear();

    QgsVectorLayer probe(gdbPath, "probe", "ogr");
    if (!probe.isValid() || !probe.dataProvider()) {
        qDebug("[Georef] probe 图层无效: %s", gdbPath.toUtf8().constData());
        return false;
    }
    QStringList subLayers = probe.dataProvider()->subLayers();
    if (subLayers.isEmpty()) {
        qDebug("[Georef] subLayers 为空");
        return false;
    }

    for (int i = 0; i < subLayers.size(); ++i) {
        QString uri = gdbPath + "|layerid=" + QString::number(i);
        QgsVectorLayer layer(uri, QStringLiteral("sub_%1").arg(i), "ogr");
        if (!layer.isValid()) continue;
        // 从 subLayers 字符串解析真实图层名
        // 常见格式: "layerid=0::name=图层名::featurecount=100" 或 "0::图层名::100::..."
        QString realName = layer.name();
        const QString& sub = subLayers[i];
        qDebug("[GDB] subLayers[%d] = '%s'", i, sub.toUtf8().constData());
        // OGR provider 格式: "索引!!::!!图层名!!::!!要素数!!::!!几何类型!!::!!Shape!!::!!"
        QStringList parts = sub.split("!!::!!");
        // parts[1] 是图层名（索引 0 是数字序号）
        if (parts.size() >= 2) {
            QString candidate = parts[1].trimmed();
            if (!candidate.isEmpty()) realName = candidate;
        }
        if (realName == QStringLiteral("sub_%1").arg(i)) {
            realName = QStringLiteral("图层 %1").arg(i);
        }
        layerNames << realName;
        layerTypes << geomTypeToString(layer.geometryType());
    }
    return !layerNames.isEmpty();
}

int SE_ImageMapCheckDialog::loadBoundaryLayer(const QString& gdbPath, int layerId)
{
    m_boundaryFeatures.clear();
    m_boundaryLoaded = false;

    if (gdbPath.isEmpty() || layerId < 0) return 0;

    QString uri = gdbPath + "|layerid=" + QString::number(layerId);
    QgsVectorLayer layer(uri, QStringLiteral("boundary_%1").arg(layerId), "ogr");
    if (!layer.isValid()) {
        qDebug("[Georef] 境界线图层 %d 无效", layerId);
        return 0;
    }

    m_boundaryCrs = layer.crs();

    // 计算范围
    QgsRectangle ext = layer.extent();
    if (!ext.isNull()) {
        m_boundaryExtent = cv::Rect2d(ext.xMinimum(), ext.yMinimum(),
                                      ext.width(), ext.height());
    }

    int featCount = 0;
    int totalPts = 0;
    QgsFeature feat;
    QgsFeatureIterator it = layer.getFeatures();
    while (it.nextFeature(feat)) {
        QgsGeometry geom = feat.geometry();
        if (geom.isNull() || !geom.constGet()) continue;

        // 转换为多段线：对线图层直接取 vertices，对面图层取外环
        const QgsAbstractGeometry* absGeom = geom.constGet()->simplifiedTypeRef();
        if (!absGeom) continue;

        // 对 multipart 几何，按 part 分段
        // 遍历所有 part，每个 part 单独提取顶点
        int partCount = absGeom->partCount();
        for (int partIdx = 0; partIdx < partCount; ++partIdx) {
            std::vector<cv::Point2d> curPart;
            int ringCount = absGeom->ringCount(partIdx);
            for (int ringIdx = 0; ringIdx < ringCount; ++ringIdx) {
                int vCount = absGeom->vertexCount(partIdx, ringIdx);
                for (int vIdx = 0; vIdx < vCount; ++vIdx) {
                    QgsVertexId vid(partIdx, ringIdx, vIdx);
                    QgsPoint pt = absGeom->vertexAt(vid);
                    if (!pt.isEmpty()) {
                        curPart.emplace_back(pt.x(), pt.y());
                    }
                }
                // 每个环作为独立段
                if (curPart.size() >= 2) {
                    m_boundaryFeatures.push_back(curPart);
                    totalPts += (int)curPart.size();
                    curPart.clear();
                }
            }
        }
        ++featCount;
    }

    m_boundaryLoaded = !m_boundaryFeatures.empty();
    qDebug("[Georef] 境界线图层加载: %d 要素, %d 段, %d 顶点",
           featCount, (int)m_boundaryFeatures.size(), totalPts);
    return featCount;
}

void SE_ImageMapCheckDialog::onLoadGdbData()
{
    addCheckLog("开始加载gdb数据源");
    // 让用户选择 gdb 文件夹
    QString path = QFileDialog::getExistingDirectory(this, "选择 gdb 文件夹",
        QString(), QFileDialog::ShowDirsOnly);
    if (path.isEmpty()) return;

    // 验证 gdb 可读且有子图层
    QStringList names, types;
    if (!listGdbSubLayers(path, names, types)) {
        QMessageBox::warning(this, "错误", "无法读取 gdb 或无可用子图层");
        return;
    }

    // 仅记录 gdb 路径，不自动选择境界线图层
    m_gdbPath = path;
    m_boundaryLayerId = -1;
    m_boundaryLayerName.clear();
    m_boundaryFeatures.clear();
    m_boundaryLoaded = false;

    m_lblGdbPath->setText(QString("gdb: %1\n(%2 个子图层)")
        .arg(QFileInfo(path).fileName()).arg(names.size()));
    setLabelStatus(m_lblGdbPath, "loaded");

    // 启用图层选择按钮
    m_btnSelectBoundaryLayer->setEnabled(true);
    m_btnSelectGazetteerLayers->setEnabled(true);

    addCheckLog(QString("加载gdb数据源: %1 (%2 个子图层)").arg(path).arg(names.size()));
}

// 从已加载 gdb 中选择境界线图层（带预览的单选对话框）
void SE_ImageMapCheckDialog::onSelectBoundaryLayer()
{
    if (m_gdbPath.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先加载 gdb 数据源");
        return;
    }

    QStringList names, types;
    if (!listGdbSubLayers(m_gdbPath, names, types)) {
        QMessageBox::warning(this, "错误", "无法读取 gdb 子图层");
        return;
    }

    // ===== 带预览的子图层选择对话框 =====
    QDialog selDlg(this);
    selDlg.setWindowTitle("选择境界线子图层");
    selDlg.resize(1000, 600);

    QVBoxLayout* outerLay = new QVBoxLayout(&selDlg);
    QHBoxLayout* hLay = new QHBoxLayout();

    // 左侧：子图层列表
    QVBoxLayout* leftLay = new QVBoxLayout();
    leftLay->addWidget(new QLabel("子图层列表:"));
    QListWidget* listWidget = new QListWidget();
    for (int i = 0; i < names.size(); ++i) {
        QString text = QString("[%1] %2 (%3)").arg(i).arg(names[i]).arg(types[i]);
        QListWidgetItem* item = new QListWidgetItem(text);
        item->setData(Qt::UserRole, i);
        listWidget->addItem(item);
    }
    listWidget->setMaximumWidth(300);
    leftLay->addWidget(listWidget, 1);

    // 右侧：预览区
    QVBoxLayout* rightLay = new QVBoxLayout();
    rightLay->addWidget(new QLabel("预览:"));
    QLabel* previewLabel = new QLabel("请选择左侧图层查看预览");
    previewLabel->setAlignment(Qt::AlignCenter);
    previewLabel->setMinimumSize(600, 420);
    previewLabel->setObjectName("previewLabel");  // 由 qss 定向设置深色背景
    rightLay->addWidget(previewLabel, 1);

    QLabel* infoLabel = new QLabel("");
    infoLabel->setWordWrap(true);
    setLabelStatus(infoLabel, "muted");
    rightLay->addWidget(infoLabel);

    hLay->addLayout(leftLay);
    hLay->addLayout(rightLay, 1);
    outerLay->addLayout(hLay, 1);

    QDialogButtonBox* btnBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    btnBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    outerLay->addWidget(btnBox);

    // 预览渲染函数
    auto renderPreview = [this](int layerId, QString& infoOut) -> QImage {
        QString uri = m_gdbPath + "|layerid=" + QString::number(layerId);
        QgsVectorLayer layer(uri, QStringLiteral("preview_%1").arg(layerId), "ogr");
        if (!layer.isValid()) {
            infoOut = "图层无效";
            return QImage();
        }

        QgsRectangle ext = layer.extent();
        if (ext.isNull() || ext.width() < 1e-9 || ext.height() < 1e-9) {
            infoOut = "图层范围为空";
            return QImage();
        }

        std::vector<std::vector<cv::Point2d>> feats;
        int featCount = 0;
        int totalPts = 0;
        const int MAX_PREVIEW_FEATS = 500;
        QgsFeature feat;
        QgsFeatureIterator it = layer.getFeatures();
        while (it.nextFeature(feat) && featCount < MAX_PREVIEW_FEATS) {
            QgsGeometry geom = feat.geometry();
            if (geom.isNull() || !geom.constGet()) continue;
            const QgsAbstractGeometry* absGeom = geom.constGet()->simplifiedTypeRef();
            if (!absGeom) continue;

            int partCount = absGeom->partCount();
            for (int p = 0; p < partCount; ++p) {
                std::vector<cv::Point2d> curPart;
                int ringCount = absGeom->ringCount(p);
                for (int r = 0; r < ringCount; ++r) {
                    int vCount = absGeom->vertexCount(p, r);
                    for (int v = 0; v < vCount; ++v) {
                        QgsVertexId vid(p, r, v);
                        QgsPoint pt = absGeom->vertexAt(vid);
                        if (!pt.isEmpty()) curPart.emplace_back(pt.x(), pt.y());
                    }
                    if (curPart.size() >= 2) {
                        feats.push_back(curPart);
                        totalPts += (int)curPart.size();
                        curPart.clear();
                    }
                }
            }
            ++featCount;
        }

        if (feats.empty()) {
            infoOut = "无几何要素";
            return QImage();
        }

        const int W = 600, H = 420;
        cv::Mat img(H, W, CV_8UC3, cv::Scalar(255, 255, 255));

        double minX = ext.xMinimum(), minY = ext.yMinimum();
        double extW = ext.width(), extH = ext.height();
        double margin = 0.05;
        minX -= extW * margin; minY -= extH * margin;
        extW *= (1 + 2 * margin); extH *= (1 + 2 * margin);

        auto toView = [&](double gx, double gy) {
            double vx = (gx - minX) / extW * W;
            double vy = H - (gy - minY) / extH * H;
            return cv::Point2d(vx, vy);
        };

        for (auto& f : feats) {
            for (size_t i = 1; i < f.size(); ++i) {
                cv::line(img, toView(f[i-1].x, f[i-1].y),
                         toView(f[i].x, f[i].y), cv::Scalar(0, 100, 255), 1, cv::LINE_AA);
            }
        }

        infoOut = QString("图层: %1\n要素: %2%3, 段: %4, 顶点: %5\n范围: (%6, %7) - (%8, %9)")
            .arg(layer.name())
            .arg(featCount)
            .arg(featCount >= MAX_PREVIEW_FEATS ? "+" : "")
            .arg(feats.size())
            .arg(totalPts)
            .arg(ext.xMinimum(), 0, 'f', 2).arg(ext.yMinimum(), 0, 'f', 2)
            .arg(ext.xMaximum(), 0, 'f', 2).arg(ext.yMaximum(), 0, 'f', 2);

        return cvMatToQImage(img);
    };

    QObject::connect(listWidget, &QListWidget::currentItemChanged,
        [&renderPreview, previewLabel, infoLabel, btnBox](QListWidgetItem* cur, QListWidgetItem*) {
            if (!cur) return;
            int id = cur->data(Qt::UserRole).toInt();
            QString info;
            QImage qimg = renderPreview(id, info);
            if (!qimg.isNull()) {
                previewLabel->setPixmap(QPixmap::fromImage(qimg).scaled(
                    previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
                previewLabel->setText("");
                setLabelStatus(infoLabel, "muted");
                btnBox->button(QDialogButtonBox::Ok)->setEnabled(true);
            } else {
                previewLabel->setText("无法预览: " + info);
                previewLabel->setPixmap(QPixmap());
                setLabelStatus(infoLabel, "error");
                btnBox->button(QDialogButtonBox::Ok)->setEnabled(false);
            }
            infoLabel->setText(info);
        });

    QObject::connect(btnBox, &QDialogButtonBox::accepted, &selDlg, &QDialog::accept);
    QObject::connect(btnBox, &QDialogButtonBox::rejected, &selDlg, &QDialog::reject);

    // 默认选中第一个 Line/Polygon 图层
    int defaultRow = 0;
    for (int i = 0; i < types.size(); ++i) {
        if (types[i] == "Line" || types[i] == "Polygon") { defaultRow = i; break; }
    }
    listWidget->setCurrentRow(defaultRow);

    if (selDlg.exec() != QDialog::Accepted) return;

    QListWidgetItem* selItem = listWidget->currentItem();
    if (!selItem) return;
    int layerId = selItem->data(Qt::UserRole).toInt();
    QString sel = selItem->text();

    // 正式加载境界线图层
    int featCount = loadBoundaryLayer(m_gdbPath, layerId);
    if (featCount == 0) {
        QMessageBox::warning(this, "错误", "境界线图层加载失败或无要素");
        return;
    }

    m_boundaryLayerId = layerId;
    m_boundaryLayerName = sel;
    m_lblBoundaryLayer->setText(QString("境界线图层: %1\n(%2 要素, %3 段)")
        .arg(sel).arg(featCount).arg(m_boundaryFeatures.size()));
    setLabelStatus(m_lblBoundaryLayer, "loaded");
    addCheckLog(QString("选择境界线图层: %1 (%2 要素, %3 段)")
        .arg(sel).arg(featCount).arg(m_boundaryFeatures.size()));

    // 启用地理配准按钮
    m_btnGeoref->setEnabled(true);
}

// 从已加载 gdb 中选择图层构建地名库（多选对话框）
void SE_ImageMapCheckDialog::onSelectGazetteerLayers()
{
    if (m_gdbPath.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先加载 gdb 数据源");
        return;
    }

    QStringList names, types;
    if (!listGdbSubLayers(m_gdbPath, names, types)) {
        QMessageBox::warning(this, "错误", "无法读取 gdb 子图层");
        return;
    }

    // ===== 多选图层对话框 =====
    QDialog selDlg(this);
    selDlg.setWindowTitle("选择地名库图层（提取 NAME 字段）");
    selDlg.resize(500, 400);

    QVBoxLayout* lay = new QVBoxLayout(&selDlg);
    lay->addWidget(new QLabel("勾选要提取 NAME 字段构建地名库的图层："));

    // 用 QListWidget 多选
    QListWidget* listWidget = new QListWidget();
    listWidget->setSelectionMode(QAbstractItemView::NoSelection);  // 用 checkbox 控制
    bool hasPrevSelection = !m_gazetteerLayerIds.isEmpty();
    for (int i = 0; i < names.size(); ++i) {
        QString text = QString("[%1] %2 (%3)").arg(i).arg(names[i]).arg(types[i]);
        QListWidgetItem* item = new QListWidgetItem(text, listWidget);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        item->setData(Qt::UserRole, i);
        // 已有选择 → 恢复已选图层；否则默认勾选 Point 图层（通常地名在点图层）
        if (hasPrevSelection) {
            if (m_gazetteerLayerIds.contains(i)) item->setCheckState(Qt::Checked);
        } else if (types[i] == "Point") {
            item->setCheckState(Qt::Checked);
        }
    }
    lay->addWidget(listWidget);

    // 统计信息标签
    QLabel* infoLabel = new QLabel("已选 0 个图层");
    setLabelStatus(infoLabel, "muted");
    lay->addWidget(infoLabel);

    QDialogButtonBox* btnBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    lay->addWidget(btnBox);

    // 更新已选数量
    QObject::connect(listWidget, &QListWidget::itemChanged,
        [listWidget, infoLabel](QListWidgetItem*) {
            int checked = 0;
            for (int i = 0; i < listWidget->count(); ++i) {
                if (listWidget->item(i)->checkState() == Qt::Checked) ++checked;
            }
            infoLabel->setText(QString("已选 %1 个图层").arg(checked));
        });
    // 初始触发一次
    emit listWidget->itemChanged(nullptr);

    QObject::connect(btnBox, &QDialogButtonBox::accepted, &selDlg, &QDialog::accept);
    QObject::connect(btnBox, &QDialogButtonBox::rejected, &selDlg, &QDialog::reject);

    if (selDlg.exec() != QDialog::Accepted) return;

    // 收集选中的图层 ID
    QList<int> selectedIds;
    QStringList selectedNames;
    for (int i = 0; i < listWidget->count(); ++i) {
        QListWidgetItem* item = listWidget->item(i);
        if (item->checkState() == Qt::Checked) {
            selectedIds << item->data(Qt::UserRole).toInt();
            selectedNames << item->text();
        }
    }

    if (selectedIds.isEmpty()) {
        QMessageBox::warning(this, "提示", "未选择任何图层");
        return;
    }

    // 从选中图层加载地名库
    int count = loadGazetteerFromGdbLayers(m_gdbPath, selectedIds);
    if (count > 0) {
        m_gazetteerLayerIds = selectedIds;
        m_lblGazetteer->setText(QString("地名库: %1 个图层\n(%2 条地名)")
            .arg(selectedIds.size()).arg(count));
        setLabelStatus(m_lblGazetteer, "loaded");
        addCheckLog(QString("从 %1 个图层加载地名库: %2 条地名")
            .arg(selectedIds.size()).arg(count));
    } else {
        m_lblGazetteer->setText("地名库: 所选图层无 NAME 字段或无有效地名");
        setLabelStatus(m_lblGazetteer, "error");
        addCheckLog(QString("地名库加载失败: 所选图层无 NAME 字段或无有效地名"));
    }
}

std::vector<std::vector<cv::Point2f>> SE_ImageMapCheckDialog::transformBoundaryToImage() const
{
    std::vector<std::vector<cv::Point2f>> result;
    if (!m_affineValid || m_boundaryFeatures.empty()) return result;

    double a = m_affine.at<double>(0,0), b = m_affine.at<double>(0,1), c = m_affine.at<double>(0,2);
    double d = m_affine.at<double>(1,0), e = m_affine.at<double>(1,1), f = m_affine.at<double>(1,2);

    result.reserve(m_boundaryFeatures.size());
    for (auto& feat : m_boundaryFeatures) {
        std::vector<cv::Point2f> pl;
        pl.reserve(feat.size());
        for (auto& p : feat) {
            float px = (float)(a * p.x + b * p.y + c);
            float py = (float)(d * p.x + e * p.y + f);
            pl.emplace_back(px, py);
        }
        result.push_back(std::move(pl));
    }
    return result;
}

// 保存地理配准结果到 JSON（图片同目录，文件名加 .georef.json 后缀）
void SE_ImageMapCheckDialog::saveGeorefCache() const
{
    if (m_imagePath.isEmpty() || !m_affineValid) return;

    QString cachePath = m_imagePath + ".georef.json";
    QJsonObject root;
    root["imagePath"] = m_imagePath;
    root["gdbPath"] = m_gdbPath;
    root["layerId"] = m_boundaryLayerId;
    root["layerName"] = m_boundaryLayerName;

    // 仿射矩阵 2x3
    QJsonArray matArr;
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 3; ++c) {
            matArr.append(m_affine.at<double>(r, c));
        }
    }
    root["affine"] = matArr;

    // 控制点
    QJsonArray imgPts, geoPts;
    for (size_t i = 0; i < m_gcpImgPts.size() && i < m_gcpGeoPts.size(); ++i) {
        imgPts.append(QJsonArray{ m_gcpImgPts[i].x, m_gcpImgPts[i].y });
        geoPts.append(QJsonArray{ m_gcpGeoPts[i].x, m_gcpGeoPts[i].y });
    }
    root["gcpImgPts"] = imgPts;
    root["gcpGeoPts"] = geoPts;

    // 地名库图层 ID 列表（用于从缓存恢复时自动重建地名库）
    if (!m_gazetteerLayerIds.isEmpty()) {
        QJsonArray gazIds;
        for (int id : m_gazetteerLayerIds) gazIds.append(id);
        root["gazetteerLayerIds"] = gazIds;
    }

    // 颜色选取信息（境界线、内晕、外晕，#RRGGBB 格式）
    if (m_currentColor.isValid())         root["colorBoundary"] = m_currentColor.name();
    if (m_currentHaloColor.isValid())     root["colorInnerHalo"] = m_currentHaloColor.name();
    if (m_currentOuterHaloColor.isValid()) root["colorOuterHalo"] = m_currentOuterHaloColor.name();

    // XML 质检规则文件路径（用于从缓存恢复时自动重载该图片对应的规则）
    if (!m_xmlConfigPath.isEmpty()) root["xmlConfigPath"] = m_xmlConfigPath;

    QJsonDocument doc(root);
    QFile file(cachePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(doc.toJson());
        qDebug("[Georef] 配准结果已缓存到 %s", qPrintable(cachePath));
    }
}

// 从 JSON 加载地理配准结果（若与当前图片路径匹配）
bool SE_ImageMapCheckDialog::loadGeorefCache()
{
    if (m_imagePath.isEmpty()) return false;

    QString cachePath = m_imagePath + ".georef.json";
    QFile file(cachePath);
    if (!file.open(QIODevice::ReadOnly)) return false;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qDebug("[Georef] 缓存文件解析失败: %s", qPrintable(err.errorString()));
        return false;
    }

    QJsonObject root = doc.object();
    // 路径必须匹配
    if (root.value("imagePath").toString() != m_imagePath) return false;

    QString gdbPath = root.value("gdbPath").toString();
    int layerId = root.value("layerId").toInt(-1);
    QString layerName = root.value("layerName").toString();
    if (gdbPath.isEmpty() || layerId < 0) return false;

    // 检查 gdb 文件存在
    QDir gdbDir(gdbPath);
    if (!gdbDir.exists()) {
        qDebug("[Georef] 缓存的 gdb 路径不存在: %s", qPrintable(gdbPath));
        return false;
    }

    // 读取 gdb 子图层数量（用于 UI 显示）
    QStringList subNames, subTypes;
    listGdbSubLayers(gdbPath, subNames, subTypes);

    // 加载境界线
    int featCount = loadBoundaryLayer(gdbPath, layerId);
    if (featCount == 0) {
        qDebug("[Georef] 缓存加载境界线失败");
        return false;
    }

    m_gdbPath = gdbPath;
    m_boundaryLayerId = layerId;
    m_boundaryLayerName = layerName;

    // 加载仿射矩阵
    QJsonArray matArr = root.value("affine").toArray();
    if (matArr.size() != 6) return false;
    m_affine = cv::Mat(2, 3, CV_64F);
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 3; ++c) {
            m_affine.at<double>(r, c) = matArr.at(r * 3 + c).toDouble();
        }
    }
    m_affineValid = true;

    // 加载控制点
    QJsonArray imgPts = root.value("gcpImgPts").toArray();
    QJsonArray geoPts = root.value("gcpGeoPts").toArray();
    m_gcpImgPts.clear();
    m_gcpGeoPts.clear();
    for (int i = 0; i < imgPts.size() && i < geoPts.size(); ++i) {
        auto ip = imgPts.at(i).toArray();
        auto gp = geoPts.at(i).toArray();
        if (ip.size() < 2 || gp.size() < 2) continue;
        m_gcpImgPts.emplace_back((float)ip.at(0).toDouble(), (float)ip.at(1).toDouble());
        m_gcpGeoPts.emplace_back(gp.at(0).toDouble(), gp.at(1).toDouble());
    }

    // 从缓存恢复地名库图层 ID 列表，并自动重建地名库
    QList<int> cachedGazIds;
    if (root.contains("gazetteerLayerIds")) {
        QJsonArray gazArr = root.value("gazetteerLayerIds").toArray();
        for (const QJsonValue& v : gazArr) {
            cachedGazIds << v.toInt(-1);
        }
    }
    if (!cachedGazIds.isEmpty()) {
        int gazCount = loadGazetteerFromGdbLayers(gdbPath, cachedGazIds);
        if (gazCount > 0) {
            m_gazetteerLayerIds = cachedGazIds;
            m_lblGazetteer->setText(QString("地名库: %1 个图层\n(%2 条地名)[缓存]")
                .arg(cachedGazIds.size()).arg(gazCount));
            setLabelStatus(m_lblGazetteer, "loaded");
            addCheckLog(QString("从缓存恢复地名库: %1 个图层, %2 条地名")
                .arg(cachedGazIds.size()).arg(gazCount));
        } else {
            m_lblGazetteer->setText("地名库: 缓存的图层无 NAME 字段或无有效地名");
            setLabelStatus(m_lblGazetteer, "error");
        }
    } else {
        m_lblGazetteer->setText("地名库: 未加载（请选择图层）");
        setLabelStatus(m_lblGazetteer, "unloaded");
    }

    // 更新 UI
    m_lblGdbPath->setText(QString("gdb: %1\n(%2 个子图层)")
        .arg(QFileInfo(gdbPath).fileName()).arg(subNames.size()));
    setLabelStatus(m_lblGdbPath, "loaded");
    m_lblGeoref->setText(QString("配准: 已配准(缓存) - %1 对控制点").arg(m_gcpImgPts.size()));
    setLabelStatus(m_lblGeoref, "loaded");
    m_lblBoundaryLayer->setText(QString("境界线图层: %1\n(%2 要素, %3 段)[缓存]")
        .arg(layerName).arg(featCount).arg(m_boundaryFeatures.size()));
    setLabelStatus(m_lblBoundaryLayer, "loaded");
    m_btnSelectBoundaryLayer->setEnabled(true);
    m_btnSelectGazetteerLayers->setEnabled(true);
    m_btnGeoref->setEnabled(true);
    m_btnExtractColor->setEnabled(true);
    m_btnExtractText->setEnabled(true);

    // 从缓存恢复 XML 质检规则文件路径（若与当前不同则自动重载）
    QString cachedXmlPath = root.value("xmlConfigPath").toString();
    if (!cachedXmlPath.isEmpty() && cachedXmlPath != m_xmlConfigPath
        && QFile::exists(cachedXmlPath)) {
        if (loadAllConfigFromXml(cachedXmlPath)) {
            if (m_lblXmlConfig) {
                m_lblXmlConfig->setText(QString("XML 规则: %1 [缓存]")
                    .arg(QFileInfo(cachedXmlPath).fileName()));
                setLabelStatus(m_lblXmlConfig, "loaded");
            }
            addCheckLog(QString("从缓存恢复 XML 质检规则: %1").arg(cachedXmlPath));
        } else {
            addCheckLog(QString("缓存中的 XML 质检规则加载失败: %1").arg(cachedXmlPath));
        }
    }

    // 恢复颜色选取信息（同步到颜色按钮和输入框）
    // 注意：setCurrentColor 等不再触发 saveGeorefCache()，缓存仅在地理配准和一键质检前保存
    if (root.contains("colorBoundary")) {
        QColor c(root.value("colorBoundary").toString());
        if (c.isValid()) setCurrentColor(c);
    }
    if (root.contains("colorInnerHalo")) {
        QColor c(root.value("colorInnerHalo").toString());
        if (c.isValid()) setCurrentHaloColor(c);
    }
    if (root.contains("colorOuterHalo")) {
        QColor c(root.value("colorOuterHalo").toString());
        if (c.isValid()) setCurrentOuterHaloColor(c);
    }

    qDebug("[Georef] 从缓存恢复配准成功: %d 对控制点", (int)m_gcpImgPts.size());
    return true;
}

// 打开地理配准窗口
void SE_ImageMapCheckDialog::onOpenGeoreference()
{
    addCheckLog("开始地理配准");
    if (m_imagePath.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先选择图片");
        return;
    }
    if (!m_boundaryLoaded || m_boundaryFeatures.empty()) {
        QMessageBox::warning(this, "提示", "请先加载 gdb 境界线数据");
        return;
    }

    cv::Mat img = imreadCN(m_imagePath);
    if (img.empty()) {
        QMessageBox::warning(this, "错误", "无法读取图片");
        return;
    }

    // 若已有仿射变换，作为初始状态传入
    GeoreferenceDialog dialog(img, m_boundaryFeatures, m_boundaryExtent, this);
    if (m_affineValid) {
        dialog.m_gcpImgPts = m_gcpImgPts;
        dialog.m_gcpGeoPts = m_gcpGeoPts;
    }

    if (dialog.exec() == QDialog::Accepted && dialog.m_affineValid) {
        // 取回配准结果
        m_affine = dialog.m_affine;
        m_affineValid = true;
        m_gcpImgPts = dialog.m_gcpImgPts;
        m_gcpGeoPts = dialog.m_gcpGeoPts;
        // 读取子图层数量用于 UI 显示
        QStringList subNames, subTypes;
        listGdbSubLayers(m_gdbPath, subNames, subTypes);
        m_lblGdbPath->setText(QString("gdb: %1\n(%2 个子图层)")
            .arg(QFileInfo(m_gdbPath).fileName())
            .arg(subNames.size()));
        setLabelStatus(m_lblGdbPath, "loaded");
        m_lblGeoref->setText(QString("配准: 已配准 - %1 对控制点").arg(m_gcpImgPts.size()));
        setLabelStatus(m_lblGeoref, "loaded");
        m_lblBoundaryLayer->setText(QString("境界线图层: %1\n(%2 要素, %3 段)[已配准]")
            .arg(m_boundaryLayerName)
            .arg(m_boundaryFeatures.size() > 0 ? (int)m_boundaryFeatures.size() : 0)
            .arg(m_boundaryFeatures.size()));
        setLabelStatus(m_lblBoundaryLayer, "loaded");

        // 启用境界线和注记提取按钮
        m_btnExtractColor->setEnabled(true);
        m_btnExtractColor->setToolTip("");
        m_btnExtractText->setEnabled(true);
        m_btnExtractText->setToolTip("");

        // 保存配准结果到缓存，下次打开同一图片时自动恢复
        saveGeorefCache();
        addCheckLog(QString("地理配准完成: %1 对控制点").arg(m_gcpImgPts.size()));

        QMessageBox::information(this, "地理配准完成",
            QString("已应用仿射变换，后续境界线提取将使用标准境界线作为引导。"));
    }
}

// Zhang-Suen 细化算法：将二值色带细化为单像素宽骨架
static void thinImageZhangSuen(cv::Mat& binary)
{
    CV_Assert(binary.type() == CV_8UC1);
    cv::threshold(binary, binary, 127, 1, cv::THRESH_BINARY);
    cv::Mat prev;
    int iter = 0;
    do {
        prev = binary.clone();
        for (int sub = 0; sub < 2; ++sub) {
            cv::Mat marker = cv::Mat::zeros(binary.size(), CV_8UC1);
            for (int y = 1; y < binary.rows - 1; ++y) {
                for (int x = 1; x < binary.cols - 1; ++x) {
                    if (binary.at<uchar>(y, x) != 1) continue;
                    int p2 = binary.at<uchar>(y - 1, x);
                    int p3 = binary.at<uchar>(y - 1, x + 1);
                    int p4 = binary.at<uchar>(y, x + 1);
                    int p5 = binary.at<uchar>(y + 1, x + 1);
                    int p6 = binary.at<uchar>(y + 1, x);
                    int p7 = binary.at<uchar>(y + 1, x - 1);
                    int p8 = binary.at<uchar>(y, x - 1);
                    int p9 = binary.at<uchar>(y - 1, x - 1);
                    int C = (p2 == 0 && p3 == 1) + (p3 == 0 && p4 == 1) + (p4 == 0 && p5 == 1) +
                            (p5 == 0 && p6 == 1) + (p6 == 0 && p7 == 1) + (p7 == 0 && p8 == 1) +
                            (p8 == 0 && p9 == 1) + (p9 == 0 && p2 == 1);
                    int N = p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
                    if (C != 1 || N < 2 || N > 6) continue;
                    if (sub == 0) {
                        if (p2 * p4 * p6 == 0 && p4 * p6 * p8 == 0)
                            marker.at<uchar>(y, x) = 1;
                    } else {
                        if (p2 * p4 * p8 == 0 && p2 * p6 * p8 == 0)
                            marker.at<uchar>(y, x) = 1;
                    }
                }
            }
            binary -= marker;
        }
        if (++iter > 200) break; // 安全上限
    } while (cv::countNonZero(binary - prev) > 0);
    binary *= 255;
}

// 计算骨架像素的 8 邻域中已访问/未访问的骨架邻居数
static int skeletonNeighborCount(const cv::Mat& skel, const cv::Mat& visited, int x, int y)
{
    int cnt = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= skel.cols || ny >= skel.rows) continue;
            if (skel.at<uchar>(ny, nx) > 0 && visited.at<uchar>(ny, nx) == 0) cnt++;
        }
    }
    return cnt;
}

// 从骨架追踪中心线（开放折线），输出真正的"线"而非闭合环
static std::vector<std::vector<cv::Point>> extractCenterlines(const cv::Mat& skeleton)
{
    std::vector<std::vector<cv::Point>> lines;
    cv::Mat visited = cv::Mat::zeros(skeleton.size(), CV_8UC1);

    auto traceFrom = [&](cv::Point start) {
        std::vector<cv::Point> line;
        cv::Point cur = start;
        line.push_back(cur);
        visited.at<uchar>(cur.y, cur.x) = 1;
        while (true) {
            cv::Point next(-1, -1);
            for (int dy = -1; dy <= 1 && next.x < 0; ++dy) {
                for (int dx = -1; dx <= 1 && next.x < 0; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = cur.x + dx, ny = cur.y + dy;
                    if (nx < 0 || ny < 0 || nx >= skeleton.cols || ny >= skeleton.rows) continue;
                    if (skeleton.at<uchar>(ny, nx) > 0 && visited.at<uchar>(ny, nx) == 0) {
                        next = cv::Point(nx, ny);
                    }
                }
            }
            if (next.x < 0) break;
            visited.at<uchar>(next.y, next.x) = 1;
            line.push_back(next);
            cur = next;
            // 到达端点（无其他未访问邻居）则停止
            if (skeletonNeighborCount(skeleton, visited, cur.x, cur.y) == 0) break;
        }
        return line;
    };

    // 收集端点（邻居数==1）优先作为起点
    std::vector<cv::Point> endpoints;
    for (int y = 0; y < skeleton.rows; ++y) {
        for (int x = 0; x < skeleton.cols; ++x) {
            if (skeleton.at<uchar>(y, x) == 0) continue;
            int nb = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= skeleton.cols || ny >= skeleton.rows) continue;
                    if (skeleton.at<uchar>(ny, nx) > 0) nb++;
                }
            }
            if (nb == 1) endpoints.push_back(cv::Point(x, y));
        }
    }
    for (const auto& ep : endpoints) {
        if (visited.at<uchar>(ep.y, ep.x) == 0) {
            auto line = traceFrom(ep);
            if (line.size() >= 2) lines.push_back(line);
        }
    }
    // 处理闭合环（无端点的连通分量）
    for (int y = 0; y < skeleton.rows; ++y) {
        for (int x = 0; x < skeleton.cols; ++x) {
            if (skeleton.at<uchar>(y, x) > 0 && visited.at<uchar>(y, x) == 0) {
                auto line = traceFrom(cv::Point(x, y));
                if (line.size() >= 2) lines.push_back(line);
            }
        }
    }
    return lines;
}

// 单条轮廓移动平均平滑（开放折线，端点保持不变避免收缩）
static std::vector<cv::Point> smoothContour(const std::vector<cv::Point>& contour, int window = 5)
{
    const size_t n = contour.size();
    if (n < 3) return contour;
    int half = window / 2;
    std::vector<cv::Point> result;
    result.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        long sx = 0, sy = 0, cnt = 0;
        for (int k = -half; k <= half; ++k) {
            long idx = static_cast<long>(i) + k;
            if (idx < 0) idx = 0;                       // 端点截断
            if (idx >= static_cast<long>(n)) idx = static_cast<long>(n) - 1;
            sx += contour[idx].x;
            sy += contour[idx].y;
            cnt++;
        }
        result.emplace_back(static_cast<int>(sx / cnt), static_cast<int>(sy / cnt));
    }
    return result;
}

// 对所有轮廓做简化去毛刺 + 移动平均平滑
static std::vector<cv::Point2f> smoothContourF(const std::vector<cv::Point2f>& contour, int window = 7)
{
    const size_t n = contour.size();
    if (n < 3) {
        return contour;
    }
    int half = window / 2;
    // 高斯加权系数
    double sigma = half / 2.0;
    std::vector<double> weights;
    weights.reserve(window);
    double wsum = 0;
    for (int k = -half; k <= half; ++k) {
        double w = std::exp(-(k * k) / (2 * sigma * sigma + 1e-9));
        weights.push_back(w);
        wsum += w;
    }

    std::vector<cv::Point2f> result;
    result.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        double sx = 0, sy = 0, wacc = 0;
        for (int k = -half; k <= half; ++k) {
            long idx = static_cast<long>(i) + k;
            if (idx < 0) idx = 0;
            if (idx >= static_cast<long>(n)) idx = static_cast<long>(n) - 1;
            double w = weights[k + half];
            sx += contour[idx].x * w;
            sy += contour[idx].y * w;
            wacc += w;
        }
        result.emplace_back(float(sx / wacc), float(sy / wacc));
    }
    return result;
}

// 对所有轮廓做高斯加权移动平均平滑（子像素精度，全程浮点，不减少采样点）
// 注意：不使用 approxPolyDP，因为它会把弯曲段压成直线段，丢失精度细节
static void smoothContours(std::vector<std::vector<cv::Point2f>>& contours,
                           int window = 5, double minLen = 20.0)
{
    std::vector<std::vector<cv::Point2f>> result;
    result.reserve(contours.size());
    for (auto& c : contours) {
        if (c.size() < 2) continue;
        if (cv::arcLength(c, false) < minLen) continue;
        // 高斯加权移动平均平滑（全程浮点，不四舍五入，不减少采样点）
        auto smoothed = smoothContourF(c, window);
        if (smoothed.size() >= 2) result.push_back(std::move(smoothed));
    }
    contours.swap(result);
}

// ===================== 拓扑修正 =====================
// 对提取出的折线集合进行拓扑修正，解决扫描/识别过程中的
// 断裂、偏移、重复和噪声等问题，转换为符合 GIS 规范的连续矢量数据。

// 计算折线端点切线方向（用端点附近若干点的差分）
static cv::Point2f tangentAtStart(const std::vector<cv::Point2f>& c, int span = 3)
{
    int n = (int)c.size();
    if (n < 2) return cv::Point2f(0, 0);
    int k = std::min(span, n - 1);
    return cv::Point2f(c[k].x - c[0].x, c[k].y - c[0].y);
}
static cv::Point2f tangentAtEnd(const std::vector<cv::Point2f>& c, int span = 3)
{
    int n = (int)c.size();
    if (n < 2) return cv::Point2f(0, 0);
    int k = std::min(span, n - 1);
    return cv::Point2f(c[n-1].x - c[n-1-k].x, c[n-1].y - c[n-1-k].y);
}
static double vecLen(cv::Point2f v) { return std::sqrt(v.x*v.x + v.y*v.y); }
static double vecAngle(cv::Point2f a, cv::Point2f b)
{
    double la = vecLen(a), lb = vecLen(b);
    if (la < 1e-6 || lb < 1e-6) return 180.0;
    double cosA = (a.x*b.x + a.y*b.y) / (la * lb);
    cosA = std::max(-1.0, std::min(1.0, cosA));
    return std::acos(cosA) * 180.0 / CV_PI;
}

// 1. 噪声清理：删除弧长过短的线段
static void topoRemoveShort(std::vector<std::vector<cv::Point2f>>& contours, double minLen = 15.0)
{
    std::vector<std::vector<cv::Point2f>> kept;
    kept.reserve(contours.size());
    int removed = 0;
    for (auto& c : contours) {
        if (c.size() >= 2 && cv::arcLength(c, false) >= minLen) {
            kept.push_back(std::move(c));
        } else {
            removed++;
        }
    }
    contours.swap(kept);
    qDebug("[Topology] 噪声清理: 删除 %d 条短片段, 保留 %d 条", removed, (int)contours.size());
}

// 1b. 小面积闭合曲线删除：过滤虚线空隙处闭运算产生的细长小环
//     仅对闭合曲线（首尾重合）按 contourArea 过滤；开放折线不受影响
static void topoRemoveSmallAreas(std::vector<std::vector<cv::Point2f>>& contours, double minArea = 50.0)
{
    std::vector<std::vector<cv::Point2f>> kept;
    kept.reserve(contours.size());
    int removed = 0;
    for (auto& c : contours) {
        if (c.size() < 3) {
            kept.push_back(std::move(c));
            continue;
        }
        // 判断是否闭合：首尾距离<3px视为闭合
        double closeDist = cv::norm(c.front() - c.back());
        bool isClosed = (closeDist < 3.0);
        if (isClosed) {
            double area = std::abs(cv::contourArea(c));
            if (area < minArea) {
                removed++;
                continue;
            }
        }
        kept.push_back(std::move(c));
    }
    contours.swap(kept);
    qDebug("[Topology] 小面积闭合曲线删除: 删除 %d 条, 保留 %d 条 (阈值=%g)",
           removed, (int)contours.size(), minArea);
}

// 2. 端点吸附：将距离<阈值的端点对合并到中点
static void topoSnapEndpoints(std::vector<std::vector<cv::Point2f>>& contours, double tol = 5.0)
{
    int n = (int)contours.size();
    int snapped = 0;
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            auto& A = contours[i];
            auto& B = contours[j];
            if (A.size() < 2 || B.size() < 2) continue;
            cv::Point2f* pa[2] = { &A.front(), &A.back() };
            cv::Point2f* pb[2] = { &B.front(), &B.back() };
            for (int a = 0; a < 2; ++a) {
                for (int b = 0; b < 2; ++b) {
                    double d = cv::norm(*pa[a] - *pb[b]);
                    if (d < tol && d > 0.01) {
                        cv::Point2f mid((pa[a]->x + pb[b]->x) / 2.f, (pa[a]->y + pb[b]->y) / 2.f);
                        *pa[a] = mid;
                        *pb[b] = mid;
                        snapped++;
                    }
                }
            }
        }
    }
    qDebug("[Topology] 端点吸附: 合并 %d 对端点", snapped);
}

// 3. 线段合并：合并首尾相接（距离<阈值）且切线方向一致的线段
static void topoMerge(std::vector<std::vector<cv::Point2f>>& contours,
                      double snapTol = 5.0, double angleMax = 15.0)
{
    bool changed = true;
    int mergeCount = 0;
    while (changed) {
        changed = false;
        int n = (int)contours.size();
        for (int i = 0; i < n && !changed; ++i) {
            for (int j = i + 1; j < n && !changed; ++j) {
                auto& A = contours[i];
                auto& B = contours[j];
                if (A.size() < 2 || B.size() < 2) continue;

                auto tA = tangentAtEnd(A);
                auto tB = tangentAtStart(B);
                double d = cv::norm(A.back() - B.front());
                double ang = vecAngle(tA, tB);

                bool canMerge = false;
                int mode = 0;
                if (d < snapTol && ang < angleMax) { canMerge = true; mode = 1; }
                else {
                    d = cv::norm(A.back() - B.back());
                    ang = vecAngle(tA, tangentAtEnd(B) * -1.f);
                    if (d < snapTol && ang < angleMax) { canMerge = true; mode = 2; }
                    else {
                        d = cv::norm(A.front() - B.front());
                        ang = vecAngle(tangentAtStart(A) * -1.f, tB);
                        if (d < snapTol && ang < angleMax) { canMerge = true; mode = 3; }
                        else {
                            d = cv::norm(A.front() - B.back());
                            ang = vecAngle(tangentAtStart(A) * -1.f, tangentAtEnd(B) * -1.f);
                            if (d < snapTol && ang < angleMax) { canMerge = true; mode = 4; }
                        }
                    }
                }

                if (canMerge) {
                    std::vector<cv::Point2f> merged;
                    if (mode == 1) { merged = A; merged.insert(merged.end(), B.begin(), B.end()); }
                    else if (mode == 2) { merged = A; merged.insert(merged.end(), B.rbegin(), B.rend()); }
                    else if (mode == 3) { merged.assign(B.rbegin(), B.rend()); merged.insert(merged.end(), A.begin(), A.end()); }
                    else { merged.assign(B.rbegin(), B.rend()); merged.insert(merged.end(), A.rbegin(), A.rend()); }
                    contours[i] = std::move(merged);
                    contours.erase(contours.begin() + j);
                    changed = true;
                    mergeCount++;
                }
            }
        }
    }
    qDebug("[Topology] 线段合并: 合并 %d 次, 剩余 %d 条", mergeCount, (int)contours.size());
}

// 4. 重复线删除：删除整体相似度高的线段
static void topoRemoveDuplicates(std::vector<std::vector<cv::Point2f>>& contours, double distTol = 3.0)
{
    std::vector<std::vector<cv::Point2f>> kept;
    std::vector<bool> removed(contours.size(), false);
    int dupCount = 0;
    for (size_t i = 0; i < contours.size(); ++i) {
        if (removed[i]) continue;
        kept.push_back(contours[i]);
        for (size_t j = i + 1; j < contours.size(); ++j) {
            if (removed[j]) continue;
            auto& A = contours[i];
            auto& B = contours[j];
            if (A.size() < 2 || B.size() < 2) continue;
            double lenA = cv::arcLength(A, false);
            double lenB = cv::arcLength(B, false);
            if (std::abs(lenA - lenB) > lenA * 0.2) continue;
            double d1 = cv::norm(A.front() - B.front()) + cv::norm(A.back() - B.back());
            double d2 = cv::norm(A.front() - B.back()) + cv::norm(A.back() - B.front());
            double d = std::min(d1, d2);
            if (d < distTol * 2) {
                removed[j] = true;
                dupCount++;
            }
        }
    }
    contours.swap(kept);
    qDebug("[Topology] 重复线删除: 删除 %d 条", dupCount);
}

// 6. 拓扑检查：检测悬挂线、重复线、自交等错误（仅诊断输出）
static void topoCheck(const std::vector<std::vector<cv::Point2f>>& contours, double snapTol = 5.0)
{
    int n = (int)contours.size();
    int dangling = 0;
    for (int i = 0; i < n; ++i) {
        const auto& A = contours[i];
        if (A.size() < 2) continue;
        cv::Point2f aS = A.front(), aE = A.back();
        bool sConn = false, eConn = false;
        if (cv::norm(aS - aE) < snapTol) { sConn = eConn = true; }
        for (int j = 0; j < n && (!sConn || !eConn); ++j) {
            if (j == i) continue;
            const auto& B = contours[j];
            if (B.size() < 2) continue;
            cv::Point2f bS = B.front(), bE = B.back();
            if (!sConn) {
                if (cv::norm(aS - bS) < snapTol || cv::norm(aS - bE) < snapTol) sConn = true;
            }
            if (!eConn) {
                if (cv::norm(aE - bS) < snapTol || cv::norm(aE - bE) < snapTol) eConn = true;
            }
        }
        if (!sConn) dangling++;
        if (!eConn) dangling++;
    }
    qDebug("[Topology] 拓扑检查: 共 %d 条线, 悬挂端点 %d 个", n, dangling);
}

// 7. 按面积过滤：只保留闭合且面积≥阈值的曲线（飞地境界线也要保留）。
//    境界线必须是闭合曲线，未闭合线段视为噪声直接删除。
//    噪声占绝大多数时中位数偏低不合适，改用最大面积为基准：
//    真实境界线面积最大，飞地面积通常不小于主境界线的 5%。
//    阈值 = 最大面积 × 0.05
static void topoKeepLongest(std::vector<std::vector<cv::Point2f>>& contours)
{
    if (contours.empty()) return;

    // 计算每条闭合曲线的面积（未闭合的直接标记为0）
    std::vector<double> areas;
    areas.reserve(contours.size());
    double maxArea = 0;
    int closedCnt = 0;
    for (auto& c : contours) {
        double a = 0;
        if (c.size() >= 3) {
            double closeDist = cv::norm(c.front() - c.back());
            if (closeDist < 3.0) {
                a = std::abs(cv::contourArea(c));
                ++closedCnt;
            }
        }
        areas.push_back(a);
        if (a > maxArea) maxArea = a;
    }

    // 阈值 = 最大面积 × 0.05
    double threshold = maxArea * 0.005;

    std::vector<std::vector<cv::Point2f>> kept;
    int removed = 0;
    for (size_t i = 0; i < contours.size(); ++i) {
        // 只保留闭合且面积达标的曲线
        if (areas[i] >= threshold) {
            kept.push_back(std::move(contours[i]));
        } else {
            ++removed;
        }
    }
    contours.swap(kept);
    qDebug("[Topology] 按面积过滤: 阈值=%.1f (最大面积=%.1f, 闭合曲线=%d), 删除 %d 条, 保留 %d 条",
           threshold, maxArea, closedCnt, removed, (int)contours.size());
}

// 8. 强制闭合：如果最终只剩一条非闭合曲线，强制首尾连接
static void topoForceClose(std::vector<std::vector<cv::Point2f>>& contours, double tol = 5.0)
{
    if (contours.empty()) return;
    int closedCnt = 0;
    for (auto& c : contours) {
        if (c.size() < 2) continue;
        double d = cv::norm(c.front() - c.back());
        if (d >= tol && d < 200.0) {
            // 添加中间点平滑连接（避免长直线突变）
            cv::Point2f mid((c.front().x + c.back().x) / 2.f, (c.front().y + c.back().y) / 2.f);
            c.push_back(mid);
            c.push_back(c.front());  // 闭合
            ++closedCnt;
            qDebug("[Topology] 强制闭合: 首尾距离=%g, 已添加连接点", d);
        } else if (d < tol) {
            c.push_back(c.front());  // 首尾接近，直接闭合
            ++closedCnt;
            qDebug("[Topology] 强制闭合: 首尾距离=%g<%g, 已闭合", d, tol);
        } else {
            qDebug("[Topology] 强制闭合: 首尾距离=%g过大(>200), 不闭合", d);
        }
    }
    qDebug("[Topology] 强制闭合: 共闭合 %d 条曲线", closedCnt);
}

// 线段相交检测：返回是否相交及交点（参数 t,u 在 [0,1] 内才算相交）
static bool segIntersect(cv::Point2f p1, cv::Point2f p2,
                         cv::Point2f p3, cv::Point2f p4,
                         cv::Point2f& pt)
{
    float x1 = p1.x, y1 = p1.y, x2 = p2.x, y2 = p2.y;
    float x3 = p3.x, y3 = p3.y, x4 = p4.x, y4 = p4.y;
    float denom = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4);
    if (std::abs(denom) < 1e-6f) return false;  // 平行/共线
    float t = ((x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4)) / denom;
    float u = -((x1 - x2) * (y1 - y3) - (y1 - y2) * (x1 - x3)) / denom;
    // 端点接触不算自相交（t 或 u 恰好为 0/1），只处理真正的穿越
    if (t > 1e-4f && t < 1 - 1e-4f && u > 1e-4f && u < 1 - 1e-4f) {
        pt.x = x1 + t * (x2 - x1);
        pt.y = y1 + t * (y2 - y1);
        return true;
    }
    return false;
}

// 9. 消除自相交：在交点处将闭合曲线分成两个环，保留较长的环
//    重复直到没有自相交（处理"8字形"等多重自相交）
static void topoRemoveSelfIntersections(std::vector<std::vector<cv::Point2f>>& contours)
{
    if (contours.empty()) return;
    int totalCuts = 0;
    for (auto& c : contours) {
        if (c.size() < 4) continue;
        bool found = true;
        int iter = 0;
        while (found && iter < 30) {  // 限制迭代防死循环
            found = false;
            iter++;
            int n = (int)c.size();
            bool isClosed = (cv::norm(c.front() - c.back()) < 3.0);

            int iBest = -1, jBest = -1;
            cv::Point2f ptBest;

            // 检测非相邻线段对的交点
            // 线段 i: c[i]→c[i+1], 线段 j: c[j]→c[j+1]
            for (int i = 0; i < n - 1 && !found; ++i) {
                for (int j = i + 2; j < n - 1 && !found; ++j) {
                    // 闭合曲线：首尾段相邻，跳过
                    if (isClosed && i == 0 && j == n - 2) continue;
                    cv::Point2f pt;
                    if (segIntersect(c[i], c[i + 1], c[j], c[j + 1], pt)) {
                        iBest = i; jBest = j; ptBest = pt;
                        found = true;
                    }
                }
            }

            if (found) {
                // 在交点 ptBest 处打断成两个环：
                //   环A（短路径）: ptBest → c[i+1..j] → ptBest
                //   环B（长路径）: ptBest → c[j+1..end-1] → c[1..i] → ptBest  （闭合时跳过重复的c[0]）
                std::vector<cv::Point2f> ringA, ringB;
                ringA.push_back(ptBest);
                for (int k = iBest + 1; k <= jBest; ++k) ringA.push_back(c[k]);
                ringA.push_back(ptBest);

                ringB.push_back(ptBest);
                for (int k = jBest + 1; k < n; ++k) ringB.push_back(c[k]);
                int startK = isClosed ? 1 : 0;
                for (int k = startK; k <= iBest; ++k) ringB.push_back(c[k]);
                ringB.push_back(ptBest);

                double lenA = cv::arcLength(ringA, false);
                double lenB = cv::arcLength(ringB, false);
                // 保留较长的环（境界线主体），丢弃较短的环（错误环回部分）
                c = (lenA >= lenB) ? std::move(ringA) : std::move(ringB);
                totalCuts++;
            }
        }
    }

    if (totalCuts > 0) {
        qDebug("[Topology] 消除自相交: 打断 %d 处", totalCuts);
    }
}

// 拓扑修正总入口
// imgDiag: 图像对角线长度，用于自适应缩放阈值（大图用大阈值）
// baseShortLen/baseSmallArea/baseSnap/baseMerge: 从 XML 质检规则加载的基准值
static void topologicalCorrection(std::vector<std::vector<cv::Point2f>>& contours, double imgDiag = 1000.0,
                                  double baseShortLen = 15.0, double baseSmallArea = 50.0,
                                  double baseSnap = 5.0, double baseMerge = 5.0)
{
    qDebug("[Topology] === 开始拓扑修正, 输入 %d 条, 图像对角线=%g ===", (int)contours.size(), imgDiag);
    int before = (int)contours.size();

    // 阈值按图像尺寸自适应缩放：以1000px对角线为基准
    // 注意：短片段和小面积阈值不能放大太多，否则会误删境界线段
    double scale = std::max(1.0, imgDiag / 1000.0);
    double shortLen     = std::min(60.0, baseShortLen * std::sqrt(scale));   // 短片段：sqrt缩放，上限60px
    double smallArea    = std::min(500.0, baseSmallArea * scale);            // 小面积：线性缩放，上限500
    double snapTol      = std::min(150.0, baseSnap * scale);                 // 端点吸附：上限150px
    double mergeDist    = std::min(150.0, baseMerge * scale);                // 合并距离：上限150px
    double mergeLen     = std::min(60.0, baseShortLen * std::sqrt(scale));   // 合并长度：上限60px

    qDebug("[Topology] 自适应阈值: 短片段<%g, 小面积<%g, 吸附<%g, 合并<%g",
           shortLen, smallArea, snapTol, mergeDist);

    topoRemoveShort(contours, shortLen);
    topoRemoveSmallAreas(contours, smallArea);
    topoRemoveDuplicates(contours, std::min(20.0, 3.0 * scale));
    topoSnapEndpoints(contours, snapTol);
    topoMerge(contours, mergeDist, mergeLen);
    topoRemoveDuplicates(contours, std::min(20.0, 3.0 * scale));
    // 先强制闭合：将首尾接近的线段闭合为环（境界线应为闭合曲线）
    topoForceClose(contours, snapTol);
    // 再按面积过滤：只保留闭合且面积达标的曲线（飞地境界线也保留）
    topoKeepLongest(contours);
    // 消除自相交：在交点处打断，保留较长环
    topoRemoveSelfIntersections(contours);
    topoCheck(contours, snapTol);

    qDebug("[Topology] === 拓扑修正完成, 输入 %d 条 → 输出 %d 条 ===", before, (int)contours.size());
}

// ---------- OCR 文字提取 ----------
// 使用 Tesseract 从图片中提取文字注记及其位置范围
static std::vector<TextResult> mergeNearbyTextResults(std::vector<TextResult> inputs,
                                                       const QStringList& gazetteerList = {}); // 前向声明
static std::vector<TextResult> filterOcrNoise(std::vector<TextResult> inputs);          // 前向声明

// 采样文字区域的代表颜色：排除接近背景（白/浅色）的像素，取深色文字像素均值
static cv::Scalar sampleTextColor(const cv::Mat& img, const cv::Rect& bbox)
{
    cv::Rect roi = bbox & cv::Rect(0, 0, img.cols, img.rows);
    if (roi.width <= 0 || roi.height <= 0) return cv::Scalar(0, 0, 0);
    cv::Mat patch = img(roi);

    // 转灰度，取较暗的像素（文字比背景深）
    cv::Mat gray;
    cv::cvtColor(patch, gray, cv::COLOR_BGR2GRAY);
    double meanGray = cv::mean(gray)[0];

    // 收集文字笔画像素（比背景明显暗）
    // 用众数(mode)而非均值：栅格图中文字边缘有抗锯齿渐变色，
    // 均值会被浅色边缘拉偏，众数取最密集的颜色更接近真实文字色。
    // 用量化分箱(每32一档, 共8档)统计众数，避免抗锯齿导致的色散。
    const int BIN = 32;
    std::map<std::tuple<int,int,int>, int> hist;
    int totalCount = 0;
    for (int y = 0; y < patch.rows; ++y) {
        for (int x = 0; x < patch.cols; ++x) {
            uchar g = gray.at<uchar>(y, x);
            if (g < meanGray * 0.75) {
                cv::Vec3b c = patch.at<cv::Vec3b>(y, x);
                int b = c[0] / BIN, gr = c[1] / BIN, r = c[2] / BIN;
                auto key = std::make_tuple(b, gr, r);
                ++hist[key];
                ++totalCount;
            }
        }
    }
    if (totalCount == 0) {
        return cv::mean(patch);
    }
    // 取出现次数最多的颜色档
    auto bestIt = hist.begin();
    int bestCount = 0;
    for (auto it = hist.begin(); it != hist.end(); ++it) {
        if (it->second > bestCount) { bestCount = it->second; bestIt = it; }
    }
    // 用该档内所有像素的均值作为代表色（比取档中心更准确）
    cv::Scalar sum(0, 0, 0, 0);
    int count = 0;
    int bBin = std::get<0>(bestIt->first);
    int gBin = std::get<1>(bestIt->first);
    int rBin = std::get<2>(bestIt->first);
    for (int y = 0; y < patch.rows; ++y) {
        for (int x = 0; x < patch.cols; ++x) {
            uchar g = gray.at<uchar>(y, x);
            if (g < meanGray * 0.75) {
                cv::Vec3b c = patch.at<cv::Vec3b>(y, x);
                if (c[0]/BIN == bBin && c[1]/BIN == gBin && c[2]/BIN == rBin) {
                    sum[0] += c[0]; sum[1] += c[1]; sum[2] += c[2];
                    ++count;
                }
            }
        }
    }
    if (count == 0) return cv::mean(patch);
    return cv::Scalar(sum[0] / count, sum[1] / count, sum[2] / count);
}

// 颜色距离（欧氏距离，BGR 三通道）
static double colorDistance(const cv::Scalar& a, const cv::Scalar& b)
{
    double db = a[0] - b[0], dg = a[1] - b[1], dr = a[2] - b[2];
    return std::sqrt(db * db + dg * dg + dr * dr);
}

// 根据颜色与字号对文字结果分组（贪心聚类）：
// 颜色距离 < colorThresh 且 字号差 < sizeThresh 视为同组。
// 栅格图像存在抗锯齿/压缩色差，阈值需适当放宽，使颜色相近的注记
// （如同一类蓝色河流名）归为一组，而非每种细微色差各成一组。
// 组的基准颜色/字号采用运行均值，使聚类对色差更稳健。
static void groupTextResults(std::vector<TextResult>& results,
                             double colorThresh = 70.0,
                             int    sizeThresh  = 8)
{
    std::vector<cv::Scalar> groupColorSum; // 组内颜色累加和
    std::vector<int>        groupColorCnt; // 组内成员数
    std::vector<long long>  groupSizeSum;  // 组内字号累加和
    std::vector<int>        groupSizeCnt;

    for (auto& tr : results) {
        int assigned = -1;
        double bestDist = std::numeric_limits<double>::max();
        for (size_t g = 0; g < groupColorSum.size(); ++g) {
            cv::Scalar avgColor(
                groupColorSum[g][0] / groupColorCnt[g],
                groupColorSum[g][1] / groupColorCnt[g],
                groupColorSum[g][2] / groupColorCnt[g]);
            int avgSize = static_cast<int>(groupSizeSum[g] / groupSizeCnt[g]);
            double cd = colorDistance(tr.avgColor, avgColor);
            int    sd = std::abs(tr.fontSize - avgSize);
            if (cd < colorThresh && sd < sizeThresh && cd < bestDist) {
                bestDist = cd;
                assigned = static_cast<int>(g);
            }
        }
        if (assigned < 0) {
            assigned = static_cast<int>(groupColorSum.size());
            groupColorSum.push_back(tr.avgColor);
            groupColorCnt.push_back(1);
            groupSizeSum.push_back(tr.fontSize);
            groupSizeCnt.push_back(1);
        } else {
            groupColorSum[assigned][0] += tr.avgColor[0];
            groupColorSum[assigned][1] += tr.avgColor[1];
            groupColorSum[assigned][2] += tr.avgColor[2];
            groupColorCnt[assigned]++;
            groupSizeSum[assigned]  += tr.fontSize;
            groupSizeCnt[assigned]++;
        }
        tr.group = assigned;
    }
}

// 为分组编号生成区分色（用于预览绘制）
static cv::Scalar groupColor(int group)
{
    static const cv::Scalar palette[] = {
        cv::Scalar(0, 0, 255),     // 红
        cv::Scalar(255, 0, 0),     // 蓝
        cv::Scalar(0, 200, 0),     // 绿
        cv::Scalar(0, 255, 255),   // 黄
        cv::Scalar(255, 0, 255),   // 紫
        cv::Scalar(255, 165, 0),   // 橙
        cv::Scalar(128, 0, 128),   // 深紫
        cv::Scalar(0, 128, 128)    // 暗黄绿
    };
    return palette[group % (sizeof(palette) / sizeof(palette[0]))];
}

// 判断字符是否为 CJK 汉字（中文注记的核心特征）
static bool isCJKChar(QChar ch)
{
    ushort code = ch.unicode();
    return (code >= 0x4E00 && code <= 0x9FFF) ||  // CJK 统一汉字
           (code >= 0x3400 && code <= 0x4DBF);    // CJK 扩展A
}

// 过滤 OCR 误识别项：地图上的圆点常被误识别为数字或符号，
// 真正的地名注记都包含中文汉字，因此筛掉不含汉字的结果。
// 同时清除文字中的非汉字字符(数字、字母、符号)，避免干扰匹配。
static std::vector<TextResult> filterOcrNoise(std::vector<TextResult> inputs)
{
    std::vector<TextResult> out;
    for (auto& tr : inputs) {
        if (tr.text.isEmpty()) continue;

        // 清除非汉字字符（数字、字母、标点、符号等）
        QString cleaned;
        for (const QChar& ch : tr.text) {
            if (isCJKChar(ch)) cleaned.append(ch);
        }
        // 只保留含汉字的结果
        if (!cleaned.isEmpty()) {
            tr.text = cleaned;
            out.push_back(std::move(tr));
        }
    }
    return out;
}

// 对图像进行 OCR 预处理：放大 -> 灰度 -> CLAHE 对比度增强 -> 双边滤波去噪
// 返回预处理后的灰度图像（不再二值化，交给各策略按需二值化），scale 为放大倍数
// targetMinDim: 放大目标最小维度（来自 XML "OCR 目标最小维度"）
// claheClipLimit: CLAHE 对比度限制（来自 XML "CLAHE 对比度限制"）
static cv::Mat preprocessForOCR(const cv::Mat& img, double& scale,
                                int targetMinDim = 3500, double claheClipLimit = 2.0)
{
    // 1. 放大：小文字识别率低，统一放大到较大尺寸便于识别
    //     提高目标尺寸到 targetMinDim，使小字注记也有足够分辨率
    const int TARGET_MIN_DIM = targetMinDim;
    int minDim = std::min(img.cols, img.rows);
    scale = 1.0;
    if (minDim < TARGET_MIN_DIM) {
        scale = static_cast<double>(TARGET_MIN_DIM) / minDim;
    }
    cv::Mat resized;
    cv::resize(img, resized, cv::Size(), scale, scale, cv::INTER_CUBIC);

    // 2. 灰度
    cv::Mat gray;
    if (resized.channels() == 3) {
        cv::cvtColor(resized, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = resized.clone();
    }

    // 3. CLAHE 对比度受限自适应直方图均衡：改善地图上光照不均/底色不一
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(claheClipLimit, cv::Size(8, 8));
    cv::Mat enhanced;
    clahe->apply(gray, enhanced);

    // 4. 双边滤波：去噪的同时保留文字边缘
    cv::Mat denoised;
    cv::bilateralFilter(enhanced, denoised, 5, 75, 75);
    return denoised;
}

// 用指定预处理图 + PSM 模式跑一次 OCR，返回候选结果（坐标已映射回原图）
static std::vector<TextResult> runOcrPass(tesseract::TessBaseAPI& api,
                                          const cv::Mat& processed,
                                          double scale,
                                          tesseract::PageSegMode psm,
                                          float minConfidence)
{
    std::vector<TextResult> results;
    api.SetPageSegMode(psm);
    api.SetImage(processed.data, processed.cols, processed.rows, 1, static_cast<int>(processed.step));
    api.Recognize(0);

    tesseract::ResultIterator* ri = api.GetIterator();
    if (ri) {
        tesseract::PageIteratorLevel level = tesseract::RIL_WORD;
        do {
            int x1, y1, x2, y2;
            if (ri->BoundingBox(level, &x1, &y1, &x2, &y2)) {
                float conf = ri->Confidence(level);
                if (conf >= minConfidence) {
                    char* word = ri->GetUTF8Text(level);
                    if (word && word[0] != '\0') {
                        TextResult tr;
                        tr.bbox = cv::Rect(
                            static_cast<int>(x1 / scale),
                            static_cast<int>(y1 / scale),
                            static_cast<int>((x2 - x1) / scale),
                            static_cast<int>((y2 - y1) / scale));
                        tr.text = QString::fromUtf8(word).trimmed();
                        tr.confidence = conf;
                        if (!tr.text.isEmpty()) {
                            results.push_back(tr);
                        }
                    }
                    if (word) delete[] word;
                }
            }
        } while (ri->Next(level));
        delete ri;
    }
    api.Clear();
    return results;
}

// ========== HSV 色相分区间多通道 OCR ==========
// 解决淡色文字（浅蓝/浅绿等）在灰度图上对比度低导致 OCR 漏识别的问题。
// 思路：将原图按 HSV 色相固定分区间生成二值掩码（文字像素=255，背景=0），
// 每个通道单独 OCR，结果带上通道代表色。同色文字归为同组，组内再做几何合并。
//
// 通道划分（OpenCV HSV: H∈[0,180], S∈[0,255], V∈[0,255]）：
//   彩色通道（S>50, V>50）：红/橙/黄/绿/青/蓝/紫
//   无彩色通道：黑（V<80, S<80）/ 白（V>200, S<30）
// 黑白通道仅作为补充，用于识别因压缩/抗锯齿色相不稳定的深浅文字。

struct HsvChannel {
    const char* name;       // 通道名称
    int hLow, hHigh;        // 色相区间（wrap=true 时表示 [hLow,180]∪[0,hHigh]）
    bool wrap;              // 色相是否跨越 0/180（红色）
    int sLow, sHigh;        // 饱和度区间
    int vLow, vHigh;        // 亮度区间
    cv::Scalar repColor;    // 代表色（BGR），用于设置识别结果的 avgColor
};

// 固定 HSV 色相区间通道列表
// 仅保留地图注记常见颜色：黑/淡蓝/品红/绿，减少通道数加快 OCR 速度
static const std::vector<HsvChannel>& hsvChannels()
{
    static const std::vector<HsvChannel> channels = {
        {"淡蓝", 95, 150,  false, 15, 255,  80, 255, cv::Scalar(220, 180,  80)},
        {"品红",125, 175,  false, 40, 255,  50, 255, cv::Scalar(160,  40, 160)},
        {"绿",  30,  85,  false, 40, 255,  50, 255, cv::Scalar( 60, 180,  60)},
    };
    return channels;
}

// 为指定 HSV 通道生成二值掩码（文字像素=255，背景=0）
static cv::Mat buildChannelMask(const cv::Mat& hsv, const HsvChannel& ch)
{
    cv::Mat mask;
    if (ch.wrap) {
        // 红色跨越 0/180：[hLow,180] ∪ [0,hHigh]
        cv::Mat m1, m2;
        cv::inRange(hsv, cv::Scalar(ch.hLow,  ch.sLow, ch.vLow),
                          cv::Scalar(180,      ch.sHigh, ch.vHigh), m1);
        cv::inRange(hsv, cv::Scalar(0,        ch.sLow, ch.vLow),
                          cv::Scalar(ch.hHigh, ch.sHigh, ch.vHigh), m2);
        cv::bitwise_or(m1, m2, mask);
    } else {
        cv::inRange(hsv, cv::Scalar(ch.hLow, ch.sLow, ch.vLow),
                          cv::Scalar(ch.hHigh, ch.sHigh, ch.vHigh), mask);
    }
    return mask;
}

// 多策略集成 OCR：用多种预处理(灰度/自适应二值化/Otsu二值化)与多种 PSM 模式
// 分别识别，按位置(中心距离+尺寸相似度)合并去重保留最高置信度，提升识别率与准确率。
// targetMinDim/claheClipLimit: 预处理参数（来自 XML "注记文字提取"）
// useColorChannels: 是否启用 HSV 分颜色通道识别（解决淡色文字漏识别）
// debugDir: 非空时将各颜色通道的掩码/OCR输入图/识别结果标注图保存到该目录，便于调试
static std::vector<TextResult> extractTextResults(const cv::Mat& img, float minConfidence = 40.0f,
                                                  int targetMinDim = 3500, double claheClipLimit = 2.0,
                                                  bool useColorChannels = true,
                                                  const QString& debugDir = QString())
{
    std::vector<TextResult> allCandidates;

    QString tessdataDir = qgetenv("TESSDATA_PREFIX");
    if (tessdataDir.isEmpty()) {
        tessdataDir = QStringLiteral("C:/Users/honlye/vcpkg/installed/x64-windows/share/tessdata");
    }

    tesseract::TessBaseAPI api;
    // 使用 chi_sim+eng：可识别地图上的数字/字母注记(河流代码、坐标等)
    // 显式指定 OEM_LSTM_ONLY：LSTM 引擎对中文识别准确率最高
    if (api.Init(tessdataDir.toStdString().c_str(), "chi_sim+eng",
                 tesseract::OEM_LSTM_ONLY) != 0) {
        return allCandidates;
    }

    // 公共预处理：放大 -> 灰度 -> CLAHE -> 双边滤波（未二值化）
    double scale = 1.0;
    cv::Mat baseGray = preprocessForOCR(img, scale, targetMinDim, claheClipLimit);

    // 准备 3 种预处理变体
    cv::Mat adaptiveBin, otsuBin;
    cv::adaptiveThreshold(baseGray, adaptiveBin, 255,
                          cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY, 31, 15);
    cv::threshold(baseGray, otsuBin, 0, 255,
                  cv::THRESH_BINARY | cv::THRESH_OTSU);

    // 策略矩阵：(图像, PSM)
    // 不同 PSM 适合不同布局，多 PSM 取并集可减少漏识别
    struct Strategy { const cv::Mat* mat; tesseract::PageSegMode psm; const char* name; };
    Strategy strategies[] = {
        { &adaptiveBin, tesseract::PSM_SPARSE_TEXT,       "adaptive+SPARSE" },
        { &otsuBin,     tesseract::PSM_SPARSE_TEXT,       "otsu+SPARSE" },
        { &baseGray,    tesseract::PSM_AUTO,              "gray+AUTO" },
        { &adaptiveBin, tesseract::PSM_AUTO,              "adaptive+AUTO" },
        { &otsuBin,     tesseract::PSM_SPARSE_TEXT_OSD,   "otsu+SPARSE_OSD" }
    };

    for (const auto& s : strategies) {
        qDebug("[OCR] 策略: %s", s.name);
        auto pass = runOcrPass(api, *s.mat, scale, s.psm, minConfidence);
        qDebug("[OCR]   -> 候选 %zu 项", pass.size());
        for (auto& tr : pass) allCandidates.push_back(std::move(tr));
    }

    // ===== HSV 分颜色通道 OCR（解决淡色文字漏识别）=====
    // 每个通道生成二值掩码 → 反转（文字=黑, 背景=白）→ OCR
    // 识别结果带上通道代表色，后续按颜色分组即可区分不同颜色文字
    if (useColorChannels) {
        cv::Mat hsv;
        cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);

        // 形态学核：连接断裂笔画
        cv::Mat morphKernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2));

        // 颜色通道 PSM 策略：掩码已二值化，只需 SPARSE 和 AUTO 两种
        struct ColorStrategy { tesseract::PageSegMode psm; const char* name; };
        ColorStrategy colorStrategies[] = {
            { tesseract::PSM_SPARSE_TEXT, "SPARSE" },
            { tesseract::PSM_AUTO,        "AUTO"  },
        };

        // 调试输出：非空目录时保存各通道图像
        // 文件命名：01_红_mask.png / 01_红_ocr.png / 01_红_result.png
        bool saveDebug = !debugDir.isEmpty();
        if (saveDebug) {
            QDir().mkpath(debugDir);
            // 同时保存一份原图便于对比
            imwriteCN(debugDir + "/00_原图.png", img);
        }

        int chIdx = 0;
        for (const auto& ch : hsvChannels()) {
            ++chIdx;
            cv::Mat mask = buildChannelMask(hsv, ch);
            int maskPixels = cv::countNonZero(mask);
            qDebug("[OCR-Color] 通道 '%s' 掩码像素=%d", ch.name, maskPixels);
            if (maskPixels == 0) {
                if (saveDebug) {
                    qDebug("[OCR-Color]   通道 '%s' 无像素，跳过", ch.name);
                }
                continue;
            }

            // 形态学闭运算：连接断裂笔画
            cv::Mat maskClosed;
            cv::morphologyEx(mask, maskClosed, cv::MORPH_CLOSE, morphKernel);
            // 放大（INTER_NEAREST 保持二值）
            cv::Mat scaled;
            cv::resize(maskClosed, scaled, cv::Size(), scale, scale, cv::INTER_NEAREST);
            // 反转：文字=0(黑), 背景=255(白)，适合 Tesseract
            cv::Mat ocrImg;
            cv::bitwise_not(scaled, ocrImg);

            // 收集本通道所有候选结果（用于绘制结果标注图）
            std::vector<TextResult> chResults;
            for (const auto& cs : colorStrategies) {
                auto pass = runOcrPass(api, ocrImg, scale, cs.psm, minConfidence);
                qDebug("[OCR-Color]   %s+%s -> 候选 %zu", ch.name, cs.name, pass.size());
                for (auto& tr : pass) {
                    tr.avgColor = ch.repColor;
                    tr.hasColor = true;
                    chResults.push_back(tr);
                    allCandidates.push_back(std::move(tr));
                }
            }

            // 保存调试图像
            if (saveDebug) {
                // 原始掩码（文字=白，背景=黑）
                imwriteCN(QString("%1/%2_%3_mask.png")
                    .arg(debugDir).arg(chIdx, 2, 10, QChar('0')).arg(ch.name), mask);
                // OCR 输入图（文字=黑，背景=白，Tesseract 实际看到的图）
                imwriteCN(QString("%1/%2_%3_ocr.png")
                    .arg(debugDir).arg(chIdx, 2, 10, QChar('0')).arg(ch.name), ocrImg);

                // 结果标注图：在 OCR 输入图上画候选框（彩色三通道以便观察）
                cv::Mat annotated;
                cv::cvtColor(ocrImg, annotated, cv::COLOR_GRAY2BGR);
                // 用通道代表色画框
                cv::Scalar boxColor = ch.repColor;
                for (const auto& tr : chResults) {
                    // bbox 在原图坐标，需按 scale 还原到 OCR 图坐标
                    cv::Rect scaledBox(
                        static_cast<int>(tr.bbox.x * scale),
                        static_cast<int>(tr.bbox.y * scale),
                        static_cast<int>(tr.bbox.width * scale),
                        static_cast<int>(tr.bbox.height * scale));
                    cv::rectangle(annotated, scaledBox, boxColor, 2);
                }
                imwriteCN(QString("%1/%2_%3_result.png")
                    .arg(debugDir).arg(chIdx, 2, 10, QChar('0')).arg(ch.name), annotated);
                qDebug("[OCR-Color]   通道 '%s' 调试图已保存到 %s", ch.name, debugDir.toUtf8().constData());
            }
        }
    }
    api.End();

    qDebug("[OCR] 总候选: %zu", allCandidates.size());

    // 过滤圆点误识别项（先过滤，避免噪声参与分组/合并）
    allCandidates = filterOcrNoise(allCandidates);

    // 先采样每个文字区域的代表颜色与字号（bbox 高度），用于分组。
    // 栅格图像存在色差，采样在单字阶段进行更准确（合并后框内背景占比大，颜色被稀释）。
    // 颜色通道识别的结果(hasColor=true)已带上通道代表色，无需再采样，
    // 且 sampleTextColor 假设文字比背景暗，对淡色文字不准确。
    for (auto& tr : allCandidates) {
        if (!tr.hasColor) {
            tr.avgColor = sampleTextColor(img, tr.bbox);
        }
        tr.fontSize = tr.bbox.height;
        tr.group    = -1;
    }

    // 按颜色 + 字号分组（容忍栅格色差，颜色/字号相近的归为同一组）。
    // 分组后再在同组内合并单字，避免把不同类别但空间相邻的注记误合并
    // （如河流名与旁边的城市名）。
    groupTextResults(allCandidates);

    // 调试：输出分组后单字分布，便于诊断"还有单字"问题
    {
        int nGroup = 0;
        for (auto& tr : allCandidates) nGroup = std::max(nGroup, tr.group + 1);
        qDebug("[OCR] 过滤后单字数=%zu, 分组数=%d", allCandidates.size(), nGroup);
        for (size_t i = 0; i < allCandidates.size(); ++i) {
            auto& tr = allCandidates[i];
            qDebug("[OCR] #%zu group=%d text='%s' bbox=[%d,%d,%dx%d] size=%d color=(%.0f,%.0f,%.0f)",
                i, tr.group, tr.text.toUtf8().constData(),
                tr.bbox.x, tr.bbox.y, tr.bbox.width, tr.bbox.height,
                tr.fontSize, tr.avgColor[0], tr.avgColor[1], tr.avgColor[2]);
        }
    }

    // 不再在此处合并，改由 onQualityCheck 在第一次匹配后进行，利用匹配结果指导合并
    return allCandidates;
}

// ========== PaddleOCR 支持函数 ==========

// 调用 PaddleOCR Python 脚本识别单张图片，返回 JSON 解析后的文字结果
// imgPath: 待识别图片路径（已生成临时文件）
// repColor: 该批次识别结果所代表的文字颜色（BGR），用于颜色分组
// debugDir: 非空时同时保存识别结果标注图到该目录
// debugName: 调试图文件名前缀（如 "01_红"）
static std::vector<TextResult> paddleOcrSingleImage(const QString& pythonPath,
                                                    const QString& scriptPath,
                                                    const QString& imgPath,
                                                    const cv::Scalar& repColor,
                                                    const QString& debugDir = QString(),
                                                    const QString& debugName = QString())
{
    std::vector<TextResult> results;

    QString paddleEnvDir = QFileInfo(pythonPath).absolutePath();
    QString jsonFilePath = paddleEnvDir + "/paddleocr_result_"
        + QString::number(QCoreApplication::applicationPid()) + ".json";
    QFile::remove(jsonFilePath);

    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove("PYTHONPATH");
    env.remove("PYTHONHOME");
    QFileInfo pyInfo(pythonPath);
    env.insert("PATH", pyInfo.absolutePath() + ";" + env.value("PATH", ""));
    proc.setProcessEnvironment(env);

    QStringList args;
    args << scriptPath << "ch" << imgPath << jsonFilePath;

    qDebug("[PaddleOCR] 启动: %s %s", pythonPath.toUtf8().constData(),
           args.join(" ").toUtf8().constData());

    proc.start(pythonPath, args);
    if (!proc.waitForStarted(10000)) {
        qDebug("[PaddleOCR] 进程启动失败");
        return results;
    }

    qDebug("[PaddleOCR] 等待进程结束...");
    proc.waitForFinished(-1);

    int exitCode = proc.exitCode();
    QByteArray output = proc.readAll();
    qDebug("[PaddleOCR] 进程结束，退出码=%d, 输出=%s",
           exitCode, QString(output).trimmed().toUtf8().constData());

    if (exitCode != 0) {
        qDebug("[PaddleOCR] 进程异常退出");
        return results;
    }

    QFile jsonFile(jsonFilePath);
    if (!jsonFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug("[PaddleOCR] 无法打开 JSON 文件: %s", jsonFilePath.toUtf8().constData());
        return results;
    }

    QByteArray jsonData = jsonFile.readAll();
    jsonFile.close();
    qDebug("[PaddleOCR] JSON 文件内容(%d bytes): %s",
           jsonData.size(), QString(jsonData).left(500).toUtf8().constData());
    jsonFile.remove();

    QJsonDocument doc = QJsonDocument::fromJson(jsonData);
    if (!doc.isArray()) {
        QJsonObject errObj = doc.object();
        if (errObj.contains("error")) {
            qDebug("[PaddleOCR] 脚本错误: %s",
                   errObj["error"].toString().toUtf8().constData());
        } else {
            qDebug("[PaddleOCR] JSON 不是数组格式: %s",
                   QString(jsonData).left(200).toUtf8().constData());
        }
        return results;
    }

    QJsonArray arr = doc.array();
    for (const QJsonValue& val : arr) {
        if (!val.isObject()) continue;
        QJsonObject obj = val.toObject();
        QString text = obj["text"].toString();
        int x1 = obj["x1"].toInt(0);
        int y1 = obj["y1"].toInt(0);
        int x2 = obj["x2"].toInt(0);
        int y2 = obj["y2"].toInt(0);
        double confidence = obj["confidence"].toDouble(0.0);

        if (text.isEmpty()) continue;

        TextResult tr;
        tr.bbox = cv::Rect(x1, y1, x2 - x1, y2 - y1);
        tr.text = text;
        tr.confidence = static_cast<float>(confidence * 100.0);
        tr.group = -1;
        tr.avgColor = repColor;
        tr.hasColor = true;

        results.push_back(tr);
    }

    // 保存调试结果标注图
    if (!debugDir.isEmpty() && !debugName.isEmpty() && !results.empty()) {
        cv::Mat annotated = imreadCN(imgPath);
        if (!annotated.empty()) {
            for (const auto& tr : results) {
                cv::rectangle(annotated, tr.bbox, repColor, 2);
            }
            imwriteCN(QString("%1/%2_result.png").arg(debugDir).arg(debugName), annotated);
            qDebug("[PaddleOCR] 调试图已保存: %s/%2_result.png",
                   debugDir.toUtf8().constData(), debugName.toUtf8().constData());
        }
    }

    qDebug("[PaddleOCR] 识别到 %zu 个文字", results.size());
    return results;
}

// PaddleOCR 识别：支持分颜色通道
// roi: 可选的多个多边形 ROI（原图坐标，像素），为空则全图识别
// useColorChannels: true=按 HSV 分通道识别（慢，可识别淡色文字）；false=原图单次识别（快）
// debugDir: 非空时将各颜色通道的掩码/OCR输入图/识别结果标注图保存到该目录
std::vector<TextResult> SE_ImageMapCheckDialog::paddleOcrImage(const QString& imgPath,
                                                              const std::vector<std::vector<cv::Point>>& roi,
                                                              bool useColorChannels,
                                                              const QString& debugDir,
                                                              std::vector<std::pair<QString, std::vector<TextResult>>>* channelRawResults,
                                                              bool* ok)
{
    std::vector<TextResult> results;
    if (ok) *ok = true;  // 默认成功, 仅在环境/进程异常时置 false

    QString pythonPath = "../paddle_env/Scripts/python.exe";
    QString scriptPath = "../paddle_env/paddleocr_service.py";

    if (!QFile::exists(pythonPath)) {
        qDebug("[PaddleOCR] python.exe 不存在: %s", pythonPath.toUtf8().constData());
        if (ok) *ok = false;
        return results;
    }
    if (!QFile::exists(scriptPath)) {
        qDebug("[PaddleOCR] 脚本不存在: %s", scriptPath.toUtf8().constData());
        if (ok) *ok = false;
        return results;
    }

    cv::Mat srcImg = imreadCN(imgPath);
    if (srcImg.empty()) {
        qDebug("[PaddleOCR] 无法读取图片: %s", imgPath.toUtf8().constData());
        if (ok) *ok = false;
        return results;
    }

    // ===== 如果有 ROI（多个多边形），将所有 ROI 外区域置白 =====
    // 面积判定: 防止境界线提取异常(断裂/碎化/过小)导致 ROI 覆盖区域过小,
    //   进而使 OCR 区域被错误限制、注记大量漏识。ROI 总面积 < 图像面积 20% 时视为异常, 弃用 ROI
    bool useRoi = !roi.empty();
    if (useRoi) {
        double roiArea = 0.0;
        for (const auto& poly : roi) {
            if (poly.size() >= 3) roiArea += std::abs(cv::contourArea(poly));
        }
        double imgArea = (double)srcImg.cols * srcImg.rows;
        double ratio = (imgArea > 0) ? (roiArea / imgArea) : 1.0;
        if (ratio < 0.2) {
            qDebug("[PaddleOCR] ROI 面积占比 %.4f 低于 20%%, 疑似境界线异常, 弃用 ROI 全图识别", ratio);
            useRoi = false;  // 弃用 ROI
        }
    }
    if (useRoi) {
        cv::Mat roiMask = cv::Mat::zeros(srcImg.size(), CV_8UC1);
        cv::fillPoly(roiMask, roi, cv::Scalar(255));
        srcImg.setTo(cv::Scalar(255, 255, 255), ~roiMask);
        qDebug("[PaddleOCR] ROI 遮罩已应用，%zu 个多边形", roi.size());
    }

    QString tempDir = QDir::tempPath();
    qlonglong pid = QCoreApplication::applicationPid();

    // ===== 不分颜色通道：原图单次识别（快速模式）=====
    if (!useColorChannels) {
        QString tmpImgPath = tempDir + QString("/paddleocr_src_%1.png").arg(pid);
        imwriteCN(tmpImgPath, srcImg, ".png");
        // 原图识别，颜色未知，用采样方式后续补充
        auto pass = paddleOcrSingleImage(pythonPath, scriptPath, tmpImgPath,
                                         cv::Scalar(0, 0, 0), QString(), QString());
        QFile::remove(tmpImgPath);
        for (auto& tr : pass) {
            tr.hasColor = false;  // 标记为无颜色，后续用 sampleTextColor 补充
            results.push_back(std::move(tr));
        }
        qDebug("[PaddleOCR] 单通道模式，识别到 %zu 个文字", results.size());
        return results;
    }

    // 调试图输出目录
    bool saveDebug = !debugDir.isEmpty();
    if (saveDebug) {
        QDir().mkpath(debugDir);
        imwriteCN(debugDir + "/00_原图.png", srcImg);
    }

    // ===== 分颜色通道识别（与 Tesseract 路径一致）=====
    // 多通道模式: 先对原图做一次完整识别(覆盖黑/白等所有颜色),
    // 再额外对彩色通道(淡蓝/品红/绿)用 HSV 掩码识别, 补充淡色文字
    cv::Mat hsv;
    cv::cvtColor(srcImg, hsv, cv::COLOR_BGR2HSV);

    cv::Mat morphKernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2));

    // 第 0 通道: 原图识别(等价于单通道模式, 覆盖所有颜色)
    {
        QString tmpImgPath = tempDir + QString("/paddleocr_src_%1.png").arg(pid);
        imwriteCN(tmpImgPath, srcImg, ".png");
        auto pass = paddleOcrSingleImage(pythonPath, scriptPath, tmpImgPath,
                                         cv::Scalar(0, 0, 0), QString(), QString());
        QFile::remove(tmpImgPath);
        for (auto& tr : pass) {
            tr.hasColor = false;
            results.push_back(std::move(tr));
        }
        qDebug("[PaddleOCR-Color] 原图通道, 识别到 %zu 个文字", results.size());

        // 保存原图通道原始结果用于调试
        if (channelRawResults) {
            channelRawResults->push_back({"原图", results});
        }

        if (saveDebug) {
            imwriteCN(QString("%1/00_原图_ocr.png").arg(debugDir), srcImg);
        }
    }

    // 彩色通道识别: 对淡蓝/品红/绿等做 HSV 掩码, 补充原图识别漏掉的淡色文字
    int chIdx = 0;
    for (const auto& ch : hsvChannels()) {
        ++chIdx;

        cv::Mat mask = buildChannelMask(hsv, ch);
        int maskPixels = cv::countNonZero(mask);
        qDebug("[PaddleOCR-Color] 通道 '%s' 掩码像素=%d", ch.name, maskPixels);
        if (maskPixels == 0) continue;

        // 形态学闭运算：连接断裂笔画
        cv::Mat maskClosed;
        cv::morphologyEx(mask, maskClosed, cv::MORPH_CLOSE, morphKernel);

        // PaddleOCR 期望彩色图输入：将二值掩码转为白底黑字的三通道图
        // 掩码中文字=255(白)，反转后文字=0(黑)、背景=255(白)，再转 BGR
        cv::Mat ocrGray, ocrBgr;
        cv::bitwise_not(maskClosed, ocrGray);
        cv::cvtColor(ocrGray, ocrBgr, cv::COLOR_GRAY2BGR);

        // 保存临时图片
        QString channelImgPath = tempDir + QString("/paddleocr_ch%1_%2.png").arg(pid).arg(chIdx);
        imwriteCN(channelImgPath, ocrBgr, ".png");

        // 保存调试图
        QString debugName;
        if (saveDebug) {
            debugName = QString("%1_%2").arg(chIdx, 2, 10, QChar('0')).arg(ch.name);
            // OCR 输入图（文字=黑，背景=白）
            imwriteCN(QString("%1/%2_ocr.png").arg(debugDir).arg(debugName), ocrBgr);
        }

        // 调用 Python 识别
        auto chResults = paddleOcrSingleImage(pythonPath, scriptPath, channelImgPath,
                                              ch.repColor, debugDir, debugName);
        qDebug("[PaddleOCR-Color]   通道 '%s' -> 候选 %zu", ch.name, chResults.size());

        // 保存该通道原始结果用于调试(在 move 之前)
        if (channelRawResults) {
            channelRawResults->push_back({ch.name, chResults});
        }

        for (auto& tr : chResults) {
            results.push_back(std::move(tr));
        }

        // 清理临时图片
        QFile::remove(channelImgPath);
    }

    qDebug("[PaddleOCR-Color] 总候选: %zu", results.size());

    // DEBUG: 输出每通道原始结果摘要
    if (channelRawResults) {
        for (auto& [chName, chRes] : *channelRawResults) {
            qDebug("[PaddleOCR-Debug] 通道 '%s' 原始结果 %zu 项:", chName.toUtf8().constData(), chRes.size());
            int shown = 0;
            for (auto& tr : chRes) {
                if (shown++ >= 15) { qDebug("  ... (省略剩余)"); break; }
                qDebug("  '%s' conf=%.1f bbox=[%d,%d,%dx%d]",
                       tr.text.toUtf8().constData(), tr.confidence,
                       tr.bbox.x, tr.bbox.y, tr.bbox.width, tr.bbox.height);
            }
        }
    }

    qDebug("[PaddleOCR-Color] 总候选: %zu", results.size());
    return results;
}

// ========== 继续原有函数 ==========

// 合并空间上相邻的文字框：仅在【同一分组】内，将相邻单字合并为整词。
// 同时支持横排（水平相邻）和竖排（垂直相邻）注记。
// 采用并查集（union-find）做空间邻近聚类：两两比较所有同组单字，
// 满足"行/列对齐 + 间距小"则合并到同一连通分量。这样保证链式合并
// （A-B-C 即使 A、C 不相邻也能合并），且不受扫描顺序影响。
// 最后按阅读顺序（横排按 x、竖排按 y）排序拼接文字。
//
// 匹配结果指导合并（调用前应先做一次地名库匹配）：
//   - 两个字匹配到同一地名 → 放宽距离限制（它们必属同一词）
//   - 两个字匹配到不同地名 → 不合并（属于不同注记）
//   - 任一字未匹配 → 用标准距离判定
static std::vector<TextResult> mergeNearbyTextResults(std::vector<TextResult> inputs,
                                                       const QStringList& gazetteerList)
{
    const size_t n = inputs.size();
    if (n <= 1) return inputs;

    // 并查集
    std::vector<size_t> parent(n);
    for (size_t i = 0; i < n; ++i) parent[i] = i;
    auto find = [&](size_t x) -> size_t {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](size_t a, size_t b) {
        a = find(a); b = find(b);
        if (a != b) parent[b] = a;
    };

    // 判断 i、j 是否应合并到同一词（假设已同组）
    auto adjacent = [&](size_t i, size_t j) -> bool {
        if (inputs[i].group != inputs[j].group) return false;
        const cv::Rect& a = inputs[i].bbox;
        const cv::Rect& b = inputs[j].bbox;
        // 字号相似度：高度比 > 0.75 才视为同字号
        double hRatio = static_cast<double>(std::min(a.height, b.height))
                      / std::max(a.height, b.height);
        if (hRatio < 0.75) return false;

        double refH = std::min(a.height, b.height);
        double refW = std::min(a.width,  b.width);
        double aYc = a.y + a.height / 2.0, bYc = b.y + b.height / 2.0;
        double aXc = a.x + a.width  / 2.0, bXc = b.x + b.width  / 2.0;
        double hGap = std::max(a.x, b.x) - std::min(a.x + a.width,  b.x + b.width);
        double vGap = std::max(a.y, b.y) - std::min(a.y + a.height, b.y + b.height);

        // 匹配结果指导合并：
        //   - 同一 matchedName（非空）→ 放宽距离到 2.0 倍，对齐容差 0.6
        //   - 不同 matchedName（都非空）→ 直接返回 false（不同注记）
        //   - 任一未匹配 → 标准距离（0.5 倍）+ 对齐 0.4
        const QString& mi = inputs[i].matchedName;
        const QString& mj = inputs[j].matchedName;
        bool bothMatched = !mi.isEmpty() && !mj.isEmpty();
        bool sameMatch   = bothMatched && (mi == mj);

        double gapLimit, alignLimitH, alignLimitW;
        if (sameMatch) {
            // 同地名：放宽，因为它们必属同一词
            gapLimit   = refH * 2.0;
            alignLimitH = refH * 0.6;
            alignLimitW = refW * 0.6;
        } else if (bothMatched) {
            // 不同地名：不合并
            return false;
        } else {
            // 任一未匹配：标准距离
            gapLimit    = refH * 0.5;
            alignLimitH = refH * 0.4;
            alignLimitW = refW * 0.4;
        }

        bool horizontal = std::abs(aYc - bYc) <= alignLimitH && hGap <= gapLimit;
        bool vertical   = std::abs(aXc - bXc) <= alignLimitW && vGap <= gapLimit;
        return horizontal || vertical;
    };

    // 两两比较，满足条件则合并
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            if (adjacent(i, j)) unite(i, j);
        }
    }

    // 按 root 收集聚类
    std::map<size_t, std::vector<size_t>> clusters;
    for (size_t i = 0; i < n; ++i) {
        clusters[find(i)].push_back(i);
    }

    auto unionBox = [](const cv::Rect& a, const cv::Rect& b) {
        int x1 = std::min(a.x, b.x);
        int y1 = std::min(a.y, b.y);
        int x2 = std::max(a.x + a.width,  b.x + b.width);
        int y2 = std::max(a.y + a.height, b.y + b.height);
        return cv::Rect(x1, y1, x2 - x1, y2 - y1);
    };

    std::vector<TextResult> merged;
    merged.reserve(clusters.size());
    for (auto& kv : clusters) {
        auto& pieces = kv.second;
        // 聚类并集框(仅用于判断阅读方向)
        cv::Rect box = inputs[pieces[0]].bbox;
        for (size_t p : pieces) box = unionBox(box, inputs[p].bbox);

        // 按阅读顺序排序：宽 >= 高 视为横排（按 x），否则竖排（按 y）
        bool isHorizontal = box.width >= box.height;
        std::sort(pieces.begin(), pieces.end(), [&](size_t a, size_t b) {
            if (isHorizontal) {
                double ax = inputs[a].bbox.x + inputs[a].bbox.width  / 2.0;
                double bx = inputs[b].bbox.x + inputs[b].bbox.width  / 2.0;
                if (std::abs(ax - bx) > 1) return ax < bx;
                return inputs[a].bbox.y < inputs[b].bbox.y;
            } else {
                double ay = inputs[a].bbox.y + inputs[a].bbox.height / 2.0;
                double by = inputs[b].bbox.y + inputs[b].bbox.height / 2.0;
                if (std::abs(ay - by) > 1) return ay < by;
                return inputs[a].bbox.x < inputs[b].bbox.x;
            }
        });

        TextResult cur;
        // bbox: 以第一个子片段(阅读顺序)坐标为准, 不取并集
        cur.bbox = inputs[pieces[0]].bbox;
        cur.group = inputs[pieces[0]].group;
        cur.confidence = 100.0f;
        cur.avgColor = inputs[pieces[0]].avgColor;
        cur.fontSize = inputs[pieces[0]].fontSize;
        for (size_t p : pieces) {
            cur.text += inputs[p].text;
            cur.confidence = std::min(cur.confidence, inputs[p].confidence);
            // 保留敏感标记：任一片段命中敏感词，合并结果也标记为敏感
            // （合并后的文本必包含命中片段的文本，即必包含敏感词）
            if (inputs[p].sensitive) {
                cur.sensitive = true;
                if (cur.sensitiveCategory.isEmpty()) {
                    cur.sensitiveCategory = inputs[p].sensitiveCategory;
                    cur.sensitiveWord = inputs[p].sensitiveWord;
                }
            }
        }
        merged.push_back(cur);
    }

    // 注意: 单字合并已移至 mergeSinglesByLayerGeometry(基于线/面要素地名)和
    //       gazetteerGuidedMerge(基于地名库引导补全), 此处不再做模糊单字合并

    return merged;
}

// (已移除像素坐标模式, 所有输出均使用地理坐标)

void SE_ImageMapCheckDialog::maskTextRegions(cv::Mat& img, const std::vector<TextResult>& texts) const
{
    for (const auto& tr : texts) {
        // 稍微扩大包围框，确保文字笔画被完全覆盖
        cv::Rect r = tr.bbox;
        r.x = std::max(0, r.x - 3);
        r.y = std::max(0, r.y - 3);
        r.width  = std::min(img.cols - r.x, r.width  + 6);
        r.height = std::min(img.rows - r.y, r.height + 6);
        // 用白色填充（与背景接近，避免产生新边缘）
        cv::rectangle(img, r, cv::Scalar(255, 255, 255), cv::FILLED);
    }
}

void SE_ImageMapCheckDialog::onExtractText()
{
    addCheckLog("开始提取注记");
    if (m_imagePath.isEmpty()) {
        // QMessageBox::warning(this, "提示", "请先选择图片");  // 独立执行弹窗已移除
        return;
    }
    cv::Mat img = imreadCN(m_imagePath);
    if (img.empty()) {
        // QMessageBox::warning(this, "错误", "无法读取图片");  // 独立执行弹窗已移除
        return;
    }

    // ===== ROI: 使用已提取的境界线轮廓作为多边形 ROI（无境界线则全图识别） =====
    // 境界线轮廓为图像坐标（Point2f），转成整数 Point 多边形传给 OCR 引擎。
    // 注意：境界线是细线，作为多边形填充的覆盖区域有限，实际作用是限制
    //       OCR 只在境界线附近识别，避免远离境界线的背景文字干扰。
    std::vector<std::vector<cv::Point>> textRoi;
    if (m_hasResult && !m_lastContours.empty()) {
        textRoi.reserve(m_lastContours.size());
        for (const auto& contour : m_lastContours) {
            if (contour.size() < 2) continue;  // 单点或空轮廓跳过
            std::vector<cv::Point> poly;
            poly.reserve(contour.size());
            for (const auto& pt : contour) {
                poly.emplace_back((int)std::round(pt.x), (int)std::round(pt.y));
            }
            textRoi.push_back(std::move(poly));
        }
        qDebug("[OCR] 使用境界线作为 ROI: %zu 条轮廓", textRoi.size());
    } else {
        qDebug("[OCR] 未提取境界线，全图识别");
    }

    if (m_oneClickMode) { setProgressSmooth(50, "注记: 准备OCR识别 %p%"); }

    // ===== OCR 提取文字注记及位置范围 =====
    // 分颜色通道识别: 由界面复选框控制(原 XML 配置已移至界面)
    bool useColorChannels = m_cbColorChannel->isChecked();
    // 收集每通道原始结果, 外层声明供后续匹配/合并阶段判断多通道模式
    std::vector<std::pair<QString, std::vector<TextResult>>> channelRawResults;
    // ===== OCR 引擎策略: PaddleOCR 优先, 失败则 Tesseract 兜底 =====
    bool paddleOk = false;
    {
        qDebug("[OCR] 尝试 PaddleOCR 引擎");
        // 分颜色通道识别：由界面复选框控制
        addCheckLog(QString("分颜色通道识别: %1").arg(useColorChannels ? "开启" : "关闭"));
        // 调试图输出开关（开发调试用，发布前改为 false）
        const bool saveChannelDebug = false;
        QString debugDir;
        if (saveChannelDebug && useColorChannels) {
            QFileInfo imgInfo(m_imagePath);
            debugDir = imgInfo.absolutePath() + "/" + imgInfo.completeBaseName() + "_ocr_channels";
        }
        // channelRawResults 已在外层声明
        m_textResults = paddleOcrImage(m_imagePath, textRoi, useColorChannels, debugDir, &channelRawResults, &paddleOk);
        if (!debugDir.isEmpty()) {
            addCheckLog(QString("颜色通道调试图已保存到: %1").arg(debugDir));
        }

        if (paddleOk && !m_textResults.empty()) {
        // 分通道汇总: 每个通道内只做 补色→分组→汇总, 然后跨通道去重
        // 敏感词检查、地名匹配、合并等统一在去重后执行
        if (useColorChannels && !channelRawResults.empty()) {
            m_textResults.clear();
            if (m_debugLayers) m_channelRawResults.clear();
            for (auto& [chName, chResults] : channelRawResults) {
                if (chResults.empty()) continue;
                // 1. 补色: 原图通道用 sampleTextColor 采样真实颜色, 彩色通道已有 repColor
                for (auto& tr : chResults) {
                    if (!tr.hasColor) {
                        tr.avgColor = sampleTextColor(img, tr.bbox);
                    }
                    tr.fontSize = tr.bbox.height;
                    tr.group = -1;
                    tr.channelName = chName;
                    tr.isOriginalChannel = (chName == "原图");
                }
                // 2. 分组(同通道内颜色一致, 分组更稳定)
                groupTextResults(chResults);
                // 3. 保存各通道原始结果副本(用于输出各通道"识别结果"图层, 需开启 m_debugLayers)
                if (m_debugLayers) m_channelRawResults.push_back({chName, chResults});
                // 4. 汇总到 m_textResults
                for (auto& tr : chResults) {
                    m_textResults.push_back(std::move(tr));
                }
            }
            // 跨通道去重: 不同通道可能在相同位置识别到同一文字, 保留更优者
            // 多阈值设计:
            //   1. 文字相同 + 位置接近(中心距 < 0.5*max(高度)) → 重复, 保留更优者
            //   2. 文字相同 + 字号接近(高度比 > 0.8) → 重复(同词不同通道)
            //   3. 文字不同 + 位置完全重叠 + 尺寸比 > 0.6 → 重复(OCR差异)
            //   4. 位置接近 + 一个文字是另一个的子串 → 重复, 保留更长的
            //   优先级: 彩色通道 > 原图通道 → 字数多 > 字数少 → 置信度高 > 低
            {
                std::vector<bool> removeFlag(m_textResults.size(), false);
                for (size_t i = 0; i < m_textResults.size(); ++i) {
                    if (removeFlag[i]) continue;
                    const auto& ti = m_textResults[i];
                    double iCx = ti.bbox.x + ti.bbox.width  / 2.0;
                    double iCy = ti.bbox.y + ti.bbox.height / 2.0;
                    for (size_t j = i + 1; j < m_textResults.size(); ++j) {
                        if (removeFlag[j]) continue;
                        auto& tj = m_textResults[j];
                        double jCx = tj.bbox.x + tj.bbox.width  / 2.0;
                        double jCy = tj.bbox.y + tj.bbox.height / 2.0;
                        double dx = std::abs(iCx - jCx);
                        double dy = std::abs(iCy - jCy);
                        double refH = std::max(ti.bbox.height, tj.bbox.height);
                        double ratio = static_cast<double>(std::min(ti.bbox.height, tj.bbox.height))
                                     / std::max(ti.bbox.height, tj.bbox.height);
                        bool sameText = (ti.text == tj.text);
                        bool samePos  = (dx < refH * 0.5) && (dy < refH * 0.5);
                        // 宽松位置阈值: 同词不同通道可能位置略有偏移,
                        // 但仍需在合理范围内, 防止远距离同词结果被误判为重复
                        bool loosePos = (dx < refH * 2.0) && (dy < refH * 2.0);
                        bool fuzzyText = samePos && (ti.text.contains(tj.text) || tj.text.contains(ti.text));

                        bool isDup = false;
                        if (sameText) {
                            // 同文同位 → 重复; 或同文+字号接近+位置较近 → 重复(同词不同通道)
                            isDup = loosePos && (ratio > 0.8);
                        } else if (fuzzyText) {
                            isDup = true;
                        } else {
                            isDup = samePos && (ratio > 0.8);
                        }
                        if (!isDup) continue;

                        // 决定保留哪个: 彩色通道 > 原图通道 → 字数多 > 字数少 → 置信度高
                        bool keepI;
                        if (ti.hasColor != tj.hasColor) {
                            keepI = ti.hasColor;
                        } else if (ti.text.size() != tj.text.size()) {
                            keepI = (ti.text.size() > tj.text.size());
                        } else if (ti.confidence != tj.confidence) {
                            keepI = (ti.confidence > tj.confidence);
                        } else {
                            keepI = true;
                        }
                        if (keepI) {
                            qDebug("[Dedup] 移除 j='%s' @(%d,%d) 保留 i='%s' @(%d,%d) dx=%.0f dy=%.0f samePos=%d loosePos=%d ratio=%.2f",
                                   tj.text.toUtf8().constData(), (int)jCx, (int)jCy,
                                   ti.text.toUtf8().constData(), (int)iCx, (int)iCy,
                                   dx, dy, (int)samePos, (int)loosePos, ratio);
                            removeFlag[j] = true;
                        } else {
                            qDebug("[Dedup] 移除 i='%s' @(%d,%d) 保留 j='%s' @(%d,%d) dx=%.0f dy=%.0f samePos=%d loosePos=%d ratio=%.2f",
                                   ti.text.toUtf8().constData(), (int)iCx, (int)iCy,
                                   tj.text.toUtf8().constData(), (int)jCx, (int)jCy,
                                   dx, dy, (int)samePos, (int)loosePos, ratio);
                            removeFlag[i] = true; break;
                        }
                    }
                }
                std::vector<TextResult> deduped;
                deduped.reserve(m_textResults.size());
                int removed = 0;
                for (size_t i = 0; i < m_textResults.size(); ++i) {
                    if (removeFlag[i]) { ++removed; continue; }
                    deduped.push_back(std::move(m_textResults[i]));
                }
                m_textResults = std::move(deduped);
                qDebug("[OCR] 跨通道去重, 移除 %d 项, 剩余 %zu", removed, m_textResults.size());
            }
            // 保存去重后结果到原始OCR结果(用于"识别结果"图层)
            m_rawOcrResults = m_textResults;
            qDebug("[OCR] 分通道汇总去重后: %zu 项", m_textResults.size());
        } else {
            // 单通道模式: 统一补色分组
            for (auto& tr : m_textResults) {
                if (!tr.hasColor) {
                    tr.avgColor = sampleTextColor(img, tr.bbox);
                }
                tr.fontSize = tr.bbox.height;
                tr.group = -1;
            }
            groupTextResults(m_textResults);
            // 保存去重后结果(单通道无跨通道去重, 直接保存补色分组后结果)
            m_rawOcrResults = m_textResults;
        }
        m_textResults = filterOcrNoise(std::move(m_textResults));
        } // end if (paddleOk && !m_textResults.empty())
    } // end PaddleOCR 块

    // 记录最终使用的引擎: true=PaddleOCR 成功, false=Tesseract 兜底
    bool ocrIsPaddle = paddleOk && !m_textResults.empty();

    // PaddleOCR 不可用或无结果, 切换 Tesseract 兜底
    if (!paddleOk || m_textResults.empty()) {
        qDebug("[OCR] PaddleOCR 不可用或无结果, 切换 Tesseract 兜底");
        addCheckLog("PaddleOCR 不可用, 已切换 Tesseract 兜底");
        // Tesseract 引擎固定参数（PaddleOCR 为主引擎, Tesseract 仅作备用, 参数不再可配）
        const float  ocrMinConf = 40.0f;
        const int    ocrMinDim  = 3500;
        const double claheClip  = 2.0;
        // 分颜色通道识别：由界面复选框控制
        // 将原图按 HSV 色相分区间生成二值掩码分别 OCR，
        // 解决淡色文字（水系淡蓝等）在灰度图上对比度低导致漏识别的问题
        // useColorChannels 已在外层声明
        // 调试图输出开关（开发调试用，发布前改为 false）
        // 开启后在原图同目录下生成 <图片名>_ocr_channels/ 子目录，
        // 保存各通道的 mask/OCR输入图/识别结果标注图，便于检查通道划分效果
        const bool saveChannelDebug = false;
        QString debugDir;
        if (saveChannelDebug) {
            QFileInfo imgInfo(m_imagePath);
            debugDir = imgInfo.absolutePath() + "/" + imgInfo.completeBaseName() + "_ocr_channels";
        }
        m_textResults = extractTextResults(img, ocrMinConf, ocrMinDim, claheClip, useColorChannels, debugDir);
        if (!debugDir.isEmpty()) {
            addCheckLog(QString("颜色通道调试图已保存到: %1").arg(debugDir));
        }
    }
    qDebug("[OCR] 提取后(未合并): %zu 项", m_textResults.size());

    if (m_oneClickMode) { setProgressSmooth(58, "注记: OCR识别完成 %p%"); }

    // ===== 敏感文字合规性检查（在地名匹配前执行）=====
    // 命中敏感词的注记标记 sensitive=true，后续地名匹配时跳过
    // 跨通道去重后统一执行
    int sensitiveCount = 0;
    addCheckLog("开始敏感文字合规性检查");
    for (auto& tr : m_textResults) {
        tr.sensitive = false;
        tr.sensitiveCategory.clear();
        tr.sensitiveWord.clear();
    }
    checkSensitiveWords();
    sensitiveCount = m_sensitiveHitCount;
    if (m_sensitiveWords.isEmpty()) {
        addCheckLog("敏感文字合规性检查: 未加载敏感词表，跳过检查");
    } else {
        addCheckLog(QString("敏感文字合规性检查完成: 命中 %1 项 (共 %2 项注记), 规则文件: %3")
            .arg(sensitiveCount).arg(m_textResults.size()).arg(m_sensitiveRulesPath));
    }

    if (m_oneClickMode) { setProgressSmooth(64, "注记: 敏感词检查完成 %p%"); }

    // ===== 地名库匹配 + 合并 + 重新识别 反馈循环 =====
    int matchedCount = 0;
    int reRecognizedCount = 0;
    if (!m_gazetteer.isEmpty()) {
        // 加载带几何信息的地名库(用于缓冲区限定匹配)
        loadGazetteerWithGeometry();

        // 第一次匹配（单字阶段）：为每个字找到所属地名，指导后续合并
        // 注意：敏感注记(sensitive=true)不参与地名匹配
        matchedCount = matchAgainstGazetteer(m_textResults);
        qDebug("[Gazetteer] 第一次匹配(单字): %zu 中 %d 项匹配成功", m_textResults.size(), matchedCount);

        if (m_oneClickMode) { setProgressSmooth(68, "注记: 第一次地名匹配 %p%"); }

        // 单字合并(基于 gdb 图层几何): 先于其他合并, 利用要素几何收集内部单字,
        // 按 线图层 → 面图层 顺序, 精确(不缺字) → 模糊(允许缺字) 两轮合并
        int mergedByGeom = mergeSinglesByLayerGeometry(m_textResults);
        if (mergedByGeom > 0) {
            addCheckLog(QString("单字合并(图层几何): 合并产生 %1 个新注记").arg(mergedByGeom));
        }

        // 其他合并：利用匹配结果指导合并
        //   - 同地名的字放宽距离合并
        //   - 不同地名的字不合并
        //   - 未匹配的字用标准距离
        m_textResults = mergeNearbyTextResults(std::move(m_textResults), m_gazetteerList);
        m_textResults = filterOcrNoise(std::move(m_textResults));
        // 地名引导合并: 用地名库引导补全前缀/后缀
        gazetteerGuidedMerge(m_textResults);
        qDebug("[OCR] 合并后: %zu 项", m_textResults.size());

        // 全局第二次匹配：所有合并完成后统一匹配
        matchedCount = matchAgainstGazetteer(m_textResults);
        qDebug("[Gazetteer] 第二次匹配(合并后): %zu 中 %d 项匹配成功", m_textResults.size(), matchedCount);

        if (m_oneClickMode) { setProgressSmooth(72, "注记: 第二次地名匹配 %p%"); }

        // 对仍未匹配的项重新识别（放大区域、多策略）
        // 注意：敏感注记(sensitive=true)不参与重新识别
        // 注意：PaddleOCR 引擎不做重新识别（避免与 Tesseract 混用导致结果不一致）
        std::vector<TextResult> unmatched;
        for (const auto& tr : m_textResults) {
            if (!tr.verified && !tr.sensitive) unmatched.push_back(tr);
        }
        if (!unmatched.empty() && !ocrIsPaddle) {
            qDebug("[Gazetteer] 对 %zu 项未匹配结果重新识别", unmatched.size());
            auto reResults = reRecognizeUnmatched(img, unmatched);
            reRecognizedCount = static_cast<int>(reResults.size());
            // 用重新识别结果替换未匹配项，并过滤纯数字/符号
            // 注意：保留 verified 项和 sensitive 项（敏感注记不参与地名匹配流程）
            std::vector<TextResult> verified;
            for (const auto& tr : m_textResults) {
                if (tr.verified || tr.sensitive) verified.push_back(tr);
            }
            reResults = filterOcrNoise(std::move(reResults));
            for (auto& tr : reResults) verified.push_back(tr);
            m_textResults = std::move(verified);

            if (m_oneClickMode) { setProgressSmooth(75, "注记: 重新识别未匹配项 %p%"); }

            // 第三次匹配
            matchedCount = matchAgainstGazetteer(m_textResults);
            qDebug("[Gazetteer] 第三次匹配(重新识别后): %zu 中 %d 项匹配成功", m_textResults.size(), matchedCount);
        } else if (!unmatched.empty()) {
            qDebug("[Gazetteer] PaddleOCR 引擎，跳过重新识别 (%zu 项未匹配)", unmatched.size());
        }
    } else {
        // 无地名库：直接用标准距离合并
        m_textResults = mergeNearbyTextResults(std::move(m_textResults), m_gazetteerList);
        m_textResults = filterOcrNoise(std::move(m_textResults));
    }

    // 在原图上标注文字区域，仅画分组色框（不绘制文字，改用表格展示）
    // 敏感注记用红色加粗框标注
    if (m_oneClickMode) { setProgressSmooth(78, "注记: 绘制标注 %p%"); }

    cv::Mat imgWithText = img.clone();
    for (const auto& tr : m_textResults) {
        if (tr.sensitive) {
            cv::rectangle(imgWithText, tr.bbox, cv::Scalar(0, 0, 255), 3);
        } else {
            cv::Scalar gc = groupColor(tr.group);
            cv::rectangle(imgWithText, tr.bbox, gc, 2);
        }
    }
    int groupCount = 0;
    for (const auto& tr : m_textResults) groupCount = std::max(groupCount, tr.group + 1);
    QString matchInfo = m_gazetteer.isEmpty() ? QString()
        : QString(" | 匹配 %1/%2").arg(matchedCount).arg(m_textResults.size());
    if (sensitiveCount > 0) {
        matchInfo += QString(" | 敏感 %1").arg(sensitiveCount);
    }
    setPreviewImage(true, imgWithText,
        QString("原图（文字: %1 处，分组: %2 类%3）").arg(m_textResults.size()).arg(groupCount).arg(matchInfo));

    // 更新表格：展示未匹配项和纠错项
    updateTextTable();

    int textCount = static_cast<int>(m_textResults.size());
    QString matchSummary;
    if (!m_gazetteer.isEmpty()) {
        matchSummary = QString("匹配 %1/%2\n").arg(matchedCount).arg(textCount);
        addCheckLog(QString("注记提取完成: %1 处 (%2 类), 地名库匹配 %3/%4, 敏感文字 %5")
            .arg(textCount).arg(groupCount).arg(matchedCount).arg(textCount).arg(sensitiveCount));
    } else {
        addCheckLog(QString("注记提取完成: %1 处 (%2 类), 未加载地名库, 敏感文字 %3")
            .arg(textCount).arg(groupCount).arg(sensitiveCount));
    }
    if (sensitiveCount > 0) {
        matchSummary += QString("敏感 %1\n").arg(sensitiveCount);
    }
    if (!m_oneClickMode) {
        // QMessageBox::information(this, "注记提取完成",  // 独立执行弹窗已移除
        //     QString("文字 %1 处（%2 类）\n%3").arg(textCount).arg(groupCount).arg(matchSummary));
    }
}

// 过滤二值图中面积过小的连通域（去噪点块）
static cv::Mat removeSmallComponents(const cv::Mat& binary, int minArea)
{
    cv::Mat labels, stats, centroids;
    int n = cv::connectedComponentsWithStats(binary, labels, stats, centroids, 8, CV_32S);
    cv::Mat out = cv::Mat::zeros(binary.size(), binary.type());
    for (int i = 1; i < n; ++i) {  // 跳过背景(0)
        if (stats.at<int>(i, cv::CC_STAT_AREA) >= minArea) {
            cv::Mat reg = (labels == i);
            out.setTo(255, reg);
        }
    }
    return out;
}

// 在 HSV 图中按颜色+容差生成 mask（H 环形处理）
static cv::Mat colorMask(const cv::Mat& hsv, const cv::Scalar& bgr, int tolerance)
{
    cv::Mat color_mat(1, 1, CV_8UC3, bgr);
    cv::Mat color_hsv;
    cv::cvtColor(color_mat, color_hsv, cv::COLOR_BGR2HSV);
    cv::Vec3b h = color_hsv.at<cv::Vec3b>(0, 0);
    int H = h[0], S = h[1], V = h[2];

    int h_tol = tolerance;
    int s_tol = std::min(255, tolerance * 2);
    int v_tol = std::min(255, tolerance * 2);

    // 对低 V（接近黑色）的颜色，HSV 分割效果差：H 不稳定，V 主导
    // 黑色境界线噪点会把 V 抬高到 80-120，需要大幅放宽 V 上限
    // 同时黑色 S 也接近 0 且不稳定，S 也需放宽
    if (V < 40) {
        v_tol = std::min(255, 120);     // V 上限放到 120，覆盖大部分噪点
        s_tol = std::min(255, S + 80);  // S 大幅放宽（黑色 S 接近 0，噪点 S 可能波动大）
        h_tol = tolerance * 2;          // H 不稳定，放宽
    }

    int s_low = std::max(0, S - s_tol),   s_high = std::min(255, S + s_tol);
    int v_low = std::max(0, V - v_tol),   v_high = std::min(255, V + v_tol);

    int h_low = H - h_tol;
    int h_high = H + h_tol;
    cv::Mat mask;
    if (h_low < 0) {
        cv::Mat mask1, mask2;
        cv::inRange(hsv, cv::Scalar(180 + h_low, s_low, v_low), cv::Scalar(180, s_high, v_high), mask1);
        cv::inRange(hsv, cv::Scalar(0, s_low, v_low), cv::Scalar(h_high, s_high, v_high), mask2);
        cv::bitwise_or(mask1, mask2, mask);
    } else if (h_high > 180) {
        cv::Mat mask1, mask2;
        cv::inRange(hsv, cv::Scalar(h_low, s_low, v_low), cv::Scalar(180, s_high, v_high), mask1);
        cv::inRange(hsv, cv::Scalar(0, s_low, v_low), cv::Scalar(h_high - 180, s_high, v_high), mask2);
        cv::bitwise_or(mask1, mask2, mask);
    } else {
        cv::inRange(hsv, cv::Scalar(h_low, s_low, v_low), cv::Scalar(h_high, s_high, v_high), mask);
    }
    return mask;
}

std::vector<std::vector<cv::Point2f>> SE_ImageMapCheckDialog::extractContoursByColor(const cv::Mat& workImg)
{
    cv::Scalar halo_color(m_currentHaloColor.blue(), m_currentHaloColor.green(), m_currentHaloColor.red());
    cv::Scalar outer_halo_color(m_currentOuterHaloColor.blue(), m_currentOuterHaloColor.green(), m_currentOuterHaloColor.red());
    // 容差优先取滑块值；若滑块为默认值(5)，则尝试用XML配置覆盖
    int tolerance = m_sliderTolerance->value();
    if (tolerance == 5) {
        tolerance = static_cast<int>(configByIdReader().getRuleValue(ITEM_ID_BOUNDARY_COLOR, "颜色容差", 5.0));
    }
    // 小连通域过滤面积从 XML 读取（默认20）
    int minArea = static_cast<int>(configByIdReader().getRuleValue(ITEM_ID_BOUNDARY_COLOR, "小连通域过滤面积", 20.0));

    qDebug("[Boundary] === 开始境界线提取 ===");
    qDebug("[Boundary] 内晕 BGR(%d,%d,%d), 外晕 BGR(%d,%d,%d), 容差=%d, 图像=%dx%d",
        (int)halo_color[0], (int)halo_color[1], (int)halo_color[2],
        (int)outer_halo_color[0], (int)outer_halo_color[1], (int)outer_halo_color[2],
        tolerance, workImg.cols, workImg.rows);

    // 0. 预处理：高斯模糊 + 中值滤波
    cv::Mat denoised;
    cv::GaussianBlur(workImg, denoised, cv::Size(3, 3), 0);
    cv::medianBlur(denoised, denoised, 3);

    // 1. 转 HSV
    cv::Mat hsv;
    cv::cvtColor(denoised, hsv, cv::COLOR_BGR2HSV);

    // 2. 生成内晕和外晕的 mask（不再使用境界线mask）
    cv::Mat maskHalo        = colorMask(hsv, halo_color, tolerance);
    cv::Mat maskOuterHalo   = colorMask(hsv, outer_halo_color, tolerance);

    if (m_oneClickMode) { setProgressSmooth(14, "境界线: 生成颜色mask %p%"); }

    qDebug("[Boundary] 步骤2 - 原始mask: 内晕=%d像素, 外晕=%d像素",
        cv::countNonZero(maskHalo), cv::countNonZero(maskOuterHalo));
    // imwriteCN("D:/Projects/testdata/_debug_02_原始内晕带.png", maskHalo);
    // imwriteCN("D:/Projects/testdata/_debug_02_原始外晕带.png", maskOuterHalo);

    // 3. 形态学清理：内/外晕线是面状，做开运算 + 连通域过滤
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(maskHalo, maskHalo, cv::MORPH_OPEN, kernel);
    cv::morphologyEx(maskOuterHalo, maskOuterHalo, cv::MORPH_OPEN, kernel);
    maskHalo      = removeSmallComponents(maskHalo, minArea);
    maskOuterHalo = removeSmallComponents(maskOuterHalo, minArea);
    // imwriteCN("D:/Projects/testdata/_debug_03a_开运算后内晕.png", maskHalo);
    // imwriteCN("D:/Projects/testdata/_debug_03a_开运算后外晕.png", maskOuterHalo);

    // 关键修复：消除境界线虚线经过内晕带时形成的细长凹陷
    // 境界线虚线空隙通常只有几个像素宽，用小核闭运算即可连接
    // 注意：不能用大核"膨胀-填充-腐蚀"，否则多个不相邻的内晕区域会
    //       合并成一个巨大连通块，填充后面积爆炸性增长且无法腐蚀回去
    // 闭运算 = 先膨胀后腐蚀，膨胀让凹陷两侧合拢，腐蚀恢复原大小
    // 闭运算不会"提取外轮廓并填充内部"，所以不会把区域间空隙填满
    {
        // 5x5椭圆核：足够连接5px以内的虚线空隙，不会合并相邻独立区域
        cv::Mat closeK = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
        cv::morphologyEx(maskHalo, maskHalo, cv::MORPH_CLOSE, closeK);
    }

    // 闭合运算：连接5px以内的虚线空隙

    qDebug("[Boundary] 步骤3 - 清理后(内晕已闭运算): 内晕=%d像素, 外晕=%d像素",
        cv::countNonZero(maskHalo), cv::countNonZero(maskOuterHalo));

    if (m_oneClickMode) { setProgressSmooth(22, "境界线: 形态学清理完成 %p%"); }

    // imwriteCN("D:/Projects/testdata/_debug_03_内晕带.png", maskHalo);
    // imwriteCN("D:/Projects/testdata/_debug_03_外晕带.png", maskOuterHalo);

    int haloWidthOrig = 0;  // 晕带宽度估计，补偿后用于腐蚀恢复边界位置
    m_compensationDone = false;  // 重置截断修复标志（每次提取前重置）
    cv::Mat maskHaloOriginal;  // 补偿前的原始晕带 mask

    // 3a. 标准境界线引导补偿（若已进行地理配准）
    //     将 gdb 标准境界线矢量数据通过配准参数变换到图片像素空间，
    //     建立标准境界位置约束；对内晕带 mask 做连通域分析识别缺失区域，
    //     根据附近晕带宽度和颜色特征生成补偿像素并融合，使断裂晕带恢复连续。
    //     全程在原分辨率操作（4x空间会爆内存），补偿后上采样到4x做边缘提取。
    if (m_affineValid && m_boundaryLoaded && !m_boundaryFeatures.empty()) {
        auto refLinesOrig = transformBoundaryToImage();
        if (!refLinesOrig.empty()) {
            // 1) 估计晕带宽度：原分辨率距离变换峰值*2
            cv::Mat distHaloOrig;
            cv::distanceTransform(maskHalo, distHaloOrig, cv::DIST_L2, 3);
            double maxDist = 0;
            cv::minMaxLoc(distHaloOrig, nullptr, &maxDist);
            // 限制晕带宽度上限，防止mask有大块误识别区域导致参数爆炸
            // 原分辨率晕带宽度通常 2-15px，上限设 30px
            haloWidthOrig = std::max(2, std::min(30, (int)std::round(maxDist * 2.0)));
            qDebug("[Boundary] 步骤3a - 估计晕带宽度: 原分辨率=%d (距离变换峰值=%.1f)",
                haloWidthOrig, maxDist);

            // 2) 在原分辨率生成标准境界线参考 mask（粗线，宽度=晕带宽度）
            cv::Mat refMask = cv::Mat::zeros(maskHalo.size(), CV_8U);
            int refThickness = std::max(2, haloWidthOrig);
            for (auto& line : refLinesOrig) {
                for (size_t i = 1; i < line.size(); ++i) {
                    cv::Point p0((int)std::lround(line[i-1].x),
                                 (int)std::lround(line[i-1].y));
                    cv::Point p1((int)std::lround(line[i].x),
                                 (int)std::lround(line[i].y));
                    if (p0.x < 0 || p0.y < 0 || p0.x >= refMask.cols || p0.y >= refMask.rows ||
                        p1.x < 0 || p1.y < 0 || p1.x >= refMask.cols || p1.y >= refMask.rows) {
                        cv::clipLine(cv::Rect(0, 0, refMask.cols, refMask.rows), p0, p1);
                    }
                    cv::line(refMask, p0, p1, 255, refThickness, cv::LINE_8);
                }
            }

            // 保存补偿前的原始晕带，用于只腐蚀补偿部分、保持原边界不变
            maskHaloOriginal = maskHalo.clone();
            // imwriteCN("D:/Projects/testdata/_debug_03_补偿前原始晕带.png", maskHaloOriginal);

            // 3) 补偿前环形连通域检查
            //    若所有连通域都是环形（中空的），说明晕带完整无断裂，无需补偿。
            //    环形判定：对每个连通域找轮廓(RETR_CCOMP)，若存在内轮廓（孔洞）则为环形。
            {
                cv::Mat ccLabels, ccStats, ccCentroids;
                int ccCount = cv::connectedComponentsWithStats(maskHalo, ccLabels, ccStats, ccCentroids, 8);
                int ccFg = ccCount - 1;

                int ringCount = 0;
                for (int ci = 1; ci < ccCount; ++ci) {
                    // 过滤极小噪点（<20px）
                    if (ccStats.at<int>(ci, cv::CC_STAT_AREA) < 20) continue;
                    cv::Mat compMask = (ccLabels == ci);
                    std::vector<std::vector<cv::Point>> cContours;
                    std::vector<cv::Vec4i> cHier;
                    cv::findContours(compMask, cContours, cHier, cv::RETR_CCOMP,
                        cv::CHAIN_APPROX_SIMPLE);
                    bool hasHole = false;
                    for (size_t j = 0; j < cHier.size(); ++j) {
                        if (cHier[j][2] != -1) { hasHole = true; break; }
                    }
                    if (hasHole) ++ringCount;
                }

                // 统计有效连通域（面积≥20）
                int validCc = 0;
                for (int ci = 1; ci < ccCount; ++ci) {
                    if (ccStats.at<int>(ci, cv::CC_STAT_AREA) >= 20) ++validCc;
                }

                bool allRing = (validCc > 0) && (ringCount == validCc);
                bool skipComp = (validCc > 0) && allRing;

                qDebug("[Boundary] 步骤3a - 环形检查: 有效连通域=%d, 环形=%d, 全部环形=%d (跳过补偿=%d)",
                    validCc, ringCount, (int)allRing, (int)skipComp);

                if (skipComp) {
                    qDebug("[Boundary] 步骤3a - 晕带完整无断裂, 跳过像素补偿");
                    goto skipCompensation;
                }
            }

            // 4) 迭代补偿晕带断裂区域
            //    核心思路：对每个标准境界线像素，检查其邻域是否有晕带像素，
            //    有则将该像素加入补偿。用 dilate(maskHalo, nearR) 实现邻域判定：
            //    膨胀后的 mask 覆盖了"附近有晕带"的区域。
            //    补偿后检查连通域数量，若>3说明仍有断裂，增大邻域半径重新补偿。
            //    最多迭代8轮，邻域半径从 haloWidth+5 起每轮 +20px，上限 100px。
            cv::Mat compMaskTotal = cv::Mat::zeros(maskHalo.size(), CV_8U);
            const int maxIter = 8;
            int iter = 0;
            for (; iter < maxIter; ++iter) {
                int nearR = std::min(100, std::max(10, haloWidthOrig + 5 + iter * 20));
                // nearMask = maskHalo 膨胀 nearR 像素 = "附近有晕带"的区域
                // （包括晕带本身和外扩 nearR 的范围）
                cv::Mat nearKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE,
                    cv::Size(nearR * 2 + 1, nearR * 2 + 1));
                cv::Mat nearMask;
                cv::dilate(maskHalo, nearMask, nearKernel);

                // 补偿像素 = refMask AND nearMask AND (NOT maskHalo)
                // 即：标准境界线经过 + 附近有真实晕带 + 当前为空洞
                cv::Mat notHalo;
                cv::bitwise_not(maskHalo, notHalo);
                cv::Mat compMask;
                cv::bitwise_and(refMask, nearMask, compMask);
                cv::bitwise_and(compMask, notHalo, compMask);

                int compPixels = cv::countNonZero(compMask);
                if (compPixels == 0) {
                    qDebug("[Boundary] 步骤3a - 迭代%d: 无新补偿 (邻域半径=%dpx), 停止",
                        iter + 1, nearR);
                    // imwriteCN("D:/Projects/testdata/_debug_03_邻域膨胀区域.png", nearMask);
                    break;
                }

                // 融合到 maskHalo
                cv::bitwise_or(maskHalo, compMask, maskHalo);
                cv::bitwise_or(compMaskTotal, compMask, compMaskTotal);
                // 闭运算平滑补偿区域边缘
                int kSize = std::max(3, haloWidthOrig / 2);
                cv::Mat smoothK = cv::getStructuringElement(cv::MORPH_ELLIPSE,
                    cv::Size(kSize * 2 + 1, kSize * 2 + 1));
                cv::morphologyEx(maskHalo, maskHalo, cv::MORPH_CLOSE, smoothK);

                // 检查连通域数量（境界线应≤3条）
                cv::Mat labels, stats, centroids;
                int numConn = cv::connectedComponentsWithStats(maskHalo, labels, stats, centroids, 8);
                int numFg = numConn - 1;
                // 过滤小连通域（<50像素的视为噪声）
                int numLarge = 0;
                for (int i = 1; i < numConn; ++i) {
                    if (stats.at<int>(i, cv::CC_STAT_AREA) >= 50) ++numLarge;
                }

                qDebug("[Boundary] 步骤3a - 迭代%d: 补偿=%dpx (邻域半径=%dpx), 连通域=%d (大=%d)",
                    iter + 1, compPixels, nearR, numFg, numLarge);

                // imwriteCN("D:/Projects/testdata/_debug_03_邻域膨胀区域.png", nearMask);

                if (numLarge <= 3) {
                    qDebug("[Boundary] 步骤3a - 连通域≤3, 补偿完成");
                    break;
                }
                // 否则继续增大邻域半径
            }
            if (iter == maxIter) {
                qDebug("[Boundary] 步骤3a - 达到最大迭代次数%d, 停止", maxIter);
            }

            int totalComp = cv::countNonZero(compMaskTotal);
            qDebug("[Boundary] 步骤3a - 总补偿像素=%d (参考mask=%d, 间隙覆盖率=%.1f%%)",
                totalComp, cv::countNonZero(refMask),
                cv::countNonZero(refMask) > 0 ? 100.0 * totalComp / cv::countNonZero(refMask) : 0);

            // imwriteCN("D:/Projects/testdata/_debug_03_标准境界线参考.png", refMask);
            // imwriteCN("D:/Projects/testdata/_debug_03_补偿像素.png", compMaskTotal);
            // imwriteCN("D:/Projects/testdata/_debug_03_补偿后晕带.png", maskHalo);
            m_compensationDone = true;  // 标记已执行截断修复
        }
    }
skipCompensation:;

    if (m_oneClickMode) { setProgressSmooth(30, "境界线: 境界线补偿完成 %p%"); }

    // 补偿使 mask 增厚，需腐蚀增厚部分以恢复边界精度
    // 策略：逐层腐蚀整个 maskHalo（每次1像素），每层腐蚀后检查大连通域数量
    //       若连通域数量增加（说明某条境界线被腐蚀断），停止并回退到上一轮结果
    // 【暂时注释掉腐蚀步骤 - 调试用】
#if 0
    if (m_compensationDone && haloWidthOrig > 0 && !maskHaloOriginal.empty()) {
        cv::Mat compOnly = maskHalo & ~maskHaloOriginal;
        int compPx = cv::countNonZero(compOnly);
        // imwriteCN("D:/Projects/testdata/_debug_03_补偿新增像素.png", compOnly);
        if (compPx > 0) {
            // 基线连通域数量（补偿后、腐蚀前的大连通域数）
            auto countLargeCc = [](const cv::Mat& m) -> int {
                cv::Mat lbl, st, ce;
                int n = cv::connectedComponentsWithStats(m, lbl, st, ce, 8);
                int c = 0;
                for (int i = 1; i < n; ++i)
                    if (st.at<int>(i, cv::CC_STAT_AREA) >= 50) ++c;
                return c;
            };
            int baseCc = countLargeCc(maskHalo);

            // 1像素十字核（4邻域）
            cv::Mat erodeK = cv::getStructuringElement(cv::MORPH_CROSS, cv::Size(3, 3));
            cv::Mat cur = maskHalo.clone();
            int layers = 0;
            int maxLayers = std::max(10, haloWidthOrig * 3);  // 安全上限
            int beforePx = cv::countNonZero(cur);

            while (layers < maxLayers) {
                cv::Mat eroded;
                cv::erode(cur, eroded, erodeK);
                int ccNow = countLargeCc(eroded);
                if (ccNow > baseCc) {
                    // 连通域增加 → 即将断裂，停止，保留 cur（上一轮结果）
                    qDebug("[Boundary] 步骤3a - 腐蚀第%d层: 大连通域=%d > 基线=%d, 停止",
                        layers + 1, ccNow, baseCc);
                    break;
                }
                cur = eroded;
                ++layers;
            }

            int afterPx = cv::countNonZero(cur);
            int removed = beforePx - afterPx;
            qDebug("[Boundary] 步骤3a - 逐层腐蚀: %d 层, 削掉=%dpx (前=%d, 后=%d), 基线连通域=%d",
                layers, removed, beforePx, afterPx, baseCc);
            // imwriteCN("D:/Projects/testdata/_debug_03_腐蚀后候选.png", cur);
            maskHalo = cur;
            // imwriteCN("D:/Projects/testdata/_debug_03_腐蚀后晕带.png", maskHalo);
        }
    }
#endif

    // 3b. 上采样4倍：在4x分辨率下做边缘提取，精度4倍
    //     INTER_NEAREST 保持二值mask的清晰边界，不引入灰度插值
    //     补偿在原分辨率完成（见步骤3a），上采样后连通性已修复
    if (m_oneClickMode) { setProgressSmooth(37, "境界线: 边缘提取 %p%"); }
    qDebug("[Boundary] 步骤3b - 上采样4倍: 原图 %dx%d -> 4x %dx%d (每张CV_8U约 %.0f MB)",
        maskHalo.cols, maskHalo.rows, maskHalo.cols * 4, maskHalo.rows * 4,
        (double)maskHalo.cols * 4 * maskHalo.rows * 4 / (1024.0 * 1024.0));
    cv::Mat maskHaloUp, maskOuterUp;
    cv::resize(maskHalo, maskHaloUp, cv::Size(), 4.0, 4.0, cv::INTER_NEAREST);
    cv::resize(maskOuterHalo, maskOuterUp, cv::Size(), 4.0, 4.0, cv::INTER_NEAREST);

    // 4. 提取内晕线的精确1像素边缘（在4x空间操作）
    cv::Mat kernelUp = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::Mat haloEroded;
    cv::erode(maskHaloUp, haloEroded, kernelUp, cv::Point(-1, -1), 1);
    cv::Mat haloEdge;
    cv::bitwise_xor(maskHaloUp, haloEroded, haloEdge);
    haloEroded.release();  // 立即释放，4x空间大Mat很耗内存

    qDebug("[Boundary] 步骤4 - 内晕线1px边缘(4x空间): %d像素", cv::countNonZero(haloEdge));
    // imwriteCN("D:/Projects/testdata/_debug_04_内晕边缘XOR.png", haloEdge);

    // 5. 减去与外晕线相交的部分（在4x空间操作）
    cv::Mat outerDilated;
    cv::dilate(maskOuterUp, outerDilated, kernelUp, cv::Point(-1, -1), 1);
    maskOuterUp.release();  // 外晕上采样图不再需要
    cv::Mat edgeWithOuter;
    cv::bitwise_and(haloEdge, outerDilated, edgeWithOuter);

    qDebug("[Boundary] 步骤5 - 与外晕相交的边(要排除): %d像素", cv::countNonZero(edgeWithOuter));
    // imwriteCN("D:/Projects/testdata/_debug_05_减去外晕后.png", edgeWithOuter);
    // imwriteCN("D:/Projects/testdata/_debug_05_外晕膨胀区域.png", outerDilated);
    outerDilated.release();  // 立即释放

    // 6. 内晕边缘 - 外晕侧边 = 境界线侧边（在4x空间操作）
    cv::Mat boundaryEdge;
    cv::bitwise_xor(haloEdge, edgeWithOuter, boundaryEdge);
    edgeWithOuter.release();
    cv::bitwise_and(boundaryEdge, haloEdge, boundaryEdge);
    haloEdge.release();  // 立即释放
    maskHaloUp.release();  // 内晕上采样图不再需要

    qDebug("[Boundary] 步骤6 - 境界线侧边1px(保留): %d像素", cv::countNonZero(boundaryEdge));
    // imwriteCN("D:/Projects/testdata/_debug_06_境界线边缘.png", boundaryEdge);

    // 7. 以内晕线境界侧边为基准
    qDebug("[Boundary] 步骤7 - 开始 clone finalMask (尺寸 %dx%d)",
        boundaryEdge.cols, boundaryEdge.rows);
    cv::Mat finalMask = boundaryEdge.clone();
    boundaryEdge.release();  // 原始边界边不再需要

    // 8. 用小核闭运算连接虚线间隙处的断点（在4x空间，用7x7核相当于原分辨率<2px）
    qDebug("[Boundary] 步骤8 - 开始闭运算");
    cv::Mat closeKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(7, 7));
    cv::morphologyEx(finalMask, finalMask, cv::MORPH_CLOSE, closeKernel);
    qDebug("[Boundary] 步骤8 - 闭运算完成: %d像素", cv::countNonZero(finalMask));

    // imwriteCN("D:/Projects/testdata/_debug_08_闭运算平滑后.png", finalMask);

    // 9. 提取外轮廓（在4x空间，然后转换为原分辨率浮点坐标）
    //    CHAIN_APPROX_SIMPLE 压缩水平/垂直/对角直线段，大幅减少点数，节省内存
    qDebug("[Boundary] 步骤9 - 开始 findContours");
    std::vector<std::vector<cv::Point>> contoursUp;
    cv::findContours(finalMask, contoursUp, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    finalMask.release();  // mask不再需要，只保留轮廓点
    qDebug("[Boundary] 步骤9 - findContours 找到 %d 条原始轮廓(4x空间)", (int)contoursUp.size());

    // 输出轮廓叠加图，便于观察提取到的轮廓位置
    // 注意：finalMask 已在 findContours 后 release，这里用纯黑底图绘制轮廓
    {
        cv::Mat contourImg = cv::Mat::zeros(cv::Size(contoursUp.empty() ? 0 : contoursUp[0].empty() ? 0 : (cv::boundingRect(contoursUp[0]).br().x + 10),
                                                       contoursUp.empty() ? 0 : (cv::boundingRect(contoursUp[0]).br().y + 10)), CV_8UC3);
        if (!contourImg.empty()) {
            cv::drawContours(contourImg, contoursUp, -1, cv::Scalar(0, 0, 255), 2);
            // imwriteCN("D:/Projects/testdata/_debug_09_轮廓_4x空间.png", contourImg);
        }
    }

    // 10. 转换为原分辨率浮点坐标（除以4），过滤过短的碎片段
    //     补偿后境界线可能是一条超长单一闭合轮廓（百万点级），必须降采样
    //     否则后续 smoothContours、topologicalCorrection、调试图绘制都会爆内存。
    //     按固定间距 step 抽取点（原分辨率约2px一个点），境界线是平滑曲线，
    //     降采样不影响精度，但点数从百万级降到万级。
    const int step = 8;  // 4x空间每8个点取1个，相当于原分辨率每2px一个点
    std::vector<std::vector<cv::Point2f>> polylines;
    for (auto& c : contoursUp) {
        if (c.size() < 2) continue;
        std::vector<cv::Point2f> pl;
        size_t sampledSize = (c.size() + step - 1) / step;
        pl.reserve(sampledSize);
        for (size_t i = 0; i < c.size(); i += step) {
            pl.emplace_back(c[i].x / 4.0f, c[i].y / 4.0f);
        }
        // 确保最后一个点也被采样（保持闭合性）
        if (!pl.empty() && (c.size() - 1) % step != 0) {
            pl.emplace_back(c.back().x / 4.0f, c.back().y / 4.0f);
        }
        polylines.push_back(std::move(pl));
    }
    contoursUp.clear();  // 释放4x空间的轮廓点
    contoursUp.shrink_to_fit();
    qDebug("[Boundary] 步骤10 - 坐标转换并降采样后: %d 条折线(原分辨率), 总点数=%d",
        (int)polylines.size(),
        [&]{ size_t s=0; for(auto&p:polylines) s+=p.size(); return (int)s; }());

    // 11. 高斯加权子像素平滑（全程浮点，不四舍五入）
    if (m_oneClickMode) { setProgressSmooth(39, "境界线: 平滑轮廓 %p%"); }
    smoothContours(polylines);
    qDebug("[Boundary] 步骤11 - 平滑后: %d 条折线", (int)polylines.size());

    // 输出平滑后的折线叠加图（在原分辨率mask上画）
    {
        cv::Mat smoothImg = cv::Mat::zeros(workImg.size(), CV_8UC3);
        for (size_t i = 0; i < polylines.size(); ++i) {
            std::vector<cv::Point> pts;
            for (auto& p : polylines[i]) pts.emplace_back((int)std::lround(p.x), (int)std::lround(p.y));
            cv::polylines(smoothImg, pts, false, cv::Scalar(0, 255, 0), 2);
        }
        // imwriteCN("D:/Projects/testdata/_debug_11_平滑后.png", smoothImg);
    }

    // 12. 拓扑修正：端点吸附、噪声清理、线段合并、拓扑检查
    //     阈值按图像对角线自适应缩放，大图用大阈值
    //     基准值从 XML 质检规则（境界线颜色提取）读取
    if (m_oneClickMode) { setProgressSmooth(41, "境界线: 拓扑检查与修正 %p%"); }
    double imgDiag = std::sqrt((double)workImg.cols * workImg.cols + (double)workImg.rows * workImg.rows);
    double baseShortLen = configByIdReader().getRuleValue(ITEM_ID_BOUNDARY_COLOR, "短片段基准值", 15.0);
    double baseSmallArea = configByIdReader().getRuleValue(ITEM_ID_BOUNDARY_COLOR, "小面积基准值", 50.0);
    double baseSnap = configByIdReader().getRuleValue(ITEM_ID_BOUNDARY_COLOR, "端点吸附基准值", 5.0);
    double baseMerge = configByIdReader().getRuleValue(ITEM_ID_BOUNDARY_COLOR, "线段合并基准值", 5.0);
    topologicalCorrection(polylines, imgDiag, baseShortLen, baseSmallArea, baseSnap, baseMerge);
    qDebug("[Boundary] 步骤12 - 拓扑修正后: %d 条折线", (int)polylines.size());

    // 输出拓扑修正后的折线叠加图
    {
        cv::Mat topoImg = cv::Mat::zeros(workImg.size(), CV_8UC3);
        for (size_t i = 0; i < polylines.size(); ++i) {
            std::vector<cv::Point> pts;
            for (auto& p : polylines[i]) pts.emplace_back((int)std::lround(p.x), (int)std::lround(p.y));
            cv::polylines(topoImg, pts, true, cv::Scalar(0, 0, 255), 2);
        }
        // imwriteCN("D:/Projects/testdata/_debug_12_拓扑修正后.png", topoImg);
    }

    // 13. 质量检查：输出提取结果与标准境界线的对比叠加图
    //     像素级补偿已在步骤3c完成（断裂晕带恢复连续后重新执行边缘提取与矢量化），
    //     此处仅输出叠加图用于检查补偿效果与偏差。
    if (m_affineValid && m_boundaryLoaded && !m_boundaryFeatures.empty()) {
        auto refLines = transformBoundaryToImage();
        qDebug("[Boundary] 步骤13 - 对比叠加: %d 条标准境界线", (int)refLines.size());

        // 输出叠加图：提取结果(红) + 标准境界线(蓝)
        {
            cv::Mat overlay;
            cv::cvtColor(workImg, overlay, cv::COLOR_BGR2RGB);
            // 画标准境界线（蓝色）
            for (auto& line : refLines) {
                for (size_t i = 1; i < line.size(); ++i) {
                    cv::line(overlay, line[i-1], line[i], cv::Scalar(255, 0, 0), 2, cv::LINE_AA);
                }
            }
            // 画提取结果（红色）
            for (auto& line : polylines) {
                for (size_t i = 1; i < line.size(); ++i) {
                    cv::line(overlay, line[i-1], line[i], cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
                }
            }
            // imwriteCN("D:/Projects/testdata/_debug_13_标准境界对比.png", overlay);
        }
    }

    qDebug("[Boundary] === 提取完成，共 %d 条境界线 ===", (int)polylines.size());
    return polylines;
}

// ============================================================
// 境界线异常检测（提取后自动调用）
// 结果存入 m_lastAnomalyResult，不直接加入 QGIS 图层；
// 用户点击"将提取结果加入 QGIS"时由 onAccept() 据此分层添加。
// ============================================================
bool SE_ImageMapCheckDialog::runBoundaryAnomalyCheck()
{
    // 重置上次结果
    m_hasAnomalyResult = false;

    // 不满足前置条件时静默返回（仅更新状态标签）
    if (!m_hasResult || m_lastContours.empty()) {
        m_lblAnomalyStatus->setText("状态: 请先提取境界线");
        setLabelStatus(m_lblAnomalyStatus, "unloaded");
        return false;
    }
    if (m_boundaryFeatures.empty() || !m_boundaryLoaded) {
        m_lblAnomalyStatus->setText("状态: 未加载标准境界线，跳过异常检测");
        setLabelStatus(m_lblAnomalyStatus, "unloaded");
        return false;
    }
    if (!m_affineValid) {
        m_lblAnomalyStatus->setText("状态: 未完成地理配准，跳过异常检测");
        setLabelStatus(m_lblAnomalyStatus, "unloaded");
        return false;
    }

    m_lblAnomalyStatus->setText("状态: 正在检测...");
    setLabelStatus(m_lblAnomalyStatus, "progress");
    QApplication::processEvents();
    addCheckLog("开始境界线异常检测");

    // 将提取境界线（图像坐标）转换为地理坐标
    std::vector<std::vector<cv::Point2d>> extractedGeoLines;
    extractedGeoLines.reserve(m_lastContours.size());
    for (auto& imgLine : m_lastContours) {
        std::vector<cv::Point2d> geoLine;
        if (imgToGeo(imgLine, geoLine)) {
            extractedGeoLines.push_back(std::move(geoLine));
        }
    }
    if (extractedGeoLines.empty()) {
        m_lblAnomalyStatus->setText("状态: 提取线坐标变换失败");
        setLabelStatus(m_lblAnomalyStatus, "error");
        return false;
    }

    // 配置检测参数（地理坐标系，单位米）
    //   bufferRadius: 标准境界线缓冲区半径，提取线落在缓冲区内视为正常
    // 参数从 XML 质检规则（境界线异常检测）读取
    BoundaryCheckConfig config;
    config.bufferRadius    = configByIdReader().getRuleValue(ITEM_ID_BOUNDARY_ANOMALY, "缓冲区半径", 5000.0);
    config.bufferSegments  = static_cast<int>(configByIdReader().getRuleValue(ITEM_ID_BOUNDARY_ANOMALY, "缓冲区分段数", 16.0));
    config.sampleInterval  = configByIdReader().getRuleValue(ITEM_ID_BOUNDARY_ANOMALY, "异常段采样间隔", 50.0);
    config.minOverlapRatio = configByIdReader().getRuleValue(ITEM_ID_BOUNDARY_ANOMALY, "最小重叠比例", 0.3);

    // 调试：输出提取线与标准线的坐标范围，便于诊断坐标是否匹配
    {
        double eMinX = 1e18, eMinY = 1e18, eMaxX = -1e18, eMaxY = -1e18;
        for (auto& ln : extractedGeoLines) {
            for (auto& p : ln) {
                if (p.x < eMinX) eMinX = p.x;
                if (p.y < eMinY) eMinY = p.y;
                if (p.x > eMaxX) eMaxX = p.x;
                if (p.y > eMaxY) eMaxY = p.y;
            }
        }
        double sMinX = 1e18, sMinY = 1e18, sMaxX = -1e18, sMaxY = -1e18;
        for (auto& ln : m_boundaryFeatures) {
            for (auto& p : ln) {
                if (p.x < sMinX) sMinX = p.x;
                if (p.y < sMinY) sMinY = p.y;
                if (p.x > sMaxX) sMaxX = p.x;
                if (p.y > sMaxY) sMaxY = p.y;
            }
        }
        qDebug("[BoundaryCheck][UI] 提取线坐标范围: X[%.3f, %.3f] Y[%.3f, %.3f]",
               eMinX, eMaxX, eMinY, eMaxY);
        qDebug("[BoundaryCheck][UI] 标准线坐标范围: X[%.3f, %.3f] Y[%.3f, %.3f]",
               sMinX, sMaxX, sMinY, sMaxY);
        bool xOverlap = (eMaxX >= sMinX) && (sMaxX >= eMinX);
        bool yOverlap = (eMaxY >= sMinY) && (sMaxY >= eMinY);
        qDebug("[BoundaryCheck][UI] 坐标范围重叠: X=%s, Y=%s",
               xOverlap ? "是" : "否", yOverlap ? "是" : "否");
    }

    // 执行异常检测（提取线与标准线均为地理坐标）
    BoundaryAnomalyResult result;
    SE_Error err = CheckBoundaryAnomalies(extractedGeoLines, m_boundaryFeatures, config, result);
    if (err != SE_ERROR_NONE) {
        m_lblAnomalyStatus->setText("状态: 检测失败 - " + QString::fromStdString(result.errorMsg));
        setLabelStatus(m_lblAnomalyStatus, "error");
        return false;
    }

    // 缓存检测结果，供 onAccept() 分层加入 QGIS
    m_lastAnomalyResult = std::move(result);
    m_lastBufferRadius = config.bufferRadius;
    m_hasAnomalyResult = true;
    addCheckLog(QString("境界线异常检测完成: 正常 %1 条, 异常 %2 条, 缓冲区半径 %3m, 平均偏离 %4m, 最大偏离 %5m")
        .arg(m_lastAnomalyResult.normalCount)
        .arg(m_lastAnomalyResult.positionOffsetCount)
        .arg(config.bufferRadius)
        .arg(m_lastAnomalyResult.overallAvgOffset)
        .arg(m_lastAnomalyResult.overallMaxOffset));

    // 生成 JSON 报告（静默保存，不弹窗）
    std::string jsonReport = GenerateAnomalyReport(m_lastAnomalyResult);
    QString reportPath = m_imagePath + ".anomaly_report.json";
    QFile reportFile(reportPath);
    if (reportFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        reportFile.write(jsonReport.c_str());
        reportFile.close();
        qDebug("[BoundaryCheck][UI] JSON 报告已保存: %s", qPrintable(reportPath));
    }

    // 更新状态标签（不弹窗、不加入图层）
    QString statusText;
    if (m_lastAnomalyResult.positionOffsetCount == 0) {
        statusText = QString("状态: 正常 - 共 %1 条")
            .arg(m_lastAnomalyResult.totalExtractedSegments);
        setLabelStatus(m_lblAnomalyStatus, "loaded");
    } else {
        statusText = QString("状态: 发现异常 - 完全正常 %1 条, 含异常段 %2 条\n")
            .arg(m_lastAnomalyResult.normalCount)
            .arg(m_lastAnomalyResult.positionOffsetCount);
        setLabelStatus(m_lblAnomalyStatus, "warning");
    }
    m_lblAnomalyStatus->setText(statusText);

    // 检测完成弹窗：汇总提取与异常检测结果（独立执行弹窗已移除，仅支持一键质检）
    if (!m_oneClickMode) {
        // QMessageBox::information(this, "境界线提取与异常检测完成", statusText);
    }

    return true;
}

void SE_ImageMapCheckDialog::setCurrentColor(const QColor& color)
{
    if (!color.isValid()) return;
    m_currentColor = color;
}

void SE_ImageMapCheckDialog::setCurrentHaloColor(const QColor& color)
{
    if (!color.isValid()) return;
    m_currentHaloColor = color;
    QString style = QString("background-color: %1; border: 1px solid #888;")
        .arg(color.name());
    m_btnHaloColor->setStyleSheet(style);
    m_editHaloColor->blockSignals(true);
    m_editHaloColor->setText(color.name());
    m_editHaloColor->blockSignals(false);
}

void SE_ImageMapCheckDialog::setCurrentOuterHaloColor(const QColor& color)
{
    if (!color.isValid()) return;
    m_currentOuterHaloColor = color;
    QString style = QString("background-color: %1; border: 1px solid #888;")
        .arg(color.name());
    m_btnOuterHaloColor->setStyleSheet(style);
    m_editOuterHaloColor->blockSignals(true);
    m_editOuterHaloColor->setText(color.name());
    m_editOuterHaloColor->blockSignals(false);
}

void SE_ImageMapCheckDialog::onHaloColorBtnClicked()
{
    QColor c = QColorDialog::getColor(m_currentHaloColor, this, "选择晕线颜色");
    if (c.isValid()) {
        setCurrentHaloColor(c);
    }
}

void SE_ImageMapCheckDialog::onHaloColorEdited()
{
    QString text = m_editHaloColor->text().trimmed();
    QColor c(text);
    if (c.isValid()) {
        setCurrentHaloColor(c);
    } else {
        m_editHaloColor->blockSignals(true);
        m_editHaloColor->setText(m_currentHaloColor.name());
        m_editHaloColor->blockSignals(false);
    }
}

void SE_ImageMapCheckDialog::onOuterHaloColorBtnClicked()
{
    QColor c = QColorDialog::getColor(m_currentOuterHaloColor, this, "选择外晕线颜色");
    if (c.isValid()) {
        setCurrentOuterHaloColor(c);
    }
}

void SE_ImageMapCheckDialog::onOuterHaloColorEdited()
{
    QString text = m_editOuterHaloColor->text().trimmed();
    QColor c(text);
    if (c.isValid()) {
        setCurrentOuterHaloColor(c);
    } else {
        m_editOuterHaloColor->blockSignals(true);
        m_editOuterHaloColor->setText(m_currentOuterHaloColor.name());
        m_editOuterHaloColor->blockSignals(false);
    }
}

void SE_ImageMapCheckDialog::onPickHaloColor()
{
    if (m_imagePath.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先选择图片");
        return;
    }
    cv::Mat img = imreadCN(m_imagePath);
    if (img.empty()) {
        QMessageBox::warning(this, "错误", "无法读取图片，请先选择有效图片");
        return;
    }
    setPreviewImage(true, img, "原图");

    // 弹出取色对话框
    ImagePickDialog dialog(img, this);
    dialog.setWindowTitle("取色窗口 - 点击图片选择内晕线颜色（境界线紧邻的晕带）");
    if (dialog.exec() != QDialog::Accepted || !dialog.m_hasPicked) {
        return;
    }

    const cv::Scalar& bgr = dialog.m_pickedColor;
    setCurrentHaloColor(QColor(bgr[2], bgr[1], bgr[0]));
}

void SE_ImageMapCheckDialog::onPickOuterHaloColor()
{
    if (m_imagePath.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先选择图片");
        return;
    }
    cv::Mat img = imreadCN(m_imagePath);
    if (img.empty()) {
        QMessageBox::warning(this, "错误", "无法读取图片，请先选择有效图片");
        return;
    }
    setPreviewImage(true, img, "原图");

    ImagePickDialog dialog(img, this);
    dialog.setWindowTitle("取色窗口 - 点击图片选择外晕线颜色（内晕带外面的晕带）");
    if (dialog.exec() != QDialog::Accepted || !dialog.m_hasPicked) {
        return;
    }

    const cv::Scalar& bgr = dialog.m_pickedColor;
    setCurrentOuterHaloColor(QColor(bgr[2], bgr[1], bgr[0]));
}

void SE_ImageMapCheckDialog::onExtractColor()
{
    addCheckLog("开始提取境界线");
    if (m_imagePath.isEmpty()) {
        // QMessageBox::warning(this, "提示", "请先选择图片");  // 独立执行弹窗已移除
        return;
    }
    cv::Mat img = imreadCN(m_imagePath);
    if (img.empty()) {
        // QMessageBox::warning(this, "错误", "无法读取图片，请先选择有效图片");  // 独立执行弹窗已移除
        return;
    }
    // 记录质检开始时间（若尚未记录，用于独立执行质检步骤时填充 JSON timing）
    if (!m_checkStartTime.isValid()) {
        m_checkStartTime = QDateTime::currentDateTime();
    }

    // 若已有文字提取结果，遮罩文字区域防止被提取为轮廓
    cv::Mat workImg = img;
    if (!m_textResults.empty()) {
        workImg = img.clone();
        maskTextRegions(workImg, m_textResults);
    }

    if (m_oneClickMode) { setProgressSmooth(3, "境界线: 读取图片 %p%"); }

    setPreviewImage(true, img, "原图");

    if (m_oneClickMode) { setProgressSmooth(8, "境界线: 颜色提取中... %p%"); }

    std::vector<std::vector<cv::Point2f>> contours = extractContoursByColor(workImg);

    if (m_oneClickMode) { setProgressSmooth(45, "境界线: 颜色提取完成 %p%"); }

    // 在原图上叠加提取的境界线（粗红线），便于直观查看提取效果
    {
        cv::Mat imgWithContours = img.clone();
        for (const auto& line : contours) {
            for (size_t i = 1; i < line.size(); ++i) {
                cv::line(imgWithContours, line[i - 1], line[i],
                         cv::Scalar(0, 0, 255), 4, cv::LINE_AA);
            }
        }
        setPreviewImage(true, imgWithContours, "原图（叠加境界线）");
    }

    int valid_count = static_cast<int>(contours.size());
    cv::Scalar boundary_cv(m_currentColor.blue(), m_currentColor.green(), m_currentColor.red());
    cv::Scalar halo_cv(m_currentHaloColor.blue(), m_currentHaloColor.green(), m_currentHaloColor.red());

    // 缓存结果，并自动触发异常检测（检测完成会在 runBoundaryAnomalyCheck 内弹窗汇总）
    cacheResult(img, contours, QStringLiteral("境界线提取"));

    if (m_oneClickMode) { setProgressSmooth(47, "境界线: 异常检测完成 %p%"); }

    // 若异常检测被跳过（未配准/未加载标准线等），则在此补一个提取结果弹窗
    // 独立执行弹窗已移除，仅支持一键质检
    if (!m_hasAnomalyResult && !m_oneClickMode) {
        // QMessageBox::information(this, "境界线提取结果",
        //     QString("境界线 BGR(%1, %2, %3)\n晕线 BGR(%4, %5, %6)\n容差: %7\n检测到 %8 条境界线。")
        //     .arg(boundary_cv[0]).arg(boundary_cv[1]).arg(boundary_cv[2])
        //     .arg(halo_cv[0]).arg(halo_cv[1]).arg(halo_cv[2])
        //     .arg(m_sliderTolerance->value())
        //     .arg(valid_count));
    }
}

// 刷新底部表格：展示未匹配项与纠错项
void SE_ImageMapCheckDialog::updateTextTable()
{
    m_tblText->setRowCount(0);
    for (const auto& tr : m_textResults) {
        // 敏感注记单独展示；非敏感项只展示未匹配项和纠错项
        if (!tr.sensitive && tr.verified && !tr.corrected) continue; // 直接匹配，不展示

        int row = m_tblText->rowCount();
        m_tblText->insertRow(row);
        m_tblText->setItem(row, 0, new QTableWidgetItem(tr.text));

        QString matchInfo, status;
        QColor  statusColor;
        if (tr.sensitive) {
            matchInfo = QString("%1:%2").arg(tr.sensitiveCategory).arg(tr.sensitiveWord);
            status = QStringLiteral("敏感异常");
            statusColor = QColor(170, 0, 170); // 紫色，与 QGIS 描边一致
        } else if (tr.corrected) {
            matchInfo = tr.matchedName;
            status = QStringLiteral("已纠错");
            statusColor = QColor(255, 140, 0); // 橙色
        } else {
            matchInfo = QStringLiteral("（未匹配）");
            status = QStringLiteral("未验证");
            statusColor = QColor(220, 0, 0);
        }
        m_tblText->setItem(row, 1, new QTableWidgetItem(matchInfo));
        m_tblText->setItem(row, 2, new QTableWidgetItem(QString::number(tr.confidence, 'f', 1)));
        QTableWidgetItem* statusItem = new QTableWidgetItem(status);
        statusItem->setForeground(statusColor);
        m_tblText->setItem(row, 3, statusItem);
    }
}

// ===== 单字合并策略（基于 gdb 图层几何）=====
// 从 gdb 图层加载要素几何（图像坐标）和名称，按几何类型分类返回
bool SE_ImageMapCheckDialog::loadLayerFeaturesForMerge(
    QList<LayerFeature>& lineFeatures,
    QList<LayerFeature>& polyFeatures) const
{
    lineFeatures.clear();
    polyFeatures.clear();

    if (m_gdbPath.isEmpty() || m_gazetteerLayerIds.isEmpty() || !m_affineValid) return false;

    // 仿射变换：geo -> img。affine 定义为 img = M * geo，直接用即可
    double a = m_affine.at<double>(0,0), b = m_affine.at<double>(0,1), c = m_affine.at<double>(0,2);
    double d = m_affine.at<double>(1,0), e = m_affine.at<double>(1,1), f = m_affine.at<double>(1,2);
    auto geoToImg = [&](double gx, double gy) -> cv::Point2f {
        return cv::Point2f((float)(a*gx + b*gy + c), (float)(d*gx + e*gy + f));
    };

    // 获取境界线图层 CRS(仿射变换在其坐标空间拟合), 地名图层需转换到此 CRS
    QgsCoordinateReferenceSystem targetCrs;
    if (!m_gdbPath.isEmpty() && m_boundaryLayerId >= 0) {
        QString buri = m_gdbPath + "|layerid=" + QString::number(m_boundaryLayerId);
        QgsVectorLayer probe(buri, "probe", "ogr");
        if (probe.isValid() && probe.crs().isValid()) targetCrs = probe.crs();
    }
    if (!targetCrs.isValid()) targetCrs = QgsCoordinateReferenceSystem("EPSG:4326");
    qDebug("[Merge] 境界线 CRS: %s", targetCrs.authid().toUtf8().constData());

    for (int layerId : m_gazetteerLayerIds) {
        QString uri = m_gdbPath + "|layerid=" + QString::number(layerId);
        QgsVectorLayer layer(uri, QStringLiteral("mrg_%1").arg(layerId), "ogr");
        if (!layer.isValid()) continue;

        // CRS 转换: 地名图层 CRS -> 境界线图层 CRS
        QgsCoordinateTransform xform;
        if (layer.crs().isValid() && targetCrs != layer.crs()) {
            xform = QgsCoordinateTransform(layer.crs(), targetCrs, QgsProject::instance());
            qDebug("[Merge] 地名图层 %d CRS: %s -> %s (需转换)", layerId,
                   layer.crs().authid().toUtf8().constData(),
                   targetCrs.authid().toUtf8().constData());
        } else {
            qDebug("[Merge] 地名图层 %d CRS: %s (无需转换)", layerId,
                   layer.crs().isValid() ? layer.crs().authid().toUtf8().constData() : "invalid");
        }

        // 名称字段: 优先 NAMES, 找不到则回退到 NAME
        int nameIdx = -1;
        const QgsFields fields = layer.fields();
        for (int j = 0; j < fields.size(); ++j) {
            if (fields[j].name().compare("NAMES", Qt::CaseInsensitive) == 0) { nameIdx = j; break; }
        }
        if (nameIdx < 0) {
            for (int j = 0; j < fields.size(); ++j) {
                if (fields[j].name().compare("NAME", Qt::CaseInsensitive) == 0) { nameIdx = j; break; }
            }
        }
        if (nameIdx < 0) continue;

        QgsWkbTypes::GeometryType gt = layer.geometryType();
        // 仅加载线要素和面要素, 点要素不参与单字合并
        if (gt != QgsWkbTypes::LineGeometry && gt != QgsWkbTypes::PolygonGeometry) continue;

        QgsFeature feat;
        QgsFeatureIterator it = layer.getFeatures();
        // 同名要素几何合并字典: 线要素同一地名可能被分成多段(多个要素),
        // 需将所有段几何合并到同一 LayerFeature, 确保缓冲区覆盖全部单字
        QHash<QString, int> lineNameIndex;  // name -> lineFeatures 下标
        QHash<QString, int> polyNameIndex;
        while (it.nextFeature(feat)) {
            QVariant val = feat.attribute(nameIdx);
            if (!val.isValid() || val.isNull()) continue;
            QString name = val.toString().trimmed();
            if (name.length() < 2) continue;  // 单字地名不参与合并
            bool ok; name.toDouble(&ok);
            if (ok) continue;  // 跳过纯数字

            const QgsGeometry& geom = feat.geometry();
            if (geom.isNull()) continue;

            // CRS 转换: 地名图层 CRS -> 境界线图层 CRS
            QgsGeometry tGeom = geom;
            if (xform.isValid()) {
                tGeom.transform(xform);
                if (tGeom.isNull()) tGeom = geom;  // 转换失败用原始几何
            }
            // 调试: 输出第一个要素的坐标采样
            if (lineFeatures.empty() && polyFeatures.empty()) {
                QgsRectangle bb = tGeom.boundingBox();
                cv::Point2f imgPt = geoToImg(bb.xMinimum(), bb.yMinimum());
                qDebug("[Merge] 首要素 '%s' 原始范围(%.6f,%.6f)-(%.6f,%.6f) -> 图像(%.1f,%.1f)",
                       name.toUtf8().constData(), bb.xMinimum(), bb.yMinimum(),
                       bb.xMaximum(), bb.yMaximum(), imgPt.x, imgPt.y);
            }

            if (gt == QgsWkbTypes::LineGeometry) {
                // 折线：可能多部件
                QgsMultiPolylineXY mpl = tGeom.asMultiPolyline();
                if (mpl.isEmpty()) {
                    QgsPolylineXY pl = tGeom.asPolyline();
                    if (!pl.isEmpty()) mpl << pl;
                }
                // 同名要素: 几何追加到已有 LayerFeature, 段间加 NaN 分隔点
                // 防止距离计算产生虚假连接段(段尾→段首)
                if (lineNameIndex.contains(name)) {
                    auto& lf = lineFeatures[lineNameIndex[name]];
                    if (!lf.geom.empty()) lf.geom.push_back(cv::Point2f(NAN, NAN));
                    for (const auto& pl : mpl) {
                        for (const QgsPointXY& p : pl) lf.geom.push_back(geoToImg(p.x(), p.y()));
                    }
                } else {
                    LayerFeature lf;
                    lf.name = name;
                    for (const auto& pl : mpl) {
                        for (const QgsPointXY& p : pl) lf.geom.push_back(geoToImg(p.x(), p.y()));
                    }
                    if (!lf.geom.empty()) {
                        lineNameIndex.insert(name, lineFeatures.size());
                        lineFeatures.push_back(lf);
                    }
                }
            } else if (gt == QgsWkbTypes::PolygonGeometry) {
                // 面：取外环
                QgsMultiPolygonXY mpg = tGeom.asMultiPolygon();
                if (mpg.isEmpty()) {
                    QgsPolygonXY pg = tGeom.asPolygon();
                    if (!pg.isEmpty()) mpg << pg;
                }
                // 同名要素: 各面独立添加(面要素合并几何意义不大, 每个面独立匹配即可)
                for (const auto& pg : mpg) {
                    if (pg.isEmpty()) continue;
                    LayerFeature lf;
                    lf.name = name;
                    for (const QgsPointXY& p : pg[0]) lf.geom.push_back(geoToImg(p.x(), p.y()));
                    polyFeatures.push_back(lf);
                }
            }
        }
    }
    return !lineFeatures.isEmpty() || !polyFeatures.isEmpty();
}

// 对单字集合尝试与一个地名合并：长名优先、任意顺序、相邻字距≤阈值
// 算法：
//   1. name 的每个字在 singles 中查找（任意顺序，可重复字按可用次数）
//   2. 选出的单字两两相邻距离 ≤ maxGap
//   3. 匹配字数需超过 name 长度的一半（允许 OCR 漏识部分单字）
//   4. 合并后坐标为组成单字 bbox 的中心
bool SE_ImageMapCheckDialog::tryMergeSinglesForName(
    const std::vector<TextResult>& singles,
    const QString& name,
    double maxGap,
    std::vector<int>& matchedIdx,
    TextResult& merged,
    const QString& dbgTag,
    int maxSkipOverride) const
{
    matchedIdx.clear();
    if (name.length() < 2) return false;

    // 收集 name 中每个字在 candidates 中的候选下标
    // 注意：同一字可能出现多次，需按可用次数匹配
    QList<QChar> chars;
    for (const QChar& ch : name) chars.append(ch);

    // 按文字内容分组索引(支持单字和多字片段)
    // 多字片段在回溯时可一次匹配多个字, 减少搜索深度
    QMap<QString, std::vector<int>> textIndices;
    for (size_t i = 0; i < singles.size(); ++i) {
        textIndices[singles[i].text].push_back((int)i);
    }
    // 最少需匹配的字数: 超过 name 长度一半
    int minMatched = chars.size() / 2 + 1;
    // 允许跳过的缺字数: maxSkipOverride>=0 时强制使用(0=第一轮不允许缺字, 完全匹配优先)
    int maxSkip = (maxSkipOverride >= 0) ? maxSkipOverride : ((int)chars.size() - minMatched);

    // 回溯搜索：尝试为 chars 的每个位置选一个未使用片段(1字或多字), 相邻距离 ≤ maxGap
    // 允许跳过缺字（不消耗片段），但跳过总数(按字数计) ≤ maxSkip
    std::vector<int> selected;
    selected.reserve(chars.size());
    std::vector<bool> used(singles.size(), false);

    // 中心点距离
    auto centerOf = [&](int idx) -> cv::Point2f {
        const cv::Rect& r = singles[idx].bbox;
        return cv::Point2f(r.x + r.width / 2.0f, r.y + r.height / 2.0f);
    };

    // skipped: 已跳过的缺字数(按字数计)
    std::function<bool(int, int)> backtrack = [&](int depth, int skipped) -> bool {
        if (depth == chars.size()) return true;
        // 尝试匹配不同长度的片段: 从长到短, 优先多字匹配
        int remaining = (int)chars.size() - depth;
        for (int len = remaining; len >= 1; --len) {
            QString sub = name.mid(depth, len);
            auto it = textIndices.find(sub);
            if (it == textIndices.end()) continue;
            for (int idx : it.value()) {
                if (used[idx]) continue;
                // 相邻距离检查: 与最近一个已选中片段比较
                if (!selected.empty()) {
                    cv::Point2f prev = centerOf(selected.back());
                    cv::Point2f cur = centerOf(idx);
                    double dist = std::hypot(cur.x - prev.x, cur.y - prev.y);
                    if (dist > maxGap) continue;
                }
                used[idx] = true;
                selected.push_back(idx);
                if (backtrack(depth + len, skipped)) return true;
                selected.pop_back();
                used[idx] = false;
            }
        }
        // 缺字: 可跳过（仅当未超过跳过配额）
        if (skipped < maxSkip) {
            return backtrack(depth + 1, skipped + 1);
        }
        return false;
    };

    if (!backtrack(0, 0)) {
        // 调试: 仅当传入 dbgTag 时输出匹配失败信息(如只看面要素)
        if (!dbgTag.isEmpty()) {
            qDebug("[Merge][%s] '%s' 匹配失败(需>%d/%d字) maxGap=%.0f, 候选:",
                   dbgTag.toUtf8().constData(), name.toUtf8().constData(),
                   minMatched - 1, (int)chars.size(), maxGap);
            for (auto it = textIndices.begin(); it != textIndices.end(); ++it) {
                for (int idx : it.value()) {
                    cv::Point2f p = centerOf(idx);
                    qDebug("[Merge][%s]   片段'%s' @ (%.0f,%.0f)",
                           dbgTag.toUtf8().constData(),
                           it.key().toUtf8().constData(), p.x, p.y);
                }
            }
        }
        return false;
    }

    // 合并成功：构造 merged
    matchedIdx = selected;
    // bbox: 以第一个子片段(名称首字对应的片段)坐标为准, 不取并集
    QString mergedText;
    int matchedChars = 0;
    for (int idx : selected) {
        mergedText += singles[idx].text;
        matchedChars += singles[idx].text.length();
    }
    merged.bbox = singles[selected.front()].bbox;
    merged.text = mergedText;
    merged.originalText = mergedText;   // 保留拼接原文
    merged.matchedName = name;          // 标准地名
    merged.verified = true;
    // 有跳过缺字(模糊匹配)才算纠正项; 全部字精确匹配算地名匹配
    merged.corrected = (matchedChars < (int)chars.size());
    merged.mergedByGeometry = true;     // 标记为图层几何合并产生
    merged.confidence = 0.0f;
    for (int idx : selected) merged.confidence += singles[idx].confidence;
    merged.confidence /= selected.size();
    merged.hasColor = false;
    merged.group = -1;
    merged.sensitive = false;
    return true;
}

// 按 线图层 → 面图层 顺序执行单字合并(仅基于线/面要素地名)
int SE_ImageMapCheckDialog::mergeSinglesByLayerGeometry(std::vector<TextResult>& results)
{
    if (!m_affineValid || m_gazetteerLayerIds.isEmpty()) {
        return 0;
    }
    QList<LayerFeature> lineFeats, polyFeats;
    if (!loadLayerFeaturesForMerge(lineFeats, polyFeats)) return 0;
    return mergeSinglesByLayerGeometryCh(results, lineFeats, polyFeats);
}

// 多通道版: 接收已加载要素, 匹配成功的要素在 lf.consumed 标记
int SE_ImageMapCheckDialog::mergeSinglesByLayerGeometryCh(std::vector<TextResult>& results,
    QList<LayerFeature>& lineFeats, QList<LayerFeature>& polyFeats,
    int phaseFilter)
{
    if (!m_affineValid || m_gazetteerLayerIds.isEmpty()) {
        return 0;
    }
    // phaseFilter: 0=两轮合并(默认), 1=仅精确(不缺字), 2=仅模糊(允许缺字)
    //   1: 仅匹配完全包含所有单字的地名(maxSkip=0)
    //   2: 仅匹配允许缺字的地名(maxSkip>0)
    //   0: 两轮都执行(向后兼容)

    // 从 XML 读取合并参数: 按要素类型(线/面)设置不同倍数
    //   线要素沿地物排列, 较松散; 面要素散布面内, 间距最大
    double bufferFactor   = configByIdReader().getRuleValue(ITEM_ID_ANNOTATION_OCR, "线图层缓冲倍数", 3.0);
    double lineGapFactor   = configByIdReader().getRuleValue(ITEM_ID_ANNOTATION_OCR, "线要素单字合并距离倍数", 15.0);
    double polyGapFactor   = configByIdReader().getRuleValue(ITEM_ID_ANNOTATION_OCR, "面要素单字合并距离倍数", 20.0);

    // 收集待合并候选: 所有非敏感注记(单字和多字片段都参与)
    // 多通道模式下, 原图通道的候选也参与合并匹配(不再丢弃)
    const bool multiChannel = !results.empty() && !results.front().channelName.isEmpty();
    std::vector<TextResult> singles;      // 待合并的候选(单字+多字片段)
    std::vector<TextResult> others;      // 敏感注记等不参与合并的
    std::vector<bool> isSingle(results.size(), false);
    for (size_t i = 0; i < results.size(); ++i) {
        if (!results[i].sensitive && !results[i].verified && results[i].text.length() >= 1) {
            singles.push_back(results[i]);
            isSingle[i] = true;
        } else {
            others.push_back(results[i]);
        }
    }
    if (singles.empty()) {
        if (multiChannel) {
            std::vector<TextResult> newResults = others;
            results = std::move(newResults);
        }
        return 0;
    }

    // 按位置去重: 多通道模式下同一字可能在不同 HSV 通道掩码边缘被重复识别
    // 两个位置阈值:
    //   阈值1(很近, 0.3*字高): 无论内容是否相同都视为重复
    //   阈值2(较近, 0.5*字高): 仅当文字相同时视为重复
    // 优先保留彩色通道(hasColor=true)的候选, 其次保留置信度高者
    {
        std::vector<TextResult> deduped;
        deduped.reserve(singles.size());
        std::vector<bool> dup(singles.size(), false);
        for (size_t i = 0; i < singles.size(); ++i) {
            if (dup[i]) continue;
            const cv::Rect& ri = singles[i].bbox;
            cv::Point2f ci(ri.x + ri.width / 2.0f, ri.y + ri.height / 2.0f);
            double hRefI = ri.height;
            for (size_t j = i + 1; j < singles.size(); ++j) {
                if (dup[j]) continue;
                const cv::Rect& rj = singles[j].bbox;
                cv::Point2f cj(rj.x + rj.width / 2.0f, rj.y + rj.height / 2.0f);
                double d = std::hypot(ci.x - cj.x, ci.y - cj.y);
                double threshRef = std::max(hRefI, (double)rj.height);
                bool isDup = false;
                if (d < threshRef * 0.3) {
                    // 很近: 无论内容是否相同都视为重复
                    isDup = true;
                } else if (d < threshRef * 0.5 && singles[j].text == singles[i].text) {
                    // 较近且文字相同: 视为重复
                    isDup = true;
                }
                if (!isDup) continue;
                // 决定保留哪一个: 彩色通道优先, 其次置信度高者
                bool keepI = true;
                if (singles[i].hasColor != singles[j].hasColor) {
                    keepI = singles[i].hasColor;  // 保留彩色通道
                } else if (singles[i].confidence != singles[j].confidence) {
                    keepI = (singles[i].confidence > singles[j].confidence);
                }
                if (keepI) dup[j] = true;
                else { dup[i] = true; break; }
            }
            if (!dup[i]) deduped.push_back(singles[i]);
        }
        singles = std::move(deduped);
    }

    // 计算单字平均高度（作为字高）
    double avgH = 0;
    for (const auto& s : singles) avgH += s.bbox.height;
    avgH /= singles.size();
    // 按要素类型计算合并距离阈值(仅线、面要素参与合并)
    double maxGapLine = avgH * lineGapFactor;
    double maxGapPoly = avgH * polyGapFactor;
    double bufferDist = avgH * bufferFactor;
    qDebug("[Merge] avgH=%.1f bufferDist=%.1f maxGapLine=%.1f maxGapPoly=%.1f",
           avgH, bufferDist, maxGapLine, maxGapPoly);

    // 输出线要素缓冲区多边形(图像→地理坐标)作为调试图层
    // 用 QGIS buffer 在图像坐标空间生成缓冲区, 再转回地理坐标
    if (m_debugLayers) {
        std::vector<std::vector<cv::Point2d>> geoPolys;
        for (const auto& lf : lineFeats) {
            // 按 NaN 分段构建线几何
            QgsPointSequence pts;
            for (const auto& pt : lf.geom) {
                if (std::isnan(pt.x)) {
                    // 段结束, 创建缓冲区
                    if (pts.size() >= 2) {
                        QgsGeometry lineGeom = QgsGeometry::fromPolyline(pts);
                        QgsGeometry buf = lineGeom.buffer(bufferDist, 8);
                        if (!buf.isNull() && buf.type() == QgsWkbTypes::PolygonGeometry) {
                            QgsPolygonXY poly = buf.asPolygon();
                            for (const auto& ring : poly) {
                                std::vector<cv::Point2d> geoRing;
                                for (const QgsPointXY& p : ring) {
                                    cv::Point2f ip((float)p.x(), (float)p.y());
                                    cv::Point2d gp;
                                    if (imgToGeo(ip, gp)) geoRing.push_back(gp);
                                }
                                if (geoRing.size() >= 3) geoPolys.push_back(geoRing);
                            }
                        }
                    }
                    pts.clear();
                    continue;
                }
                pts.append(QgsPoint(pt.x, pt.y));
            }
            // 最后一段
            if (pts.size() >= 2) {
                QgsGeometry lineGeom = QgsGeometry::fromPolyline(pts);
                QgsGeometry buf = lineGeom.buffer(bufferDist, 8);
                if (!buf.isNull() && buf.type() == QgsWkbTypes::PolygonGeometry) {
                    QgsPolygonXY poly = buf.asPolygon();
                    for (const auto& ring : poly) {
                        std::vector<cv::Point2d> geoRing;
                        for (const QgsPointXY& p : ring) {
                            cv::Point2f ip((float)p.x(), (float)p.y());
                            cv::Point2d gp;
                            if (imgToGeo(ip, gp)) geoRing.push_back(gp);
                        }
                        if (geoRing.size() >= 3) geoPolys.push_back(geoRing);
                    }
                }
            }
        }
        if (!geoPolys.empty()) {
            // 创建面图层
            QgsCoordinateReferenceSystem crs = getOutputCrs();
            QgsVectorLayer* bufLayer = new QgsVectorLayer(QStringLiteral("Polygon"), "线要素缓冲区", "memory");
            if (bufLayer->isValid()) {
                bufLayer->setCrs(crs);
                QgsFeatureList features;
                for (auto& ring : geoPolys) {
                    QgsPolygonXY poly;
                    QgsPolylineXY pl;
                    for (auto& pt : ring) pl.append(QgsPointXY(pt.x, pt.y));
                    poly << pl;
                    QgsFeature f;
                    f.setGeometry(QgsGeometry::fromPolygonXY(poly));
                    features << f;
                }
                bufLayer->dataProvider()->addFeatures(features);
                bufLayer->commitChanges();
                bufLayer->updateExtents();
                // 设置半透明填充样式
                QgsFillSymbol* fillSym = QgsFillSymbol::createSimple(
                    QVariantMap{
                        {"color", QColor(255, 165, 0, 80).name(QColor::HexArgb)},
                        {"outline_color", QColor(255, 165, 0, 200).name(QColor::HexArgb)},
                        {"outline_width", 0.5}
                    });
                QgsSingleSymbolRenderer* renderer = new QgsSingleSymbolRenderer(fillSym);
                bufLayer->setRenderer(renderer);
                QgsProject::instance()->addMapLayer(bufLayer);
                qDebug("[Merge] 输出线要素缓冲区图层: %zu 个多边形", geoPolys.size());
            }
        }
    }

    // 被消耗的单字标记
    std::vector<bool> consumed(singles.size(), false);
    std::vector<TextResult> mergedList;

    // 辅助：点到折线段最短距离(跳过 NaN 分隔的段)
    auto pointToPolylineDist = [](const cv::Point2f& p, const std::vector<cv::Point2f>& line) -> double {
        if (line.empty()) return 1e18;
        double minD = 1e18;
        for (size_t i = 0; i + 1 < line.size(); ++i) {
            if (std::isnan(line[i].x) || std::isnan(line[i+1].x)) continue;  // 段分隔点
            cv::Point2f a = line[i], b = line[i + 1];
            cv::Point2f ab(b.x - a.x, b.y - a.y);
            double t = ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / (ab.x * ab.x + ab.y * ab.y);
            t = std::max(0.0, std::min(1.0, t));
            cv::Point2f proj(a.x + t * ab.x, a.y + t * ab.y);
            double d = std::hypot(p.x - proj.x, p.y - proj.y);
            minD = std::min(minD, d);
        }
        // 单点要素(无段)
        if (line.size() == 1) {
            double d = std::hypot(p.x - line[0].x, p.y - line[0].y);
            minD = std::min(minD, d);
        }
        return minD;
    };

    // 辅助：点是否在多边形内（射线法）
    auto pointInPolygon = [](const cv::Point2f& p, const std::vector<cv::Point2f>& poly) -> bool {
        int n = (int)poly.size();
        if (n < 3) return false;
        bool inside = false;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            if (((poly[i].y > p.y) != (poly[j].y > p.y)) &&
                (p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x))
                inside = !inside;
        }
        return inside;
    };

    // 辅助：对一组候选单字尝试用地名合并，成功则消耗并记录
    // 按颜色分组(group)分别尝试合并：只有同色单字才能合并为同一地名，
    // 避免把河流名（蓝色）与旁边的城市名（黑色）误合并到同一要素名称。
    // 注意：仅当存在可靠颜色信息(hasColor=true，即分通道识别)时才分组；
    //       单通道模式下 group 基于像素采样色，不准确，直接合并避免拆散同注记单字。
    bool hasReliableColor = false;
    for (const auto& s : singles) {
        if (s.hasColor) { hasReliableColor = true; break; }
    }

    // 对一组单字（已确定组内）执行合并, maxGap 由要素类型决定
    auto mergeGroup = [&](const std::vector<int>& grpIdx, const QString& name, double maxGap,
                          const QString& dbgTag = QString(), int srcType = 0,
                          int maxSkipOverride = -1) -> bool {
        if (grpIdx.empty()) return false;
        std::vector<TextResult> tmpSingles;
        std::vector<int> tmpToOrig;  // tmpSingles 下标 -> singles 下标
        for (int idx : grpIdx) {
            tmpSingles.push_back(singles[idx]);
            tmpToOrig.push_back(idx);
        }
        std::vector<int> tmpMatched;
        TextResult merged;
        if (!tryMergeSinglesForName(tmpSingles, name, maxGap, tmpMatched, merged, dbgTag, maxSkipOverride)) return false;
        // 映射回原 singles 下标并标记消耗
        for (int mi : tmpMatched) consumed[tmpToOrig[mi]] = true;
        // 继承组内代表色，保持颜色分组信息
        if (!tmpMatched.empty()) {
            merged.avgColor = singles[tmpToOrig[tmpMatched[0]]].avgColor;
            merged.hasColor = singles[tmpToOrig[tmpMatched[0]]].hasColor;
            merged.group    = singles[tmpToOrig[tmpMatched[0]]].group;
        }
        merged.geomMergeSrc = srcType;
        mergedList.push_back(merged);
        return true;
    };

    auto tryMerge = [&](const std::vector<int>& candIdx, const QString& name, double maxGap,
                        const QString& dbgTag = QString(), int srcType = 0,
                        int maxSkipOverride = -1) -> bool {
        if (candIdx.empty()) return false;
        // 单通道模式：无可靠颜色，不分组直接合并（恢复原行为）
        if (!hasReliableColor) {
            std::vector<int> active;
            for (int idx : candIdx) {
                if (!consumed[idx]) active.push_back(idx);
            }
            return mergeGroup(active, name, maxGap, dbgTag, srcType, maxSkipOverride);
        }
        // 分通道模式：按颜色分组后分别合并
        std::map<int, std::vector<int>> byGroup;
        for (int idx : candIdx) {
            if (consumed[idx]) continue;
            byGroup[singles[idx].group].push_back(idx);
        }
        bool anyMerged = false;
        for (auto& kv : byGroup) {
            if (mergeGroup(kv.second, name, maxGap, dbgTag, srcType, maxSkipOverride)) anyMerged = true;
        }
        return anyMerged;
    };

    // 两轮合并:
    //   第一轮(maxSkipOverride=0): 不允许缺字, 优先让能完全组成某地名的单字匹配到该地名,
    //     避免被带缺字的长地名抢先消耗(如"广州市"的三个字本可完全组成, 不应被"广州经济区"以缺字方式抢走)
    //   第二轮(maxSkipOverride=-1): 允许缺字(按默认配额), 匹配剩余单字
    auto processAllLayers = [&](int maxSkipOverride) {
        const bool enableDebug = (maxSkipOverride < 0);  // 仅第二轮输出调试日志, 避免第一轮缺字失败刷屏

        // 1. 线图层：每个要素做缓冲区，收集缓冲区内单字，与要素名称合并
        //    长名优先（按 name 长度降序处理要素）
        //    线要素缓冲区已是强空间约束, 不再按颜色分组,
        //    避免分通道模式下同一线要素的单字因颜色微差被拆散到不同组
        std::sort(lineFeats.begin(), lineFeats.end(),
                  [](const LayerFeature& a, const LayerFeature& b) { return a.name.length() > b.name.length(); });
        for (auto& lf : lineFeats) {
            if (lf.consumed || lf.geom.empty()) continue;
            // 收集缓冲区内未消耗单字
            std::vector<int> cand;
            for (size_t i = 0; i < singles.size(); ++i) {
                if (consumed[i]) continue;
                cv::Point2f c(singles[i].bbox.x + singles[i].bbox.width / 2.0f,
                              singles[i].bbox.y + singles[i].bbox.height / 2.0f);
                if (pointToPolylineDist(c, lf.geom) <= bufferDist) cand.push_back((int)i);
            }
            // 调试: 线要素缓冲区内候选与名称匹配情况(仅第二轮输出)
            if (enableDebug) {
                QSet<QChar> presentChars;
                for (int idx : cand) {
                    for (const QChar& ch : singles[idx].text) presentChars.insert(ch);
                }
                QString missing;
                for (const QChar& ch : lf.name) {
                    if (!presentChars.contains(ch)) missing += ch;
                }
                qDebug("[Merge][线] '%s' 缓冲区内候选=%zu 缺字='%s' maxGap=%.0f bufferDist=%.0f",
                       lf.name.toUtf8().constData(), cand.size(), missing.toUtf8().constData(),
                       maxGapLine, bufferDist);
            }
            // 线要素: 不分组, 直接尝试所有缓冲区内单字
            if (mergeGroup(cand, lf.name, maxGapLine,
                           enableDebug ? QStringLiteral("线") : QString(), 2, maxSkipOverride)) lf.consumed = true;
        }

        // 2. 面图层：筛选每个面内的候选(单字+多字片段)，与面名称合并
        //    面要素已有强空间约束(候选必须在面内), 不再按颜色分组,
        //    避免分通道模式下组号冲突或同通道内字号微差导致同色候选被拆散到不同组
        std::sort(polyFeats.begin(), polyFeats.end(),
                  [](const LayerFeature& a, const LayerFeature& b) { return a.name.length() > b.name.length(); });
        for (auto& lf : polyFeats) {
            if (lf.consumed || lf.geom.size() < 3) continue;
            std::vector<int> cand;
            for (size_t i = 0; i < singles.size(); ++i) {
                if (consumed[i]) continue;
                cv::Point2f c(singles[i].bbox.x + singles[i].bbox.width / 2.0f,
                              singles[i].bbox.y + singles[i].bbox.height / 2.0f);
                if (pointInPolygon(c, lf.geom)) cand.push_back((int)i);
            }
            if (cand.empty()) continue;

            // 调试: 面内候选与名称匹配情况(仅第二轮输出)
            if (enableDebug) {
                QSet<QChar> presentChars;
                for (int idx : cand) {
                    for (const QChar& ch : singles[idx].text) presentChars.insert(ch);
                }
                QString missing;
                for (const QChar& ch : lf.name) {
                    if (!presentChars.contains(ch)) missing += ch;
                }
                qDebug("[Merge][面] '%s' 面内候选=%zu 缺字='%s'",
                       lf.name.toUtf8().constData(), cand.size(), missing.toUtf8().constData());
            }

            // 面要素: 不分组, 直接尝试所有面内候选, 使用面要素距离阈值
            if (mergeGroup(cand, lf.name, maxGapPoly,
                           enableDebug ? QStringLiteral("面") : QString(), 3, maxSkipOverride)) lf.consumed = true;
        }
    };

    // phaseFilter: 0=两轮(默认), 1=仅精确(不缺字), 2=仅模糊(允许缺字)
    if (phaseFilter == 0 || phaseFilter == 1) {
        // 第一轮: 不允许缺字, 完全匹配优先
        processAllLayers(0);
    }
    if (phaseFilter == 0 || phaseFilter == 2) {
        // 第二轮: 允许缺字(按默认配额), 匹配剩余单字
        processAllLayers(-1);
    }

    // 重组结果：非单字 + 合并产生的新注记 + 未消耗的单字
    std::vector<TextResult> newResults = others;
    for (const auto& m : mergedList) newResults.push_back(m);
    for (size_t i = 0; i < singles.size(); ++i) {
        if (!consumed[i]) newResults.push_back(singles[i]);
    }
    int mergedCount = (int)mergedList.size();
    results = std::move(newResults);
    return mergedCount;
}

// 从 gdb 指定图层中提取 NAMES 字段构建地名库
int SE_ImageMapCheckDialog::loadGazetteerFromGdbLayers(const QString& gdbPath,
                                                        const QList<int>& layerIds)
{
    m_gazetteer.clear();
    m_gazetteerList.clear();

    if (gdbPath.isEmpty() || layerIds.isEmpty()) return 0;

    qDebug("[Gazetteer] 开始从 gdb 加载 %d 个图层: %s",
           layerIds.size(), gdbPath.toUtf8().constData());

    for (int layerId : layerIds) {
        QString uri = gdbPath + "|layerid=" + QString::number(layerId);
        QgsVectorLayer layer(uri, QStringLiteral("gaz_%1").arg(layerId), "ogr");
        if (!layer.isValid()) {
            qDebug("[Gazetteer] 图层 %d 无效", layerId);
            continue;
        }

        // 查找名称字段: 优先 NAMES, 找不到则回退到 NAME (不区分大小写)
        int nameIdx = -1;
        const QgsFields fields = layer.fields();
        for (int j = 0; j < fields.size(); ++j) {
            if (fields[j].name().compare("NAMES", Qt::CaseInsensitive) == 0) {
                nameIdx = j;
                break;
            }
        }
        if (nameIdx < 0) {
            for (int j = 0; j < fields.size(); ++j) {
                if (fields[j].name().compare("NAME", Qt::CaseInsensitive) == 0) {
                    nameIdx = j;
                    break;
                }
            }
        }
        if (nameIdx < 0) {
            qDebug("[Gazetteer] 图层 %d (%s) 无 NAMES/NAME 字段",
                   layerId, layer.name().toUtf8().constData());
            continue;
        }

        // 遍历要素，提取名称
        int layerCount = 0;
        QgsFeature feat;
        QgsFeatureIterator it = layer.getFeatures();
        while (it.nextFeature(feat)) {
            QVariant val = feat.attribute(nameIdx);
            if (!val.isValid() || val.isNull()) continue;
            QString name = val.toString().trimmed();
            if (name.length() < 2) continue;
            bool ok;
            name.toDouble(&ok);
            if (ok) continue;  // 跳过纯数字
            if (!m_gazetteer.contains(name)) {
                m_gazetteer.insert(name);
                m_gazetteerList << name;
            }
            ++layerCount;
        }
        qDebug("[Gazetteer] 图层 %d (%s): 提取 %d 条地名",
               layerId, layer.name().toUtf8().constData(), layerCount);
    }

    // 按长度降序排序：匹配时优先长地名
    std::sort(m_gazetteerList.begin(), m_gazetteerList.end(),
              [](const QString& a, const QString& b) { return a.length() > b.length(); });

    qDebug("[Gazetteer] 共加载 %d 条地名", m_gazetteer.size());
    return m_gazetteer.size();
}

// 加载带几何信息的地名库(需 m_affineValid)，在仿射变换后、匹配前调用
// 每个地名条目存储源要素的几何(像素坐标)和类型，用于缓冲区限定匹配
void SE_ImageMapCheckDialog::loadGazetteerWithGeometry()
{
    m_gazetteerEntries.clear();

    if (m_gdbPath.isEmpty() || m_gazetteerLayerIds.isEmpty() || !m_affineValid) return;

    // 仿射变换：geo -> img。affine 定义为 img = M * geo
    double a = m_affine.at<double>(0,0), b = m_affine.at<double>(0,1), c = m_affine.at<double>(0,2);
    double d = m_affine.at<double>(1,0), e = m_affine.at<double>(1,1), f = m_affine.at<double>(1,2);
    auto geoToImg = [&](double gx, double gy) -> cv::Point2f {
        return cv::Point2f((float)(a*gx + b*gy + c), (float)(d*gx + e*gy + f));
    };

    // 获取境界线图层 CRS(仿射变换在其坐标空间拟合), 地名图层需转换到此 CRS
    QgsCoordinateReferenceSystem targetCrs;
    if (!m_gdbPath.isEmpty() && m_boundaryLayerId >= 0) {
        QString buri = m_gdbPath + "|layerid=" + QString::number(m_boundaryLayerId);
        QgsVectorLayer probe(buri, "probe", "ogr");
        if (probe.isValid() && probe.crs().isValid()) targetCrs = probe.crs();
    }
    if (!targetCrs.isValid()) targetCrs = QgsCoordinateReferenceSystem("EPSG:4326");

    for (int layerId : m_gazetteerLayerIds) {
        QString uri = m_gdbPath + "|layerid=" + QString::number(layerId);
        QgsVectorLayer layer(uri, QStringLiteral("gaz_%1").arg(layerId), "ogr");
        if (!layer.isValid()) continue;

        // CRS 转换: 地名图层 CRS -> 境界线图层 CRS
        QgsCoordinateTransform xform;
        if (layer.crs().isValid() && targetCrs != layer.crs()) {
            xform = QgsCoordinateTransform(layer.crs(), targetCrs, QgsProject::instance());
        }

        // 名称字段: 优先 NAMES, 找不到则回退到 NAME
        int nameIdx = -1;
        const QgsFields fields = layer.fields();
        for (int j = 0; j < fields.size(); ++j) {
            if (fields[j].name().compare("NAMES", Qt::CaseInsensitive) == 0) { nameIdx = j; break; }
        }
        if (nameIdx < 0) {
            for (int j = 0; j < fields.size(); ++j) {
                if (fields[j].name().compare("NAME", Qt::CaseInsensitive) == 0) { nameIdx = j; break; }
            }
        }
        if (nameIdx < 0) continue;

        // 加载面/线/点要素几何:
        //   - 面、线要素: 用于缓冲区限定匹配(isWithinGazetteerBuffer)
        //   - 点要素: 用于距离检测(点要素地名匹配时, 注记到点距离超过阈值不允许匹配)
        QgsWkbTypes::GeometryType gt = layer.geometryType();
        int gtype = -1;
        if (gt == QgsWkbTypes::PolygonGeometry) gtype = 0;
        else if (gt == QgsWkbTypes::LineGeometry) gtype = 1;
        else if (gt == QgsWkbTypes::PointGeometry) gtype = 2;
        else continue;

        QgsFeature feat;
        QgsFeatureIterator it = layer.getFeatures();
        // 同名要素几何合并字典: 线要素同一地名可能被分成多段(多个要素),
        // 需将所有段几何合并到同一 GazetteerEntry, 确保缓冲区覆盖全部单字
        QHash<QString, int> lineNameIndex;  // name -> m_gazetteerEntries 下标
        while (it.nextFeature(feat)) {
            QVariant val = feat.attribute(nameIdx);
            if (!val.isValid() || val.isNull()) continue;
            QString name = val.toString().trimmed();
            if (name.length() < 2) continue;
            bool ok; name.toDouble(&ok);
            if (ok) continue;

            const QgsGeometry& geom = feat.geometry();
            if (geom.isNull()) continue;

            // CRS 转换: 地名图层 CRS -> 境界线图层 CRS
            QgsGeometry tGeom = geom;
            if (xform.isValid()) {
                tGeom.transform(xform);
                if (tGeom.isNull()) tGeom = geom;
            }

            if (gtype == 2) {
                // 点要素
                GazetteerEntry entry;
                entry.name = name;
                entry.geomType = gtype;
                QgsPointXY pt = tGeom.asPoint();
                entry.geom.push_back(geoToImg(pt.x(), pt.y()));
                if (!entry.geom.empty()) m_gazetteerEntries.push_back(entry);
            } else if (gtype == 1) {
                // 线要素: 同名多段合并到同一 entry
                QgsMultiPolylineXY mpl = tGeom.asMultiPolyline();
                if (mpl.isEmpty()) {
                    QgsPolylineXY pl = tGeom.asPolyline();
                    if (!pl.isEmpty()) mpl << pl;
                }
                if (lineNameIndex.contains(name)) {
                    auto& entry = m_gazetteerEntries[lineNameIndex[name]];
                    if (!entry.geom.empty()) entry.geom.push_back(cv::Point2f(NAN, NAN));
                    for (const auto& pl : mpl) {
                        for (const QgsPointXY& p : pl) entry.geom.push_back(geoToImg(p.x(), p.y()));
                    }
                } else {
                    GazetteerEntry entry;
                    entry.name = name;
                    entry.geomType = gtype;
                    for (const auto& pl : mpl) {
                        for (const QgsPointXY& p : pl) entry.geom.push_back(geoToImg(p.x(), p.y()));
                    }
                    if (!entry.geom.empty()) {
                        lineNameIndex.insert(name, m_gazetteerEntries.size());
                        m_gazetteerEntries.push_back(entry);
                    }
                }
            } else {
                // 面要素：取外环, 同名各面独立添加
                GazetteerEntry entry;
                entry.name = name;
                entry.geomType = gtype;
                QgsMultiPolygonXY mpg = tGeom.asMultiPolygon();
                if (mpg.isEmpty()) {
                    QgsPolygonXY pg = tGeom.asPolygon();
                    if (!pg.isEmpty()) mpg << pg;
                }
                for (const auto& pg : mpg) {
                    if (pg.isEmpty()) continue;
                    for (const QgsPointXY& p : pg[0]) entry.geom.push_back(geoToImg(p.x(), p.y()));
                    break;  // 只取第一个外环
                }
                if (!entry.geom.empty()) m_gazetteerEntries.push_back(entry);
            }
        }
    }
    qDebug("[Gazetteer] 带几何地名条目: %d", m_gazetteerEntries.size());
}

// 检查像素点是否在某地名的源要素缓冲区内
// bufferPx: 缓冲距离(像素)
// 对于面要素：点在多边形内或距边界 < bufferPx
// 对于线要素：点到最近线段 < bufferPx
// 对于点要素：点到要素点 < bufferPx
bool SE_ImageMapCheckDialog::isWithinGazetteerBuffer(const cv::Point2f& pt,
                                                       const QString& name,
                                                       double bufferPx) const
{
    if (m_gazetteerEntries.isEmpty()) return true;  // 无几何信息时不限制

    bool found = false;
    for (const auto& entry : m_gazetteerEntries) {
        if (entry.name != name) continue;
        found = true;

        if (entry.geomType == 0) {
            // 面要素：pointPolygonTest
            if (entry.geom.size() < 3) continue;
            double r = cv::pointPolygonTest(entry.geom, pt, true);
            if (r >= -bufferPx) return true;  // 在多边形内或缓冲区内
        } else if (entry.geomType == 1) {
            // 线要素：点到线段距离(跳过 NaN 分隔的段)
            for (size_t k = 0; k + 1 < entry.geom.size(); ++k) {
                const cv::Point2f& a = entry.geom[k];
                const cv::Point2f& b = entry.geom[k+1];
                if (std::isnan(a.x) || std::isnan(b.x)) continue;  // 段分隔点
                cv::Point2f ab = b - a, ap = pt - a;
                double t = ab.dot(ab);
                if (t > 0) t = std::max(0.0, std::min(1.0, ap.dot(ab) / t));
                cv::Point2f closest = a + ab * (float)t;
                double dist = cv::norm(pt - closest);
                if (dist <= bufferPx) return true;
            }
        } else if (entry.geomType == 2) {
            // 点要素: 不做缓冲区限制, 直接放行
            // 点要素地名匹配时的距离阈值限制在 matchAgainstGazetteer 中单独处理
            return true;
        }
    }
    // 如果地名在库中但没有几何条目(可能在加载几何时被跳过)，不限制
    return found ? false : true;
}

// 计算像素点到某地名源点要素的最近距离
// 仅检查点要素(geomType==2), 用于点要素地名匹配时的距离阈值检测
// 返回 < 0 表示该地名无点要素几何(不限制); >= 0 表示最近距离(像素)
double SE_ImageMapCheckDialog::pointDistanceToGazetteer(const cv::Point2f& pt,
                                                          const QString& name) const
{
    if (m_gazetteerEntries.isEmpty()) return -1.0;

    bool found = false;
    double minDist = std::numeric_limits<double>::max();
    for (const auto& entry : m_gazetteerEntries) {
        if (entry.name != name) continue;
        if (entry.geomType != 2) continue;  // 仅点要素
        found = true;
        for (const auto& g : entry.geom) {
            double d = cv::norm(pt - g);
            if (d < minDist) minDist = d;
        }
    }
    return found ? minDist : -1.0;
}

// 计算两个字符串的编辑距离（Levenshtein）
static int editDistance(const QString& a, const QString& b)
{
    int n = a.size(), m = b.size();
    if (n == 0) return m;
    if (m == 0) return n;
    std::vector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; ++j) prev[j] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        for (int j = 1; j <= m; ++j) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j-1] + 1, prev[j-1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

// 地名引导合并: 对未匹配结果, 找到包含其文本的地名,
// 尝试用附近注记补全缺失部分(前缀/后缀), 组成完整地名
// 在最终 matchAgainstGazetteer 之前调用, 减少不必要的模糊匹配
void SE_ImageMapCheckDialog::gazetteerGuidedMerge(std::vector<TextResult>& results) const
{
    if (m_gazetteer.isEmpty() || results.size() < 2) return;

    auto isNearby = [](const cv::Rect& a, const cv::Rect& b) -> bool {
        double hRatio = (double)std::min(a.height, b.height) / std::max(a.height, b.height);
        if (hRatio < 0.75) return false;
        double refH = std::min(a.height, b.height);
        double hGap = std::max(a.x, b.x) - std::min(a.x + a.width, b.x + b.width);
        double vGap = std::max(a.y, b.y) - std::min(a.y + a.height, b.y + b.height);
        // 不要求对齐, 仅限制间距(河流注记沿地物排列, 不对齐)
        return hGap <= refH * 3.0 || vGap <= refH * 3.0;
    };

    bool changed = true;
    while (changed && results.size() > 1) {
        changed = false;
        for (size_t i = 0; i < results.size() && !changed; ++i) {
            TextResult& tr = results[i];
            if (tr.verified || tr.sensitive) continue;
            const QString& text = tr.text;
            if (text.isEmpty()) continue;

            // 收集同组、空间上邻近的未匹配结果
            std::vector<size_t> nearby;
            for (size_t j = 0; j < results.size(); ++j) {
                if (j == i || results[j].verified || results[j].sensitive) continue;
                if (results[j].group != tr.group) continue;
                if (!isNearby(tr.bbox, results[j].bbox)) continue;
                nearby.push_back(j);
            }
            if (nearby.empty()) continue;

            // 遍历地名库, 找包含 text 的地名
            // 缓冲区中心点和缓冲距离
            cv::Point2f center(tr.bbox.x + tr.bbox.width / 2.0f, tr.bbox.y + tr.bbox.height / 2.0f);
            double bufferPx = tr.bbox.height * configByIdReader().getRuleValue(ITEM_ID_ANNOTATION_OCR, "线图层缓冲倍数", 2.0);
            if (bufferPx < 50.0) bufferPx = 50.0;
            for (const QString& name : m_gazetteerList) {
                if (name.length() < 2 || name == text || !name.contains(text)) continue;
                // 缓冲区检查: 当前结果必须在候选地名源要素缓冲区内
                if (!isWithinGazetteerBuffer(center, name, bufferPx)) continue;

                int idx = name.indexOf(text);
                QString prefix = name.left(idx);
                QString suffix = name.mid(idx + text.length());

                // 1. 尝试用单个邻近结果匹配前缀
                if (!prefix.isEmpty()) {
                    for (size_t j : nearby) {
                        if (results[j].text == prefix) {
                            tr.text = prefix + text;
                            // 前缀在阅读顺序首位, 用前缀片段坐标
                            tr.bbox = results[j].bbox;
                            tr.confidence = std::min(tr.confidence, results[j].confidence);
                            tr.mergedByGeometry = true;
                            results.erase(results.begin() + j);
                            changed = true;
                            break;
                        }
                    }
                    if (changed) break;
                }

                // 2. 尝试用单个邻近结果匹配后缀
                if (!suffix.isEmpty()) {
                    for (size_t j : nearby) {
                        if (results[j].text == suffix) {
                            tr.text = text + suffix;
                            // tr 在阅读顺序首位, 保持 tr 原坐标不变
                            tr.confidence = std::min(tr.confidence, results[j].confidence);
                            tr.mergedByGeometry = true;
                            results.erase(results.begin() + j);
                            changed = true;
                            break;
                        }
                    }
                    if (changed) break;
                }

                // 3. 尝试用多个单字组合后缀
                if (!suffix.isEmpty() && suffix.length() >= 2) {
                    std::vector<size_t> singles;
                    for (size_t j : nearby) {
                        if (results[j].text.length() == 1) singles.push_back(j);
                    }
                    if (!singles.empty()) {
                        bool isH = std::abs((double)tr.bbox.y - (double)results[nearby[0]].bbox.y)
                                   < tr.bbox.height * 0.5;
                        std::sort(singles.begin(), singles.end(), [&](size_t a, size_t b) {
                            return isH ? results[a].bbox.x < results[b].bbox.x
                                       : results[a].bbox.y < results[b].bbox.y;
                        });
                        QString built;
                        std::vector<size_t> used;
                        size_t si = 0;
                        for (int c = 0; c < suffix.length() && si < singles.size(); ++c) {
                            bool found = false;
                            for (size_t k = si; k < singles.size(); ++k) {
                                if (results[singles[k]].text[0] == suffix[c]) {
                                    built += suffix[c];
                                    used.push_back(singles[k]);
                                    si = k + 1;
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) break;
                        }
                        if (built == suffix && (int)used.size() == suffix.length()) {
                            // tr 在阅读顺序首位, 保持 tr 原坐标不变
                            for (size_t j : used) {
                                tr.confidence = std::min(tr.confidence, results[j].confidence);
                            }
                            tr.text = text + suffix;
                            tr.mergedByGeometry = true;
                            std::sort(used.rbegin(), used.rend());
                            for (size_t j : used) results.erase(results.begin() + j);
                            changed = true;
                            break;
                        }
                    }
                }

                // 4. 尝试用多个单字组合前缀
                if (!prefix.isEmpty() && prefix.length() >= 2) {
                    std::vector<size_t> singles;
                    for (size_t j : nearby) {
                        if (results[j].text.length() == 1) singles.push_back(j);
                    }
                    if (!singles.empty()) {
                        bool isH = std::abs((double)tr.bbox.y - (double)results[nearby[0]].bbox.y)
                                   < tr.bbox.height * 0.5;
                        std::sort(singles.begin(), singles.end(), [&](size_t a, size_t b) {
                            return isH ? results[a].bbox.x < results[b].bbox.x
                                       : results[a].bbox.y < results[b].bbox.y;
                        });
                        QString built;
                        std::vector<size_t> used;
                        size_t si = 0;
                        for (int c = 0; c < prefix.length() && si < singles.size(); ++c) {
                            bool found = false;
                            for (size_t k = si; k < singles.size(); ++k) {
                                if (results[singles[k]].text[0] == prefix[c]) {
                                    built += prefix[c];
                                    used.push_back(singles[k]);
                                    si = k + 1;
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) break;
                        }
                        if (built == prefix && (int)used.size() == prefix.length()) {
                            // 前缀在阅读顺序首位, 用第一个前缀单字坐标
                            tr.bbox = results[used.front()].bbox;
                            for (size_t j : used) {
                                tr.confidence = std::min(tr.confidence, results[j].confidence);
                            }
                            tr.text = prefix + text;
                            tr.mergedByGeometry = true;
                            std::sort(used.rbegin(), used.rend());
                            for (size_t j : used) results.erase(results.begin() + j);
                            changed = true;
                            break;
                        }
                    }
                }
            }
        }
    }
}

// 用地名库对 OCR 结果做模糊匹配+纠错
// 匹配规则：
//   1. 精确匹配：text 完全等于某地名 → verified=true
//   2. 子串匹配：text 是某地名的子串，或某地名是 text 的子串 → verified=true, matchedName=长者
//   3. 模糊匹配：长度>=2 且编辑距离<=2 → verified=true, matchedName=纠错后的地名
// 未匹配则 verified=false（标记为"未验证"）
// 注意: 已验证(verified=true)的结果跳过, 保留之前的匹配结果
int SE_ImageMapCheckDialog::matchAgainstGazetteer(std::vector<TextResult>& results) const
{
    if (m_gazetteer.isEmpty()) return 0;
    int matched = 0;

    // 模糊匹配参数从 XML 质检规则（地名库匹配，id=605）读取，循环外只读一次
    const int    fuzzyMinLen   = static_cast<int>(configByIdReader().getRuleValue(ITEM_ID_GAZETTEER_MATCH, "模糊匹配最小字数", 3.0));
    const double fuzzyAccuracy = configByIdReader().getRuleValue(ITEM_ID_GAZETTEER_MATCH, "模糊匹配正确率", 50.0);
    // 点要素匹配距离倍数: 注记高度 * 倍数 = 最大允许距离, 超过则不允许匹配点要素地名
    const double pointDistFactor = configByIdReader().getRuleValue(ITEM_ID_GAZETTEER_MATCH, "点要素匹配距离倍数", 10.0);

    // 点要素距离检查: 当地名来源于点要素时, 注记中心到点要素距离超过阈值则不允许匹配
    // 返回 true 表示允许匹配(无点要素几何或距离在阈值内), false 表示不允许
    auto checkPointDistance = [&](const cv::Point2f& center, const QString& name, double bboxH) -> bool {
        double maxDist = bboxH * pointDistFactor;
        double dist = pointDistanceToGazetteer(center, name);
        if (dist < 0) return true;  // 无点要素几何, 不限制
        if (dist > maxDist) {
            qDebug("[Gazetteer][点要素] '%s' 距离=%.0f 超过阈值=%.0f, 不允许匹配",
                   name.toUtf8().constData(), dist, maxDist);
            return false;
        }
        return true;
    };

    // 多趟匹配策略: 精确 → 子串 → 模糊
    // 避免短文本(如"广州")通过子串匹配消耗长地名(如"广州市"),
    // 导致后面真正的"广州市"无法精确匹配
    const int PASS_EXACT = 0;
    const int PASS_SUBSTR = 1;
    const int PASS_FUZZY = 2;

    // 模糊匹配候选: 等精确/子串匹配结束后, 按正确率从高到低依次完成
    //   - 子串匹配候选: text 是 name 的子串或缺字, 计算编辑距离正确率
    //   - 模糊匹配候选: text 与 name 编辑距离在阈值内
    struct FuzzyCand { size_t idx; QString bestName; int bestDist; int refLen; double ratio; };
    std::vector<FuzzyCand> substrCandidates;
    std::vector<FuzzyCand> fuzzyCandidates;

    for (int pass = PASS_EXACT; pass <= PASS_FUZZY; ++pass) {
        for (size_t idx = 0; idx < results.size(); ++idx) {
            auto& tr = results[idx];
            // 敏感注记不参与地名匹配
            if (tr.sensitive) continue;
            // 已验证的结果跳过, 保留之前的匹配结果
            if (tr.verified) { if (pass == PASS_EXACT) ++matched; continue; }
            const QString& text = tr.text;
            if (text.isEmpty()) continue;

            // 缓冲区中心点和缓冲距离(取 bbox 高度的3倍, 限定匹配范围)
            cv::Point2f center(tr.bbox.x + tr.bbox.width / 2.0f, tr.bbox.y + tr.bbox.height / 2.0f);
            double bufferPx = tr.bbox.height * configByIdReader().getRuleValue(ITEM_ID_ANNOTATION_OCR, "线图层缓冲倍数", 2.0);
            if (bufferPx < 50.0) bufferPx = 50.0;  // 最低50像素

            // 单字(长度=1)只做精确匹配，不做子串/模糊匹配。
            if (text.length() == 1) {
                if (pass == PASS_EXACT && m_gazetteer.contains(text)) {
                    if (isWithinGazetteerBuffer(center, text, bufferPx) &&
                        checkPointDistance(center, text, tr.bbox.height)) {
                        tr.verified = true;
                        tr.matchedName = text;
                        tr.originalText = text;
                        ++matched;
                    }
                }
                continue;
            }

            if (pass == PASS_EXACT) {
                // 1. 精确匹配（text == matchedName）
                if (m_gazetteer.contains(text)) {
                    if (isWithinGazetteerBuffer(center, text, bufferPx) &&
                        checkPointDistance(center, text, tr.bbox.height)) {
                        tr.verified = true;
                        tr.matchedName = text;
                        tr.originalText = text;
                        ++matched;
                    }
                }
            } else if (pass == PASS_SUBSTR) {
                // 2. 子串匹配：遍历地名库(已按长度降序)，找包含关系
                //    无论 text 是 name 的子串还是反过来，matchedName 统一用标准地名(name)
                //    子串匹配意味着缺字(text 是 name 的一部分)或多字(text 包含 name),
                //    属于模糊匹配, 先收集候选, 等精确匹配结束后按正确率从高到低依次完成
                for (const QString& name : m_gazetteerList) {
                    if (name.length() < 2) continue;
                    if (name.contains(text) || text.contains(name)) {
                        // 缓冲区检查: OCR 结果必须在候选地名源要素缓冲区内
                        if (!isWithinGazetteerBuffer(center, name, bufferPx)) continue;
                        // 点要素距离检查: 超过阈值则不允许匹配
                        if (!checkPointDistance(center, name, tr.bbox.height)) continue;
                        // 计算正确率: 编辑距离 / max(text长度, name长度), 值越小正确率越高
                        int d = editDistance(text, name);
                        int refLen = std::max(text.length(), name.length());
                        double ratio = refLen > 0 ? (double)d / refLen : 1.0;
                        substrCandidates.push_back({idx, name, d, refLen, ratio});
                        break;  // 只取第一个(最长)匹配的地名作为候选
                    }
                }
            } else { // PASS_FUZZY
                // 3. 模糊匹配（标记为纠错）：
                //   - 两字地名不模糊匹配（太短，误纠风险高）
                //   - 三字及以上：要求正确率 > 阈值，编辑距离 = floor(len * (100 - 正确率) / 100)
                //   - 不立即匹配, 先收集所有候选, 等精确/子串匹配结束后
                //     按正确率从高到低依次完成匹配, 避免低准确率结果抢占高准确率的地名
                if (text.length() < fuzzyMinLen) continue;
                int textLen = text.length();
                int maxDist = static_cast<int>(std::floor(textLen * (100.0 - fuzzyAccuracy) / 100.0));
                int bestDist = maxDist + 1;
                QString bestName;
                for (const QString& name : m_gazetteerList) {
                    if (std::abs(name.length() - textLen) > maxDist) continue;
                    // 缓冲区检查: 只考虑在候选地名源要素缓冲区内的结果
                    if (!isWithinGazetteerBuffer(center, name, bufferPx)) continue;
                    // 点要素距离检查: 超过阈值则不允许匹配
                    if (!checkPointDistance(center, name, tr.bbox.height)) continue;
                    int d = editDistance(text, name);
                    if (d < bestDist) {
                        bestDist = d;
                        bestName = name;
                    }
                }
                if (bestDist <= maxDist && !bestName.isEmpty()) {
                    // 计算正确率: 编辑距离 / 文本长度, 值越小正确率越高
                    double ratio = textLen > 0 ? (double)bestDist / textLen : 1.0;
                    fuzzyCandidates.push_back({idx, bestName, bestDist, textLen, ratio});
                }
            }
        }
    }

    // 模糊匹配候选处理: 子串候选和模糊候选合并, 按正确率从高到低(ratio 从小到大)排序后依次匹配
    // 避免低正确率结果抢占高正确率的地名
    std::vector<FuzzyCand> allCandidates;
    allCandidates.reserve(substrCandidates.size() + fuzzyCandidates.size());
    for (const auto& c : substrCandidates) allCandidates.push_back(c);
    for (const auto& c : fuzzyCandidates) allCandidates.push_back(c);
    if (!allCandidates.empty()) {
        std::sort(allCandidates.begin(), allCandidates.end(),
            [](const FuzzyCand& a, const FuzzyCand& b) {
                return a.ratio < b.ratio;
            });
        for (const auto& fc : allCandidates) {
            // 跳过在等待期间已被精确匹配的结果
            if (results[fc.idx].verified) continue;
            auto& tr = results[fc.idx];
            // 点要素距离检查: 模糊/子串匹配候选在最终确认时再次检查距离
            cv::Point2f center(tr.bbox.x + tr.bbox.width / 2.0f, tr.bbox.y + tr.bbox.height / 2.0f);
            if (!checkPointDistance(center, fc.bestName, tr.bbox.height)) continue;
            tr.verified = true;
            tr.matchedName = fc.bestName;
            tr.originalText = tr.text;
            tr.corrected = (tr.text != fc.bestName);
            ++matched;
        }
    }
    return matched;
}

// 对未匹配的 bbox 区域放大裁剪后用不同参数重新识别
std::vector<TextResult> SE_ImageMapCheckDialog::reRecognizeUnmatched(
    const cv::Mat& img, const std::vector<TextResult>& unmatched) const
{
    std::vector<TextResult> newResults;
    if (unmatched.empty()) return newResults;

    QString tessdataDir = qgetenv("TESSDATA_PREFIX");
    if (tessdataDir.isEmpty()) {
        tessdataDir = QStringLiteral("C:/Users/honlye/vcpkg/installed/x64-windows/share/tessdata");
    }
    tesseract::TessBaseAPI api;
    if (api.Init(tessdataDir.toStdString().c_str(), "chi_sim+eng",
                 tesseract::OEM_LSTM_ONLY) != 0) {
        return newResults;
    }

    // 对每个未匹配区域：放大裁剪 + 多种预处理 + 多种 PSM 重新识别
    for (const auto& tr : unmatched) {
        // 单字未匹配更可能是合并失败，用更宽的裁剪区域尝试获取完整词
        // 多字未匹配则用正常放大倍数
        bool isSingleChar = (tr.text.length() <= 1);
        int padMul = isSingleChar ? 4 : 2;  // 单字放大 4 倍范围，多字 2 倍
        int padX = tr.bbox.width * padMul;
        int padY = tr.bbox.height;
        cv::Rect roi(std::max(0, tr.bbox.x - padX / 2),
                     std::max(0, tr.bbox.y - padY / 2),
                     tr.bbox.width + padX,
                     tr.bbox.height + padY);
        // 边界裁剪
        roi &= cv::Rect(0, 0, img.cols, img.rows);
        if (roi.width < 5 || roi.height < 5) continue;

        // 放大到至少 300px 宽
        double scale = std::max(2.0, 300.0 / roi.width);
        cv::Mat crop;
        cv::resize(img(roi), crop, cv::Size(), scale, scale, cv::INTER_CUBIC);

        // 预处理
        cv::Mat gray;
        if (crop.channels() == 3) cv::cvtColor(crop, gray, cv::COLOR_BGR2GRAY);
        else gray = crop.clone();
        // CLAHE 对比度限制（固定值, Tesseract 引擎专用）
        const double claheClip = 2.0;
        cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(claheClip, cv::Size(8, 8));
        clahe->apply(gray, gray);
        cv::Mat adaptiveBin, otsuBin;
        cv::adaptiveThreshold(gray, adaptiveBin, 255,
                              cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY, 31, 15);
        cv::threshold(gray, otsuBin, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);

        // 用 PSM_SINGLE_WORD 和 PSM_SINGLE_LINE 针对性识别
        // 单字情况额外加 PSM_SPARSE_TEXT 尝试获取周围更多文字
        struct Pass { const cv::Mat* mat; tesseract::PageSegMode psm; };
        Pass passes[] = {
            { &adaptiveBin, tesseract::PSM_SINGLE_LINE },
            { &otsuBin,     tesseract::PSM_SINGLE_LINE },
            { &gray,        tesseract::PSM_SINGLE_LINE },
            { &adaptiveBin, tesseract::PSM_SINGLE_WORD },
            { &otsuBin,     tesseract::PSM_SINGLE_WORD },
            { &adaptiveBin, tesseract::PSM_SPARSE_TEXT },
            { &gray,        tesseract::PSM_SPARSE_TEXT }
        };

        // 收集所有 pass 的识别结果，优先取字数多的，其次置信度高的
        struct Cand { QString text; float conf; cv::Rect bbox; };
        std::vector<Cand> cands;
        for (const auto& p : passes) {
            api.SetPageSegMode(p.psm);
            api.SetImage(p.mat->data, p.mat->cols, p.mat->rows, 1, static_cast<int>(p.mat->step));
            api.Recognize(0);
            tesseract::ResultIterator* ri = api.GetIterator();
            if (ri) {
                do {
                    int x1, y1, x2, y2;
                    if (ri->BoundingBox(tesseract::RIL_WORD, &x1, &y1, &x2, &y2)) {
                        float conf = ri->Confidence(tesseract::RIL_WORD);
                        char* word = ri->GetUTF8Text(tesseract::RIL_WORD);
                        if (word && word[0] != '\0') {
                            QString t = QString::fromUtf8(word).trimmed();
                            if (!t.isEmpty() && conf >= 30.0f) {
                                // bbox 映射回原图坐标
                                cv::Rect wordBbox(
                                    roi.x + static_cast<int>(x1 / scale),
                                    roi.y + static_cast<int>(y1 / scale),
                                    static_cast<int>((x2 - x1) / scale),
                                    static_cast<int>((y2 - y1) / scale));
                                cands.push_back(Cand{t, conf, wordBbox});
                            }
                        }
                        if (word) delete[] word;
                    }
                } while (ri->Next(tesseract::RIL_WORD));
                delete ri;
            }
            api.Clear();
        }

        // 优先取字数多的，其次置信度高的
        if (!cands.empty()) {
            auto bestIt = std::max_element(cands.begin(), cands.end(),
                [](const Cand& a, const Cand& b) {
                    if (a.text.length() != b.text.length())
                        return a.text.length() < b.text.length();
                    return a.conf < b.conf;
                });
            TextResult nr;
            nr.bbox = bestIt->bbox;
            nr.text = bestIt->text;
            nr.confidence = bestIt->conf;
            nr.avgColor = tr.avgColor;
            nr.fontSize = tr.fontSize;
            nr.group = tr.group;
            nr.reRecognized = true;
            newResults.push_back(nr);
        }
    }
    api.End();
    return newResults;
}

void SE_ImageMapCheckDialog::onLoadXmlConfig()
{
    addCheckLog("开始加载 XML 质检规则文件");
    // 参考 ScaleCheckerNormativeDialog::on_loadXmlButton_clicked 的做法
    // 默认目录优先用上次路径，其次用 testdata 目录
    QgsSettings settings;
    QString lastDir = settings.value(
        QStringLiteral("SE_IMAGE_MAP_CHECK/LastXmlDir"), QStringLiteral(""),
        QgsSettings::Section::Plugins).toString();

    QString filePath = QFileDialog::getOpenFileName(
        this,
        tr("选择 XML 质检规则文件"),
        lastDir,
        tr("XML文件 (*.xml);;所有文件 (*)")
    );
    if (filePath.isEmpty()) {
        addCheckLog("已取消选择 XML 质检规则文件");
        return;
    }

    // 记录上次使用的目录，下次打开对话框时默认定位到此目录
    QFileInfo fi(filePath);
    settings.setValue(QStringLiteral("SE_IMAGE_MAP_CHECK/LastXmlDir"),
        fi.absolutePath(), QgsSettings::Section::Plugins);

    if (loadAllConfigFromXml(filePath)) {
        m_lblXmlConfig->setText(QString("XML 规则: %1").arg(fi.fileName()));
        setLabelStatus(m_lblXmlConfig, "loaded");
        addCheckLog(QString("加载 XML 质检规则成功: %1").arg(filePath));
        QMessageBox::information(this, tr("成功"),
            tr("XML 质检规则文件加载成功！\n%1\n\n已加载全部质检项参数及敏感词表。").arg(filePath));
    } else {
        m_lblXmlConfig->setText("XML 规则: 加载失败");
        setLabelStatus(m_lblXmlConfig, "error");
        addCheckLog(QString("XML 质检规则文件加载失败: %1").arg(filePath));
        QMessageBox::warning(this, tr("加载失败"),
            tr("无法加载 XML 质检规则文件！\n%1\n\n请检查文件格式是否正确。").arg(filePath));
    }
}

// 平滑设置进度条：同步插值过渡，每步 processEvents 刷新 UI
// 用同步方式而非 QPropertyAnimation，因为后续耗时操作会阻塞事件循环，
// 异步动画的定时器无法触发，导致进度条不动
// 进度条只显示百分比，文字描述显示在下方 m_progressLabel
void SE_ImageMapCheckDialog::setProgressSmooth(int value, const QString& format)
{
    if (!m_oneClickMode) return;

    // 提取文字描述（去掉 %p% 占位符），设置到下方标签
    QString text = format;
    text.remove(QStringLiteral("%p%"));
    m_progressLabel->setText(text.trimmed());

    const int startVal = m_progressBar->value();
    const int delta = value - startVal;
    if (delta == 0) {
        QCoreApplication::processEvents();
        return;
    }

    // 200ms 内分 20 步过渡，每步 10ms
    const int steps = 20;
    const int stepMs = 10;
    QElapsedTimer timer;
    timer.start();

    for (int i = 1; i <= steps; ++i) {
        // OutCubic 缓动：t' = 1 - (1-t)^3
        double t = (double)i / steps;
        double eased = 1.0 - std::pow(1.0 - t, 3.0);
        int v = startVal + (int)(delta * eased);
        m_progressBar->setValue(v);
        QCoreApplication::processEvents();

        // 按真实时间对齐，避免总耗时超过 200ms
        int elapsed = (int)timer.elapsed();
        int target = i * stepMs;
        if (elapsed < target) {
            QThread::msleep(target - elapsed);
        }
    }
    m_progressBar->setValue(value);
    QCoreApplication::processEvents();
}

// 一键质检：自动串联境界线检测→注记检测，进度条实时反馈
void SE_ImageMapCheckDialog::onOneClickCheck()
{
    addCheckLog("开始一键质检");

    // 前置条件检查
    if (m_imagePath.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先选择图片");
        return;
    }
    if (!m_currentColor.isValid()) {
        QMessageBox::warning(this, "提示", "请先选择境界线颜色");
        return;
    }

    // 进入一键模式：抑制各步骤的完成弹窗，由本函数统一汇总
    // 进度更新在各子函数内部按阶段完成
    m_oneClickMode = true;
    m_btnOneClickCheck->setEnabled(false);
    m_progressBar->setValue(0);
    m_progressBar->setFormat("%p%");
    m_progressLabel->setText("开始质检...");
    // 记录质检开始时间（用于 JSON 报告 timing 字段）
    m_checkStartTime = QDateTime::currentDateTime();

    // 执行质检步骤前保存配准缓存（含境界线/地名库图层 ID、控制点、颜色等），
    // 确保下次打开同一图片时自动恢复
    saveGeorefCache();

    QCoreApplication::processEvents();

    // ===== 步骤1：境界线检测（内部自动触发异常检测，进度 3%→47%）=====
    onExtractColor();

    // ===== 步骤2：注记检测（OCR + 敏感词 + 地名匹配，进度 50%→78%）=====
    onExtractText();

    // ===== 步骤3：将结果加入系统图层（进度 80%→90%）=====
    setProgressSmooth(80, "正在加入图层... %p%");
    onAccept();

    // ===== 步骤4：输出质检文件（进度 90%→100%）=====
    setProgressSmooth(90, "正在输出质检文件... %p%");
    onExportReport();

    setProgressSmooth(100, "质检完成 %p%");

    // 退出一键模式
    m_oneClickMode = false;
    m_btnOneClickCheck->setEnabled(true);
    // 记录质检结束时间（用于 JSON 报告 timing 字段）
    m_checkEndTime = QDateTime::currentDateTime();

    // 汇总结果
    QString summary = QString("一键质检完成\n境界线: %1 条").arg(m_lastContours.size());
    if (m_hasAnomalyResult) {
        summary += QString("\n异常检测: 正常 %1 条, 含异常 %2 条")
            .arg(m_lastAnomalyResult.normalCount)
            .arg(m_lastAnomalyResult.positionOffsetCount);
    }
    summary += QString("\n注记: %1 处").arg(m_textResults.size());
    if (!m_textResults.empty()) {
        int nDirect = 0, nFuzzy = 0, nUnmatched = 0, nSensitive = 0;
        for (const auto& tr : m_textResults) {
            if (tr.sensitive) ++nSensitive;
            else if (tr.verified && tr.corrected) ++nFuzzy;
            else if (tr.verified) ++nDirect;
            else ++nUnmatched;
        }
        int total = (int)m_textResults.size();
        auto pct = [total](int n) -> QString {
            return QString("%1%").arg(total ? QString::number(n * 100.0 / total, 'f', 1) : "0.0");
        };
        summary += QString("\n  直接匹配: %1 (%2)").arg(nDirect).arg(pct(nDirect));
        summary += QString("\n  模糊匹配: %1 (%2)").arg(nFuzzy).arg(pct(nFuzzy));
        summary += QString("\n  未匹配:   %1 (%2)").arg(nUnmatched).arg(pct(nUnmatched));
        summary += QString("\n  敏感地名: %1 (%2)").arg(nSensitive).arg(pct(nSensitive));
    }
    addCheckLog("一键质检完成");
    QMessageBox::information(this, "一键质检完成", summary);
}

void SE_ImageMapCheckDialog::onAccept()
{
    addCheckLog("开始将结果加入系统图层");
    if (!m_hasResult) {
        // if (!m_oneClickMode) QMessageBox::warning(this, "提示", "请先进行境界线或注记提取");  // 独立执行弹窗已移除
        return;
    }
    if (m_imagePath.isEmpty()) {
        // if (!m_oneClickMode) QMessageBox::warning(this, "提示", "图片路径为空");  // 独立执行弹窗已移除
        return;
    }

    QString layerNameToExport = m_lastLayerName;

    // 若有地理配准，统一使用地理坐标导出
    if (m_affineValid) {
        if (m_oneClickMode) { setProgressSmooth(82, "加入图层: 创建栅格图层 %p%"); }
        QgsCoordinateReferenceSystem crs = getOutputCrs();

        // ---- 1) 用 VRT 加入配准后原图栅格 ----
        QgsRasterLayer* raster_layer = nullptr;
        {
            cv::Mat M3 = cv::Mat::eye(3, 3, CV_64F);
            m_affine.copyTo(M3(cv::Rect(0, 0, 3, 2)));
            cv::Mat M3inv;
            if (cv::invert(M3, M3inv)) {
                double ia = M3inv.at<double>(0,0), ib = M3inv.at<double>(0,1), ic = M3inv.at<double>(0,2);
                double id = M3inv.at<double>(1,0), ie = M3inv.at<double>(1,1), if_ = M3inv.at<double>(1,2);
                int imgW = m_lastImg.cols, imgH = m_lastImg.rows;

                QgsRasterLayer srcProbe(m_imagePath, "probe", "gdal");
                int bandCount = srcProbe.isValid() ? srcProbe.bandCount() : 1;
                if (bandCount < 1) bandCount = 1;

                QString vrtPath = QDir::tempPath() + "/" +
                    QFileInfo(m_imagePath).completeBaseName() + "_georef.vrt";
                QString srcPath = m_imagePath;
                srcPath.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");

                QString bandsXml;
                for (int b = 1; b <= bandCount; ++b) {
                    bandsXml += QString(
                        "  <VRTRasterBand dataType=\"Byte\" band=\"%1\">\n"
                        "    <SimpleSource>\n"
                        "      <SourceFilename relativeToVRT=\"0\">%2</SourceFilename>\n"
                        "      <SourceBand>%1</SourceBand>\n"
                        "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"%3\" ySize=\"%4\"/>\n"
                        "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"%3\" ySize=\"%4\"/>\n"
                        "    </SimpleSource>\n"
                        "  </VRTRasterBand>\n")
                        .arg(b).arg(srcPath).arg(imgW).arg(imgH);
                }

                QString wkt = crs.toWkt();
                wkt.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");

                QString vrtXml = QString(
                    "<VRTDataset rasterXSize=\"%1\" rasterYSize=\"%2\">\n"
                    "  <SRS>%3</SRS>\n"
                    "  <GeoTransform>%4, %5, %6, %7, %8, %9</GeoTransform>\n"
                    "%10"
                    "</VRTDataset>\n")
                    .arg(imgW).arg(imgH).arg(wkt)
                    .arg(ic, 0, 'g', 15).arg(ia, 0, 'g', 15).arg(ib, 0, 'g', 15)
                    .arg(if_, 0, 'g', 15).arg(id, 0, 'g', 15).arg(ie, 0, 'g', 15)
                    .arg(bandsXml);

                QFile vrtFile(vrtPath);
                if (vrtFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    vrtFile.write(vrtXml.toUtf8());
                    vrtFile.close();
                    raster_layer = new QgsRasterLayer(vrtPath,
                        QFileInfo(m_imagePath).completeBaseName() + "_配准", "gdal");
                    if (!raster_layer->isValid()) {
                        delete raster_layer; raster_layer = nullptr;
                    }
                }
            }
        }

        // 先将栅格图层加入工程（QgsProject::addMapLayer 默认把新图层放到图层树顶部，
        // 后续 addGeoLineLayer 添加的矢量图层会把它推到更下方，保证图片在最底层。
        // 这与已移除的像素坐标模式 addImageAndContoursToCanvas 中的处理顺序一致。）
        if (raster_layer) QgsProject::instance()->addMapLayer(raster_layer);

        // ---- 2) 标准境界线（蓝色，用于对比） ----
        QgsVectorLayer* refLayer = nullptr;
        if (m_boundaryLoaded && !m_boundaryFeatures.empty()) {
            refLayer = addGeoLineLayer("标准境界线", QColor(0, 0, 255), m_boundaryFeatures, 1.5);
        }

        // ---- 3) 提取境界线 ----
        // 若有异常检测结果，则按正常段（绿）/异常段（红）分层加入；
        // 否则整条作为提取结果（红）加入。
        QgsVectorLayer* normalLayer = nullptr;   // 正常段（绿）
        QgsVectorLayer* anomalyLayer = nullptr;  // 异常段（红）
        QgsVectorLayer* extractedLayer = nullptr; // 退化情况：无异常检测结果时的整条线
        if (m_hasAnomalyResult) {
            std::vector<std::vector<cv::Point2d>> greenLines;  // 正常段
            std::vector<std::vector<cv::Point2d>> redLines;    // 异常段
            for (auto& seg : m_lastAnomalyResult.segments) {
                for (auto& p : seg.normalParts) {
                    if (p.size() >= 2) greenLines.push_back(p);
                }
                for (auto& p : seg.anomalyParts) {
                    if (p.size() >= 2) redLines.push_back(p);
                }
            }
            if (!greenLines.empty()) {
                normalLayer = addGeoLineLayer("提取-正常", QColor(0, 160, 0), greenLines, 1.5);
            }
            if (!redLines.empty()) {
                anomalyLayer = addGeoLineLayer("提取-异常", QColor(220, 30, 30), redLines, 1.5);
            }
            // 若两类都为空（全部匹配/全部不匹配边界情况），回退到整条线
            if (!normalLayer && !anomalyLayer) {
                auto geoContours = imgContoursToGeo(m_lastContours);
                extractedLayer = addGeoLineLayer(layerNameToExport, QColor(220, 30, 30),
                    geoContours, 1.5);
            }
        } else {
            auto geoContours = imgContoursToGeo(m_lastContours);
            extractedLayer = addGeoLineLayer(layerNameToExport, QColor(220, 30, 30),
                geoContours, 1.5);
        }

        // ---- 4) 注记图层（像素→地理坐标） ----
        // 多通道模式: 为每个通道创建独立的"识别结果-通道名"图层
        // 单通道模式: 创建单个"识别结果"图层
        std::vector<QgsVectorLayer*> geoRawocrChannelLayers;
        QgsVectorLayer* geoRawocr_layer = nullptr;
        QgsVectorLayer* geoMatched_layer = nullptr;
        QgsVectorLayer* geoCorrected_layer = nullptr;
        QgsVectorLayer* geoUnmatched_layer = nullptr;
        QgsVectorLayer* geoSensitive_layer = nullptr;
        if (!m_textResults.empty() && m_affineValid) {
            if (m_oneClickMode) { setProgressSmooth(85, "加入图层: 创建注记图层 %p%"); }
            if (m_debugLayers) {
                if (!m_channelRawResults.empty()) {
                    // 多通道模式: 每个通道一个图层
                    for (auto& [chName, chResults] : m_channelRawResults) {
                        if (chResults.empty()) continue;
                        QString layerName = QString("识别结果-%1").arg(chName);
                        auto* lyr = createTextLayer(layerName, QColor(0xCC,0xCC,0xCC),
                            [](auto& t){ return true; }, true, &chResults);
                        if (lyr) geoRawocrChannelLayers.push_back(lyr);
                    }
                } else {
                    // 单通道模式
                    geoRawocr_layer = createTextLayer("识别结果", QColor(0xCC,0xCC,0xCC),
                        [](auto& t){ return true; }, true, &m_rawOcrResults);
                }
            }
            geoMatched_layer   = createTextLayer("地名匹配", QColor(255,255,255),
                [](auto& t){ return t.verified && !t.corrected; });
            geoCorrected_layer = createTextLayer("地名纠正", QColor(0xFF,0x8C,0x00),
                [](auto& t){ return t.verified && t.corrected; });
            geoUnmatched_layer = createTextLayer("未匹配注记", QColor(0xDC,0x00,0x00),
                [](auto& t){ return !t.verified && !t.sensitive; });
            geoSensitive_layer = createTextLayer("敏感文字异常注记", QColor(170,0,170),
                [](auto& t){ return t.sensitive; });
        }

        // ---- 5) 设置画布图层顺序 ----
        // 渲染顺序（QgsMapCanvas::setLayers 列表从前到后 = 从下到上）：
        //   栅格图片(底) → 标准境界线 → 提取-正常 → 提取-异常 → 注记(顶)
        // 栅格图层已在上方 addMapLayer（先于矢量图层），图层树默认顺序已保证其在最底层；
        // 此处再通过 setLayers 显式排列，双重保障。
        if (mIface) {
            if (m_oneClickMode) { setProgressSmooth(88, "加入图层: 设置画布顺序 %p%"); }
            QList<QgsMapLayer*> layers = mIface->mapCanvas()->layers();
            // 移除本次相关的所有图层（防止重复/位置错乱）
            for (int i = layers.size() - 1; i >= 0; --i) {
                if (layers[i] == raster_layer || layers[i] == refLayer ||
                    layers[i] == normalLayer || layers[i] == anomalyLayer ||
                    layers[i] == extractedLayer ||
                    layers[i] == geoRawocr_layer || layers[i] == geoMatched_layer ||
                    layers[i] == geoCorrected_layer ||
                    layers[i] == geoUnmatched_layer || layers[i] == geoSensitive_layer) {
                    layers.removeAt(i);
                    continue;
                }
                // 检查是否为识别结果通道图层
                for (auto* chLyr : geoRawocrChannelLayers) {
                    if (layers[i] == chLyr) { layers.removeAt(i); break; }
                }
            }
            // 按目标顺序重新插入：栅格在最前（最底层）
            if (raster_layer) layers.prepend(raster_layer);
            if (refLayer) layers.append(refLayer);
            if (normalLayer) layers.append(normalLayer);
            if (anomalyLayer) layers.append(anomalyLayer);
            if (extractedLayer) layers.append(extractedLayer);
            if (geoRawocr_layer) layers.append(geoRawocr_layer);
            for (auto* chLyr : geoRawocrChannelLayers) layers.append(chLyr);
            if (geoMatched_layer) layers.append(geoMatched_layer);
            if (geoCorrected_layer) layers.append(geoCorrected_layer);
            if (geoUnmatched_layer) layers.append(geoUnmatched_layer);
            if (geoSensitive_layer) layers.append(geoSensitive_layer);
            mIface->mapCanvas()->setLayers(layers);
            if (raster_layer) mIface->mapCanvas()->setExtent(raster_layer->extent());
            // 显式触发注记图层重绘，确保标注文本显示
            if (geoRawocr_layer) geoRawocr_layer->triggerRepaint();
            for (auto* chLyr : geoRawocrChannelLayers) chLyr->triggerRepaint();
            if (geoMatched_layer) geoMatched_layer->triggerRepaint();
            if (geoCorrected_layer) geoCorrected_layer->triggerRepaint();
            if (geoUnmatched_layer) geoUnmatched_layer->triggerRepaint();
            if (geoSensitive_layer) geoSensitive_layer->triggerRepaint();
            mIface->mapCanvas()->refresh();
        }

        layerNameToExport = m_lastLayerName + "_地理坐标";
    } else {
        // 无地理配准 → 无法输出图层
        addCheckLog("未完成地理配准, 无法加入图层");
        return;
    }

    QString msg = QString("已将结果加入系统图层（图层: %1）").arg(layerNameToExport);
    if (m_affineValid) {
        msg += "\n[已变换到地理坐标系]";
        if (m_boundaryLoaded) msg += "\n[已加入标准境界线（蓝色）用于对比]";
        if (m_hasAnomalyResult) {
            msg += QString("\n[境界线异常检测: 正常 %1 条, 异常 %2 条]")
                .arg(m_lastAnomalyResult.normalCount)
                .arg(m_lastAnomalyResult.positionOffsetCount);
            msg += "\n[图层: 提取-正常(绿) / 提取-异常(红)]";
        }
    }
    if (!m_textResults.empty()) {
        int verified = 0, unmatched = 0;
        for (auto& tr : m_textResults) {
            if (tr.verified) ++verified; else ++unmatched;
        }
        msg += QString("\n已验证注记: %1 处").arg(verified);
        if (unmatched > 0) msg += QString("\n未匹配注记: %1 处").arg(unmatched);
    }
    addCheckLog("已将结果加入系统图层");
    // if (!m_oneClickMode) QMessageBox::information(this, "完成", msg);  // 独立执行弹窗已移除

    // 加入图层后允许继续输出质检文件，仅禁用"加入图层"按钮避免重复加入
    // m_hasResult 保留为 true（onExportReport 依赖此标志判断是否有结果可导出）
    // m_hasAnomalyResult 也保留为 true（onExportReport 依赖此标志生成 602 报告）
    m_btnAccept->setEnabled(false);
}

// 选择输出目录：为空则恢复默认（图片同目录）规则
void SE_ImageMapCheckDialog::onBrowseOutputDir()
{
    QgsSettings settings;
    QString startDir = settings.value(
        QStringLiteral("SE_IMAGE_MAP_CHECK/LastOutputDir"),
        m_outputDir.isEmpty() ? QString() : m_outputDir,
        QgsSettings::Section::Plugins).toString();

    QString dir = QFileDialog::getExistingDirectory(this, "选择输出目录",
        startDir, QFileDialog::ShowDirsOnly);
    if (dir.isEmpty()) return;

    m_outputDir = dir;
    if (m_editOutputDir) m_editOutputDir->setText(dir);
    settings.setValue(QStringLiteral("SE_IMAGE_MAP_CHECK/LastOutputDir"),
        dir, QgsSettings::Section::Plugins);
    addCheckLog(QString("设置输出目录: %1").arg(dir));
}

void SE_ImageMapCheckDialog::onExportReport()
{
    addCheckLog("开始输出质检文件");
    if (!m_hasResult) {
        // if (!m_oneClickMode) QMessageBox::warning(this, "提示", "请先进行境界线或注记提取");  // 独立执行弹窗已移除
        return;
    }
    if (m_imagePath.isEmpty()) {
        // if (!m_oneClickMode) QMessageBox::warning(this, "提示", "图片路径为空");  // 独立执行弹窗已移除
        return;
    }
    // 记录质检结束时间（若尚未记录，用于独立执行质检步骤时填充 JSON timing）
    if (!m_checkEndTime.isValid()) {
        m_checkEndTime = QDateTime::currentDateTime();
    }

    // ===== 创建标准输出目录结构（与其他质检模块一致）=====
    QFileInfo imgInfo(m_imagePath);
    QString baseName = imgInfo.completeBaseName();
    // 优先使用用户指定的输出目录，为空则回退到图片同目录默认规则
    QString outputRoot = m_outputDir.isEmpty()
        ? (imgInfo.absolutePath() + "/" + baseName + "_质检输出")
        : (m_outputDir + "/" + baseName + "_质检输出");
    QString jsonDir = outputRoot + "/" + QString::fromUtf8("质检json文件");
    QString logDir = outputRoot + "/" + QString::fromUtf8("质检日志文件");
    QString errorDir = outputRoot + "/" + QString::fromUtf8("质检错误结果");
    QDir().mkpath(jsonDir);
    QDir().mkpath(logDir);
    QDir().mkpath(errorDir);
    QString jsonPath = QDir(jsonDir).filePath("图片地图质检_" + baseName + ".json");
    QString logPath = QDir(logDir).filePath("图片地图质检_" + baseName + ".log");

    // 使用 m_checkLog（各操作发生时实时记录，带真实时间戳）作为日志主体
    // 导出过程的日志追加到末尾
    auto writeLog = [&](const QString& msg) {
        QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
        m_checkLog << QString("%1 - %2").arg(ts).arg(msg);
    };
    writeLog("=== 输出质检文件 ===");
    writeLog(QString("输出目录: %1").arg(outputRoot));

    // 记录需要导出SHP的图层列表
    QList<QgsVectorLayer*> layersToExport;

    // ===== 创建临时图层（不加入QGIS工程），用于导出 =====
    if (m_oneClickMode) { setProgressSmooth(92, "输出: 创建临时图层 %p%"); }
    if (m_affineValid) {
        // 标准境界线（蓝色）
        if (m_boundaryLoaded && !m_boundaryFeatures.empty()) {
            QgsVectorLayer* refLayer = addGeoLineLayer("标准境界线", QColor(0, 0, 255),
                m_boundaryFeatures, 1.5, false);
            if (refLayer) layersToExport << refLayer;
        }

        // 提取境界线（正常/异常分层）
        if (m_hasAnomalyResult) {
            std::vector<std::vector<cv::Point2d>> greenLines, redLines;
            for (auto& seg : m_lastAnomalyResult.segments) {
                for (auto& p : seg.normalParts)
                    if (p.size() >= 2) greenLines.push_back(p);
                for (auto& p : seg.anomalyParts)
                    if (p.size() >= 2) redLines.push_back(p);
            }
            if (!greenLines.empty()) {
                QgsVectorLayer* normalLayer = addGeoLineLayer("提取-正常", QColor(0, 160, 0),
                    greenLines, 1.5, false);
                if (normalLayer) layersToExport << normalLayer;
            }
            if (!redLines.empty()) {
                QgsVectorLayer* anomalyLayer = addGeoLineLayer("提取-异常", QColor(220, 30, 30),
                    redLines, 1.5, false);
                if (anomalyLayer) layersToExport << anomalyLayer;
            }
            if (greenLines.empty() && redLines.empty()) {
                auto geoContours = imgContoursToGeo(m_lastContours);
                QgsVectorLayer* extractedLayer = addGeoLineLayer(m_lastLayerName, QColor(220, 30, 30),
                    geoContours, 1.5, false);
                if (extractedLayer) layersToExport << extractedLayer;
            }
        } else {
            auto geoContours = imgContoursToGeo(m_lastContours);
            QgsVectorLayer* extractedLayer = addGeoLineLayer(m_lastLayerName, QColor(220, 30, 30),
                geoContours, 1.5, false);
            if (extractedLayer) layersToExport << extractedLayer;
        }

        // 注记图层
        if (!m_textResults.empty()) {
            QgsVectorLayer* matched = createTextLayer("地名匹配", QColor(255,255,255),
                [](auto& t){ return t.verified && !t.corrected; }, false);
            if (matched) layersToExport << matched;
            QgsVectorLayer* corrected = createTextLayer("地名纠正", QColor(0xFF,0x8C,0x00),
                [](auto& t){ return t.verified && t.corrected; }, false);
            if (corrected) layersToExport << corrected;
            QgsVectorLayer* unmatched = createTextLayer("未匹配注记", QColor(0xDC,0x00,0x00),
                [](auto& t){ return !t.verified && !t.sensitive; }, false);
            if (unmatched) layersToExport << unmatched;
            QgsVectorLayer* sensitive = createTextLayer("敏感文字异常注记", QColor(170,0,170),
                [](auto& t){ return t.sensitive; }, false);
            if (sensitive) layersToExport << sensitive;
        }
    } else {
        auto geoContours = imgContoursToGeo(m_lastContours);
        QgsVectorLayer* extractedLayer = addGeoLineLayer(m_lastLayerName, QColor(220, 30, 30),
            geoContours, 1.5, false);
        if (extractedLayer) layersToExport << extractedLayer;
    }

    // ===== 导出 SHP =====
    writeLog("开始导出SHP文件...");
    if (m_oneClickMode) { setProgressSmooth(95, "输出: 导出SHP文件 %p%"); }
    static const QHash<QString, QString> shpNameMap = {
        { "提取-正常",   "境界线_正常" },
        { "提取-异常",   "境界线_异常" },
        { "地名匹配",    "注记_完全匹配" },
        { "地名纠正",    "注记_模糊匹配" },
        { "未匹配注记",  "注记_未匹配" },
        { "敏感文字异常注记", "注记_含敏感文字" }
    };
    for (QgsVectorLayer* lyr : layersToExport) {
        if (!lyr || !lyr->isValid()) continue;
        QString shpName = shpNameMap.value(lyr->name(), lyr->name());
        shpName.replace(QRegularExpression(R"([\\/:*?"<>|])"), "_");
        QString shpPath = QDir(errorDir).filePath(shpName + ".shp");
        QgsCoordinateReferenceSystem lyrCrs = lyr->crs().isValid() ? lyr->crs() : QgsCoordinateReferenceSystem("EPSG:4326");
        QgsVectorFileWriter::WriterError err = QgsVectorFileWriter::writeAsVectorFormat(
            lyr, shpPath, "UTF-8", lyrCrs, "ESRI Shapefile");
        if (err == QgsVectorFileWriter::NoError) {
            writeLog(QString("✓ 已导出 %1: %2 (%3 个要素)").arg(shpName).arg(shpPath).arg(lyr->featureCount()));
        } else {
            writeLog(QString("❌ 导出失败 %1 (错误码: %2)").arg(shpPath).arg(err));
        }
    }

    // ===== 输出标准 JSON 报告（仿照 map_check_task_manager 结构） =====
    if (m_oneClickMode) { setProgressSmooth(97, "输出: 生成JSON报告 %p%"); }

    // 按质检项分类统计错误
    QStringList colorErrors, anomalyErrors, ocrErrors, sensitiveErrors, matchErrors;
    int colorFeatureNum = 0, anomalyFeatureNum = 0, ocrFeatureNum = 0;
    int sensitiveFeatureNum = 0, matchFeatureNum = 0;
    bool colorHasErr = false, anomalyHasErr = false, ocrHasErr = false;
    bool sensitiveHasErr = false, matchHasErr = false;

    // 601 境界线颜色提取
    // 错误判定：根据是否存在"出现截断后使用标准境界线进行修复"（m_compensationDone）
    if (m_hasResult) {
        colorFeatureNum = (int)m_lastContours.size();
        if (colorFeatureNum == 0) {
            colorHasErr = true;
            colorErrors << "未提取到境界线";
        }
        if (m_compensationDone) {
            colorHasErr = true;
            colorErrors << QString::fromUtf8("出现截断后使用标准境界线进行修复");
        }
    }
    // 602 境界线异常检测
    if (m_hasAnomalyResult) {
        anomalyFeatureNum = m_lastAnomalyResult.totalExtractedSegments;
        if (m_lastAnomalyResult.positionOffsetCount > 0) {
            anomalyHasErr = true;
            for (int i = 0; i < (int)m_lastAnomalyResult.segments.size() && anomalyErrors.size() < 50; ++i) {
                for (auto& p : m_lastAnomalyResult.segments[i].anomalyParts) {
                    if (p.size() >= 2) {
                        anomalyErrors << QString("异常境界线段%1: 偏离标准线, 含%2个顶点")
                            .arg(anomalyErrors.size() + 1).arg(p.size());
                    }
                }
            }
        }
    }
    // 603 注记文字提取
    if (!m_textResults.empty()) {
        ocrFeatureNum = (int)m_textResults.size();
    }
    // 604 敏感文字合规性检查
    if (!m_textResults.empty()) {
        for (auto& tr : m_textResults) {
            if (tr.sensitive) {
                ++sensitiveFeatureNum;
                sensitiveHasErr = true;
                if (sensitiveErrors.size() < 50) {
                    sensitiveErrors << QString("敏感文字异常注记: '%1' [分类:%2, 命中词:%3]")
                        .arg(tr.text).arg(tr.sensitiveCategory).arg(tr.sensitiveWord);
                }
            }
        }
    }
    // 605 地名库匹配（模糊匹配也记为异常）
    int correctedCount = 0;
    int unmatchedCount = 0;
    if (!m_textResults.empty()) {
        for (auto& tr : m_textResults) {
            if (!tr.sensitive) {
                ++matchFeatureNum;
                if (!tr.verified) {
                    // 未匹配：完全找不到对应地名
                    ++unmatchedCount;
                    matchHasErr = true;
                    if (matchErrors.size() < 50) {
                        matchErrors << QString("未匹配注记: '%1'").arg(tr.text);
                    }
                } else if (tr.corrected) {
                    // 模糊匹配：经过纠正后匹配，记为异常
                    ++correctedCount;
                    matchHasErr = true;
                    if (matchErrors.size() < 50) {
                        matchErrors << QString("模糊匹配注记: '%1' → '%2'")
                            .arg(tr.originalText).arg(tr.matchedName);
                    }
                }
            }
        }
    }

    auto buildCheckItem = [](const QString& id, const QString& name,
                             const QString& status, bool errExist,
                             const QString& info, const QStringList& errs) {
        QJsonObject item;
        item["id"] = id;
        item["name"] = name;
        item["status"] = status;
        item["err_exist"] = errExist;
        item["info"] = info;
        QJsonObject errorsObj;
        for (int i = 0; i < errs.size(); ++i) {
            errorsObj[QString("error%1").arg(i + 1)] = errs[i];
        }
        item["errors"] = errorsObj;
        return item;
    };

    QString statusOk   = QString::fromUtf8("质检成功");
    QString statusSkip = QString::fromUtf8("未质检");

    QJsonObject check601 = buildCheckItem("601", QString::fromUtf8("境界线颜色提取"),
        m_hasResult ? statusOk : statusSkip, colorHasErr,
        QString("提取境界线段数: %1, 截断修复: %2")
            .arg(colorFeatureNum)
            .arg(m_compensationDone ? QString::fromUtf8("是") : QString::fromUtf8("否")),
        colorErrors);
    QJsonObject check602 = buildCheckItem("602", QString::fromUtf8("境界线异常检测"),
        m_hasAnomalyResult ? statusOk : statusSkip, anomalyHasErr,
        m_hasAnomalyResult ? QString("提取%1段, 正常%2, 异常%3, 平均偏离%4m, 最大偏离%5m")
            .arg(m_lastAnomalyResult.totalExtractedSegments)
            .arg(m_lastAnomalyResult.normalCount)
            .arg(m_lastAnomalyResult.positionOffsetCount)
            .arg(m_lastAnomalyResult.overallAvgOffset, 0, 'f', 1)
            .arg(m_lastAnomalyResult.overallMaxOffset, 0, 'f', 1)
            : QString::fromUtf8("未执行"), anomalyErrors);
    QJsonObject check603 = buildCheckItem("603", QString::fromUtf8("注记文字提取"),
        m_textResults.empty() ? statusSkip : statusOk, false,
        QString("OCR识别注记数: %1").arg(ocrFeatureNum), ocrErrors);
    QJsonObject check604 = buildCheckItem("604", QString::fromUtf8("敏感文字合规性检查"),
        m_textResults.empty() ? statusSkip : statusOk, sensitiveHasErr,
        QString("敏感注记数: %1 / %2").arg(sensitiveFeatureNum).arg(ocrFeatureNum), sensitiveErrors);
    QJsonObject check605 = buildCheckItem("605", QString::fromUtf8("地名库匹配"),
        m_textResults.empty() ? statusSkip : statusOk, matchHasErr,
        QString("非敏感注记数: %1, 未匹配: %2, 模糊匹配: %3")
            .arg(matchFeatureNum).arg(unmatchedCount).arg(correctedCount), matchErrors);

    // 构建 task 节点
    QJsonObject taskNode;
    taskNode["layer_name"] = QString::fromUtf8("图片地图质检");
    taskNode["check_item1"] = check601;
    taskNode["check_item2"] = check602;
    taskNode["check_item3"] = check603;
    taskNode["check_item4"] = check604;
    taskNode["check_item5"] = check605;

    QJsonObject mapTasks;
    mapTasks["task1"] = taskNode;

    // 统计信息
    int totalErr = (colorHasErr ? 1 : 0) + (anomalyHasErr ? 1 : 0)
                 + (sensitiveHasErr ? 1 : 0) + (matchHasErr ? 1 : 0);
    int completedNum = 0;
    if (m_hasResult) completedNum++;
    if (m_hasAnomalyResult) completedNum++;
    if (!m_textResults.empty()) completedNum += 3;  // 603/604/605 同时完成
    int uncompletedNum = 5 - completedNum;

    QJsonObject mapInfo;
    mapInfo["tasks_num"] = 1;
    mapInfo["completed_num"] = completedNum;
    mapInfo["err_num"] = totalErr;
    mapInfo["uncompleted_num"] = uncompletedNum;

    QJsonObject geopdfNode;
    geopdfNode["name"] = baseName;
    geopdfNode["info"] = mapInfo;
    geopdfNode["tasks"] = mapTasks;

    // 用户名：使用默认 operator（与其他模块未配置执行人时的回退值一致）
    QString userName = QString::fromUtf8("operator");
    // 质检时间：优先使用一键质检记录的起止时间；未记录则用当前时间
    QDateTime startTime = m_checkStartTime.isValid() ? m_checkStartTime : QDateTime::currentDateTime();
    QDateTime endTime   = m_checkEndTime.isValid() ? m_checkEndTime : QDateTime::currentDateTime();
    QJsonObject timing;
    timing["start_time"] = startTime.toString("yyyy-MM-ddTHH:mm:ss.zzzZ");
    timing["end_time"]   = endTime.toString("yyyy-MM-ddTHH:mm:ss.zzzZ");

    // QJsonDocument 默认按键名字典序输出，为保证 user → timing → Geopdf 顺序，手动拼接 JSON
    QJsonObject jsonRoot;
    jsonRoot["user"] = userName;
    jsonRoot["timing"] = timing;
    jsonRoot["Geopdf"] = geopdfNode;

    // 按指定顺序序列化：先构造顺序键列表，再依次输出
    QStringList orderedKeys = { "user", "timing", "Geopdf" };
    QString jsonText = "{\n";
    for (int i = 0; i < orderedKeys.size(); ++i) {
        const QString& key = orderedKeys[i];
        QJsonValue val = jsonRoot.value(key);
        QJsonDocument tmpDoc(val.isObject() ? val.toObject() : QJsonObject{{key, val}});
        QString part = tmpDoc.toJson(QJsonDocument::Indented);
        // tmpDoc 对象序列化会带外层 {}，需去掉首尾的 { } 并缩进
        part = part.mid(1, part.length() - 2); // 去掉首尾的 { }
        // 缩进 4 空格
        part.replace("\n", "\n    ");
        // 去掉末尾多余空格
        while (part.endsWith(" ")) part.chop(1);
        jsonText += QString("    \"%1\": %2").arg(key).arg(part);
        if (i < orderedKeys.size() - 1) jsonText += ",";
        jsonText += "\n";
    }
    jsonText += "}";

    QFile jsonFile(jsonPath);
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        jsonFile.write(jsonText.toUtf8());
        jsonFile.close();
        writeLog(QString("✓ JSON报告已生成: %1").arg(jsonPath));
    } else {
        writeLog(QString("❌ JSON报告生成失败: %1").arg(jsonPath));
    }

    // ===== 输出日志文件 =====
    writeLog("=== 质检结束 ===");
    QFile logFile(logPath);
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream logStream(&logFile);
        logStream.setCodec("UTF-8");
        logStream << m_checkLog.join("\n") << "\n";
        logFile.close();
    }

    // ===== 清理临时图层 =====
    for (QgsVectorLayer* lyr : layersToExport) {
        delete lyr;
    }

    QString msg = QString("质检文件已输出至:\n%1").arg(outputRoot);
    msg += QString("\n\nJSON报告: %1").arg(jsonPath);
    msg += QString("\n日志文件: %1").arg(logPath);
    msg += QString("\n错误要素: %1").arg(errorDir);
    // if (!m_oneClickMode) QMessageBox::information(this, "完成", msg);  // 独立执行弹窗已移除
}
 
