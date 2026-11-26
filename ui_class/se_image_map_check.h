#ifndef SE_IMAGE_MAP_CHECK_H
#define SE_IMAGE_MAP_CHECK_H

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QColor>
#include <QComboBox>
#include <QCheckBox>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QLabel>
#include <QDateTime>
#include <vector>
#include <functional>
#include <opencv2/core.hpp>
#include <qgscoordinatereferencesystem.h>
#include "se_boundary_anomaly_check_algorithm.h"
#include "XmlConfigReader.h"

class QgisInterface;
class QLineEdit;
class QLabel;
class QSlider;
class QPushButton;
class QCheckBox;
class QTableWidget;
class QComboBox;
class QProgressBar;
class GeoreferenceDialog;
class QgsVectorLayer;

// OCR 文字提取结果
struct TextResult {
    cv::Rect bbox;       // 文字区域像素包围框
    QString   text;      // 识别出的文字
    float     confidence; // 置信度 (0-100)
    cv::Scalar avgColor; // 文字像素平均颜色（BGR），用于分组
    bool     hasColor = false;     // 颜色是否已设置（用于区分默认值和实际颜色）
    int      fontSize;   // 字号（bbox 高度，像素），用于分组
    int      group;      // 分组编号（-1 表示未分组）
    // 地名库匹配状态
    bool     verified = false;     // 是否已通过地名库匹配验证
    QString  matchedName;          // 匹配到的标准地名（纠错后的文字，未匹配时为空）
    QString  originalText;         // 匹配前的原始识别文本（匹配时 text 会被替换，此处保留原始值）
    bool     reRecognized = false; // 是否经过反馈循环重新识别
    bool     corrected = false;    // 是否经模糊匹配纠错(text≠matchedName 且非精确/子串匹配)
    // 敏感文字合规性检查
    bool     sensitive = false;    // 是否含敏感文字（含则不参与地名匹配，单独作为异常注记输出）
    QString  sensitiveCategory;    // 命中的敏感词分类（安全/涉军/...）
    QString  sensitiveWord;        // 命中的具体敏感词
    bool     mergedByGeometry = false; // 是否由图层几何合并策略产生（调试用，单独成图层便于检查）
    bool     isOriginalChannel = false; // 是否来自原图通道(影响单字合并距离倍数)
    int      geomMergeSrc = 0;          // 几何合并来源: 0=无(OCR直出), 1=点, 2=线, 3=面
    QString  channelName;               // 通道名称(原图/淡蓝/品红/绿等), 分通道模式下用于隔离合并与匹配
};

// 图片地图质检对话框
// 基于 OpenCV 对图片地图进行境界线/边缘提取与颜色提取，
class SE_ImageMapCheckDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SE_ImageMapCheckDialog(QgisInterface* iface, QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
    ~SE_ImageMapCheckDialog() override;

private slots:
    void onSelectImage();      // 选择待处理图片
    void onExtractText();      // 提取注记（OCR）
    void onPickHaloColor();    // 在图片上取内晕线颜色（弹出取色窗口）
    void onPickOuterHaloColor(); // 在图片上取外晕线颜色（弹出取色窗口）
    void onExtractColor();     // 按当前选中颜色提取境界线
    void onHaloColorBtnClicked();  // 点击内晕线颜色按钮弹出 QColorDialog
    void onHaloColorEdited();      // 内晕线颜色代码输入框编辑完成
    void onOuterHaloColorBtnClicked();  // 点击外晕线颜色按钮弹出 QColorDialog
    void onOuterHaloColorEdited();      // 外晕线颜色代码输入框编辑完成
    void onAccept();           // 将结果加入图层
    void onExportReport();     // 输出质检文件（JSON报告+日志+SHP）
    void onBrowseOutputDir();  // 选择输出目录
    void onLoadGdbData();      // 加载 gdb 数据源（仅加载路径，不自动选境界线图层）
    void onSelectBoundaryLayer(); // 从已加载 gdb 中选择境界线图层
    void onSelectGazetteerLayers(); // 从已加载 gdb 中选择图层构建地名库
    void onOpenGeoreference(); // 打开地理配准窗口
    void onLoadXmlConfig();    // 加载 XML 质检规则文件（让用户通过对话框选择）
    void onOneClickCheck();    // 一键质检：自动执行境界线检测→注记检测全流程
    // 平滑设置进度条：用动画从当前值过渡到目标值（仅一键质检模式生效）
    void setProgressSmooth(int value, const QString& format);

protected:
    // 预览标签的滚轮缩放事件过滤器（安装在 m_lblOriginal/m_lblResult 上）
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // 构建左侧控制面板与右侧预览区
    void buildUi();
    // 境界线异常检测：提取后自动调用，结果存入 m_lastAnomalyResult，不加入 QGIS 图层
    // 返回 true 表示已执行检测（无论是否发现异常）；false 表示因前置条件不满足而跳过
    bool runBoundaryAnomalyCheck();
    // 缓存本次提取结果，启用"将结果加入图层"按钮
    void cacheResult(const cv::Mat& img,
                     const std::vector<std::vector<cv::Point2f>>& contours,
                     const QString& layerName);
    // 将图像像素坐标转换为地理坐标（需 m_affineValid），返回是否成功
    bool imgToGeo(const std::vector<cv::Point2f>& imgPts,
                  std::vector<cv::Point2d>& geoPts) const;
    bool imgToGeo(const cv::Point2f& imgPt, cv::Point2d& geoPt) const;
    // 将边界线从图像坐标整体转到地理坐标
    std::vector<std::vector<cv::Point2d>> imgContoursToGeo(
        const std::vector<std::vector<cv::Point2f>>& contours) const;
    // 获取当前输出用 CRS
    QgsCoordinateReferenceSystem getOutputCrs() const;
    // 创建地理坐标内存线图层（addToProject=true 时加入工程）
    QgsVectorLayer* addGeoLineLayer(const QString& name, const QColor& color,
        const std::vector<std::vector<cv::Point2d>>& lines, double lineWidth = 2.0,
        bool addToProject = true);
    // 创建地理坐标内存注记点图层（addToProject=true 时加入工程）
    QgsVectorLayer* createTextLayer(const QString& name, const QColor& bufColor,
        std::function<bool(const TextResult&)> filter,
        bool addToProject = true,
        const std::vector<TextResult>* dataSource = nullptr);
    // 设置当前颜色并同步所有颜色控件显示
    void setCurrentColor(const QColor& color);
    void setCurrentHaloColor(const QColor& color);
    void setCurrentOuterHaloColor(const QColor& color);
    // 在图像上用白色填充文字区域（防止文字被提取为轮廓）
    void maskTextRegions(cv::Mat& img, const std::vector<TextResult>& texts) const;
    // 按当前选中颜色从图像提取境界线轮廓（HSV 分割 + 骨架化 + 平滑）
    std::vector<std::vector<cv::Point2f>> extractContoursByColor(const cv::Mat& workImg);
    // 从 gdb 指定图层中提取 NAMES 字段构建地名库，返回加载数量
    int loadGazetteerFromGdbLayers(const QString& gdbPath, const QList<int>& layerIds);
    // 设置预览图（保存原始 QImage 并按当前缩放比例渲染到对应 QLabel）
    // 保存原始 QImage 到 m_origImg 并按当前缩放比例渲染（单画面预览，isOriginal 参数保留以兼容旧调用点）
    void setPreviewImage(bool isOriginal, const cv::Mat& mat, const QString& title);
    // 按 m_previewScale 重新渲染预览图
    void applyPreviewScale();
    // 计算并设置"适应窗口"的缩放比例（基于原图尺寸和 scrollArea 可视区域）
    void fitPreviewToWindow();
    // 刷新底部表格：展示未匹配项与纠错项
    void updateTextTable();
    // 用地名库对 OCR 结果做模糊匹配+纠错，返回匹配成功数量
    int matchAgainstGazetteer(std::vector<TextResult>& results) const;
    // 地名引导合并: 对未匹配结果, 找到包含其文本的地名, 尝试用附近注记补全
    void gazetteerGuidedMerge(std::vector<TextResult>& results) const;

    // ===== 单字合并策略（基于 gdb 图层几何）=====
    // 按 线图层 → 面图层 顺序，用每个要素的几何（缓冲区/面范围）收集内部单字，
    // 与要素 NAMES 对应的地名进行合并。合并成功者 corrected=true，合并后坐标为组成单字中心。
    // 仅处理非敏感单字(text.length()==1 && !sensitive)。返回合并产生的新注记数。
    // 从 gdb 图层加载要素几何（图像坐标）和名称，按几何类型分类返回
    // lineFeatures: 每个要素 {折线点序列(图像坐标), 名称}
    // polyFeatures: 每个要素 {多边形外环点序列(图像坐标), 名称}
    struct LayerFeature { std::vector<cv::Point2f> geom; QString name; bool consumed = false; };
    int mergeSinglesByLayerGeometry(std::vector<TextResult>& results);
    // 多通道版: 接收已加载要素, 匹配成功的要素在 LayerFeature.consumed 标记,
    // 后续通道跳过已消费要素, 避免不同通道对同一要素重复合并
    // phaseFilter: 0=两轮合并(默认), 1=仅精确(不缺字), 2=仅模糊(允许缺字)
    int mergeSinglesByLayerGeometryCh(std::vector<TextResult>& results,
                                      QList<LayerFeature>& lineFeats,
                                      QList<LayerFeature>& polyFeats,
                                      int phaseFilter = 0);
    bool loadLayerFeaturesForMerge(QList<LayerFeature>& lineFeatures,
                                   QList<LayerFeature>& polyFeatures) const;
    // 对单字集合尝试与一个地名合并：长名优先、任意顺序、相邻字距≤阈值
    // indices: 候选单字在 singles 中的下标；返回合并后的新 TextResult，未合并返回空
    // matchedIdx: 合并成功时记录被消耗的单字下标
    bool tryMergeSinglesForName(const std::vector<TextResult>& singles,
                                const QString& name,
                                double maxGap,
                                std::vector<int>& matchedIdx,
                                TextResult& merged,
                                const QString& dbgTag = QString(),
                                int maxSkipOverride = -1) const;
    // 对未匹配的 bbox 区域放大裁剪后用不同参数重新识别，返回新结果
    std::vector<TextResult> reRecognizeUnmatched(const cv::Mat& img,
                                                  const std::vector<TextResult>& unmatched) const;
    // PaddleOCR 相关函数
    // roi: 可选的多个多边形 ROI（原图坐标，像素），为空则全图识别
    // useColorChannels: true=按 HSV 分通道识别（慢，可识别淡色文字）；false=原图单次识别（快）
    // debugDir: 非空时将各颜色通道的掩码/OCR输入图/识别结果标注图保存到该目录
    std::vector<TextResult> paddleOcrImage(const QString& imgPath,
                                           const std::vector<std::vector<cv::Point>>& roi = {},
                                           bool useColorChannels = true,
                                           const QString& debugDir = QString(),
                                           std::vector<std::pair<QString, std::vector<TextResult>>>* channelRawResults = nullptr,
                                           bool* ok = nullptr);

    // 从 gdb 获取子图层信息（名称+几何类型），返回是否成功
    bool listGdbSubLayers(const QString& gdbPath, QStringList& layerNames, QStringList& layerTypes);
    // 从 gdb 加载境界线图层，填充 m_boundaryFeatures，返回要素数量
    int loadBoundaryLayer(const QString& gdbPath, int layerId);
    // 将 gdb 境界线（地理坐标）用仿射变换逆变换到图片坐标系
    std::vector<std::vector<cv::Point2f>> transformBoundaryToImage() const;
    // 保存/加载地理配准结果到图片同目录的 .georef.json
    void saveGeorefCache() const;
    bool loadGeorefCache();

    QgisInterface* mIface = nullptr;

    // 控件
    QLineEdit*   m_editImage       = nullptr;
    QPushButton* m_btnImage        = nullptr;
    QPushButton* m_btnExtractText  = nullptr;  // 提取注记按钮
    QLabel*      m_lblTolerance    = nullptr;
    QSlider*     m_sliderTolerance = nullptr;
    QPushButton* m_btnPickHaloColor = nullptr; // 在图片上取内晕线颜色
    QPushButton* m_btnHaloColor     = nullptr; // 内晕线颜色显示/选择按钮
    QLineEdit*   m_editHaloColor    = nullptr; // 内晕线颜色代码输入框
    QPushButton* m_btnPickOuterHaloColor = nullptr; // 在图片上取外晕线颜色
    QPushButton* m_btnOuterHaloColor     = nullptr; // 外晕线颜色显示/选择按钮
    QLineEdit*   m_editOuterHaloColor    = nullptr; // 外晕线颜色代码输入框
    QPushButton* m_btnExtractColor = nullptr;  // 按颜色提取
    QPushButton* m_btnExportReport = nullptr;  // 输出质检文件
    // 输出路径设置控件
    QLineEdit*   m_editOutputDir   = nullptr;  // 输出目录显示框
    QPushButton* m_btnBrowseOutput = nullptr;  // 浏览选择输出目录
    QString      m_outputDir;                   // 用户指定的输出目录（为空则使用图片同目录默认规则）
    QPushButton* m_btnAccept       = nullptr;
    QPushButton* m_btnOneClickCheck = nullptr; // 一键质检按钮
    QProgressBar* m_progressBar    = nullptr;  // 一键质检进度条
    QLabel*       m_progressLabel  = nullptr;  // 进度条下方文字标签

    // 地名库控件
    QPushButton* m_btnSelectGazetteerLayers = nullptr; // 从 gdb 选择图层构建地名库
    QLabel*      m_lblGazetteer     = nullptr; // 显示地名库加载状态

    // XML 质检规则控件
    QPushButton* m_btnLoadXmlConfig = nullptr; // 加载 XML 质检规则文件
    QLabel*      m_lblXmlConfig     = nullptr; // 显示 XML 规则文件加载状态

    // gdb 加载控件
    QPushButton* m_btnLoadGdb   = nullptr; // 加载 gdb 数据源
    QLabel*      m_lblGdbPath   = nullptr; // 显示 gdb 路径
    QPushButton* m_btnSelectBoundaryLayer = nullptr; // 从 gdb 选择境界线图层
    QLabel*      m_lblBoundaryLayer = nullptr; // 显示境界线图层选择状态
    QPushButton* m_btnGeoref    = nullptr; // 打开地理配准窗口
    QLabel*      m_lblGeoref    = nullptr; // 显示地理配准状态（控制点数量等）

    // 境界线异常检测控件（仅状态标签，检测在提取后自动执行）
    QLabel*      m_lblAnomalyStatus = nullptr; // 异常检测结果状态

    // OCR 通道识别控件
    QCheckBox*   m_cbColorChannel  = nullptr; // 分颜色通道识别开关

    // 预览标签（原图 + 境界线叠加显示，单画面）
    QLabel* m_lblOriginal = nullptr;
    // 预览滚动区域（支持缩放后拖拽平移）
    QScrollArea* m_scrollOriginal = nullptr;
    // 预览缩放控件
    QPushButton* m_btnZoomIn     = nullptr;  // 放大
    QPushButton* m_btnZoomOut    = nullptr;  // 缩小
    QPushButton* m_btnZoomReset  = nullptr;  // 1:1 原始大小
    QPushButton* m_btnZoomFit    = nullptr;  // 适应窗口
    QLabel*      m_lblZoomStatus = nullptr;  // 显示当前缩放比例
    // 预览原始图像缓存（用于缩放时按比例重新渲染）
    QImage m_origImg;
    double m_previewScale = 1.0;  // 预览缩放比例（1.0 = 原始像素大小）
    // 鼠标拖拽平移状态（用于滚轮缩放后的大图查看）
    QLabel*  m_dragLabel  = nullptr;  // 当前正在拖拽的预览标签（nullptr 表示未拖拽）
    QPoint   m_dragLastPos;           // 上次鼠标全局位置（用于计算位移）
    // 未匹配/纠错项表格
    QTableWidget* m_tblText = nullptr;

    QString m_imagePath;   // 当前待处理图片路径
    QColor  m_currentColor; // 当前选中境界线颜色（RGB）
    QColor  m_currentHaloColor; // 当前选中内晕线颜色（RGB）
    QColor  m_currentOuterHaloColor; // 当前选中外晕线颜色（RGB）

    // 缓存上一次提取的结果，供 onAccept() 使用
    cv::Mat m_lastImg;
    std::vector<std::vector<cv::Point2f>> m_lastContours;
    QString m_lastLayerName;
    bool m_hasResult = false;

    // 质检日志缓冲区：在各操作发生时实时记录（带真实时间戳），onExportReport 时统一写文件
    QStringList m_checkLog;
    void addCheckLog(const QString& msg); // 记录一条带时间戳的日志

    // 境界线异常检测结果缓存（提取后自动检测生成，onAccept 时据此分层加入 QGIS）
    BoundaryAnomalyResult m_lastAnomalyResult;
    bool m_hasAnomalyResult = false;
    double m_lastBufferRadius = 0.0;
    bool m_compensationDone = false;  // 境界线是否进行过截断修复（标准境界线引导补偿）

    // 一键质检模式标志：为 true 时抑制各步骤的完成弹窗，由 onOneClickCheck 统一汇总
    bool m_oneClickMode = false;
    // 调试图层输出开关：开启时输出各通道识别结果图层、线要素缓冲区等调试图层
    bool m_debugLayers = false;
    // 质检起止时间（用于 JSON 报告的 timing 字段）
    QDateTime m_checkStartTime;
    QDateTime m_checkEndTime;

    // OCR 文字提取结果（质检时提取，供境界线提取时遮罩、加入 QGIS 时使用）
    std::vector<TextResult> m_textResults;
    // 原始OCR识别结果(未经任何合并/匹配处理, 用于输出"识别结果"图层)
    std::vector<TextResult> m_rawOcrResults;
    // 多通道模式下各通道的原始识别结果(用于输出各通道"识别结果"图层)
    std::vector<std::pair<QString, std::vector<TextResult>>> m_channelRawResults;

    // 标准地名库：加载后用于匹配验证与纠错
    QSet<QString> m_gazetteer;      // 完整地名集合（精确匹配）
    QStringList   m_gazetteerList;  // 完整地名列表（模糊匹配用，按长度排序）
    QList<int>    m_gazetteerLayerIds; // 构建地名库的 gdb 子图层 ID 列表

    // 带几何信息的地名条目（用于空间缓冲区限定匹配）
    // 每个地名可能来自多个要素，geom 为源要素几何在图像像素坐标系下的点序列
    struct GazetteerEntry {
        QString name;
        std::vector<cv::Point2f> geom;  // 源要素几何(像素坐标)
        int geomType;                   // 0=面, 1=线, 2=点
    };
    QList<GazetteerEntry> m_gazetteerEntries;  // 带几何的地名条目
    // 加载带几何信息的地名库(需 m_affineValid)，在仿射变换后、匹配前调用
    void loadGazetteerWithGeometry();
    // 检查像素点是否在某地名的源要素缓冲区内(仅面/线要素)
    bool isWithinGazetteerBuffer(const cv::Point2f& pt, const QString& name, double bufferPx) const;
    // 计算像素点到某地名源要素的最近距离(仅点要素, 用于点要素距离阈值检测)
    // 返回 < 0 表示该地名无点要素几何(不限制)
    double pointDistanceToGazetteer(const cv::Point2f& pt, const QString& name) const;

    // 敏感文字词表：从 XML 质检规则文件加载，按分类组织
    // 结构: { 分类名 -> 敏感词列表 }，如 { "安全" -> ["公安","特警",...] }
    QList<QPair<QString, QStringList>> m_sensitiveWords;
    QString m_sensitiveRulesPath;   // 当前加载的敏感词规则文件路径
    int     m_sensitiveHitCount = 0; // 本次注记提取中命中敏感词的注记数（用于日志/JSON报告）

    // 对指定结果集执行敏感文字合规性检查，返回命中数量
    // 命中者标记 tr.sensitive=true 并记录分类和命中词，不参与后续地名匹配
    int checkSensitiveWordsInResults(std::vector<TextResult>& results);
    // 对所有 OCR 提取文本执行敏感文字合规性检查
    // 命中者标记 tr.sensitive=true 并记录分类和命中词，不参与后续地名匹配
    void checkSensitiveWords();
    // 从 XML 文件加载敏感词词表（质检项: 敏感文字合规性检查，id=104）
    bool loadSensitiveWordsFromXml(const QString& xmlPath);

    // ===== XML 质检规则配置 =====
    // 系统启动时自动加载 XML 文件中的所有参数（境界线/异常检测/OCR/敏感词/地名匹配）
    // 路径持久化于 QgsSettings（key: SE_IMAGE_MAP_CHECK/XmlConfigPath），参考 xml_editor 模块做法
    XmlConfigReader* m_configReader = nullptr;  // XML 配置读取器（QObject 子对象，parent=this）
    QString          m_xmlConfigPath;           // 当前加载的 XML 质检规则文件路径
    // 加载 XML 中所有质检项参数到内存（同时联动加载敏感词表），返回是否成功
    bool loadAllConfigFromXml(const QString& xmlPath);

    // ===== 地理配准数据 =====
    QString m_gdbPath;              // 当前加载的 gdb 路径
    int     m_boundaryLayerId = -1; // 境界线子图层 ID
    QString m_boundaryLayerName;    // 境界线子图层名
    // gdb 境界线要素（地理坐标，CV_64F 双精度以便变换）
    std::vector<std::vector<cv::Point2d>> m_boundaryFeatures;
    // 境界线地理范围
    cv::Rect2d m_boundaryExtent;
    bool m_boundaryLoaded = false;
    QgsCoordinateReferenceSystem m_boundaryCrs;

    cv::Mat m_affine;               // 仿射变换矩阵 2x3 CV_64F：img = M * geo
    bool    m_affineValid = false;  // 仿射变换是否已计算

    // 控制点（配准结果，由 GeoreferenceDialog 返回）
    std::vector<cv::Point2f> m_gcpImgPts;
    std::vector<cv::Point2d> m_gcpGeoPts;

    // 地理配准窗口指针（每次打开前重建，关闭后取回结果）
    GeoreferenceDialog* m_georefDialog = nullptr;
};

#endif // SE_IMAGE_MAP_CHECK_H
