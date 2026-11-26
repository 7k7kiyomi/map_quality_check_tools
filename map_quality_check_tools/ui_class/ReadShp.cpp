#include "ReadShp.h"
#include "line_topology_issue_aggregation.h"
#include "road_topology_network.h"
#include "water_reference_chain.h"
#include "water_result_classification.h"
struct WaterMatchCacheEntry {
    WaterResultClassification::Match match;
    WaterReferenceChain::Result chain;
};
#include <QApplication>
#include <QStringList>
#include <QStyleFactory>
#include <qgsapplication.h>
#include <qgsproviderregistry.h>
#include <qgssinglesymbolrenderer.h>
#include <qgssymbol.h>
#include <qgsmarkersymbol.h>
#include <qgslinesymbol.h>
#include <qgsfillsymbol.h>
#include <qgsrectangle.h>
#include <qgscoordinatetransform.h>
#include <qgscoordinatereferencesystem.h>
#include <QFileInfo>
#include <QTextStream>
#include <QStandardPaths>
#include <QDir>
#include <QGridLayout>
#include <qgsvectorfilewriter.h>  
#include <qgsfields.h>            
#include <QgsWkbTypes.h>  
#include <qgsgeometry.h>
#include <qgslinestring.h>
#include <qgspolygon.h>
#include <qgsmultipolygon.h>
#include <queue>  
#include <algorithm>  
#include <QSet>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDirIterator>
#include <qgsproject.h>
#include <qgsspatialindex.h>

namespace {
// Compare linework with polygon rings, never with the filled polygon interior.
QgsGeometry rightAngleBoundary(const QgsGeometry& geometry)
{
    if (geometry.isNull() || geometry.isEmpty()) return QgsGeometry();
    if (geometry.type() == QgsWkbTypes::PolygonGeometry)
        return QgsGeometry(geometry.constGet()->boundary());
    if (geometry.type() == QgsWkbTypes::LineGeometry) return geometry;
    return QgsGeometry();
}

double rightAngleDirectionTolerance(double configured)
{
    // Keep an effective straight edge distinct from a genuine right-angle turn.
    return std::isfinite(configured) ? std::max(0.0, std::min(45.0, configured)) : 0.0;
}

struct RightAngleArm {
    int endIndex = -1;
    double length = 0.0;
};

RightAngleArm traceRightAngleArm(const QgsLineString* line, int cornerIndex, int step, double tolerance)
{
    RightAngleArm arm;
    if (!line || (step != -1 && step != 1)) return arm;
    const bool closed = line->isClosed();
    const int count = line->numPoints() - (closed ? 1 : 0);
    if (count < 2 || cornerIndex < 0 || cornerIndex >= count) return arm;
    const double minCosine = std::cos(rightAngleDirectionTolerance(tolerance) * M_PI / 180.0);
    QgsPointXY previous(line->pointN(cornerIndex));
    double initialX = 0.0, initialY = 0.0, lastX = 0.0, lastY = 0.0;
    int index = cornerIndex;
    for (int visited = 0; visited < count - 1; ++visited) {
        int next = index + step;
        if (closed) next = (next + count) % count;
        else if (next < 0 || next >= count) break;
        const QgsPointXY point(line->pointN(next));
        const double length = previous.distance(point);
        index = next;
        if (length <= 1e-8) continue;
        const double dx = (point.x() - previous.x()) / length;
        const double dy = (point.y() - previous.y()) / length;
        if (arm.endIndex >= 0 &&
            (dx * initialX + dy * initialY < minCosine - 1e-12 ||
             dx * lastX + dy * lastY < minCosine - 1e-12)) break;
        if (arm.endIndex < 0) { initialX = dx; initialY = dy; }
        arm.endIndex = next;
        arm.length += length;
        lastX = dx;
        lastY = dy;
        previous = point;
    }
    return arm;
}

QgsGeometry rightAngleArmGeometry(const QgsLineString* line, int cornerIndex, int endIndex, int step)
{
    if (!line || endIndex < 0) return QgsGeometry();
    const bool closed = line->isClosed();
    const int count = line->numPoints() - (closed ? 1 : 0);
    QgsPolylineXY points { QgsPointXY(line->pointN(cornerIndex)) };
    int index = cornerIndex;
    for (int visited = 0; visited < count - 1 && index != endIndex; ++visited) {
        int next = index + step;
        if (closed) next = (next + count) % count;
        else if (next < 0 || next >= count) return QgsGeometry();
        points.push_back(QgsPointXY(line->pointN(next)));
        index = next;
    }
    return index == endIndex && points.size() >= 2 ? QgsGeometry::fromPolylineXY(points) : QgsGeometry();
}
}

int mergeDuplicateLineTopologyErrorRecords(std::vector<QualityCheckResult>& results)
{
    using LineTopologyIssueAggregation::Issue;
    using LineTopologyIssueAggregation::IssueGroup;

    std::vector<Issue> issues;
    std::vector<std::size_t> resultIndices;
    std::vector<bool> isLineTopologyIssue(results.size(), false);
    issues.reserve(results.size());
    resultIndices.reserve(results.size());

    for (std::size_t i = 0; i < results.size(); ++i) {
        const QualityCheckResult& result = results[i];
        const bool isLineIssue = result.checkType == CheckType::LINE_TOPOLOGY_ERROR
            || result.checkType == CheckType::NETWORK_CONNECTIVITY_ERROR;
        if (!isLineIssue || result.passed) continue;

        isLineTopologyIssue[i] = true;
        qint64 featureId = result.featureId;
        if (featureId < 0 && result.feature.isValid()) featureId = result.feature.id();
        QString sourceKey = result.sourceFile.trimmed();
        if (!sourceKey.isEmpty()) sourceKey = QDir::cleanPath(sourceKey).toCaseFolded();
        if (result.checkType == CheckType::NETWORK_CONNECTIVITY_ERROR &&
            result.description.contains(QStringLiteral("成果接点")))
            sourceKey += QStringLiteral("|网络接点|") + result.description;
        issues.push_back({
            sourceKey.toUtf8().toStdString(),
            static_cast<std::int64_t>(featureId),
            result.errorType.toUtf8().toStdString(),
            result.description.toUtf8().toStdString()
        });
        resultIndices.push_back(i);
    }

    const std::vector<IssueGroup> groups =
        LineTopologyIssueAggregation::groupBySourceFeature(issues);
    std::map<std::size_t, const IssueGroup*> groupsByFirstResultIndex;
    std::vector<bool> keepResult(results.size(), true);
    int removedCount = 0;

    for (const IssueGroup& group : groups) {
        if (group.memberIndices.empty()) continue;
        const std::size_t firstResultIndex = resultIndices[group.memberIndices.front()];
        groupsByFirstResultIndex[firstResultIndex] = &group;
        for (std::size_t member = 1; member < group.memberIndices.size(); ++member) {
            const std::size_t duplicateResultIndex = resultIndices[group.memberIndices[member]];
            keepResult[duplicateResultIndex] = false;
            ++removedCount;
        }
    }

    std::vector<QualityCheckResult> mergedResults;
    mergedResults.reserve(results.size() - static_cast<std::size_t>(removedCount));
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (!keepResult[i]) continue;

        QualityCheckResult merged = results[i];
        const auto groupIt = groupsByFirstResultIndex.find(i);
        if (isLineTopologyIssue[i] && groupIt != groupsByFirstResultIndex.end()) {
            const IssueGroup& group = *groupIt->second;
            QStringList errorTypes;
            QStringList descriptions;
            for (const std::string& type : group.errorTypes)
                errorTypes.push_back(QString::fromUtf8(type.c_str()));
            for (const std::string& description : group.descriptions)
                descriptions.push_back(QString::fromUtf8(description.c_str()));

            // A wholly isolated river with both ends dangling is one issue;
            // keep the endpoint details in its description.
            if (errorTypes.contains(QStringLiteral("河流独立")) &&
                descriptions.join(QString()).contains(QStringLiteral("起点和终点都悬挂")))
                errorTypes.removeAll(QStringLiteral("河流悬挂"));

            if (!errorTypes.isEmpty()) merged.errorType = errorTypes.join(QStringLiteral("；"));
            if (!descriptions.isEmpty()) merged.description = descriptions.join(QStringLiteral("；"));

            bool conflictingReferenceCategories = false;
            for (std::size_t member = 1; member < group.memberIndices.size(); ++member) {
                const QualityCheckResult& additional =
                    results[resultIndices[group.memberIndices[member]]];
                if (merged.errorGeometry.isNull() && !additional.errorGeometry.isNull())
                    merged.errorGeometry = additional.errorGeometry;
                if (merged.geometry.isNull() && !additional.geometry.isNull())
                    merged.geometry = additional.geometry;
                if (!merged.feature.isValid() && additional.feature.isValid())
                    merged.feature = additional.feature;
                if (!additional.referenceCategory.isEmpty() && !conflictingReferenceCategories) {
                    if (merged.referenceCategory.isEmpty()) {
                        merged.referenceCategory = additional.referenceCategory;
                        merged.referenceValue = additional.referenceValue;
                        merged.referenceSource = additional.referenceSource;
                        merged.referenceFeatureId = additional.referenceFeatureId;
                    } else if (merged.referenceCategory != additional.referenceCategory) {
                        merged.referenceCategory.clear();
                        merged.referenceValue.clear();
                        merged.referenceFeatureId = -1;
                        conflictingReferenceCategories = true;
                    }
                }
            }
        }
        mergedResults.push_back(std::move(merged));
    }

    results.swap(mergedResults);
    return removedCount;
}

ReadShpDialog::ReadShpDialog(QWidget* parent, Qt::WindowFlags flags)
    : QDialog(parent, flags),
    currentLayer(nullptr),
    referenceLayer(nullptr),
    mapCanvas(nullptr),
    m_config(new QualityCheckConfig(this)),
    errorLayer(nullptr),
    currentFileLabel(nullptr),
    referenceFileLabel(nullptr)       //置空，避免未初始化指针
{
    setWindowTitle("错漏检查");
    setMinimumSize(1600, 1000);
    resize(1800, 1200);
    // 创建主布局
    mainLayout = new QVBoxLayout(this);
    // 标准图层（CSV）选择区
    standardLayerGroupBox = new QGroupBox("标准图层（CSV）", this);
    QHBoxLayout* standardLayerLayout = new QHBoxLayout(standardLayerGroupBox);
    selectCsvButton = new QPushButton("选择标准图层CSV", this);
    csvPathLabel = new QLabel("未选择CSV", this);
    standardLayerCombo = new QComboBox(this);
    standardLayerCombo->setEnabled(false);
    standardLayerCombo->setPlaceholderText("请选择标准图层");
    standardLayerLayout->addWidget(selectCsvButton);
    standardLayerLayout->addWidget(csvPathLabel);
    standardLayerLayout->addWidget(new QLabel("标准图层:"));
    standardLayerLayout->addWidget(standardLayerCombo);
    standardLayerLayout->addStretch();
    mainLayout->addWidget(standardLayerGroupBox);

    // 信号连接
    connect(selectCsvButton, &QPushButton::clicked, this, &ReadShpDialog::selectStandardCsvFile);
    connect(standardLayerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ReadShpDialog::onStandardLayerChanged);
    // 创建顶部布局（用于配置文件选择）
    topLayout = new QHBoxLayout();
    selectConfigButton = new QPushButton("选择配置文件", this);
    configPathLabel = new QLabel("未选择配置文件", this);
    configPathLabel->setStyleSheet("QLabel { color: gray; }");
    // 新增：比例尺输入框（保留创建，但不加入 topLayout）
    scaleLabel = new QLabel("比例尺:", this);
    scaleComboBox = new QComboBox(this);
    scaleComboBox->setEditable(true);
    scaleComboBox->setPlaceholderText("请选择或输入比例尺");
    scaleComboBox->setFixedWidth(120);
    connect(scaleComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ReadShpDialog::onScaleSelectionChanged);
    connect(scaleComboBox, &QComboBox::editTextChanged, this, &ReadShpDialog::onScaleTextChanged);

    // 添加到布局中
    topLayout->addWidget(selectConfigButton);
    topLayout->addWidget(configPathLabel);
    topLayout->addStretch();
    mainLayout->addLayout(topLayout);
    // 把比例尺放在“配置XML”下一行
    QHBoxLayout* scaleLayout = new QHBoxLayout();
    scaleLayout->addWidget(scaleLabel);
    scaleLayout->addWidget(scaleComboBox);
    scaleLayout->addStretch();
    mainLayout->addLayout(scaleLayout);
    // 连接信号
    connect(selectConfigButton, &QPushButton::clicked, this, &ReadShpDialog::selectConfigFile);
    // 创建批量文件管理组
    batchFilesGroupBox = new QGroupBox("批量文件管理", this);
    batchFilesGroupBox->setStyleSheet("QGroupBox { font-size: 11pt; font-weight: bold; }");
    QHBoxLayout* batchFilesLayout = new QHBoxLayout(batchFilesGroupBox);
    // 解析文件组
    parseFilesGroupBox = new QGroupBox("解析SHP文件", this);
    parseFilesGroupBox->setStyleSheet("QGroupBox { font-size: 10pt; }");
    QVBoxLayout* parseFilesLayout = new QVBoxLayout(parseFilesGroupBox);
    parseFilesList = new QListWidget(this);
    parseFilesList->setSelectionMode(QAbstractItemView::SingleSelection);
    parseFilesList->setStyleSheet("QListWidget { font-size: 9pt; }");
    QHBoxLayout* parseButtonsLayout = new QHBoxLayout();
    addParseFilesButton = new QPushButton("添加文件", this);
    removeSelectedParseFileButton = new QPushButton("移除选中", this);
    clearAllParseFilesButton = new QPushButton("清空全部", this);
    parseFilesCountLabel = new QLabel("文件数量: 0", this);
    parseButtonsLayout->addWidget(addParseFilesButton);
    parseButtonsLayout->addWidget(removeSelectedParseFileButton);
    parseButtonsLayout->addWidget(clearAllParseFilesButton);
    parseFilesLayout->addWidget(parseFilesList);
    parseFilesLayout->addLayout(parseButtonsLayout);
    parseFilesLayout->addWidget(parseFilesCountLabel);
    // 库体文件组
    referenceFilesGroupBox = new QGroupBox("库体SHP文件", this);
    referenceFilesGroupBox->setStyleSheet("QGroupBox { font-size: 10pt; }");
    QVBoxLayout* referenceFilesLayout = new QVBoxLayout(referenceFilesGroupBox);
    referenceFilesList = new QListWidget(this);
    referenceFilesList->setSelectionMode(QAbstractItemView::SingleSelection);
    referenceFilesList->setStyleSheet("QListWidget { font-size: 9pt; }");
    QHBoxLayout* referenceButtonsLayout = new QHBoxLayout();
    addReferenceFilesButton = new QPushButton("添加文件", this);
    removeSelectedReferenceFileButton = new QPushButton("移除选中", this);
    clearAllReferenceFilesButton = new QPushButton("清空全部", this);
    referenceFilesCountLabel = new QLabel("文件数量: 0", this);
    referenceButtonsLayout->addWidget(addReferenceFilesButton);
    referenceButtonsLayout->addWidget(removeSelectedReferenceFileButton);
    referenceButtonsLayout->addWidget(clearAllReferenceFilesButton);
    referenceFilesLayout->addWidget(referenceFilesList);
    referenceFilesLayout->addLayout(referenceButtonsLayout);
    referenceFilesLayout->addWidget(referenceFilesCountLabel);
    // 添加到批量文件管理布局
    batchFilesLayout->addWidget(parseFilesGroupBox);
    batchFilesLayout->addWidget(referenceFilesGroupBox);
    mainLayout->addWidget(batchFilesGroupBox);
    // 连接批量文件管理信号
    connect(addParseFilesButton, &QPushButton::clicked, this, &ReadShpDialog::addParseFiles);
    connect(addReferenceFilesButton, &QPushButton::clicked, this, &ReadShpDialog::addReferenceFiles);
    connect(removeSelectedParseFileButton, &QPushButton::clicked, this, &ReadShpDialog::removeSelectedParseFile);
    connect(removeSelectedReferenceFileButton, &QPushButton::clicked, this, &ReadShpDialog::removeSelectedReferenceFile);
    connect(clearAllParseFilesButton, &QPushButton::clicked, this, &ReadShpDialog::clearAllParseFiles);
    connect(clearAllReferenceFilesButton, &QPushButton::clicked, this, &ReadShpDialog::clearAllReferenceFiles);
    // 创建按钮布局
    buttonLayout = new QHBoxLayout();
    selectFileButton = new QPushButton("选择单个解析SHP文件", this);
    selectReferenceFileButton = new QPushButton("选择单个库体SHP文件", this);
    performCheckButton = new QPushButton("执行错误检查", this);
    // 设置按钮样式和尺寸
    QString buttonStyle = "QPushButton { padding: 8px 16px; font-size: 10pt; min-height: 30px; }";
    selectFileButton->setStyleSheet(buttonStyle);
    selectReferenceFileButton->setStyleSheet(buttonStyle + " QPushButton { background-color: #e8f4fd; }");
    performCheckButton->setStyleSheet(buttonStyle);
    selectConfigButton->setStyleSheet(buttonStyle);
    // 输出格式选择
    outputFormatCombo = new QComboBox(this);
    outputFormatCombo->addItem("txt");
    outputFormatCombo->addItem("xlsx");
    outputFormatCombo->setStyleSheet("QComboBox { padding: 5px; font-size: 10pt; min-height: 30px; }");
    outputButton = new QPushButton("输出到文件", this);
    outputButton->setStyleSheet(buttonStyle);
    // 新增：创建并加入“解析文件/库体文件”标签，避免 setText 对空指针
    currentFileLabel = new QLabel("解析文件: 未选择", this);
    currentFileLabel->setStyleSheet("QLabel { color: gray; }");
    referenceFileLabel = new QLabel("库体文件: 未选择", this);
    referenceFileLabel->setStyleSheet("QLabel { color: gray; }");
    buttonLayout->addWidget(selectFileButton);
    buttonLayout->addWidget(selectReferenceFileButton);
    buttonLayout->addWidget(performCheckButton);
    buttonLayout->addWidget(currentFileLabel);      // 新增：加入布局
    buttonLayout->addWidget(referenceFileLabel);    // 新增：加入布局
    buttonLayout->addWidget(new QLabel("输出格式:"));
    buttonLayout->addWidget(outputFormatCombo);
    buttonLayout->addWidget(outputButton);
    buttonLayout->addStretch();
    mainLayout->addLayout(buttonLayout);
    // 输出格式选项
    outputFormatCombo->addItem("txt");
    outputFormatCombo->addItem("xlsx");
    outputFormatCombo->addItem("shp");
    outputFormatCombo->addItem("json");
    outputFormatCombo->addItem("log");
    // 创建质检选项组
    checkOptionsGroupBox = new QGroupBox("质检选项", this);
    checkOptionsGroupBox->setStyleSheet("QGroupBox { font-size: 11pt; font-weight: bold; }");
    QHBoxLayout* checkOptionsLayout = new QHBoxLayout(checkOptionsGroupBox);
    directionErrorCheckBox = new QCheckBox("走向错误检查", this);
    polygonOverlapGapCheckBox = new QCheckBox("面要素重叠/间隙检查", this);
    lineTopologyCheckBox = new QCheckBox("线要素拓扑检查", this);
    rightAngleCheckBox = new QCheckBox("直角化角度阈值控制检查", this);
    onlyErrorsCheckBox = new QCheckBox("仅显示错误", this);
    // 默认选中
    directionErrorCheckBox->setChecked(false);
    polygonOverlapGapCheckBox->setChecked(false);
    lineTopologyCheckBox->setChecked(false);
    rightAngleCheckBox->setChecked(false);
    onlyErrorsCheckBox->setChecked(false);
    checkOptionsLayout->addWidget(directionErrorCheckBox);
    checkOptionsLayout->addWidget(polygonOverlapGapCheckBox);
    checkOptionsLayout->addWidget(lineTopologyCheckBox);
    checkOptionsLayout->addWidget(rightAngleCheckBox);
    checkOptionsLayout->addWidget(onlyErrorsCheckBox);
    checkOptionsLayout->addStretch();
    mainLayout->addWidget(checkOptionsGroupBox);
    // 创建主分割器（水平分割）
    mainSplitter = new QSplitter(Qt::Horizontal, this);
    // 地图显示区域
    leftLayout = new QVBoxLayout();
    mapGroupBox = new QGroupBox("地图显示", this);
    mapGroupBox->setStyleSheet("QGroupBox { font-size: 11pt; font-weight: bold; }");
    mapLayout = new QVBoxLayout(mapGroupBox);
    // 创建普通日志文本框
    logTextEdit = new QTextEdit(this);
    logTextEdit->setStyleSheet(
        "QTextEdit { "
        "    font-family: 'Consolas', 'Monaco', monospace; "
        "    font-size: 9pt; "
        "    background-color: #2b2b2b; "
        "    color: #ffffff; "
        "    border: 1px solid #555; "
        "}"
    );
    logTextEdit->setMinimumHeight(300);
    // 创建XML调试日志文本框
    xmlDebugTextEdit = new QTextEdit(this);
    xmlDebugTextEdit->setStyleSheet(
        "QTextEdit { "
        "    font-family: 'Consolas', 'Monaco', monospace; "
        "    font-size: 9pt; "
        "    background-color: #1e3a5f; "
        "    color: #e6f3ff; "
        "    border: 1px solid #4a90e2; "
        "}"
    );
    xmlDebugTextEdit->setMinimumHeight(300);
    // 初始化地图画布
    initializeMapCanvas();
    mapLayout->addWidget(mapCanvas);
    mapWidget = new QWidget();
    mapWidget->setLayout(leftLayout);
    leftLayout->addWidget(mapGroupBox);
    // 表格和日志
    rightLayout = new QVBoxLayout();
    // 表格组
    tableGroupBox = new QGroupBox("检查结果", this);
    tableGroupBox->setStyleSheet("QGroupBox { font-size: 11pt; font-weight: bold; }");
    QVBoxLayout* tableLayout = new QVBoxLayout(tableGroupBox);
    tableWidget = new QTableWidget(this);
    tableWidget->setStyleSheet(
        "QTableWidget { "
        "    gridline-color: #ddd; "
        "    font-size: 9pt; "
        "    selection-background-color: #3daee9; "
        "}"
        "QHeaderView::section { "
        "    background-color: #f0f0f0; "
        "    padding: 4px; "
        "    border: 1px solid #ddd; "
        "    font-weight: bold; "
        "}"
    );
    tableLayout->addWidget(tableWidget);
    QWidget* rightWidget = new QWidget();
    rightWidget->setLayout(rightLayout);
    rightLayout->addWidget(tableGroupBox);
    // 日志选项卡
    logTabWidget = new QTabWidget(this);
    logTabWidget->addTab(logTextEdit, "运行日志");
    logTabWidget->addTab(xmlDebugTextEdit, "配置调试");
    logTabWidget->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #ddd; }"
        "QTabBar::tab { padding: 8px 12px; margin-right: 2px; }"
        "QTabBar::tab:selected { background-color: #3daee9; color: white; }"
    );
    // 日志组
    logGroupBox = new QGroupBox("日志信息", this);
    logGroupBox->setStyleSheet("QGroupBox { font-size: 11pt; font-weight: bold; }");
    QVBoxLayout* logLayout = new QVBoxLayout(logGroupBox);
    logLayout->addWidget(logTabWidget);
    rightLayout->addWidget(logGroupBox);
    // 添加到主分割器
    mainSplitter->addWidget(mapWidget);
    mainSplitter->addWidget(rightWidget);
    mainSplitter->setSizes({ 800, 600 });
    mainLayout->addWidget(mainSplitter);
    // 连接信号和槽
    connect(selectFileButton, &QPushButton::clicked, this, &ReadShpDialog::selectShpFile);
    connect(selectReferenceFileButton, &QPushButton::clicked, this, &ReadShpDialog::selectReferenceFile);
    connect(outputButton, &QPushButton::clicked, this, &ReadShpDialog::outputToFile);
    connect(performCheckButton, &QPushButton::clicked, this, &ReadShpDialog::performQualityCheck);

    // 连接"仅显示错误"复选框的信号
    connect(onlyErrorsCheckBox, &QCheckBox::toggled, this, &ReadShpDialog::onOnlyErrorsCheckBoxToggled);
    // 初始化日志
    logTextEdit->append("=== SHP文件走向错误检查工具 ===");
    logTextEdit->append("请先选择解析SHP文件和库体SHP文件");
}
ReadShpDialog::~ReadShpDialog()
{
    clearAllLayers();
    // 清理错误标记
    clearErrorMarkers();
    // 清理单个图层
    if (currentLayer) {
        delete currentLayer;
        currentLayer = nullptr;
    }
    if (referenceLayer) {
        delete referenceLayer;
        referenceLayer = nullptr;
    }
    // 清理批量图层
    for (auto layer : parseLayers) {
        if (layer) {
            delete layer;
        }
    }
    parseLayers.clear();
    for (auto layer : referenceLayers) {
        if (layer) {
            delete layer;
        }
    }
    referenceLayers.clear();
    // 清理地图画布
    if (mapCanvas) {
        mapCanvas->setParent(nullptr);
        delete mapCanvas;
        mapCanvas = nullptr;
    }
}

//选择标准图层csv
void ReadShpDialog::selectStandardCsvFile()
{
    // ... existing code ...
    QString fileName = QFileDialog::getOpenFileName(this, "选择标准图层CSV", "", "CSV Files (*.csv);;All Files (*.*)");
    if (fileName.isEmpty()) {
        return;
    }
    m_standardCsvFilePath = fileName;
    csvPathLabel->setText(QString("CSV: %1").arg(QFileInfo(fileName).fileName()));
    logTextEdit->append(QString("标准图层CSV已选择：%1").arg(fileName));

    if (!loadStandardLayersFromCsv(fileName)) {
        QMessageBox::warning(this, "错误", "解析CSV失败，请检查文件格式和列名。");
        return;
    }

    // 填充标准图层下拉框
    standardLayerCombo->clear();
    for (const auto& entry : m_standardLayers) {
        standardLayerCombo->addItem(entry.standardLayerName);
    }
    standardLayerCombo->setEnabled(!m_standardLayers.isEmpty());
    if (!m_standardLayers.isEmpty()) {
        logTextEdit->append(QString("已载入 %1 个标准图层条目").arg(m_standardLayers.size()));
    }
    // ... existing code ...
}
//实现 CSV 解析
bool ReadShpDialog::loadStandardLayersFromCsv(const QString& csvPath)
{
    QFile file(csvPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        logTextEdit->append(QString("无法打开CSV文件：%1").arg(csvPath));
        return false;
    }

    QTextStream in(&file);
#if QT_VERSION < QT_VERSION_CHECK(6,0,0)
    // 兼容 UTF-8 保存；Qt6 已移除 setCodec
    in.setCodec("UTF-8");
#endif

    if (in.atEnd()) {
        logTextEdit->append("CSV为空");
        return false;
    }

    QString headerLine = in.readLine();
    // 去除 UTF-8 BOM
    if (!headerLine.isEmpty() && headerLine.at(0) == QChar(0xFEFF)) {
        headerLine.remove(0, 1);
    }

    // 分隔符检测：逗号/制表符/分号/中文逗号
    QChar delim = ',';
    if (headerLine.contains('\t')) delim = '\t';
    else if (headerLine.contains(';')) delim = ';';
    else if (headerLine.contains(QChar(0xFF0C))) delim = QChar(0xFF0C); // 全角逗号“，”

    auto normalize = [](QString s) {
        if (!s.isEmpty() && s.at(0) == QChar(0xFEFF)) s.remove(0, 1); // 去 BOM
        s = s.trimmed();
        // 去掉整行被双引号包起来的情况
        if (s.startsWith('"') && s.endsWith('"') && s.size() >= 2) {
            s = s.mid(1, s.size() - 2);
        }
        // 统一路径分隔符（不再替换括号，保持CSV里的原始路径）
        s = QDir::toNativeSeparators(s);
        return s;
    };
    // 简单 CSV 拆分（支持引号包裹，避免误切分）
    auto splitRow = [&](const QString& line) -> QStringList {
        QStringList parts;
        QString cur;
        bool inQuotes = false;
        for (QChar ch : line) {
            if (ch == '"') { inQuotes = !inQuotes; continue; }
            if (ch == delim && !inQuotes) { parts << normalize(cur); cur.clear(); }
            else { cur.append(ch); }
        }
        parts << normalize(cur);
        return parts;
    };

    const QStringList headers = splitRow(headerLine);

    auto idxByCandidates = [&](const QStringList& cands) -> int {
        for (int i = 0; i < headers.size(); ++i) {
            const QString h = normalize(headers[i]);
            for (const QString& c : cands) {
                if (h.compare(normalize(c), Qt::CaseInsensitive) == 0)
                    return i;
            }
        }
        return -1;
    };

    // 必要列：标准图层 / gdb图层 / 实际图层
    const int iStd = idxByCandidates({ "标准图层" });
    const int iGdb = idxByCandidates({ "gdb图层", "库体图层", "库体SHP" });
    const int iAct = idxByCandidates({ "实际图层", "解析图层", "解析SHP" });
    // 可选列
    const int iActAnno = idxByCandidates({ "实际注记图层" });
    const int iStdAnno = idxByCandidates({ "对应注记层（标准图层）", "对应注记层" });
    const int iLabel = idxByCandidates({ "标注字段", "标注" });
    const int iSubAttr = idxByCandidates({ "图层分层属性" });

    if (iStd < 0 || iGdb < 0 || iAct < 0) {
        logTextEdit->append(
            QString("CSV标题识别失败，当前列：[%1]；必须包含：标准图层 / gdb图层 / 实际图层")
            .arg(headers.join(",")));
        return false;
    }

    QVector<StandardLayerEntry> entries;
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.trimmed().isEmpty()) continue;
        QString useLine = line;
        if (!useLine.isEmpty() && useLine.at(0) == QChar(0xFEFF)) useLine.remove(0, 1); // 行级BOM保护
        const QStringList cols = splitRow(useLine);
        const int needMax = std::max({ iStd, iGdb, iAct, iActAnno, iStdAnno, iLabel, iSubAttr });
        if (cols.size() <= needMax) continue;

        StandardLayerEntry e;
        e.standardLayerName = normalize(cols[iStd]);
        e.gdbShpPath = normalize(cols[iGdb]);
        e.actualShpPath = normalize(cols[iAct]);
        e.actualAnnoLayerPath = (iActAnno >= 0) ? normalize(cols[iActAnno]) : QString();
        e.standardAnnoLayerName = (iStdAnno >= 0) ? normalize(cols[iStdAnno]) : QString();
        e.labelField = (iLabel >= 0) ? normalize(cols[iLabel]) : QString();
        e.layerSubAttribute = (iSubAttr >= 0) ? normalize(cols[iSubAttr]) : QString();

        if (e.standardLayerName.isEmpty()) continue;
        if (e.gdbShpPath.isEmpty() || e.actualShpPath.isEmpty()) continue;

        entries.push_back(e);
    }

    file.close();
    m_standardLayers = entries;
    if (m_standardLayers.isEmpty()) {
        logTextEdit->append("CSV未解析到有效数据行（请检查路径列是否空或分隔符是否正确）。");
    }
    return !m_standardLayers.isEmpty();

}
//实现标准图层选择变化时加载对应图层
void ReadShpDialog::onStandardLayerChanged(int index)
{
    // ... existing code ...
    if (index < 0 || index >= m_standardLayers.size()) return;
    const StandardLayerEntry& entry = m_standardLayers[index];
    applyStandardLayerEntry(entry);
    // ... existing code ...
}
//实现应用标准图层条目（加载解析与库体图层）
void ReadShpDialog::applyStandardLayerEntry(const StandardLayerEntry& entry)
{
    // 仅按 CSV 的绝对路径加载（去掉两端引号、统一路径分隔符）
    auto sanitizePath = [](QString s) -> QString {
        s = s.trimmed();
        if (s.startsWith('"') && s.endsWith('"') && s.size() >= 2) {
            s = s.mid(1, s.size() - 2);
        }
        return QDir::toNativeSeparators(s);
    };
    const QString actualPath = sanitizePath(entry.actualShpPath);
    const QString gdbPath = sanitizePath(entry.gdbShpPath);

    auto isOgrDataSource = [](const QString& p) -> bool {
        QFileInfo fi(p);
        // 支持 .shp 文件
        if (fi.exists() && fi.suffix().compare("shp", Qt::CaseInsensitive) == 0) return true;
        // 支持 .gdb 数据源串（必须包含 layername）
        if (p.contains(".gdb", Qt::CaseInsensitive) && p.contains("|layername=", Qt::CaseInsensitive)) return true;
        return false;
    };

    if (!isOgrDataSource(actualPath)) {
        QMessageBox::warning(this, "解析路径无效", QString("仅支持 .shp 或 .gdb|layername=... 格式，当前：%1").arg(actualPath));
        return;
    }
    if (!isOgrDataSource(gdbPath)) {
        QMessageBox::warning(this, "库体路径无效", QString("仅支持 .shp 或 .gdb|layername=... 格式，当前：%1").arg(gdbPath));
        return;
    }

    // 切换前先安全移除画布中的旧图层，避免悬挂指针
    if (mapCanvas && !allDisplayLayers.isEmpty()) {
        allDisplayLayers.clear();
        mapCanvas->setLayers(allDisplayLayers);
        mapCanvas->refresh();
    }
    if (currentLayer) { delete currentLayer; currentLayer = nullptr; }
    if (referenceLayer) { delete referenceLayer; referenceLayer = nullptr; }

    auto mkLayerName = [](const QString& path) -> QString {
        if (path.contains("|layername=")) {
            int pos = path.indexOf("|layername=");
            return path.mid(pos + QString("|layername=").length());
        }
        return QFileInfo(path).completeBaseName();
    };

    // 构造新图层并校验
    QgsVectorLayer* parsed = new QgsVectorLayer(actualPath, mkLayerName(actualPath), "ogr");
    if (!parsed->isValid()) {
        QMessageBox::warning(this, "无法加载解析图层", QString("路径：%1").arg(actualPath));
        delete parsed;
        return;
    }
    QgsVectorLayer* reference = new QgsVectorLayer(gdbPath, mkLayerName(gdbPath), "ogr");
    if (!reference->isValid()) {
        QMessageBox::warning(this, "无法加载库体图层", QString("路径：%1").arg(gdbPath));
        delete reference;
        delete parsed;
        return;
    }

    // 设置当前成员（用于单对单检查）
    currentLayer = parsed;
    referenceLayer = reference;
    // 保留CSV提供的标准图层语义，后续道路网络检查据此识别库体道路类型。
    parsed->setCustomProperty(QStringLiteral("quality/standard_layer_name"), entry.standardLayerName);
    reference->setCustomProperty(QStringLiteral("quality/standard_layer_name"), entry.standardLayerName);
    currentFilePath = actualPath;
    referenceFilePath = gdbPath;

    if (currentFileLabel)   currentFileLabel->setText(QString("解析文件: %1").arg(QFileInfo(actualPath).fileName()));
    if (referenceFileLabel) referenceFileLabel->setText(QString("库体文件: %1").arg(QFileInfo(gdbPath).fileName()));

    if (!mapCanvas) initializeMapCanvas();

    // 设置样式与显示
    setLayerStyle(currentLayer, "yellow", 2.0);
    setLayerStyle(referenceLayer, "blue", 2.0);
    allDisplayLayers << currentLayer << referenceLayer;
    mapCanvas->setLayers(allDisplayLayers);

    // 合并范围缩放
    QgsRectangle combinedExtent;
    bool first = true;
    for (QgsMapLayer* lyr : allDisplayLayers) {
        auto* vl = qobject_cast<QgsVectorLayer*>(lyr);
        if (vl && vl->isValid()) {
            if (first) { combinedExtent = vl->extent(); first = false; }
            else { combinedExtent.combineExtentWith(vl->extent()); }
        }
    }
    if (!first) { mapCanvas->setExtent(combinedExtent); }
    mapCanvas->refresh();

    // 加入解析/库体列表（去重并补齐索引；列表中的图层使用独立对象，避免与 currentLayer/referenceLayer 双重释放）
    auto addPathToList = [&](QStringList& paths, QListWidget* list, std::vector<QgsVectorLayer*>& layers, const QString& path) {
        int idx = paths.indexOf(path);
        if (idx < 0) {
            paths.append(path);
            if (list) list->addItem(QFileInfo(path).fileName());
            QgsVectorLayer* p = new QgsVectorLayer(path, mkLayerName(path), "ogr");
            if (p->isValid()) {
                layers.push_back(p);
                logTextEdit->append(QString("列表新增图层: %1").arg(path));
            }
            else {
                QMessageBox::warning(this, "加入列表失败", QString("无法加载: %1").arg(path));
                delete p;
            }
        }
        else {
            logTextEdit->append(QString("列表已包含: %1").arg(path));
            // 若 layers 缺位则补齐
            if (idx >= static_cast<int>(layers.size())) {
                layers.resize(paths.size(), nullptr);
            }
            if (!layers[idx]) {
                QgsVectorLayer* p = new QgsVectorLayer(path, mkLayerName(path), "ogr");
                if (p->isValid()) {
                    layers[idx] = p;
                }
                else {
                    delete p;
                }
            }
        }
    };

    addPathToList(parseFilePaths, parseFilesList, parseLayers, actualPath);
    addPathToList(referenceFilePaths, referenceFilesList, referenceLayers, gdbPath);
    updateFileCountLabels();

    logTextEdit->append(QString("标准图层[%1]已应用，解析: %2，库体: %3").arg(entry.standardLayerName).arg(currentFilePath).arg(referenceFilePath));

}
QStringList ReadShpDialog::candidateSearchDirs(bool isGdb) const
{
    // 以 CSV 所在目录为根（你的场景即 E:\GSZJPlugins）
    QString root = m_standardCsvFilePath.isEmpty()
        ? QDir::currentPath()
        : QFileInfo(m_standardCsvFilePath).absoluteDir().absolutePath();

    QStringList dirs;
    QDir rootDir(root);
    QFileInfoList subs = rootDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& fi : subs) {
        bool hasGdb = fi.fileName().contains("gdb", Qt::CaseInsensitive);
        if (isGdb ? hasGdb : !hasGdb) {
            dirs << fi.absoluteFilePath();            // 如：E:\GSZJPlugins\永登县_gdb 或 E:\GSZJPlugins\永登县
        }
    }
    dirs << root;                                     // 把根也纳入候选以防文件就在根下
    return dirs;
}

// 按名称/代号解析出实际可用的 .shp 路径
QString ReadShpDialog::resolveShpPath(const QString& token, bool isGdb)
{
    QString t = token.trimmed();

    // 1) 已是路径且存在
    if ((t.endsWith(".shp", Qt::CaseInsensitive) || t.contains('\\') || t.contains('/') || t.contains(':'))
        && QFile::exists(t)) {
        return QFileInfo(t).canonicalFilePath();
    }

    // 2) 在界面已添加的文件列表里按文件名匹配
    const QStringList& list = isGdb ? referenceFilePaths : parseFilePaths;
    for (const QString& p : list) {
        QFileInfo fi(p);
        QString base = fi.completeBaseName();
        if (base.compare(t, Qt::CaseInsensitive) == 0 || fi.fileName().contains(t, Qt::CaseInsensitive)) {
            return fi.canonicalFilePath();
        }
    }

    // 3) 在候选目录（根下的子目录）递归查找
    QStringList searchDirs = candidateSearchDirs(isGdb);
    for (const QString& dir : searchDirs) {
        // 递归（会覆盖 “永登县\永登县” 和 “永登县_gdb\永登县_gdb” 这种二级目录）
        QDirIterator it(dir, QStringList() << "*.shp", QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString p = it.next();
            QFileInfo fi(p);
            QString base = fi.completeBaseName();
            if (base.compare(t, Qt::CaseInsensitive) == 0 || fi.fileName().contains(t, Qt::CaseInsensitive)) {
                return fi.canonicalFilePath();
            }
        }
    }

    return QString();
}


//添加批量多个解析文件
void ReadShpDialog::addParseFiles()
{
    QStringList fileNames = QFileDialog::getOpenFileNames(this, "选择解析SHP文件", "", "Shapefile (*.shp)");
    if (fileNames.isEmpty()) {
        return;
    }
    // 添加文件到列表
    for (const QString& fileName : fileNames) {
        if (!parseFilePaths.contains(fileName)) {
            parseFilePaths.append(fileName);
            parseFilesList->addItem(QFileInfo(fileName).fileName());
            // 加载图层
            QgsVectorLayer* layer = new QgsVectorLayer(fileName, QFileInfo(fileName).baseName(), "ogr");
            if (layer->isValid()) {
                parseLayers.push_back(layer);
                logTextEdit->append(QString("解析文件加载成功: %1").arg(fileName));
                logTextEdit->append(QString("要素数量: %1").arg(layer->featureCount()));
            }
            else {
                QMessageBox::warning(this, "错误", QString("无法加载解析SHP文件: %1").arg(fileName));
                delete layer;
            }
        }
    }
    // 更新文件数量标签
    updateFileCountLabels();
}
//添加批量库体文件
void ReadShpDialog::addReferenceFiles()
{
    QStringList fileNames = QFileDialog::getOpenFileNames(this, "选择库体SHP文件", "", "Shapefile (*.shp)");
    if (fileNames.isEmpty()) {
        return;
    }
    // 添加文件到列表
    for (const QString& fileName : fileNames) {
        if (!referenceFilePaths.contains(fileName)) {
            referenceFilePaths.append(fileName);
            referenceFilesList->addItem(QFileInfo(fileName).fileName());
            // 加载图层
            QgsVectorLayer* layer = new QgsVectorLayer(fileName, QFileInfo(fileName).baseName(), "ogr");
            if (layer->isValid()) {
                referenceLayers.push_back(layer);
                logTextEdit->append(QString("库体文件加载成功: %1").arg(fileName));
                logTextEdit->append(QString("库体要素数量: %1").arg(layer->featureCount()));
            }
            else {
                QMessageBox::warning(this, "错误", QString("无法加载库体SHP文件: %1").arg(fileName));
                delete layer;
            }
        }
    }
    // 更新文件数量标签
    updateFileCountLabels();
}
//移除选中的解析文件
void ReadShpDialog::removeSelectedParseFile()
{
    int currentRow = parseFilesList->currentRow();
    if (currentRow >= 0 && currentRow < parseFilePaths.size()) {
        // 移除图层
        if (currentRow < parseLayers.size()) {
            delete parseLayers[currentRow];
            parseLayers.erase(parseLayers.begin() + currentRow);
        }
        // 移除文件路径
        parseFilePaths.removeAt(currentRow);
        delete parseFilesList->takeItem(currentRow);
        // 更新文件数量标签
        updateFileCountLabels();
    }
}
//移除选中的库体文件
void ReadShpDialog::removeSelectedReferenceFile()
{
    int currentRow = referenceFilesList->currentRow();
    if (currentRow >= 0 && currentRow < referenceFilePaths.size()) {
        // 移除图层
        if (currentRow < referenceLayers.size()) {
            delete referenceLayers[currentRow];
            referenceLayers.erase(referenceLayers.begin() + currentRow);
        }
        // 移除文件路径
        referenceFilePaths.removeAt(currentRow);
        delete referenceFilesList->takeItem(currentRow);
        // 更新文件数量标签
        updateFileCountLabels();
    }
}
//移除所有的解析文件
void ReadShpDialog::clearAllParseFiles()
{
    // 清理图层
    for (auto layer : parseLayers) {
        delete layer;
    }
    parseLayers.clear();
    // 清理文件列表
    parseFilePaths.clear();
    parseFilesList->clear();
    // 更新文件数量标签
    updateFileCountLabels();
}
//移除所有的库体文件
void ReadShpDialog::clearAllReferenceFiles()
{
    // 清理图层
    for (auto layer : referenceLayers) {
        delete layer;
    }
    referenceLayers.clear();
    // 清理文件列表
    referenceFilePaths.clear();
    referenceFilesList->clear();
    // 更新文件数量标签
    updateFileCountLabels();
}
//更新文件数量
void ReadShpDialog::updateFileCountLabels()
{
    parseFilesCountLabel->setText(QString("文件数量: %1").arg(parseFilePaths.size()));
    referenceFilesCountLabel->setText(QString("文件数量: %1").arg(referenceFilePaths.size()));
}
//显示模式（全部结果/错误）
void ReadShpDialog::onOnlyErrorsCheckBoxToggled(bool checked)
{
    // 当复选框状态改变时，重新显示结果
    if (!lastCheckResults.empty()) {
        showQualityResults(lastCheckResults);
        logTextEdit->append(QString("显示模式已切换为: %1").arg(checked ? "仅显示错误" : "显示全部结果"));
    }
}
//选择配置文件
void ReadShpDialog::selectConfigFile()
{
    QString fileName = QFileDialog::getOpenFileName(this, "选择配置文件", "", "XML Files (*.xml)");
    if (!fileName.isEmpty()) {
        m_configFilePath = fileName;
        configPathLabel->setText(QString("配置文件: %1").arg(QFileInfo(fileName).fileName()));
        loadConfigFromXML();
        logTextEdit->append(QString("配置文件已加载: %1").arg(fileName));
    }
}
//添加一个当前解析文件
void ReadShpDialog::selectShpFile()
{
    QString fileName = QFileDialog::getOpenFileName(this, "选择解析SHP文件", "", "Shapefile (*.shp)");
    if (!fileName.isEmpty()) {
        currentFilePath = fileName;
        currentFileLabel->setText(QString("解析文件: %1").arg(QFileInfo(fileName).fileName()));
        // 清理之前的图层：先从画布移除，避免悬挂指针
        if (currentLayer) {
            if (mapCanvas && allDisplayLayers.contains(currentLayer)) {
                allDisplayLayers.removeAll(currentLayer);
                mapCanvas->setLayers(allDisplayLayers);
                mapCanvas->refresh();
            }
            delete currentLayer;
            currentLayer = nullptr;
        }
        // 加载新图层
        currentLayer = new QgsVectorLayer(fileName, "解析图层", "ogr");
        if (currentLayer->isValid()) {
            // ... existing code ...
            loadLayerToMap(currentLayer);
            zoomToLayer();
        }
        else {
            QMessageBox::warning(this, "错误", "无法加载解析SHP文件");
            delete currentLayer;
            currentLayer = nullptr;
        }
    }
}
//添加一个当前库体文件
void ReadShpDialog::selectReferenceFile()
{
    QString fileName = QFileDialog::getOpenFileName(this, "选择库体SHP文件", "", "Shapefile (*.shp)");
    if (!fileName.isEmpty()) {
        referenceFilePath = fileName;
        referenceFileLabel->setText(QString("库体文件: %1").arg(QFileInfo(fileName).fileName()));
        // 清理之前的库体图层：先从画布移除，避免悬挂指针
        if (referenceLayer) {
            if (mapCanvas && allDisplayLayers.contains(referenceLayer)) {
                allDisplayLayers.removeAll(referenceLayer);
                mapCanvas->setLayers(allDisplayLayers);
                mapCanvas->refresh();
            }
            delete referenceLayer;
            referenceLayer = nullptr;
        }
        // 加载库体图层
        referenceLayer = new QgsVectorLayer(fileName, "库体图层", "ogr");
        if (referenceLayer->isValid()) {
            // ... existing code ...
            // 不强制加入显示列表，这里保持原逻辑
        }
        else {
            QMessageBox::warning(this, "错误", "无法加载库体SHP文件");
            delete referenceLayer;
            referenceLayer = nullptr;
        }
    }
}
//判断是不是有效shp图层
bool ReadShpDialog::isShpLayer(QgsVectorLayer* layer)
{
    if (!layer) return false;
    if (!layer->isValid()) return false;
    // Shapefile 通常走 OGR provider，数据源包含 .shp
    if (layer->providerType().compare("ogr", Qt::CaseInsensitive) != 0) return false;
    const QString src = layer->source();
    return src.contains(".shp", Qt::CaseInsensitive);

}
void ReadShpDialog::resetExecutionFlags() {
    executedLineTopology = false;
    executedDirection = false;
    executedPolygon = false;
    executedRightAngle = false;
    setProperty("quality/right_angle_diagnostics", QJsonArray{});
}
//执行错误检查主入口函数
void ReadShpDialog::performQualityCheck()
{
    bool hasParseLayers = !parseLayers.empty() || currentLayer;
    bool hasReferenceLayers = !referenceLayers.empty() || referenceLayer;
    if (!hasParseLayers || !hasReferenceLayers) {
        QMessageBox::warning(this, "错误", "请先选择解析SHP文件和库体SHP文件");
        return;
    }
    logTextEdit->append("\n=== 开始执行质量检查 ===");
    // 每次执行检查前重置“是否执行”标志，保证导出状态正确
    resetExecutionFlags();
    try {
        // 清空之前的检查结果 
        lastCheckResults.clear();
        clearErrorMarkers();
        // 从配置文件读取参数 
        updateParametersFromConfig();
        // 执行走向错误检查
        if (directionErrorCheckBox->isChecked()) {
            logTextEdit->append("正在执行几何走向异常检查...");
            int initialResultCount = lastCheckResults.size();

            // 合并解析候选（批量 + 单个）并按规范化路径去重
            QStringList parseCandidates = parseFilePaths;
            if (!currentFilePath.isEmpty()) parseCandidates << currentFilePath;
            QSet<QString> uniqueParse;
            QVector<QPair<QString, QgsVectorLayer*>> parseList;
            for (const QString& p : parseCandidates) {
                const QString canon = QFileInfo(p).canonicalFilePath();
                if (uniqueParse.contains(canon)) continue;
                uniqueParse.insert(canon);

                QgsVectorLayer* parsedLayerPtr = nullptr;
                // 从批量解析集合中匹配
                for (int i = 0; i < parseFilePaths.size(); ++i) {
                    if (QFileInfo(parseFilePaths[i]).canonicalFilePath() == canon) {
                        if (i >= 0 && i < static_cast<int>(parseLayers.size())) {
                            parsedLayerPtr = parseLayers[i];
                        }
                        break;
                    }
                }
                // 若匹配到当前单个解析文件
                if (!parsedLayerPtr && !currentFilePath.isEmpty() &&
                    QFileInfo(currentFilePath).canonicalFilePath() == canon) {
                    parsedLayerPtr = currentLayer;
                }

                if (parsedLayerPtr && parsedLayerPtr->isValid()) {
                    parseList.push_back(qMakePair(p, parsedLayerPtr));
                }
                else {
                    logTextEdit->append(QString("跳过：解析图层无效 %1").arg(QFileInfo(p).fileName()));
                }
            }

            // 合并库体候选（批量 + 单个）并按规范化路径去重
            QStringList refCandidates = referenceFilePaths;
            if (!referenceFilePath.isEmpty()) refCandidates << referenceFilePath;
            QSet<QString> uniqueRef;
            QVector<QPair<QString, QgsVectorLayer*>> refList;
            for (const QString& r : refCandidates) {
                const QString canonR = QFileInfo(r).canonicalFilePath();
                if (uniqueRef.contains(canonR)) continue;
                uniqueRef.insert(canonR);

                QgsVectorLayer* refLayerPtr = nullptr;
                // 从批量库体集合中匹配
                for (int i = 0; i < referenceFilePaths.size(); ++i) {
                    if (QFileInfo(referenceFilePaths[i]).canonicalFilePath() == canonR) {
                        if (i >= 0 && i < static_cast<int>(referenceLayers.size())) {
                            refLayerPtr = referenceLayers[i];
                        }
                        break;
                    }
                }
                // 若匹配到当前单个库体文件
                if (!refLayerPtr && !referenceFilePath.isEmpty() &&
                    QFileInfo(referenceFilePath).canonicalFilePath() == canonR) {
                    refLayerPtr = referenceLayer;
                }

                if (refLayerPtr && refLayerPtr->isValid()) {
                    refList.push_back(qMakePair(r, refLayerPtr));
                }
                else {
                    logTextEdit->append(QString("跳过：库体图层无效 %1").arg(QFileInfo(r).fileName()));
                }
            }

            // 取消文件名识别门控，按所有唯一配对执行检查
            QSet<QString> pairSeen;
            for (const auto& pr : parseList) {
                const QString parseFileName = QFileInfo(pr.first).fileName();
                // ... existing code ...
                for (const auto& rr : refList) {
                    const QString key = QFileInfo(pr.first).canonicalFilePath() + "|" + QFileInfo(rr.first).canonicalFilePath();
                    if (pairSeen.contains(key)) continue;
                    pairSeen.insert(key);

                    auto results = checkWaterSystemDirection(pr.second, rr.second, parseFileName);
                    lastCheckResults.insert(lastCheckResults.end(), results.begin(), results.end());
                }
            }

            int directionResultCount = lastCheckResults.size() - initialResultCount;
            logTextEdit->append(QString("几何走向异常检查完成，发现 %1 个问题").arg(directionResultCount));
        }
        // 执行面要素重叠/间隙检查
        if (polygonOverlapGapCheckBox->isChecked()) {
            logTextEdit->append("正在执行面要素重叠/间隙检查...");
            int initialResultCount = lastCheckResults.size();
            // 处理单个文件的情况：不再基于文件名识别，直接检查当前解析图层（若存在）
            if (currentLayer) {
                QString parseFileName = QFileInfo(currentFilePath).fileName();
                auto results = checkPolygonOverlapGap(currentLayer, parseFileName);
                lastCheckResults.insert(lastCheckResults.end(), results.begin(), results.end());
            }
            // 处理批量文件的情况：不再基于文件名识别，遍历所有解析图层
            for (size_t i = 0; i < parseLayers.size(); ++i) {
                QString parseFileName = QFileInfo(parseFilePaths[i]).fileName();
                auto results = checkPolygonOverlapGap(parseLayers[i], parseFileName);
                lastCheckResults.insert(lastCheckResults.end(), results.begin(), results.end());
            }
            int polygonResultCount = lastCheckResults.size() - initialResultCount;
            logTextEdit->append(QString("面要素重叠/间隙检查完成，发现 %1 个问题").arg(polygonResultCount));
        }
        // 直角化角度阈值控制检查
        if (rightAngleCheckBox->isChecked()) {
            logTextEdit->append("=== 开始直角化角度阈值控制 ===");
            QString checkItemName = "直角化角度阈值控制";
            if (m_config && m_config->isCheckItemEnabled(checkItemName)) {
                logTextEdit->append("正在执行直角化角度阈值控制...");
                // 获取配置参数
                rightAngleLowerLimit = m_config->getRightAngleLowerLimit();
                rightAngleUpperLimit = m_config->getRightAngleUpperLimit();
                rightAngleDatabaseMatchTolerance = getUnscaledDistanceParameter(
                    QStringLiteral("直角化角度阈值控制"),
                    QStringLiteral("right_angle_database_match_tolerance"));
                rightAngleGeometrySimilarityThreshold = m_config->getRightAngleGeometrySimilarityThreshold();
                rightAngleTurningPointTolerance = m_config->getRightAngleTurningPointTolerance();
                rightAngleTurningPointMatchTolerance = getUnscaledDistanceParameter(
                    QStringLiteral("直角化角度阈值控制"),
                    QStringLiteral("right_angle_turning_point_match_tolerance"));
                rightAngleMinSegmentLength = getConvertedParameter(QStringLiteral("直角化角度阈值控制"), QStringLiteral("right_angle_min_segment_length"));
                logTextEdit->append(QString("直角化检查参数 - 角度范围: %1°-%2°, 匹配容差: %3m, 相似度阈值: %4, 转折点容差: %5°, 最小线段长度: %6m")
                    .arg(rightAngleLowerLimit).arg(rightAngleUpperLimit)
                    .arg(rightAngleDatabaseMatchTolerance).arg(rightAngleGeometrySimilarityThreshold)
                    .arg(rightAngleTurningPointTolerance).arg(rightAngleMinSegmentLength));

                int initialResultCount = lastCheckResults.size();

                // 所有有效输入图层都可以检查，但解析图层与库体图层必须属于同一标准类型。
                auto rightAngleLayerKey = [](QgsVectorLayer* layer, const QString& filePath) {
                    QString key = QFileInfo(filePath).baseName().trimmed();
                    if (key.isEmpty() && layer) key = layer->name().trimmed();
                    key = QFileInfo(key).baseName().toLower().trimmed();
                    if (key.endsWith("_标注")) key.chop(3);
                    if (key.endsWith("_代码标注")) key.chop(5);
                    if (key.endsWith("_名称标注")) key.chop(5);
                    if (key.endsWith("_linestring")) key.chop(11);
                    if (key.endsWith("_line")) key.chop(5);
                    if (key.contains("省级行政区界线") || key == "省界") return QStringLiteral("boundary:province");
                    if (key.contains("市级行政区界线") || key == "市界") return QStringLiteral("boundary:city");
                    if (key.contains("县级行政区界线") || key == "县界") return QStringLiteral("boundary:county");
                    if (key.contains("乡镇级行政区界线") || key == "乡镇界") return QStringLiteral("boundary:township");
                    if (key.contains("国道")) return QStringLiteral("road:national");
                    if (key.contains("县道")) return QStringLiteral("road:county");
                    if (key.contains("乡道")) return QStringLiteral("road:township");
                    if (key.contains("村道")) return QStringLiteral("road:village");
                    if (key.contains("总干渠")) return QStringLiteral("canal:main");
                    if (key.contains("干渠")) return QStringLiteral("canal:trunk");
                    if (key.contains("支渠")) return QStringLiteral("canal:branch");
                    if (key.contains("沟渠")) return QStringLiteral("canal:ditch");
                    return key;
                };

                // 只按同类型图层配对，不再执行所有解析图层与库体图层的交叉配对。
                if (!parseLayers.empty() && !referenceLayers.empty()) {
                    for (size_t i = 0; i < parseLayers.size(); ++i) {
                        QgsVectorLayer* parsed = parseLayers[i];
                        if (!isShpLayer(parsed)) {
                            logTextEdit->append(QString("跳过：解析图层非SHP或无效 -> %1").arg(parsed ? parsed->name() : "null"));
                            continue;
                        }
                        for (size_t j = 0; j < referenceLayers.size(); ++j) {
                            QgsVectorLayer* reference = referenceLayers[j];
                            if (!isShpLayer(reference)) {
                                logTextEdit->append(QString("跳过：参考图层非SHP或无效 -> %1").arg(reference ? reference->name() : "null"));
                                continue;
                            }

                            QString parseLayerName = parsed->name();
                            QString referenceLayerName = reference->name();
                            const QString parseKey = rightAngleLayerKey(parsed, parseFilePaths.value(static_cast<int>(i), parseLayerName));
                            const QString referenceKey = rightAngleLayerKey(reference, referenceFilePaths.value(static_cast<int>(j), referenceLayerName));
                            if (parseKey.isEmpty() || referenceKey.isEmpty() || parseKey != referenceKey) {
                                logTextEdit->append(QString("跳过直角化配对：图层类型不一致（%1 / %2）")
                                    .arg(parseKey, referenceKey));
                                continue;
                            }

                            logTextEdit->append(QString("正在对解析图层 %1 使用参考图层 %2 进行直角化检查")
                                .arg(parseLayerName).arg(referenceLayerName));

                            std::vector<QualityCheckResult> results =
                                checkRightAngleThreshold(parsed, reference, parseLayerName);

                            lastCheckResults.insert(lastCheckResults.end(), results.begin(), results.end());

                            logTextEdit->append(QString("解析图层 %1 与参考图层 %2 的直角化检查完成，发现 %3 个问题")
                                .arg(parseLayerName).arg(referenceLayerName).arg(results.size()));
                        }
                    }
                }
                // 单图层情形：使用当前选中的解析图层与参考图层
                else if (currentLayer && referenceLayer) {
                    if (!isShpLayer(currentLayer) || !isShpLayer(referenceLayer)) {
                        logTextEdit->append("跳过直角化检查：当前图层或参考图层不是有效的SHP图层");
                    }
                    else {
                        QString parseLayerName = currentLayer->name();
                        QString referenceLayerName = referenceLayer->name();
                        const QString parseKey = rightAngleLayerKey(currentLayer, currentFilePath);
                        const QString referenceKey = rightAngleLayerKey(referenceLayer, referenceFilePath);
                        if (parseKey.isEmpty() || referenceKey.isEmpty() || parseKey != referenceKey) {
                            logTextEdit->append(QString("跳过直角化检查：图层类型不一致（%1 / %2）")
                                .arg(parseKey, referenceKey));
                        }
                        else {

                        logTextEdit->append(QString("正在对解析图层 %1 使用参考图层 %2 进行直角化检查")
                            .arg(parseLayerName).arg(referenceLayerName));

                        std::vector<QualityCheckResult> results =
                            checkRightAngleThreshold(currentLayer, referenceLayer, parseLayerName);

                        lastCheckResults.insert(lastCheckResults.end(), results.begin(), results.end());

                        logTextEdit->append(QString("解析图层 %1 与参考图层 %2 的直角化检查完成，发现 %3 个问题")
                            .arg(parseLayerName).arg(referenceLayerName).arg(results.size()));
                        }
                    }
                }
                else {
                    logTextEdit->append("跳过直角化检查：至少需要一个解析图层和一个参考图层");
                }

                int rightAngleResultCount = lastCheckResults.size() - initialResultCount;
                logTextEdit->append(QString("直角化角度阈值控制完成，发现 %1 个问题").arg(rightAngleResultCount));
            }
            else {
                logTextEdit->append("直角化角度阈值控制未在配置文件中启用，跳过检查");
            }
        }
        if (lineTopologyCheckBox->isChecked()) {
            logTextEdit->append("正在执行线要素拓扑检查...");

            int initialResultCount = lastCheckResults.size();

            // 统一的路/河流判定（基于图层名或文件名）
            auto isRoadByLayerOrFile = [&](QgsVectorLayer* layer, const QString& filePath) -> bool {
                if (!layer) return false;
                const QString layerName = layer->name();
                const QString fileName = QFileInfo(filePath).fileName();
                return isRoadFile(fileName) || isRoadFile(layerName);
            };

            // 去重后的唯一配对集合
            struct Pair { QgsVectorLayer* p; QgsVectorLayer* r; QString pPath; QString rPath; };
            QList<Pair> uniquePairs;
            QSet<QString> seen; // key = canonical(parse) || canonical(reference)

            auto addPair = [&](QgsVectorLayer* p, const QString& pPath, QgsVectorLayer* r, const QString& rPath) {
                if (!p || !r) return;
                const QString cp = QFileInfo(pPath).canonicalFilePath();
                const QString cr = QFileInfo(rPath).canonicalFilePath();
                if (cp.isEmpty() || cr.isEmpty()) return;
                const QString key = cp + "||" + cr;
                if (seen.contains(key)) return;
                uniquePairs.push_back({ p, r, pPath, rPath });
                seen.insert(key);
            };

            // 汇总所有可能的配对，但只保留唯一组合
            addPair(currentLayer, currentFilePath, referenceLayer, referenceFilePath);
            for (size_t i = 0; i < parseLayers.size(); ++i) {
                for (size_t j = 0; j < referenceLayers.size(); ++j) {
                    addPair(parseLayers[i], parseFilePaths[i], referenceLayers[j], referenceFilePaths[j]);
                }
                // 单个库体图层也参与配对，但仍然去重
                addPair(parseLayers[i], parseFilePaths[i], referenceLayer, referenceFilePath);
            }
            // 单个解析图层与批量库体的组合（若 currentLayer 也在 parseLayers 中，去重会拦截）
            if (currentLayer) {
                for (size_t j = 0; j < referenceLayers.size(); ++j) {
                    addPair(currentLayer, currentFilePath, referenceLayers[j], referenceFilePaths[j]);
                }
            }

            // 对唯一配对逐一执行检查
            QSet<QString> riverTopoSeen; // 仅用于水系_LineString 去重
            auto tagLineTopologySourcePath = [](std::vector<QualityCheckResult>& sourceResults,
                const QString& sourcePath) {
                    QString identity = QFileInfo(sourcePath).canonicalFilePath();
                    if (identity.isEmpty()) identity = QFileInfo(sourcePath).absoluteFilePath();
                    identity = QDir::cleanPath(identity);
                    for (QualityCheckResult& result : sourceResults) {
                        if (!result.passed && (result.checkType == CheckType::LINE_TOPOLOGY_ERROR
                            || result.checkType == CheckType::NETWORK_CONNECTIVITY_ERROR)) {
                            result.sourceFile = identity;
                        }
                    }
                };
            for (const auto& pair : uniquePairs) {
                const QString parseFileName = QFileInfo(pair.pPath).fileName();
                std::vector<QualityCheckResult> results;

                // 先判断是否道路
                if (isRoadByLayerOrFile(pair.p, pair.pPath)) {
                    results = checkRoadLineTopology(pair.p, pair.r, parseFileName);
                    tagLineTopologySourcePath(results, pair.pPath);
                    lastCheckResults.insert(lastCheckResults.end(), results.begin(), results.end());
                    continue;
                }

                // 再判断是否水系线图层（只对水系_LineString 执行河流拓扑）
                if (isRiverFile(parseFileName)) {
                    const QString canonParse = QFileInfo(pair.pPath).canonicalFilePath();
                    if (riverTopoSeen.contains(canonParse)) {
                        logTextEdit->append(QString("跳过重复河流拓扑检查：%1").arg(parseFileName));
                        continue;
                    }
                    // 河流拓扑不依赖库体图层，传 nullptr 即可避免无意义重复
                    results = checkRiverLineTopology(pair.p, nullptr, parseFileName);
                    tagLineTopologySourcePath(results, pair.pPath);
                    lastCheckResults.insert(lastCheckResults.end(), results.begin(), results.end());
                    riverTopoSeen.insert(canonParse);
                }
                else {
                    // 非道路且非水系线（井/泉/水库/拦水坝/水系面等），不参与线要素河流拓扑
                    logTextEdit->append(QString("跳过线要素拓扑（非水系线图层）：%1").arg(parseFileName));
                }
            }

            int lineResultCount = lastCheckResults.size() - initialResultCount;
            logTextEdit->append(QString("线要素拓扑检查完成，发现 %1 个问题").arg(lineResultCount));
            // 河网通达性检查（只有在勾选线要素查时才执行）
            logTextEdit->append("正在执行河网通达性检查...");
            int connectivityInitialCount = lastCheckResults.size();

            QSet<QString> connSeen; // 新增：去重集合（解析|库体的规范路径）

       // 处理单个文件的情况
            if (currentLayer && isRiverFile(QFileInfo(currentFilePath).fileName())) {
                const QString key = QFileInfo(currentFilePath).canonicalFilePath() + "|" +
                    QFileInfo(referenceFilePath).canonicalFilePath();
                if (!connSeen.contains(key)) {
                    logTextEdit->append("开始河网通达性检查...");

                    std::vector<QualityCheckResult> connectivityResults =
                        checkRiverNetworkConnectivity(currentLayer, referenceLayer, QFileInfo(currentFilePath).fileName());
                    tagLineTopologySourcePath(connectivityResults, currentFilePath);
                    if (!connectivityResults.empty()) {
                        lastCheckResults.insert(lastCheckResults.end(), connectivityResults.begin(), connectivityResults.end());
                        logTextEdit->append(QString("河网通达性检查完成，发现 %1 个问题").arg(connectivityResults.size()));
                    }
                    else {
                        logTextEdit->append("河网通达性检查通过");
                    }
                    connSeen.insert(key);
                }
                else {
                    logTextEdit->append("跳过通达性：该解析-库体配对已检查过");
                }
            }

            // 处理批量文件的情况
            for (size_t i = 0; i < parseLayers.size(); ++i) {
                QString parseFileName = QFileInfo(parseFilePaths[i]).fileName();
                if (!isRiverFile(parseFileName)) continue;

                logTextEdit->append(QString("开始对 %1 执行河网通达性检查...").arg(parseFileName));

                // 对每个解析图层，检查所有库体图层
                for (size_t j = 0; j < referenceLayers.size(); ++j) {
                    const QString key = QFileInfo(parseFilePaths[i]).canonicalFilePath() + "|" +
                        QFileInfo(referenceFilePaths[j]).canonicalFilePath();
                    if (connSeen.contains(key)) {
                        logTextEdit->append(QString("跳过组合 %1-%2（通达性已检查过）")
                            .arg(parseFileName).arg(QFileInfo(referenceFilePaths[j]).fileName()));
                        continue;
                    }

                    std::vector<QualityCheckResult> connectivityResults =
                        checkRiverNetworkConnectivity(parseLayers[i], referenceLayers[j], parseFileName);
                    tagLineTopologySourcePath(connectivityResults, parseFilePaths[i]);
                    if (!connectivityResults.empty()) {
                        lastCheckResults.insert(lastCheckResults.end(), connectivityResults.begin(), connectivityResults.end());
                        logTextEdit->append(QString("完成对 %1 的河网通达性检查，发现 %2 个问题")
                            .arg(parseFileName).arg(connectivityResults.size()));
                    }
                    else {
                        logTextEdit->append(QString("%1 的河网通达性检查通过").arg(parseFileName));
                    }
                    connSeen.insert(key);
                }
            }
            int connectivityResultCount = lastCheckResults.size() - connectivityInitialCount;
            logTextEdit->append(QString("河网通达性检查完成，发现 %1 个问题").arg(connectivityResultCount));
        }

        const int mergedLineTopologyRows = mergeDuplicateLineTopologyErrorRecords(lastCheckResults);
        for (QualityCheckResult& result : lastCheckResults) {
            if (!result.passed && (result.checkType == CheckType::LINE_TOPOLOGY_ERROR
                || result.checkType == CheckType::NETWORK_CONNECTIVITY_ERROR)) {
                result.sourceFile = QFileInfo(result.sourceFile).fileName();
            }
        }
        if (mergedLineTopologyRows > 0) {
            logTextEdit->append(QString("线要素拓扑检查合并了 %1 条重复错误记录，同一源线保留一条记录并汇总错误原因")
                .arg(mergedLineTopologyRows));
        }
        logTextEdit->append(QString("总计发现 %1 个问题").arg(lastCheckResults.size()));

        // 显示结果
        showQualityResults(lastCheckResults);
        addErrorMarkersToMap(lastCheckResults);

        if (!lastCheckResults.empty()) {
            zoomToErrors();
        }

        logTextEdit->append("=== 质检完成 ===");
        logTextEdit->append("\n=== 加载多图层显示 ===");
        loadMultipleLayersToMap();
        logTextEdit->append("多图层显示完成");
    }
    catch (const std::exception& e) {
        QString errorMsg = QString("质检过程中发生错误: %1").arg(e.what());
        logTextEdit->append(errorMsg);
        QMessageBox::critical(this, "错误", errorMsg);
    }
}
//核心的线串提取工具
std::vector<const QgsLineString*> ReadShpDialog::extractLineStrings(const QgsGeometry& geom)
{
    std::vector<const QgsLineString*> lineStrings;

    // 支持线与面：面要素拆解为外环与内环
    if (geom.type() == QgsWkbTypes::LineGeometry) {
        if (geom.isMultipart()) {
            const QgsMultiLineString* multiLine = qgsgeometry_cast<const QgsMultiLineString*>(geom.constGet());
            if (multiLine) {
                for (int i = 0; i < multiLine->numGeometries(); ++i) {
                    const QgsLineString* line = qgsgeometry_cast<const QgsLineString*>(multiLine->geometryN(i));
                    if (line) {
                        lineStrings.push_back(line);
                    }
                }
            }
        }
        else {
            const QgsLineString* line = qgsgeometry_cast<const QgsLineString*>(geom.constGet());
            if (line) {
                lineStrings.push_back(line);
            }
        }
    }
    else if (geom.type() == QgsWkbTypes::PolygonGeometry) {
        if (geom.isMultipart()) {
            const QgsMultiPolygon* multiPoly = qgsgeometry_cast<const QgsMultiPolygon*>(geom.constGet());
            if (multiPoly) {
                for (int i = 0; i < multiPoly->numGeometries(); ++i) {
                    const QgsPolygon* poly = qgsgeometry_cast<const QgsPolygon*>(multiPoly->geometryN(i));
                    if (!poly) continue;

                    // 外环
                    const QgsCurve* ext = poly->exteriorRing();
                    if (const QgsLineString* extLine = qgsgeometry_cast<const QgsLineString*>(ext)) {
                        lineStrings.push_back(extLine);
                    }
                    // 内环
                    int innerCount = poly->numInteriorRings();
                    for (int r = 0; r < innerCount; ++r) {
                        const QgsCurve* ring = poly->interiorRing(r);
                        if (const QgsLineString* ringLine = qgsgeometry_cast<const QgsLineString*>(ring)) {
                            lineStrings.push_back(ringLine);
                        }
                    }
                }
            }
        }
        else {
            const QgsPolygon* poly = qgsgeometry_cast<const QgsPolygon*>(geom.constGet());
            if (poly) {
                const QgsCurve* ext = poly->exteriorRing();
                if (const QgsLineString* extLine = qgsgeometry_cast<const QgsLineString*>(ext)) {
                    lineStrings.push_back(extLine);
                }
                int innerCount = poly->numInteriorRings();
                for (int r = 0; r < innerCount; ++r) {
                    const QgsCurve* ring = poly->interiorRing(r);
                    if (const QgsLineString* ringLine = qgsgeometry_cast<const QgsLineString*>(ring)) {
                        lineStrings.push_back(ringLine);
                    }
                }
            }
        }
    }

    return lineStrings;
}
//从任意几何中取“第一个”线串
const QgsLineString* ReadShpDialog::getFirstLineString(const QgsGeometry& geom)
{
    auto lineStrings = extractLineStrings(geom);
    return lineStrings.empty() ? nullptr : lineStrings[0];
}
//检查图层是否存在无效几何；若存在，将错误添加到 results，并返回 true（提示上层应终止检查）
bool ReadShpDialog::collectInvalidGeometry(QgsVectorLayer* layer, const QString& sourceFile, CheckType checkType, std::vector<QualityCheckResult>& results)
{
    if (!layer) return false;

    QgsFeatureIterator it = layer->getFeatures();
    QgsFeature f;
    bool foundInvalid = false;

    while (it.nextFeature(f)) {
        if (!f.hasGeometry()) {
            foundInvalid = true;
        }
        else {
            const QgsGeometry geom = f.geometry();
            // 几何为空 / Null / GEOS 校验失败 都视为无效
            if (geom.isEmpty() || geom.isNull() || !geom.isGeosValid()) {
                foundInvalid = true;
            }
        }

        if (foundInvalid) {
            QualityCheckResult r;
            r.passed = false;
            r.errorType = QString::fromUtf8("几何体无效");
            r.description = QString::fromUtf8("发现无效几何体（空/Null/GEOS校验失败），已跳过该要素继续检查");
            r.feature = f;
            r.errorGeometry = f.hasGeometry() ? f.geometry() : QgsGeometry();
            r.calculatedValue = 0.0;
            r.standardValue = 0.0;
            r.pointIndex = -1;
            r.sourceFile = sourceFile;
            r.checkType = checkType;
            r.featureId = f.id();
            r.geometry = f.hasGeometry() ? f.geometry() : QgsGeometry();
            r.severity = QString::fromUtf8("ERROR");

            results.push_back(r);
           
            foundInvalid = false; // 复位，逐条判断并收集
        }
    }

    return foundInvalid;
}

//========================= 直角化角度阈值检查 =========================
//直角化角度阈值控制检查主函数
std::vector<QualityCheckResult> ReadShpDialog::checkRightAngleThreshold(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile)
{
    std::vector<QualityCheckResult> results;
    if (!parsedLayer || !referenceLayer || !parsedLayer->isValid() || !referenceLayer->isValid()) {
        executedRightAngle = false;
        return results;
    }
    executedRightAngle = true;
    const double lowerLimit = m_config->getRightAngleLowerLimit();
    const double upperLimit = m_config->getRightAngleUpperLimit();
    const double matchTolerance = std::max(0.0, rightAngleDatabaseMatchTolerance);
    const double pointTolerance = std::max(0.0, rightAngleTurningPointMatchTolerance);
    const double similarityThreshold = m_config->getRightAngleGeometrySimilarityThreshold();
    struct MatchedBoundary {
        QgsFeature reference;
        std::vector<QgsFeature> sources;
        QVector<QgsGeometry> boundaries;
    };
    std::map<QgsFeatureId, MatchedBoundary> matchedBoundaries;
    QSet<QgsFeatureId> invalidReferenceIds;
    QgsFeature parsedFeature;
    auto iter = parsedLayer->getFeatures();
    int totalParsedFeatures = 0;
    int matchedParsedFeatures = 0;
    int checkedCorners = 0;
    int angleCandidates = 0;
    int lengthEligibleCorners = 0;
    int evaluatedCorners = 0;
    int missingCorners = 0;
    int incompleteCorners = 0;
    int invalidGeometryCount = 0;
    while (iter.nextFeature(parsedFeature)) {
        ++totalParsedFeatures;
        const QgsGeometry geometry = parsedFeature.geometry();
        if (geometry.isNull() || geometry.isEmpty() || !geometry.isGeosValid()) {
            ++invalidGeometryCount;
            QualityCheckResult invalid{};
            invalid.featureId = parsedFeature.id();
            invalid.checkType = CheckType::RIGHT_ANGLE_THRESHOLD_ERROR;
            invalid.description = QStringLiteral("几何无效（空/Null/GEOS校验失败），请先修复后再检查");
            invalid.feature = parsedFeature;
            invalid.errorGeometry = geometry;
            invalid.errorType = QStringLiteral("几何无效");
            invalid.sourceFile = sourceFile;
            results.push_back(invalid);
            continue;
        }
        const QgsFeature reference = findMatchingDatabaseFeatureForRightAngle(
            parsedFeature, parsedLayer, referenceLayer, matchTolerance, similarityThreshold);
        if (!reference.isValid()) continue;
        if (invalidReferenceIds.contains(reference.id())) continue;
        if (matchedBoundaries.find(reference.id()) == matchedBoundaries.end()) {
            const QgsGeometry referenceGeometry = reference.geometry();
            // A polygon may have a valid boundary while its surface is invalid.
            if (referenceGeometry.isNull() || referenceGeometry.isEmpty() || !referenceGeometry.isGeosValid()) {
                invalidReferenceIds.insert(reference.id());
                logTextEdit->append(QStringLiteral("库体要素FID %1几何无效，跳过该参考对象，请先修复库体后再检查").arg(reference.id()));
                continue;
            }
        }
        auto& group = matchedBoundaries[reference.id()];
        group.reference = reference;
        group.sources.push_back(parsedFeature);
        group.boundaries.push_back(rightAngleBoundary(geometry));
        ++matchedParsedFeatures;
    }

    // A reference corner is checked once against all corresponding fragments.
    // Union also removes duplicate fill/stroke representations of the same path.
    for (const auto& entry : matchedBoundaries) {
        const auto& group = entry.second;
        QgsGeometry parsedBoundary = QgsGeometry::unaryUnion(group.boundaries);
        if (parsedBoundary.isMultipart()) parsedBoundary = parsedBoundary.mergeLines();
        if (parsedBoundary.isNull() || parsedBoundary.isEmpty()) {
            logTextEdit->append(QStringLiteral("解析边界合并失败，跳过库体要素 %1").arg(entry.first));
            continue;
        }
        angleCandidates += static_cast<int>(extractRightAnglePoints(
            group.reference.geometry(), lowerLimit, upperLimit, 0.0).size());
        const auto corners = extractRightAnglePoints(group.reference.geometry(), lowerLimit, upperLimit,
                                                     rightAngleMinSegmentLength);
        lengthEligibleCorners += static_cast<int>(corners.size());
        QSet<QString> checkedPositions;
        // Keep these geometries alive while borrowing their line-string pointers.
        const auto lines = extractLineStrings(parsedBoundary);
        auto sourceFor = [&](const QgsGeometry& errorGeometry) {
            const QgsGeometry location = errorGeometry.type() == QgsWkbTypes::PointGeometry
                ? errorGeometry : errorGeometry.centroid();
            QgsFeature source = group.sources.front();
            double nearest = std::numeric_limits<double>::max();
            for (const auto& candidate : group.sources) {
                const double distance = rightAngleBoundary(candidate.geometry()).distance(location);
                if (distance >= 0.0 && distance < nearest) {
                    nearest = distance;
                    source = candidate;
                }
            }
            return source;
        };
        for (const auto& corner : corners) {
            const QString key = QStringLiteral("%1,%2")
                .arg(corner.position.x(), 0, 'f', 8).arg(corner.position.y(), 0, 'f', 8);
            if (checkedPositions.contains(key)) continue;
            checkedPositions.insert(key);
            const QgsGeometry cornerGeometry = QgsGeometry::fromPointXY(corner.position);
            const double boundaryDistance = parsedBoundary.distance(cornerGeometry);
            // A partial outline says nothing about distant corners of a large block.
            if (boundaryDistance < 0.0 || boundaryDistance > matchTolerance + 1e-8) continue;
            ++checkedCorners;
            const CornerNeighbors neighbors = findCornerNeighbors(parsedBoundary, corner.position, pointTolerance);
            if (neighbors.valid) {
                ++evaluatedCorners;
                const QgsPointXY previous(neighbors.line->pointN(neighbors.prevIndex));
                const QgsPointXY vertex(neighbors.line->pointN(neighbors.cornerIndex));
                const QgsPointXY next(neighbors.line->pointN(neighbors.nextIndex));
                const double angle = calculateVertexAngle(previous, vertex, next);
                if (isAngleInRange(angle, lowerLimit, upperLimit)) continue;
                const QString description = QStringLiteral(
                    "直角化角度不符（拐角: %1, %2；解析角度: %3°；标准角度: %4°；要求: %5°-%6°；库体FID: %7）")
                    .arg(vertex.x(), 0, 'f', 6).arg(vertex.y(), 0, 'f', 6)
                    .arg(angle, 0, 'f', 2).arg(corner.angle, 0, 'f', 2)
                    .arg(lowerLimit).arg(upperLimit).arg(entry.first);
                // Preserve the actual vertices of both effective arms, including ring seams.
                for (int step : { -1, 1 }) {
                    QualityCheckResult error{};
                    error.errorGeometry = rightAngleArmGeometry(neighbors.line, neighbors.cornerIndex,
                        step < 0 ? neighbors.prevIndex : neighbors.nextIndex, step);
                    error.feature = sourceFor(error.errorGeometry);
                    error.featureId = error.feature.id();
                    error.checkType = CheckType::RIGHT_ANGLE_THRESHOLD_ERROR;
                    error.description = description;
                    error.errorType = QStringLiteral("直角化角度不满足阈值");
                    error.sourceFile = sourceFile;
                    error.calculatedValue = angle;
                    error.standardValue = corner.angle;
                    results.push_back(error);
                }
                continue;
            }
            // Inspect independent paths separately. A nearby unrelated endpoint
            // cannot invalidate evidence from a complete ring or an interior segment.
            bool completeLocalBoundary = false;
            for (const auto* line : lines) {
                if (!line || line->numPoints() < 2) continue;
                const QgsGeometry path(line->clone());
                const double pathDistance = path.distance(cornerGeometry);
                if (pathDistance < 0.0 || pathDistance > matchTolerance + 1e-8) continue;
                const double endpointDistance = std::min(
                    QgsPointXY(line->startPoint()).distance(corner.position),
                    QgsPointXY(line->endPoint()).distance(corner.position));
                if (line->isClosed() || pathDistance + 1e-8 < endpointDistance) {
                    completeLocalBoundary = true;
                    break;
                }
            }
            if (!completeLocalBoundary) { ++incompleteCorners; continue; }
            ++missingCorners;
            QualityCheckResult missing{};
            missing.feature = sourceFor(cornerGeometry);
            missing.featureId = missing.feature.id();
            missing.checkType = CheckType::RIGHT_ANGLE_THRESHOLD_ERROR;
            missing.description = QStringLiteral(
                "直角化点缺失（库体角点: %1, %2；标准角度: %3°；最近解析边界距离: %4米；角点匹配容差: %5米；库体FID: %6）")
                .arg(corner.position.x(), 0, 'f', 6).arg(corner.position.y(), 0, 'f', 6)
                .arg(corner.angle, 0, 'f', 2).arg(boundaryDistance, 0, 'f', 3)
                .arg(pointTolerance, 0, 'f', 3).arg(entry.first);
            missing.errorType = QStringLiteral("直角化点缺失");
            missing.errorGeometry = cornerGeometry;
            missing.sourceFile = sourceFile;
            missing.calculatedValue = boundaryDistance;
            missing.standardValue = corner.angle;
            results.push_back(missing);
        }
    }
    logTextEdit->append(QStringLiteral("直角化检查完成：解析要素=%1，边界匹配=%2，局部角点=%3，错误记录=%4")
        .arg(totalParsedFeatures).arg(matchedParsedFeatures).arg(checkedCorners).arg(results.size()));
    logTextEdit->append(QStringLiteral("直角化筛选统计：角度候选=%1，有效边长达标=%2，局部范围内=%3，已核验角度=%4，确认缺角=%5，端点证据不足=%6；最小有效边长=%7米，方向容差=%8°，要素匹配容差=%9米，角点匹配容差=%10米")
        .arg(angleCandidates).arg(lengthEligibleCorners).arg(checkedCorners).arg(evaluatedCorners)
        .arg(missingCorners).arg(incompleteCorners).arg(rightAngleMinSegmentLength)
        .arg(rightAngleDirectionTolerance(m_config->getRightAngleTurningPointTolerance()))
        .arg(matchTolerance).arg(pointTolerance));
    QJsonObject diagnostics;
    diagnostics.insert(QStringLiteral("source_file"), sourceFile);
    diagnostics.insert(QStringLiteral("source_path"), parsedLayer->source());
    diagnostics.insert(QStringLiteral("reference_path"), referenceLayer->source());
    diagnostics.insert(QStringLiteral("source_feature_count"), totalParsedFeatures);
    diagnostics.insert(QStringLiteral("invalid_geometry_count"), invalidGeometryCount);
    diagnostics.insert(QStringLiteral("invalid_reference_geometry_count"), invalidReferenceIds.size());
    diagnostics.insert(QStringLiteral("matched_feature_count"), matchedParsedFeatures);
    diagnostics.insert(QStringLiteral("angle_candidate_count"), angleCandidates);
    diagnostics.insert(QStringLiteral("length_eligible_corner_count"), lengthEligibleCorners);
    diagnostics.insert(QStringLiteral("local_corner_count"), checkedCorners);
    diagnostics.insert(QStringLiteral("evaluated_angle_count"), evaluatedCorners);
    diagnostics.insert(QStringLiteral("missing_corner_count"), missingCorners);
    diagnostics.insert(QStringLiteral("incomplete_corner_count"), incompleteCorners);
    diagnostics.insert(QStringLiteral("verified_corner_count"), evaluatedCorners + missingCorners);
    diagnostics.insert(QStringLiteral("min_segment_length_m"), rightAngleMinSegmentLength);
    diagnostics.insert(QStringLiteral("direction_tolerance_deg"), rightAngleDirectionTolerance(m_config->getRightAngleTurningPointTolerance()));
    diagnostics.insert(QStringLiteral("database_match_tolerance_m"), matchTolerance);
    diagnostics.insert(QStringLiteral("corner_match_tolerance_m"), pointTolerance);
    if (evaluatedCorners + missingCorners == 0) {
        const QString reason = !invalidReferenceIds.isEmpty() ? QStringLiteral("匹配的库体参考要素几何无效，请先修复库体")
            : matchedParsedFeatures == 0 ? QStringLiteral("未匹配到对应边界")
            : angleCandidates == 0 ? QStringLiteral("匹配库体中无角度范围内的候选角点")
            : lengthEligibleCorners == 0 ? QStringLiteral("候选角点两侧有效边长未达到门槛")
            : checkedCorners == 0 ? QStringLiteral("达标角点不在解析边界局部覆盖范围内")
            : QStringLiteral("仅有片段端点，缺少完整拐角证据");
        diagnostics.insert(QStringLiteral("coverage_reason"), reason);
        logTextEdit->append(QStringLiteral("提示：本层未进入有效角点核查（%1），不能据此判定角度合格。原最小线段长度参数保持不变。").arg(reason));
    }
    QJsonArray layerDiagnostics = property("quality/right_angle_diagnostics").toJsonArray();
    layerDiagnostics.append(diagnostics);
    setProperty("quality/right_angle_diagnostics", layerDiagnostics);
    return results;
}
//在“候选角点”附近定位其前后相邻线段索引，得到稳定三点组合，使角度计算和相似性比较建立在稳定的拓扑邻域
CornerNeighbors ReadShpDialog::findCornerNeighbors(const QgsGeometry& parsedGeom, const QgsPointXY& corner, double searchRadius)
{
    CornerNeighbors best{ nullptr, -1, -1, -1, false };
    double nearest = std::max(0.0, searchRadius) + 1e-8;
    for (const auto* line : extractLineStrings(parsedGeom)) {
        if (!line) continue;
        const bool closed = line->isClosed();
        const int count = line->numPoints() - (closed ? 1 : 0);
        for (int i = 0; i < count; ++i) {
            const QgsPointXY vertex(line->pointN(i));
            const double distance = vertex.distance(corner);
            if (distance > nearest) continue;
            int prev = i - 1;
            int next = i + 1;
            if (closed && count >= 3) {
                prev = (i + count - 1) % count;
                next = (i + 1) % count;
            }
            // Ignore repeated consecutive coordinates when choosing the arms.
            while (prev >= 0 && prev < count && prev != i &&
                   vertex.distance(QgsPointXY(line->pointN(prev))) <= 1e-8)
                prev = closed ? (prev + count - 1) % count : prev - 1;
            while (next >= 0 && next < count && next != i &&
                   vertex.distance(QgsPointXY(line->pointN(next))) <= 1e-8)
                next = closed ? (next + 1) % count : next + 1;
            const bool valid = prev >= 0 && next < count && prev != i && next != i && prev != next;
            if (distance == nearest && best.valid && !valid) continue;
            nearest = distance;
            best = { line, prev, i, next, valid };
        }
    }
    // Trace only the final nearest vertex, not every progressively nearer node.
    if (best.valid) {
        const double tolerance = rightAngleDirectionTolerance(m_config->getRightAngleTurningPointTolerance());
        best.prevIndex = traceRightAngleArm(best.line, best.cornerIndex, -1, tolerance).endIndex;
        best.nextIndex = traceRightAngleArm(best.line, best.cornerIndex, 1, tolerance).endIndex;
        best.valid = best.prevIndex >= 0 && best.nextIndex >= 0 && best.prevIndex != best.nextIndex;
    }
    return best;
}
//改进的点匹配函数
QgsPointXY ReadShpDialog::findCorrespondingPointImproved(const QgsGeometry& parsedGeom, const QgsGeometry& /*databaseGeom*/, const QgsPointXY& databasePoint, double tolerance)
{
    QgsPointXY nearestPoint;
    double nearestDistance = std::max(0.0, tolerance) + 1e-8;
    for (const auto* line : extractLineStrings(parsedGeom)) {
        if (!line) continue;
        for (int i = 0; i < line->numPoints(); ++i) {
            const QgsPointXY vertex(line->pointN(i));
            const double distance = databasePoint.distance(vertex);
            if (distance <= nearestDistance) {
                nearestDistance = distance;
                nearestPoint = vertex;
            }
        }
    }
    return nearestPoint;
}
//改进的角度计算函数
double ReadShpDialog::calculateAngleAtPointImproved(const QgsGeometry& geometry, const QgsPointXY& point, double searchRadius)
{
    const CornerNeighbors neighbors = findCornerNeighbors(geometry, point, searchRadius);
    if (!neighbors.valid) return 180.0;
    return calculateVertexAngle(QgsPointXY(neighbors.line->pointN(neighbors.prevIndex)),
                                QgsPointXY(neighbors.line->pointN(neighbors.cornerIndex)),
                                QgsPointXY(neighbors.line->pointN(neighbors.nextIndex)));
}
//计算点到线段最近点的函数
QgsPointXY ReadShpDialog::getClosestPointOnSegment(const QgsPointXY& point, const QgsPointXY& segStart, const QgsPointXY& segEnd)
{
    double dx = segEnd.x() - segStart.x();
    double dy = segEnd.y() - segStart.y();

    if (dx == 0 && dy == 0) {
        return segStart; // 线段退化为点
    }

    double t = ((point.x() - segStart.x()) * dx + (point.y() - segStart.y()) * dy) / (dx * dx + dy * dy);

    // 限制t在[0,1]范围内
    t = qMax(0.0, qMin(1.0, t));

    return QgsPointXY(segStart.x() + t * dx, segStart.y() + t * dy);
}
//与库体文件要素优化匹配
QgsFeature ReadShpDialog::findMatchingDatabaseFeatureForRightAngle(const QgsFeature& parsedFeature, QgsVectorLayer* parsedLayer, QgsVectorLayer* databaseLayer, double tolerance, double similarityThreshold)
{
    QgsFeature invalidFeature;

    if (!parsedFeature.hasGeometry() || !parsedLayer || !databaseLayer) {
        logTextEdit->append("错误：解析要素无几何体或图层为空");
        return invalidFeature;
    }

    // 解析几何的线/面边界。
    QgsGeometry parsedGeom = parsedFeature.geometry();
    const QgsGeometry parsedBoundary = rightAngleBoundary(parsedGeom);
    if (parsedBoundary.isNull() || parsedBoundary.isEmpty()) return invalidFeature;

    // CRS 信息与坐标转换器
    QgsCoordinateReferenceSystem parsedCrs = parsedLayer->crs();
    QgsCoordinateReferenceSystem databaseCrs = databaseLayer->crs();

    logTextEdit->append(QString("CRS信息 - 解析图层CRS: %1, 数据库图层CRS: %2")
        .arg(parsedCrs.authid()).arg(databaseCrs.authid()));

    QgsCoordinateTransform transform;
    bool needsTransform = false;
    if (parsedCrs != databaseCrs) {
        transform = QgsCoordinateTransform(databaseCrs, parsedCrs, QgsProject::instance());
        needsTransform = true;
        logTextEdit->append("检测到CRS不匹配，将进行坐标转换");
    }

    // 严格的空间搜索范围：只按 tolerance 扩张
    QgsRectangle parsedBounds = parsedGeom.boundingBox();
    QgsRectangle searchBounds = parsedBounds;
    searchBounds.grow(tolerance);

    // 使用空间过滤的查询（若 provider 支持）
    QgsFeatureRequest request;
    try {
        request.setFilterRect(needsTransform
            ? transform.transformBoundingBox(searchBounds, Qgis::TransformDirection::Reverse)
            : searchBounds);
    }
    catch (const QgsCsException& e) {
        logTextEdit->append(QStringLiteral("直角化搜索范围转换失败: %1").arg(e.what()));
        return invalidFeature;
    }
    QgsFeatureIterator iter = databaseLayer->getFeatures(request);

    QgsFeature databaseFeature;
    double bestSimilarity = 0.0;
    double bestBoundaryOffset = std::numeric_limits<double>::max();
    QgsFeature bestMatch;
    int totalCandidates = 0;
    int validCandidates = 0;
    int spatialFilteredCandidates = 0;
    int distanceFilteredCandidates = 0;

    while (iter.nextFeature(databaseFeature)) {
        totalCandidates++;

        if (!databaseFeature.hasGeometry()) {
            continue;
        }
        validCandidates++;

        QgsGeometry dbGeom = databaseFeature.geometry();

        // 转到解析图层的 CRS
        if (needsTransform) {
            try {
                dbGeom.transform(transform);
            }
            catch (const QgsCsException& e) {
                logTextEdit->append(QString("坐标转换失败: %1").arg(e.what()));
                continue;
            }
        }

        // 边界框预筛选（保险：provider 不支持时仍可用）
        if (!searchBounds.intersects(dbGeom.boundingBox())) {
            continue;
        }
        spatialFilteredCandidates++;

        // 仅比较边界；线在面内部不能据此认定为对应要素。
        const QgsGeometry dbBoundary = rightAngleBoundary(dbGeom);
        if (dbBoundary.isNull() || dbBoundary.isEmpty() || !dbBoundary.isGeosValid()) continue;
        const double distanceToParsed = dbBoundary.distance(parsedBoundary);
        if (distanceToParsed < 0.0 || distanceToParsed > tolerance + 1e-8) {
            continue;
        }
        distanceFilteredCandidates++;

        // 解析边界在库体边界容差范围内的长度占比。
        double similarity = calculateRightAngleSimilarity(parsedGeom, dbGeom, tolerance);

        if (similarity < bestSimilarity - 1e-9) continue;
        // Buffer coverage saturates at 1 for nearby candidates. Break ties by
        // directed mean distance, so large reference polygons do not penalize
        // short correct fragments and provider iteration order is irrelevant.
        double weightedOffset = 0.0;
        double sampledLength = 0.0;
        for (const auto* line : extractLineStrings(parsedBoundary)) {
            if (!line) continue;
            for (int i = 1; i < line->numPoints(); ++i) {
                const QgsPointXY start(line->pointN(i - 1));
                const QgsPointXY end(line->pointN(i));
                const double length = start.distance(end);
                if (length <= 0.0) continue;
                for (double fraction : { 0.25, 0.5, 0.75 }) {
                    const QgsPointXY sample(start.x() + (end.x() - start.x()) * fraction,
                                            start.y() + (end.y() - start.y()) * fraction);
                    const double distance = dbBoundary.distance(QgsGeometry::fromPointXY(sample));
                    if (distance < 0.0) continue;
                    weightedOffset += length * distance;
                    sampledLength += length;
                }
            }
        }
        const double boundaryOffset = sampledLength > 0.0
            ? weightedOffset / sampledLength : std::numeric_limits<double>::max();
        if (similarity > bestSimilarity + 1e-9 ||
            (std::abs(similarity - bestSimilarity) <= 1e-9 && boundaryOffset < bestBoundaryOffset)) {
            bestSimilarity = similarity;
            bestBoundaryOffset = boundaryOffset;
            bestMatch = databaseFeature;
        }
    }

    // 诊断输出
    logTextEdit->append(QString("匹配诊断 - 总候选:%1, 有效几何:%2, 空间筛选后:%3, 边界距离筛选后:%4, 最佳边界覆盖率:%5, 阈值:%6")
        .arg(totalCandidates)
        .arg(validCandidates)
        .arg(spatialFilteredCandidates)
        .arg(distanceFilteredCandidates)
        .arg(bestSimilarity, 0, 'f', 3)
        .arg(similarityThreshold, 0, 'f', 3));

    // 不再进行“全量匹配”兜底；不再动态下调相似度阈值
    if (bestMatch.isValid() && bestSimilarity >= similarityThreshold) {
        // 匹配阶段可能对库体几何做了 CRS 转换。后续角点提取、角点对应和
        // 错误几何输出都必须使用解析图层 CRS，否则会把库体点画到错误位置。
        if (needsTransform) {
            try {
                QgsGeometry matchedGeometry = bestMatch.geometry();
                matchedGeometry.transform(transform);
                bestMatch.setGeometry(matchedGeometry);
                logTextEdit->append("已将匹配库体要素转换到解析图层 CRS");
            }
            catch (const QgsCsException& e) {
                logTextEdit->append(QString("匹配库体要素 CRS 转换失败，跳过该匹配: %1").arg(e.what()));
                return invalidFeature;
            }
        }
        logTextEdit->append(QString("找到匹配要素 ID:%1, 相似度:%2").arg(bestMatch.id()).arg(bestSimilarity, 0, 'f', 3));
        return bestMatch;
    }

    logTextEdit->append("未找到匹配的库体要素（严格过滤下）");
    return invalidFeature;
}
//直角化阈值检查错误输出
QgsGeometry ReadShpDialog::createRightAngleErrorGeometry(const QgsGeometry& parsedGeometry, const QgsPointXY& errorPoint, double tolerance)
{
    // 创建一个包含错误点和周围线段的复合几何体
    QgsGeometryCollection collection;

    // 1. 添加错误点的缓冲区（圆形标记）
    QgsGeometry pointBuffer = QgsGeometry::fromPointXY(errorPoint).buffer(tolerance * 2, 16);
    collection.addGeometry(pointBuffer.constGet()->clone());

    if (parsedGeometry.type() == QgsWkbTypes::LineGeometry) {
        const QgsLineString* line = getFirstLineString(parsedGeometry);
        if (!line) {
            return QgsGeometry(collection.clone());
        }

        int bestStartIndex = -1;
        int bestEndIndex = -1;
        double minDistance = std::numeric_limits<double>::max();

        // 找到包含错误点的线段
        for (int i = 0; i < line->numPoints() - 1; ++i) {
            QgsPointXY segStart = QgsPointXY(line->pointN(i).x(), line->pointN(i).y());
            QgsPointXY segEnd = QgsPointXY(line->pointN(i + 1).x(), line->pointN(i + 1).y());

            QgsGeometry segmentGeom = QgsGeometry::fromPolylineXY({ segStart, segEnd });
            double distance = segmentGeom.distance(QgsGeometry::fromPointXY(errorPoint));

            if (distance < minDistance && distance <= tolerance * 3) {
                minDistance = distance;
                bestStartIndex = i;
                bestEndIndex = i + 1;
            }
        }

        // 2. 添加相关的线段（高亮显示问题区域）
        if (bestStartIndex >= 0 && bestEndIndex >= 0) {
            // 添加前一个线段（如果存在）
            if (bestStartIndex > 0) {
                QgsPointXY prevStart = QgsPointXY(line->pointN(bestStartIndex - 1).x(), line->pointN(bestStartIndex - 1).y());
                QgsPointXY prevEnd = QgsPointXY(line->pointN(bestStartIndex).x(), line->pointN(bestStartIndex).y());
                QgsGeometry prevSegment = QgsGeometry::fromPolylineXY({ prevStart, prevEnd });
                collection.addGeometry(prevSegment.constGet()->clone());
            }

            // 添加当前线段
            QgsPointXY segStart = QgsPointXY(line->pointN(bestStartIndex).x(), line->pointN(bestStartIndex).y());
            QgsPointXY segEnd = QgsPointXY(line->pointN(bestEndIndex).x(), line->pointN(bestEndIndex).y());
            QgsGeometry currentSegment = QgsGeometry::fromPolylineXY({ segStart, segEnd });
            collection.addGeometry(currentSegment.constGet()->clone());

            // 添加下一个线段（如果存在）
            if (bestEndIndex < line->numPoints() - 1) {
                QgsPointXY nextStart = QgsPointXY(line->pointN(bestEndIndex).x(), line->pointN(bestEndIndex).y());
                QgsPointXY nextEnd = QgsPointXY(line->pointN(bestEndIndex + 1).x(), line->pointN(bestEndIndex + 1).y());
                QgsGeometry nextSegment = QgsGeometry::fromPolylineXY({ nextStart, nextEnd });
                collection.addGeometry(nextSegment.constGet()->clone());
            }
        }
    }
    else if (parsedGeometry.type() == QgsWkbTypes::PolygonGeometry) {
        // 对于面几何体，获取其边界线并递归调用
        const QgsAbstractGeometry* geom = parsedGeometry.constGet();
        if (geom) {
            if (const QgsPolygon* polygon = qgsgeometry_cast<const QgsPolygon*>(geom)) {
                const QgsCurve* exteriorRing = polygon->exteriorRing();
                if (exteriorRing) {
                    QgsGeometry ringGeometry = QgsGeometry(exteriorRing->clone());
                    return createRightAngleErrorGeometry(ringGeometry, errorPoint, tolerance);
                }
            }
            else if (const QgsMultiPolygon* multiPolygon = qgsgeometry_cast<const QgsMultiPolygon*>(geom)) {
                if (multiPolygon->numGeometries() > 0) {
                    const QgsPolygon* firstPolygon = qgsgeometry_cast<const QgsPolygon*>(multiPolygon->geometryN(0));
                    if (firstPolygon && firstPolygon->exteriorRing()) {
                        QgsGeometry ringGeometry = QgsGeometry(firstPolygon->exteriorRing()->clone());
                        return createRightAngleErrorGeometry(ringGeometry, errorPoint, tolerance);
                    }
                }
            }
        }
    }

    return QgsGeometry(collection.clone());
}
//在解析要素中找到对应数据库要素中某点的对应位置
QgsPointXY ReadShpDialog::findCorrespondingPoint(const QgsGeometry& parsedGeom, const QgsGeometry& databaseGeom, const QgsPointXY& databasePoint, double tolerance)
{
    // 在解析要素中找到距离数据库点最近的顶点
    QgsPointXY closestPoint;
    double minDistance = std::numeric_limits<double>::max();

    if (parsedGeom.type() == QgsWkbTypes::LineGeometry) {
        std::vector<const QgsLineString*> lineStrings = extractLineStrings(parsedGeom);

        for (const QgsLineString* line : lineStrings) {
            if (!line) continue;

            for (int i = 0; i < line->numPoints(); i++) {
                QgsPointXY vertex(line->pointN(i));
                double distance = vertex.distance(databasePoint);

                if (distance < minDistance && distance <= tolerance) {
                    minDistance = distance;
                    closestPoint = vertex;
                }
            }
        }
    }

    return (minDistance <= tolerance) ? closestPoint : QgsPointXY();
}
//计算几何体在指定点位置的角度
double ReadShpDialog::calculateAngleAtPoint(const QgsGeometry& geometry, const QgsPointXY& point)
{
    if (geometry.type() != QgsWkbTypes::LineGeometry) {
        return 180.0; // 无效角度
    }

    std::vector<const QgsLineString*> lineStrings = extractLineStrings(geometry);

    for (const QgsLineString* line : lineStrings) {
        if (!line || line->numPoints() < 3) continue;

        // 找到最接近指定点的顶点
        int closestIndex = -1;
        double minDistance = std::numeric_limits<double>::max();

        for (int i = 1; i < line->numPoints() - 1; i++) {
            QgsPointXY vertex(line->pointN(i));
            double distance = vertex.distance(point);

            if (distance < minDistance) {
                minDistance = distance;
                closestIndex = i;
            }
        }

        if (closestIndex > 0 && closestIndex < line->numPoints() - 1) {
            QgsPointXY p1(line->pointN(closestIndex - 1));
            QgsPointXY vertex(line->pointN(closestIndex));
            QgsPointXY p2(line->pointN(closestIndex + 1));

            return calculateVertexAngle(p1, vertex, p2);
        }
    }

    return 180.0; // 无效角度
}
//提取直角化特征点
std::vector<RightAnglePoint> ReadShpDialog::extractRightAnglePoints(const QgsGeometry& geometry, double minAngle, double maxAngle,
                                                                double minSegmentLength)
{
    std::vector<RightAnglePoint> points;
    const double directionTolerance = rightAngleDirectionTolerance(m_config->getRightAngleTurningPointTolerance());
    // Keep each exterior/interior ring and each multipart line independent.
    for (const auto* line : extractLineStrings(geometry)) {
        if (!line) continue;
        const bool closed = line->isClosed();
        const int count = line->numPoints() - (closed ? 1 : 0);
        std::vector<int> vertices;
        for (int i = 0; i < count; ++i) {
            if (vertices.empty() || QgsPointXY(line->pointN(i)).distance(
                    QgsPointXY(line->pointN(vertices.back()))) > 1e-8)
                vertices.push_back(i);
        }
        if (closed && vertices.size() > 1 && QgsPointXY(line->pointN(vertices.front())).distance(
                QgsPointXY(line->pointN(vertices.back()))) <= 1e-8)
            vertices.pop_back();
        const int size = static_cast<int>(vertices.size());
        if (size < 3) continue;
        for (int i = closed ? 0 : 1; i < (closed ? size : size - 1); ++i) {
            const int vertexIndex = vertices[i];
            const QgsPointXY vertex(line->pointN(vertexIndex));
            const double localAngle = calculateVertexAngle(
                QgsPointXY(line->pointN(vertices[(i + size - 1) % size])), vertex,
                QgsPointXY(line->pointN(vertices[(i + 1) % size])));
            // Each effective arm can deviate from its initial segment by at
            // most directionTolerance. Outside this expanded angle interval a
            // vertex cannot qualify, so dense straight interiors need no trace.
            if (localAngle < minAngle - 2.0 * directionTolerance - 1e-8 ||
                localAngle > maxAngle + 2.0 * directionTolerance + 1e-8) continue;
            const auto backward = traceRightAngleArm(line, vertexIndex, -1, directionTolerance);
            const auto forward = traceRightAngleArm(line, vertexIndex, 1, directionTolerance);
            if (backward.endIndex < 0 || forward.endIndex < 0 || backward.endIndex == forward.endIndex) continue;
            if (backward.length + 1e-8 < minSegmentLength || forward.length + 1e-8 < minSegmentLength) continue;
            const QgsPointXY previous(line->pointN(backward.endIndex));
            const QgsPointXY next(line->pointN(forward.endIndex));
            const double angle = calculateVertexAngle(previous, vertex, next);
            if (isAngleInRange(angle, minAngle, maxAngle))
                points.push_back({ vertex, angle, backward.endIndex, vertexIndex, true });
        }
    }
    return points;
}
//直角化阈值检查中计算顶点角度
double ReadShpDialog::calculateVertexAngle(const QgsPointXY& p1, const QgsPointXY& vertex, const QgsPointXY& p2)
{
    // 计算两个向量
    double dx1 = p1.x() - vertex.x();
    double dy1 = p1.y() - vertex.y();
    double dx2 = p2.x() - vertex.x();
    double dy2 = p2.y() - vertex.y();

    // 计算向量长度
    double len1 = sqrt(dx1 * dx1 + dy1 * dy1);
    double len2 = sqrt(dx2 * dx2 + dy2 * dy2);

    if (len1 == 0 || len2 == 0) {
        return 180.0; // 无效角度
    }

    // 计算点积
    double dotProduct = dx1 * dx2 + dy1 * dy2;

    // 计算余弦值
    double cosAngle = dotProduct / (len1 * len2);
    cosAngle = std::max(-1.0, std::min(1.0, cosAngle));

    // 计算角度（弧度转度）
    double angle = acos(cosAngle) * 180.0 / M_PI;

    return angle;
}
//直角化阈值检查角度是否在指定范围内
bool ReadShpDialog::isAngleInRange(double angle, double minAngle, double maxAngle)
{
    return angle >= minAngle && angle <= maxAngle;
}
//为“直角化角度阈值控制检查”的库体文件匹配阶段提供解释性日志，帮助定位某个候选为何被判为相似度不够或不理想
void ReadShpDialog::analyzeDetailedSimilarity(const QgsGeometry& geom1, const QgsGeometry& geom2, double tolerance, qint64 candidateId)
{
    if (geom1.isEmpty() || geom2.isEmpty()) {
        logTextEdit->append(QString("  候选要素%1: 几何体为空").arg(candidateId));
        return;
    }

    // 检查几何体有效性
    bool geom1Valid = geom1.isGeosValid();
    bool geom2Valid = geom2.isGeosValid();
    logTextEdit->append(QString("  候选要素%1: 几何体有效性 - 解析要素:%2, 数据库要素:%3")
        .arg(candidateId).arg(geom1Valid ? "有效" : "无效").arg(geom2Valid ? "有效" : "无效"));

    if (!geom1Valid || !geom2Valid) {
        logTextEdit->append(QString("  候选要素%1: 由于几何体无效，给予基础分0.8").arg(candidateId));
        return;
    }

    double length1 = geom1.length();
    double length2 = geom2.length();
    double avgLength = (length1 + length2) / 2.0;

    logTextEdit->append(QString("  候选要素%1: 长度信息 - 解析要素:%.2f, 数据库要素:%.2f, 平均:%.2f")
        .arg(candidateId).arg(length1).arg(length2).arg(avgLength));

    // 长度相似度分析
    double lengthSimilarity = 0.8;
    if (avgLength > 0) {
        double lengthRatio = std::min(length1, length2) / std::max(length1, length2);
        lengthSimilarity = std::max(0.8, lengthRatio * 0.2 + 0.8);
        logTextEdit->append(QString("  候选要素%1: 长度相似度 - 比率:%.3f, 相似度:%.3f")
            .arg(candidateId).arg(lengthRatio).arg(lengthSimilarity));
    }
    else {
        logTextEdit->append(QString("  候选要素%1: 长度相似度 - 平均长度为0，使用基础分:%.3f")
            .arg(candidateId).arg(lengthSimilarity));
    }

    //距离相似度分析
    double distance = geom1.distance(geom2);
    double distanceThreshold = std::max(tolerance * 50.0, avgLength * 10.0);
    double distanceSimilarity = 0.8;

    if (distance <= distanceThreshold) {
        distanceSimilarity = std::max(0.8, 1.0 - (distance / distanceThreshold) * 0.2);
        logTextEdit->append(QString("  候选要素%1: 距离相似度 - 距离:%.2f, 阈值:%.2f, 相似度:%.3f")
            .arg(candidateId).arg(distance).arg(distanceThreshold).arg(distanceSimilarity));
    }
    else {
        logTextEdit->append(QString("  候选要素%1: 距离相似度 - 距离:%.2f > 阈值:%.2f, 使用基础分:%.3f")
            .arg(candidateId).arg(distance).arg(distanceThreshold).arg(distanceSimilarity));
    }

    // 空间关系分析
    double spatialSimilarity = 0.8;
    try {
        double bufferSize = std::max(tolerance * 20.0, avgLength * 1.0);
        QgsGeometry buffer1 = geom1.buffer(bufferSize, 4);

        if (!buffer1.isEmpty()) {
            bool intersects = buffer1.intersects(geom2);
            if (intersects) {
                spatialSimilarity = 0.95;
                logTextEdit->append(QString("  候选要素%1: 空间关系 - 缓冲区:%.2f, 有交集, 相似度:%.3f")
                    .arg(candidateId).arg(bufferSize).arg(spatialSimilarity));
            }
            else {
                logTextEdit->append(QString("  候选要素%1: 空间关系 - 缓冲区:%.2f, 无交集, 使用基础分:%.3f")
                    .arg(candidateId).arg(bufferSize).arg(spatialSimilarity));
            }
        }
        else {
            logTextEdit->append(QString("  候选要素%1: 空间关系 - 缓冲区创建失败, 使用基础分:%.3f")
                .arg(candidateId).arg(spatialSimilarity));
        }
    }
    catch (const std::exception& e) {
        spatialSimilarity = 0.9;
        logTextEdit->append(QString("  候选要素%1: 空间关系 - 异常:%2, 使用异常分:%.3f")
            .arg(candidateId).arg(e.what()).arg(spatialSimilarity));
    }
    catch (...) {
        spatialSimilarity = 0.9;
        logTextEdit->append(QString("  候选要素%1: 空间关系 - 未知异常, 使用异常分:%.3f")
            .arg(candidateId).arg(spatialSimilarity));
    }

    // 综合相似度计算
    double totalSimilarity = lengthSimilarity * 0.1 +      // 长度权重10%
        distanceSimilarity * 0.2 +     // 距离权重20%
        spatialSimilarity * 0.7;       // 空间关系权重70%

    totalSimilarity = std::max(0.8, totalSimilarity);

    logTextEdit->append(QString("  候选要素%1: 综合相似度计算 - 长度:%.3f(10%%) + 距离:%.3f(20%%) + 空间:%.3f(70%%) = %.3f")
        .arg(candidateId).arg(lengthSimilarity).arg(distanceSimilarity).arg(spatialSimilarity).arg(totalSimilarity));

    // 分析为什么相似度不够高
    if (totalSimilarity < 0.9) {
        logTextEdit->append(QString("  候选要素%1: 相似度偏低的主要原因:").arg(candidateId));
        if (lengthSimilarity < 0.9) {
            logTextEdit->append(QString("    - 长度差异较大 (相似度:%.3f)").arg(lengthSimilarity));
        }
        if (distanceSimilarity < 0.9) {
            logTextEdit->append(QString("    - 距离较远 (相似度:%.3f)").arg(distanceSimilarity));
        }
        if (spatialSimilarity < 0.95) {
            logTextEdit->append(QString("    - 空间关系不佳 (相似度:%.3f)").arg(spatialSimilarity));
        }
    }
}
//检查直角化点的一致性
std::vector<RightAnglePoint> ReadShpDialog::checkRightAnglePointsConsistency(const std::vector<RightAnglePoint>& parsedPoints, const std::vector<RightAnglePoint>& databasePoints, double tolerance)
{
    std::vector<RightAnglePoint> inconsistentPoints;
    double lowerLimit = m_config->getRightAngleLowerLimit();
    double upperLimit = m_config->getRightAngleUpperLimit();

    // 对于库体数据中的每个直角化特征点，检查解析数据中对应位置的角度
    for (const auto& dbPoint : databasePoints) {
        // 在解析数据中找到最近的点
        double minDistance = std::numeric_limits<double>::max();
        RightAnglePoint closestParsedPoint;
        bool foundMatch = false;

        for (const auto& parsedPoint : parsedPoints) {
            double distance = dbPoint.position.distance(parsedPoint.position);
            if (distance < minDistance && distance <= tolerance) {
                minDistance = distance;
                closestParsedPoint = parsedPoint;
                foundMatch = true;
            }
        }

        if (foundMatch) {
            // 检查角度是否在标准范围内
            if (!isAngleInRange(closestParsedPoint.angle, lowerLimit, upperLimit)) {
                inconsistentPoints.push_back(closestParsedPoint);
            }
        }
    }

    return inconsistentPoints;
}
//查找最佳匹配的要素
QgsFeature ReadShpDialog::findBestMatchingFeature(const QgsFeature& parsedFeature, QgsVectorLayer* referenceLayer)
{
    QgsGeometry parsedGeom = parsedFeature.geometry();
    QgsRectangle searchBounds = parsedGeom.boundingBox().buffered(m_quickDistanceThreshold);

    // 使用QGIS内置的空间过滤，比自定义索引更高效
    QgsFeatureRequest request;
    request.setFilterRect(searchBounds);
    request.setLimit(m_maxCandidatesPerFeature); // 限制候选数量

    QgsFeatureIterator iter = referenceLayer->getFeatures(request);
    QgsFeature candidate;
    QgsFeature bestMatch;
    double bestScore = 0.0;

    // 获取库体文件名
    QString referenceFileName = referenceLayer->name();

    int candidateCount = 0;
    int filteredCount = 0;
    int typeFilteredCount = 0;

    // 添加详细调试信息
    qDebug() << "=== findBestMatchingFeature 调试信息 ===";
    qDebug() << "库体文件名:" << referenceFileName;
    qDebug() << "搜索范围:" << searchBounds.toString();

    // 获取解析文件中与当前要素相交的要素列表（用于拓扑验证）
    std::vector<QgsFeature> parsedIntersectingFeatures = getIntersectingFeatures(parsedFeature, currentLayer);

    while (iter.nextFeature(candidate) && candidateCount < m_maxCandidatesPerFeature) {
        candidateCount++;

        QgsGeometry candidateGeom = candidate.geometry();
        if (candidateGeom.type() != QgsWkbTypes::LineGeometry) {
            filteredCount++;
            continue;
        }

        // 计算几何相似度
        double geometricScore = calculateQuickSimilarity(parsedGeom, candidateGeom);

        // 计算拓扑一致性评分
        double topologyScore = calculateTopologyConsistency(candidate, referenceLayer, parsedIntersectingFeatures);

        // 综合评分：几何相似度50% + 拓扑一致性50%
        double totalScore = geometricScore * 0.5 + topologyScore * 0.5;

        qDebug() << "候选要素" << candidateCount << "几何评分:" << geometricScore << "拓扑评分:" << topologyScore << "总评分:" << totalScore;

        if (totalScore > bestScore) {
            bestScore = totalScore;
            bestMatch = candidate;
        }
    }

    qDebug() << "匹配统计 - 候选要素总数:" << candidateCount
        << ", 类型过滤数:" << typeFilteredCount
        << ", 总过滤数:" << filteredCount
        << ", 最佳评分:" << bestScore;

    // 设置更严格的匹配阈值，要求拓扑一致性
    if (bestScore > 0.3 && bestMatch.isValid()) { // 提高阈值从0.0到0.3
        qDebug() << "找到匹配要素，评分:" << bestScore;
        return bestMatch;
    }

    qDebug() << "未找到匹配要素，最佳评分:" << bestScore;
    return QgsFeature(); // 返回无效要素
}
//直角化角度阈值控制的线要素相似度
double ReadShpDialog::calculateRightAngleSimilarity(const QgsGeometry& geom1, const QgsGeometry& geom2, double tolerance)
{
    const QgsGeometry parsedBoundary = rightAngleBoundary(geom1);
    const QgsGeometry referenceBoundary = rightAngleBoundary(geom2);
    if (parsedBoundary.isNull() || referenceBoundary.isNull() ||
        !parsedBoundary.isGeosValid() || !referenceBoundary.isGeosValid()) return 0.0;
    const double length = parsedBoundary.length();
    if (length <= 0.0) return 0.0;
    // Directed coverage permits short outline fragments, but rejects hatching
    // which merely intersects a polygon or crosses its boundary at one point.
    const QgsGeometry neighborhood = referenceBoundary.buffer(std::max(1e-8, tolerance), 8);
    if (neighborhood.isNull() || neighborhood.isEmpty()) return 0.0;
    const QgsGeometry supported = parsedBoundary.intersection(neighborhood);
    if (supported.isNull() || supported.isEmpty()) return 0.0;
    return std::max(0.0, std::min(1.0, supported.length() / length));
}
//创建线段错误几何体（用于标记有问题的线段）
QgsGeometry ReadShpDialog::createSegmentErrorGeometry(const QgsLineString* line, int startIndex, int endIndex)
{
    if (!line || startIndex < 0 || endIndex >= line->numPoints() || startIndex > endIndex) {
        return QgsGeometry();
    }

    QgsLineString* errorSegment = new QgsLineString();
    for (int i = startIndex; i <= endIndex; i++) {
        errorSegment->addVertex(line->pointN(i));
    }

    return QgsGeometry(errorSegment);
}

// ========================= 水系走向错误（主入口与子函数） =========================
// 走向错误检查主函数
std::vector<QualityCheckResult> ReadShpDialog::checkWaterSystemDirection(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile)
{
    std::vector<QualityCheckResult> results;

    if (!parsedLayer || !referenceLayer || !parsedLayer->isValid() || !referenceLayer->isValid()) {
        executedDirection = false;
        logTextEdit->append("错误: 图层为空，无法进行水系走向检查");
        return results;
    }
    executedDirection = true;

    // 获取配置参数
   // 获取“经比例尺转换”的参数（替代原先直接从 m_config 读取的原始值）
    double intersectionDetectionTolerance = m_intersectionDetectionTolerance;
    const double intersectionAngleTolerance = std::clamp(
        m_config->getParameterDouble(QStringLiteral("几何走向异常检查"),
            QStringLiteral("intersection_angle_tolerance"), 120.0), 0.0, 180.0);
    double minRiverLength = m_minRiverLength;
    double danglingDetectionTolerance = m_directionDanglingDetectionTolerance;
    double connectionTolerance = m_connectivityTolerance;

    logTextEdit->append(QString("=== 开始水系走向错误检查 ==="));
    logTextEdit->append(QString("配置参数 - 交叉检测容差: %1, 最小河流长度: %2, 悬挂检测容差: %3, 连接容差: %4")
        .arg(intersectionDetectionTolerance).arg(minRiverLength).arg(danglingDetectionTolerance).arg(connectionTolerance));
    // 统一前置无效几何体检查：记录错误
    if (collectInvalidGeometry(parsedLayer, sourceFile, CheckType::DIRECTION_ERROR, results)) {
        logTextEdit->append(QString::fromUtf8("发现解析图层存在无效几何体，已记录并跳过对应要素，继续检查"));
    }
    QString refSource = referenceFilePath.isEmpty() ? referenceLayer->name() : referenceFilePath;
    if (collectInvalidGeometry(referenceLayer, refSource, CheckType::DIRECTION_ERROR, results)) {
        logTextEdit->append(QString::fromUtf8("发现库体图层存在无效几何体，已记录并跳过对应要素，继续检查"));
    }

    int errorCount = 0;
    int angleErrors = 0;

    // 第一步：收集所有河流线段信息
    logTextEdit->append("=== 第一步：收集河流线段信息 ===");

    struct RiverSegmentSimple {
        QgsFeature feature;
        QgsGeometry geometry;
        double strokeWidth;
        double length;
        QgsPoint startPoint;
        QgsPoint endPoint;
        bool visited = false;  // 用于河流追踪
    };

    std::vector<RiverSegmentSimple> riverSegments;
    int totalSegments = 0, validSegments = 0;
    int skippedGeometry = 0, skippedLength = 0, invalidWidth = 0;

    QgsFeatureIterator widthIterator = parsedLayer->getFeatures();
    QgsFeature widthFeature;
    while (widthIterator.nextFeature(widthFeature)) {
        totalSegments++;

        if (!widthFeature.hasGeometry()) {
            skippedGeometry++;
            continue;
        }

        QgsGeometry geom = widthFeature.geometry();
        if (geom.isNull() || geom.isEmpty()) {
            skippedGeometry++;
            continue;
        }

        // 处理MultiLineString
        if (geom.wkbType() == QgsWkbTypes::MultiLineString) {
            QgsMultiLineString* multiLine = qgsgeometry_cast<QgsMultiLineString*>(geom.get());
            if (multiLine && multiLine->numGeometries() > 0) {
                geom = QgsGeometry(multiLine->geometryN(0)->clone());
            }
            else {
                skippedGeometry++;
                continue;
            }
        }

        double length = geom.length();
        if (length < minRiverLength) {
            skippedLength++;
            continue;
        }

        // 解析 stroke_wid 属性，不跳过无效宽度的线段
        QVariant strokeWidVar = widthFeature.attribute("stroke_wid");
        double strokeWidth = 1.0; // 默认宽度

        if (!strokeWidVar.isNull() && !strokeWidVar.toString().isEmpty()) {
            QString strokeWidStr = strokeWidVar.toString().trimmed();

            // 处理多值情况（分号或逗号分隔），取第一个值
            QStringList widthValues;
            if (strokeWidStr.contains(';')) {
                widthValues = strokeWidStr.split(';', Qt::SkipEmptyParts);
            }
            else if (strokeWidStr.contains(',')) {
                widthValues = strokeWidStr.split(',', Qt::SkipEmptyParts);
            }
            else {
                widthValues << strokeWidStr;
            }

            if (!widthValues.isEmpty()) {
                bool ok;
                double parsedWidth = widthValues[0].trimmed().toDouble(&ok);
                if (ok && parsedWidth > 0) {
                    strokeWidth = parsedWidth;
                }
                else {
                    invalidWidth++;
                    // 不跳过，使用默认宽度1.0继续处理
                    logTextEdit->append(QString("  警告: 要素ID %1 宽度值无效 '%2'，使用默认宽度1.0m")
                        .arg(widthFeature.id()).arg(strokeWidStr));
                }
            }
        }
        else {
            invalidWidth++;
            // 不跳过，使用默认宽度1.0继续处理
        }

        const QgsLineString* line = qgsgeometry_cast<const QgsLineString*>(geom.constGet());
        if (!line || line->numPoints() < 2) {
            skippedGeometry++;
            continue;
        }

        QgsPoint startPoint = line->startPoint();
        QgsPoint endPoint = line->endPoint();

        RiverSegmentSimple segmentInfo;
        segmentInfo.feature = widthFeature;
        segmentInfo.geometry = geom;
        segmentInfo.strokeWidth = strokeWidth;
        segmentInfo.length = length;
        segmentInfo.startPoint = startPoint;
        segmentInfo.endPoint = endPoint;

        riverSegments.push_back(segmentInfo);
        validSegments++;

        // 显示前5个有效线段的详细信息
        if (validSegments <= 5) {
            logTextEdit->append(QString("  有效线段 %1: ID=%2, 长度=%3m, 宽度=%4m")
                .arg(validSegments)
                .arg(widthFeature.id())
                .arg(QString::number(length, 'f', 2))
                .arg(QString::number(strokeWidth, 'f', 1)));
        }
    }

    logTextEdit->append(QString("收集统计: 总线段=%1, 有效线段=%2, 跳过几何=%3, 跳过长度=%4, 无效宽度=%5")
        .arg(totalSegments).arg(validSegments).arg(skippedGeometry).arg(skippedLength).arg(invalidWidth));

    if (validSegments < 3) {
        logTextEdit->append("有效线段数量不足，无法进行河流宽度变化检查（至少需要3个线段）");
        return results;
    }
    //第二步：基于角度连续性识别河流主干流向的三段连续线段
    logTextEdit->append("=== 第二步：基于角度连续性识别河流主干流向的三段连续线段 ===");

    std::vector<std::vector<int>> threeSegmentGroups;
    int groupCount = 0;

    //计算线段的方向角度（弧度）
    auto calculateSegmentAngle = [](const QgsPoint& start, const QgsPoint& end) -> double {
        double dx = end.x() - start.x();
        double dy = end.y() - start.y();
        return std::atan2(dy, dx);
    };

    //计算两个角度之间的差值（考虑周期性）
    auto calculateAngleDifference = [](double angle1, double angle2) -> double {
        double diff = std::abs(angle1 - angle2);
        if (diff > M_PI) {
            diff = 2 * M_PI - diff;
        }
        return diff;
    };

    //为每个线段作为起点，沿流向查找连续的三段路径
    for (size_t i = 0; i < riverSegments.size(); i++) {
        const auto& startSegment = riverSegments[i];

        // 查找与起始线段连接的下一个线段
        for (size_t j = 0; j < riverSegments.size(); j++) {
            if (i == j) continue;

            const auto& middleSegment = riverSegments[j];

            // 检查起始线段和中间线段的连接
            double dist1 = startSegment.endPoint.distance(middleSegment.startPoint);
            double dist2 = startSegment.endPoint.distance(middleSegment.endPoint);
            double dist3 = startSegment.startPoint.distance(middleSegment.startPoint);
            double dist4 = startSegment.startPoint.distance(middleSegment.endPoint);

            double minDist12 = std::min({ dist1, dist2, dist3, dist4 });

            if (minDist12 <= connectionTolerance) { // 连接容差
                // 确定连接点和流向
                QgsPoint connectionPoint;
                QgsPoint middleSegmentOtherEnd;
                QgsPoint startSegmentStart, startSegmentEnd;
                QgsPoint middleSegmentStart, middleSegmentEnd;
                bool validConnection = false;

                if (dist1 <= connectionTolerance) { // startSegment.end -> middleSegment.start
                    connectionPoint = startSegment.endPoint;
                    middleSegmentOtherEnd = middleSegment.endPoint;
                    startSegmentStart = startSegment.startPoint;
                    startSegmentEnd = startSegment.endPoint;
                    middleSegmentStart = middleSegment.startPoint;
                    middleSegmentEnd = middleSegment.endPoint;
                    validConnection = true;
                }
                else if (dist4 <= connectionTolerance) { // startSegment.start -> middleSegment.end
                    connectionPoint = startSegment.startPoint;
                    middleSegmentOtherEnd = middleSegment.startPoint;
                    startSegmentStart = startSegment.endPoint;
                    startSegmentEnd = startSegment.startPoint;
                    middleSegmentStart = middleSegment.endPoint;
                    middleSegmentEnd = middleSegment.startPoint;
                    validConnection = true;
                }

                if (!validConnection) continue;

                // 计算第一段和第二段的角度
                double angle1 = calculateSegmentAngle(startSegmentStart, startSegmentEnd);
                double angle2 = calculateSegmentAngle(middleSegmentStart, middleSegmentEnd);
                double angleDiff12 = calculateAngleDifference(angle1, angle2);

                // 查找与中间线段连接的第三个线段，确保流向连续
                for (size_t k = 0; k < riverSegments.size(); k++) {
                    if (k == i || k == j) continue;

                    const auto& endSegment = riverSegments[k];

                    // 检查中间线段的另一端是否与第三个线段连接
                    double dist31 = middleSegmentOtherEnd.distance(endSegment.startPoint);
                    double dist32 = middleSegmentOtherEnd.distance(endSegment.endPoint);

                    double minDist23 = std::min(dist31, dist32);

                    if (minDist23 <= connectionTolerance) {
                        // 确定第三段的方向
                        QgsPoint endSegmentStart, endSegmentEnd;
                        if (dist31 <= connectionTolerance) { // middleSegment.otherEnd -> endSegment.start
                            endSegmentStart = endSegment.startPoint;
                            endSegmentEnd = endSegment.endPoint;
                        }
                        else { // middleSegment.otherEnd -> endSegment.end
                            endSegmentStart = endSegment.endPoint;
                            endSegmentEnd = endSegment.startPoint;
                        }

                        // 计算第二段和第三段的角度
                        double angle3 = calculateSegmentAngle(endSegmentStart, endSegmentEnd);
                        double angleDiff23 = calculateAngleDifference(angle2, angle3);

                        // 计算总的角度变化
                        double totalAngleChange = angleDiff12 + angleDiff23;

                        // 统计连接点处的线段数量
                        int connectionsAtStart = 0;
                        int connectionsAtMiddle = 0;

                        // 统计在起始连接点的线段数量
                        for (size_t check = 0; check < riverSegments.size(); check++) {
                            if (check == i) continue;
                            const auto& checkSegment = riverSegments[check];
                            double checkDist1 = connectionPoint.distance(checkSegment.startPoint);
                            double checkDist2 = connectionPoint.distance(checkSegment.endPoint);
                            if (std::min(checkDist1, checkDist2) <= connectionTolerance) {
                                connectionsAtStart++;
                            }
                        }

                        // 统计在中间连接点的线段数量
                        for (size_t check = 0; check < riverSegments.size(); check++) {
                            if (check == j) continue;
                            const auto& checkSegment = riverSegments[check];
                            double checkDist1 = middleSegmentOtherEnd.distance(checkSegment.startPoint);
                            double checkDist2 = middleSegmentOtherEnd.distance(checkSegment.endPoint);
                            if (std::min(checkDist1, checkDist2) <= connectionTolerance) {
                                connectionsAtMiddle++;
                            }
                        }

                        // 主干流向判断条件：
                        // 1. 连接点处线段数量不超过2个（避免多路汇入）
                        // 2. 总角度变化小于90度（约1.57弧度），表示流向相对连续
                        // 3. 单段角度变化都小于60度（约1.05弧度），避免急转弯
                        bool isMainStream = (connectionsAtStart <= 2 && connectionsAtMiddle <= 2) &&
                            (totalAngleChange < 1.57) &&  // 总角度变化 < 90度
                            (angleDiff12 < 1.05 && angleDiff23 < 1.05);  // 单段角度变化 < 60度

                        if (isMainStream) {
                            std::vector<int> threeSegmentGroup = { static_cast<int>(i), static_cast<int>(j), static_cast<int>(k) };

                            // 检查是否已经存在相同的组合（避免重复）
                            bool alreadyExists = false;
                            for (const auto& existingGroup : threeSegmentGroups) {
                                std::vector<int> sortedExisting = existingGroup;
                                std::vector<int> sortedNew = threeSegmentGroup;
                                std::sort(sortedExisting.begin(), sortedExisting.end());
                                std::sort(sortedNew.begin(), sortedNew.end());
                                if (sortedExisting == sortedNew) {
                                    alreadyExists = true;
                                    break;
                                }
                            }

                            if (!alreadyExists) {
                                threeSegmentGroups.push_back(threeSegmentGroup);
                                groupCount++;

                                logTextEdit->append(QString("  发现主干流向三段连续线段 %1: ID=%2→%3→%4")
                                    .arg(groupCount)
                                    .arg(riverSegments[i].feature.id())
                                    .arg(riverSegments[j].feature.id())
                                    .arg(riverSegments[k].feature.id()));

                                // 显示宽度序列
                                QString widthSequence = QString("%1→%2→%3")
                                    .arg(QString::number(riverSegments[i].strokeWidth, 'f', 1))
                                    .arg(QString::number(riverSegments[j].strokeWidth, 'f', 1))
                                    .arg(QString::number(riverSegments[k].strokeWidth, 'f', 1));
                                logTextEdit->append(QString("    宽度序列: %1").arg(widthSequence));

                                // 显示角度信息
                                logTextEdit->append(QString("    角度变化: 第1-2段=%.1f°, 第2-3段=%.1f°, 总变化=%.1f°")
                                    .arg(angleDiff12 * 180.0 / M_PI)
                                    .arg(angleDiff23 * 180.0 / M_PI)
                                    .arg(totalAngleChange * 180.0 / M_PI));

                                // 显示连接点信息
                                logTextEdit->append(QString("    连接点线段数: 起始=%1, 中间=%2")
                                    .arg(connectionsAtStart + 1).arg(connectionsAtMiddle + 1));
                            }
                        }
                        else {
                            // 记录被排除的情况
                            QString reason;
                            if (connectionsAtStart > 2 || connectionsAtMiddle > 2) {
                                reason = QString("多路汇入(起始%1个,中间%2个连接)").arg(connectionsAtStart + 1).arg(connectionsAtMiddle + 1);
                            }
                            else if (totalAngleChange >= 1.57) {
                                reason = QString("总角度变化过大(%.1f°)").arg(totalAngleChange * 180.0 / M_PI);
                            }
                            else if (angleDiff12 >= 1.05 || angleDiff23 >= 1.05) {
                                reason = QString("单段角度变化过大(%.1f°,%.1f°)").arg(angleDiff12 * 180.0 / M_PI).arg(angleDiff23 * 180.0 / M_PI);
                            }

                            logTextEdit->append(QString("  排除非主干流向: ID=%1→%2→%3 (%4)")
                                .arg(riverSegments[i].feature.id())
                                .arg(riverSegments[j].feature.id())
                                .arg(riverSegments[k].feature.id())
                                .arg(reason));
                        }
                    }
                }
            }
        }
    }

    logTextEdit->append(QString("找到 %1 个符合主干流向条件的三段连续线段组合").arg(groupCount));

    // 第三步：检查主干流向三段连续线段的宽度变化
    if (groupCount > 0) {
        logTextEdit->append("=== 第三步：主干流向三段连续线段宽度变化检查 ===");

        double minValidWidth = 0.1;
        int totalWidthErrors = 0;
        int totalCheckedGroups = 0;

        for (const auto& threeSegmentGroup : threeSegmentGroups) {
            totalCheckedGroups++;

            int idx1 = threeSegmentGroup[0];
            int idx2 = threeSegmentGroup[1];
            int idx3 = threeSegmentGroup[2];

            const auto& segment1 = riverSegments[idx1];
            const auto& segment2 = riverSegments[idx2];
            const auto& segment3 = riverSegments[idx3];

            // 检查宽度是否有效
            if (segment1.strokeWidth <= minValidWidth ||
                segment2.strokeWidth <= minValidWidth ||
                segment3.strokeWidth <= minValidWidth) {
                logTextEdit->append(QString("  组合 %1: 存在无效宽度，跳过检查").arg(totalCheckedGroups));
                continue;
            }

            double width1 = segment1.strokeWidth;
            double width2 = segment2.strokeWidth;
            double width3 = segment3.strokeWidth;

            QgsFeatureId featureId1 = segment1.feature.id();
            QgsFeatureId featureId2 = segment2.feature.id();
            QgsFeatureId featureId3 = segment3.feature.id();

            // 检测"大→小→大"或"小→大→小"模式
            bool pattern1 = (width1 > width2) && (width3 > width2); // 大→小→大
            bool pattern2 = (width1 < width2) && (width3 < width2); // 小→大→小

            if (pattern1 || pattern2) {
                totalWidthErrors++;

                QString patternType = pattern1 ? "大→小→大" : "小→大→小";

                // 创建质量检查结果
                QualityCheckResult result;
                result.passed = false;
                result.checkType = CheckType::DIRECTION_ERROR;
                result.errorType = "水系走向错误";
                result.description = QString("河流主干流向宽度变化异常 - 检测到%1模式：%2m→%3m→%4m，违反河流宽度单调变化规律")
                    .arg(patternType)
                    .arg(QString::number(width1, 'f', 1))
                    .arg(QString::number(width2, 'f', 1))
                    .arg(QString::number(width3, 'f', 1));

                // 使用中间线段的几何信息
                result.feature = segment2.feature;
                result.geometry = segment2.geometry;
                result.errorGeometry = segment2.geometry;
                result.sourceFile = sourceFile;
                result.featureId = featureId2;

                results.push_back(result);

                logTextEdit->append(QString("  ✗ 发现宽度变化错误 %1: ID=%2→%3→%4, 模式=%5")
                    .arg(totalWidthErrors)
                    .arg(featureId1).arg(featureId2).arg(featureId3)
                    .arg(patternType));

                logTextEdit->append(QString("    宽度序列: %1m→%2m→%3m")
                    .arg(QString::number(width1, 'f', 1))
                    .arg(QString::number(width2, 'f', 1))
                    .arg(QString::number(width3, 'f', 1)));
            }
            else {
                logTextEdit->append(QString("  ✓ 组合 %1: 宽度变化正常 - %2m→%3m→%4m")
                    .arg(totalCheckedGroups)
                    .arg(QString::number(width1, 'f', 1))
                    .arg(QString::number(width2, 'f', 1))
                    .arg(QString::number(width3, 'f', 1)));
            }
        }

        logTextEdit->append(QString("=== 河流主干流向宽度变化检查完成 ==="));
        logTextEdit->append(QString("检查了 %1 个主干流向三段线段组合，发现 %2 个宽度变化错误")
            .arg(totalCheckedGroups).arg(totalWidthErrors));
    }
    else {
        logTextEdit->append("未找到符合条件的主干流向三段连续线段，无法进行宽度变化检查");
    }

    // 继续原有的交叉角度检查逻辑
    logTextEdit->append("=== 开始交叉角度检查 ===");
    int angleCheckFeatures = 0;


    QgsFeatureIterator iterator = parsedLayer->getFeatures();
    QgsFeature feature;
    while (iterator.nextFeature(feature)) {
        angleCheckFeatures++;

        QgsGeometry parsedGeom = feature.geometry();
        if (parsedGeom.isNull() || parsedGeom.isEmpty()) {
            continue;
        }

        if (parsedGeom.type() != QgsWkbTypes::LineGeometry) {
            continue;
        }

        double length = parsedGeom.length();
        if (length < minRiverLength) {
            continue;
        }

        if (isHangingWaterFeature(parsedGeom, parsedLayer, danglingDetectionTolerance)) {
            continue;
        }

        if (!hasIntersectingWaterFeatures(parsedGeom, parsedLayer, intersectionDetectionTolerance)) {
            continue;
        }

        // 在库体文件中找到最匹配的要素
        QgsFeature matchedFeature = findBestMatchingFeature(feature, referenceLayer);
        if (!matchedFeature.isValid()) {
            continue;
        }

        // 检查交叉角度一致性
        bool hasAngleError = false;
        QString angleErrorDetails;

        // 获取解析文件中的交叉点和角度
        std::vector<IntersectionAngleInfo> parsedIntersections = findInternalIntersectionAngles(parsedGeom, parsedLayer, intersectionDetectionTolerance);

        // 获取库体文件中对应要素的交叉点和角度
        std::vector<IntersectionAngleInfo> libraryIntersections = findInternalIntersectionAngles(matchedFeature.geometry(), referenceLayer, intersectionDetectionTolerance);


        // 比较对应的交叉角度
        for (const auto& parsedIntersection : parsedIntersections) {
            // 在库体交叉点中找到对应的交叉点
            IntersectionAngleInfo correspondingLibraryIntersection;
            bool foundCorresponding = false;
            double bestMatchDistance = std::numeric_limits<double>::max();
            double bestAngleSimilarity = std::numeric_limits<double>::max();

            for (const auto& libraryIntersection : libraryIntersections) {
                double distance = parsedIntersection.intersectionPoint.distance(libraryIntersection.intersectionPoint);

                if (distance <= intersectionDetectionTolerance) {
                    // 计算角度相似性（角度差异）
                    double angleDiff = std::abs(parsedIntersection.intersectionAngle - libraryIntersection.intersectionAngle);
                    if (angleDiff > 180.0) {
                        angleDiff = 360.0 - angleDiff;
                    }

                    // 优先选择位置最近且角度最相似的匹配
                    double combinedScore = distance + angleDiff * 0.1; // 给角度差异一个权重

                    if (combinedScore < bestMatchDistance + bestAngleSimilarity * 0.1) {
                        correspondingLibraryIntersection = libraryIntersection;
                        foundCorresponding = true;
                        bestMatchDistance = distance;
                        bestAngleSimilarity = angleDiff;
                    }
                }
            }

            if (foundCorresponding) {
                const double angleDifference = std::abs(parsedIntersection.intersectionAngle - correspondingLibraryIntersection.intersectionAngle);
                const bool angleExceedsLimit = angleDifference > intersectionAngleTolerance;

                if (angleExceedsLimit) {
                    hasAngleError = true;
                    angleErrorDetails += QString("交叉点(%1,%2): 解析角度%3°, 库体角度%4°, 角度差异%5°; ")
                        .arg(parsedIntersection.intersectionPoint.x(), 0, 'f', 2)
                        .arg(parsedIntersection.intersectionPoint.y(), 0, 'f', 2)
                        .arg(parsedIntersection.intersectionAngle, 0, 'f', 1)
                        .arg(correspondingLibraryIntersection.intersectionAngle, 0, 'f', 1)
                        .arg(angleDifference, 0, 'f', 1);
                }
            }
        }
        if (hasAngleError) {
            QualityCheckResult result;
            result.passed = false;
            result.errorType = "水系走向错误";
            result.description = QString("河流交叉角度与库体数据不一致。%1").arg(angleErrorDetails);
            result.feature = feature;
            result.errorGeometry = parsedGeom;
            result.sourceFile = sourceFile;
            result.checkType = CheckType::DIRECTION_ERROR;
            result.featureId = feature.id();
            result.geometry = parsedGeom;
            const QString field = referenceLayer->fields().indexFromName(QStringLiteral("LEVEL")) >= 0
                ? QStringLiteral("LEVEL") : QStringLiteral("TYPE");
            result.referenceSource = referenceLayer->source();
            result.referenceFeatureId = matchedFeature.id();
            result.referenceValue = matchedFeature.attribute(field).toString().trimmed();
            result.referenceCategory = WaterResultClassification::categoryFor(field, result.referenceValue);
            results.push_back(result);
            angleErrors++;

            logTextEdit->append(QString("发现走向错误: 要素ID %1, %2").arg(feature.id()).arg(angleErrorDetails));
        }
    }

    logTextEdit->append(QString("=== 交叉角度检查完成 ==="));
    logTextEdit->append(QString("统计信息 - 检查要素: %1, 发现角度错误: %2").arg(angleCheckFeatures).arg(angleErrors));

    logTextEdit->append(QString("=== 水系走向错误检查总结 ==="));
    logTextEdit->append(QString("总错误数: %1").arg(results.size()));
    logTextEdit->append(QString("  - 宽度变化异常: %1").arg(errorCount));
    logTextEdit->append(QString("  - 角度错误: %1").arg(angleErrors));

    return results;
}
//走向错误检查计算线要素相似度
double ReadShpDialog::calculateLineSimilarity(const QgsGeometry& geom1, const QgsGeometry& geom2)
{
    if (geom1.type() != QgsWkbTypes::LineGeometry || geom2.type() != QgsWkbTypes::LineGeometry) {
        return 0.0;
    }

    double length1 = geom1.length();
    double length2 = geom2.length();

    if (length1 == 0 || length2 == 0) {
        return 0.0;
    }

    // 长度相似度
    double lengthSimilarity = std::min(length1, length2) / std::max(length1, length2);

    // 重叠度计算
    QgsGeometry buffer1 = geom1.buffer(m_distanceTolerance / 2.0, 8);
    QgsGeometry intersection = buffer1.intersection(geom2);
    double overlapLength = intersection.length();
    double overlapRatio = overlapLength / std::min(length1, length2);

    // 综合相似度（长度相似度权重0.3，重叠度权重0.7）
    return lengthSimilarity * 0.3 + overlapRatio * 0.7;
}
//查找内部交叉角度信息
std::vector<IntersectionAngleInfo> ReadShpDialog::findInternalIntersectionAngles(const QgsGeometry& targetGeom, QgsVectorLayer* layer, double tolerance)
{
    std::vector<IntersectionAngleInfo> intersectionAngles;

    if (!layer || targetGeom.type() != QgsWkbTypes::LineGeometry) {
        return intersectionAngles;
    }

    // 获取目标要素的边界框，用于空间索引
    QgsRectangle targetBounds = targetGeom.boundingBox();
    targetBounds.grow(tolerance);

    QgsFeatureRequest request;
    request.setFilterRect(targetBounds);
    request.setFlags(QgsFeatureRequest::ExactIntersect);

    QgsFeatureIterator iterator = layer->getFeatures(request);
    QgsFeature feature;

    while (iterator.nextFeature(feature)) {
        if (!feature.hasGeometry() || feature.geometry().type() != QgsWkbTypes::LineGeometry) {
            continue;
        }

        QgsGeometry otherGeom = feature.geometry();

        // 检查是否相交
        if (!targetGeom.intersects(otherGeom)) {
            continue;
        }

        // 获取交点
        QgsGeometry intersection = targetGeom.intersection(otherGeom);
        if (intersection.isEmpty()) {
            continue;
        }

        // 处理交点（可能是点或多点）
        std::vector<QgsPointXY> intersectionPoints;

        if (intersection.type() == QgsWkbTypes::PointGeometry) {
            if (intersection.isMultipart()) {
                QgsMultiPointXY multiPoint = intersection.asMultiPoint();
                for (const auto& point : multiPoint) {
                    intersectionPoints.push_back(point);
                }
            }
            else {
                intersectionPoints.push_back(intersection.asPoint());
            }
        }

        // 为每个交点计算交叉角度
        for (const auto& intersectionPoint : intersectionPoints) {
            double angle = calculateIntersectionAngle(targetGeom, otherGeom, intersectionPoint);

            if (angle >= 0) { // 有效角度
                IntersectionAngleInfo angleInfo;
                angleInfo.intersectionPoint = intersectionPoint;
                angleInfo.intersectionAngle = angle;
                angleInfo.featureId1 = feature.id(); // 目标要素ID需要从外部传入
                angleInfo.featureId2 = feature.id();
                angleInfo.isValid = true;

                intersectionAngles.push_back(angleInfo);
            }
        }
    }

    return intersectionAngles;
}
//获取线在指定点处的方向向量
QgsVector ReadShpDialog::getLineDirectionAtPoint(const QgsLineString* line, const QgsPointXY& point)
{
    if (!line || line->numPoints() < 2) {
        return QgsVector(0, 0);
    }

    double minDistance = std::numeric_limits<double>::max();
    int closestSegmentIndex = -1;
    QgsPointXY closestPointOnSegment;

    // 找到最接近指定点的线段，并计算在该线段上的投影点
    for (int i = 0; i < line->numPoints() - 1; ++i) {
        QgsPointXY segStart(line->pointN(i).x(), line->pointN(i).y());
        QgsPointXY segEnd(line->pointN(i + 1).x(), line->pointN(i + 1).y());

        // 计算点到线段的最短距离和投影点
        QgsGeometry segmentGeom = QgsGeometry::fromPolylineXY({ segStart, segEnd });
        QgsGeometry closestPoint = segmentGeom.nearestPoint(QgsGeometry::fromPointXY(point));

        if (!closestPoint.isEmpty()) {
            QgsPointXY projectedPoint = closestPoint.asPoint();
            double distance = point.distance(projectedPoint);

            if (distance < minDistance) {
                minDistance = distance;
                closestSegmentIndex = i;
                closestPointOnSegment = projectedPoint;
            }
        }
    }

    if (closestSegmentIndex >= 0) {
        QgsPointXY segStart(line->pointN(closestSegmentIndex).x(), line->pointN(closestSegmentIndex).y());
        QgsPointXY segEnd(line->pointN(closestSegmentIndex + 1).x(), line->pointN(closestSegmentIndex + 1).y());

        // 根据投影点在线段上的位置，确定方向向量的方向
        QgsVector segmentVector(segEnd.x() - segStart.x(), segEnd.y() - segStart.y());

        // 如果投影点更接近线段起点，使用正向；如果更接近终点，可能需要考虑反向
        double distToStart = closestPointOnSegment.distance(segStart);
        double distToEnd = closestPointOnSegment.distance(segEnd);
        double segmentLength = segStart.distance(segEnd);

        // 确保方向向量指向正确的方向
        if (segmentLength > 0) {
            return segmentVector.normalized();
        }
    }

    return QgsVector(0, 0);
}
//查找水系交叉点
std::vector<WaterSystemIntersection> ReadShpDialog::findWaterSystemIntersections(const QgsGeometry& waterGeom, QgsVectorLayer* referenceLayer, double tolerance)
{
    std::vector<WaterSystemIntersection> intersections;

    QgsRectangle searchBounds = waterGeom.boundingBox().buffered(tolerance);
    QgsFeatureRequest request;
    request.setFilterRect(searchBounds);

    QgsFeatureIterator iter = referenceLayer->getFeatures(request);
    QgsFeature refFeature;

    while (iter.nextFeature(refFeature)) {
        QgsGeometry refGeom = refFeature.geometry();
        if (refGeom.type() != QgsWkbTypes::LineGeometry) {
            continue;
        }

        // 检查是否相交
        if (waterGeom.intersects(refGeom)) {
            QgsGeometry intersection = waterGeom.intersection(refGeom);

            if (intersection.type() == QgsWkbTypes::PointGeometry) {
                QgsPointXY intersectionPoint;
                if (intersection.isMultipart()) {
                    QgsMultiPointXY multiPoint = intersection.asMultiPoint();
                    if (!multiPoint.isEmpty()) {
                        intersectionPoint = multiPoint.first();
                    }
                }
                else {
                    intersectionPoint = intersection.asPoint();
                }

                WaterSystemIntersection waterIntersection;
                waterIntersection.intersectionPoint = intersectionPoint;
                waterIntersection.inflowAngle = calculateInflowAngle(refGeom, intersectionPoint);
                waterIntersection.featureId = refFeature.id();
                waterIntersection.isValid = true;

                intersections.push_back(waterIntersection);
            }
        }
    }

    return intersections;
}
//计算汇入角度
double ReadShpDialog::calculateInflowAngle(const QgsGeometry& waterGeom, const QgsPointXY& intersectionPoint)
{
    if (waterGeom.type() != QgsWkbTypes::LineGeometry) {
        return 0.0;
    }

    const QgsLineString* lineString = nullptr;
    if (waterGeom.isMultipart()) {
        QgsMultiLineString* multiLine = qgsgeometry_cast<QgsMultiLineString*>(waterGeom.constGet());
        if (multiLine && multiLine->numGeometries() > 0) {
            lineString = qgsgeometry_cast<const QgsLineString*>(multiLine->geometryN(0));
        }
    }
    else {
        lineString = qgsgeometry_cast<const QgsLineString*>(waterGeom.constGet());
    }

    if (!lineString || lineString->numPoints() < 2) {
        return 0.0;
    }

    // 找到最接近交叉点的线段
    double minDistance = std::numeric_limits<double>::max();
    int closestSegmentIndex = -1;

    for (int i = 0; i < lineString->numPoints() - 1; i++) {
        QgsPointXY p1(lineString->pointN(i).x(), lineString->pointN(i).y());
        QgsPointXY p2(lineString->pointN(i + 1).x(), lineString->pointN(i + 1).y());

        QgsGeometry segmentGeom = QgsGeometry::fromPolylineXY({ p1, p2 });
        double distance = segmentGeom.distance(QgsGeometry::fromPointXY(intersectionPoint));

        if (distance < minDistance) {
            minDistance = distance;
            closestSegmentIndex = i;
        }
    }

    if (closestSegmentIndex >= 0) {
        QgsPointXY p1(lineString->pointN(closestSegmentIndex).x(), lineString->pointN(closestSegmentIndex).y());
        QgsPointXY p2(lineString->pointN(closestSegmentIndex + 1).x(), lineString->pointN(closestSegmentIndex + 1).y());

        double dx = p2.x() - p1.x();
        double dy = p2.y() - p1.y();

        // 计算方位角（从北方向顺时针测量，0-360度）
        double angle = atan2(dx, dy) * 180.0 / M_PI;
        if (angle < 0) {
            angle += 360.0;
        }

        return angle;
    }

    return 0.0;
}
//检查是否为悬挂河流
bool ReadShpDialog::isHangingWaterFeature(const QgsGeometry& waterGeom, QgsVectorLayer* layer, double tolerance)
{
    if (waterGeom.type() != QgsWkbTypes::LineGeometry) {
        return false;
    }

    // 获取河流的端点
    const QgsLineString* lineString = nullptr;
    if (waterGeom.isMultipart()) {
        QgsMultiLineString* multiLine = qgsgeometry_cast<QgsMultiLineString*>(waterGeom.constGet());
        if (multiLine && multiLine->numGeometries() > 0) {
            lineString = qgsgeometry_cast<const QgsLineString*>(multiLine->geometryN(0));
        }
    }
    else {
        lineString = qgsgeometry_cast<const QgsLineString*>(waterGeom.constGet());
    }

    if (!lineString || lineString->numPoints() < 2) {
        return false;
    }

    QgsPointXY startPoint(lineString->startPoint().x(), lineString->startPoint().y());
    QgsPointXY endPoint(lineString->endPoint().x(), lineString->endPoint().y());

    // 检查端点是否连接到其他河流
    QgsFeatureIterator iter = layer->getFeatures();
    QgsFeature feature;
    int connectionCount = 0;

    while (iter.nextFeature(feature)) {
        QgsGeometry otherGeom = feature.geometry();
        if (otherGeom.type() != QgsWkbTypes::LineGeometry) {
            continue;
        }

        // 跳过自身
        if (waterGeom.equals(otherGeom)) {
            continue;
        }

        // 检查是否与端点相交
        QgsGeometry startBuffer = QgsGeometry::fromPointXY(startPoint).buffer(tolerance, 8);
        QgsGeometry endBuffer = QgsGeometry::fromPointXY(endPoint).buffer(tolerance, 8);

        if (otherGeom.intersects(startBuffer) || otherGeom.intersects(endBuffer)) {
            connectionCount++;
        }
    }

    // 如果连接数少于1，认为是悬挂河流
    return connectionCount < 1;
}
//检查是否有交叉河流
bool ReadShpDialog::hasIntersectingWaterFeatures(const QgsGeometry& waterGeom, QgsVectorLayer* layer, double tolerance)
{
    QgsFeatureIterator iter = layer->getFeatures();
    QgsFeature feature;

    while (iter.nextFeature(feature)) {
        QgsGeometry otherGeom = feature.geometry();
        if (otherGeom.type() != QgsWkbTypes::LineGeometry) {
            continue;
        }

        // 跳过自身
        if (waterGeom.equals(otherGeom)) {
            continue;
        }

        // 检查是否相交
        if (waterGeom.intersects(otherGeom)) {
            return true;
        }
    }

    return false;
}
//提取河流宽度信息
std::vector<RiverWidthInfo> ReadShpDialog::extractRiverWidthInfo(const QgsFeature& feature)
{
    std::vector<RiverWidthInfo> widthInfo;

    // 检查stroke_wid字段
    QgsFields fields = feature.fields();
    int widthFieldIndex = fields.lookupField("stroke_wid");

    if (widthFieldIndex == -1) {
        // 添加日志：没有stroke_wid字段
        logTextEdit->append(QString("要素ID %1: 未找到stroke_wid字段").arg(feature.id()));
        return widthInfo;
    }

    QVariant widthValue = feature.attribute(widthFieldIndex);
    if (!widthValue.isValid() || widthValue.isNull()) {
        // 添加日志：stroke_wid字段为空
        logTextEdit->append(QString("要素ID %1: stroke_wid字段为空或无效").arg(feature.id()));
        return widthInfo;
    }

    QgsGeometry geom = feature.geometry();
    if (geom.type() != QgsWkbTypes::LineGeometry) {
        // 添加日志：不是线几何
        logTextEdit->append(QString("要素ID %1: 不是线几何类型").arg(feature.id()));
        return widthInfo;
    }

    // 添加日志：开始解析stroke_wid
    QString widthStr = widthValue.toString();
    logTextEdit->append(QString("要素ID %1: 开始解析stroke_wid值 = '%2'").arg(feature.id()).arg(widthStr));

    // 检查是否为多值宽度（用分号或逗号分隔）
    QStringList widthValues;

    if (widthStr.contains(";")) {
        widthValues = widthStr.split(";", Qt::SkipEmptyParts);
        logTextEdit->append(QString("要素ID %1: 检测到分号分隔的多值宽度，共 %2 个值").arg(feature.id()).arg(widthValues.size()));
    }
    else if (widthStr.contains(",")) {
        widthValues = widthStr.split(",", Qt::SkipEmptyParts);
        logTextEdit->append(QString("要素ID %1: 检测到逗号分隔的多值宽度，共 %2 个值").arg(feature.id()).arg(widthValues.size()));
    }
    else {
        widthValues << widthStr;
        logTextEdit->append(QString("要素ID %1: 单一宽度值").arg(feature.id()));
    }

    // 获取线几何
    const QgsLineString* lineString = nullptr;
    if (geom.isMultipart()) {
        QgsMultiLineString* multiLine = qgsgeometry_cast<QgsMultiLineString*>(geom.constGet());
        if (multiLine && multiLine->numGeometries() > 0) {
            lineString = qgsgeometry_cast<const QgsLineString*>(multiLine->geometryN(0));
        }
    }
    else {
        lineString = qgsgeometry_cast<const QgsLineString*>(geom.constGet());
    }

    if (!lineString || lineString->numPoints() < 2) {
        logTextEdit->append(QString("要素ID %1: 线几何无效或点数少于2个").arg(feature.id()));
        return widthInfo;
    }

    double totalLength = geom.length();
    logTextEdit->append(QString("要素ID %1: 线总长度 = %2 米").arg(feature.id()).arg(totalLength, 0, 'f', 2));

    // 如果只有一个宽度值，应用到整条线
    if (widthValues.size() == 1) {
        bool ok;
        double width = widthValues[0].toDouble(&ok);
        if (ok && width > 0) {
            RiverWidthInfo info;
            info.width = width;
            info.distance = 0.0;
            info.segmentIndex = 0;
            widthInfo.push_back(info);

            logTextEdit->append(QString("要素ID %1: 单一宽度值 = %2 米，应用到整条线")
                .arg(feature.id()).arg(width, 0, 'f', 2));
        }
        else {
            logTextEdit->append(QString("要素ID %1: 宽度值 '%2' 无法转换为有效数字")
                .arg(feature.id()).arg(widthValues[0]));
        }
    }
    // 如果有多个宽度值，沿线分布
    else if (widthValues.size() > 1) {
        logTextEdit->append(QString("要素ID %1: 多值宽度分布详情:").arg(feature.id()));

        for (int i = 0; i < widthValues.size(); i++) {
            bool ok;
            double width = widthValues[i].toDouble(&ok);
            if (ok && width > 0) {
                RiverWidthInfo info;
                info.width = width;
                // 计算沿线距离
                if (widthValues.size() == 1) {
                    info.distance = 0.0;
                }
                else {
                    info.distance = (totalLength * i) / (widthValues.size() - 1);
                }
                info.segmentIndex = i;
                widthInfo.push_back(info);

                logTextEdit->append(QString("  - 第%1个宽度点: 宽度=%2米, 距离=%3米, 段索引=%4")
                    .arg(i + 1).arg(width, 0, 'f', 2).arg(info.distance, 0, 'f', 2).arg(i));
            }
            else {
                logTextEdit->append(QString("  - 第%1个宽度值 '%2' 无法转换为有效数字")
                    .arg(i + 1).arg(widthValues[i]));
            }
        }
    }

    logTextEdit->append(QString("要素ID %1: 解析完成，有效宽度点数量 = %2").arg(feature.id()).arg(widthInfo.size()));
    return widthInfo;
}
//检查河流宽度变化是否合理
bool ReadShpDialog::checkRiverWidthProgression(const std::vector<RiverWidthInfo>& widthInfo)
{
    if (widthInfo.size() < 3) {
        if (widthInfo.size() > 0) {
            logTextEdit->append(QString("宽度检查: 宽度点数量 %1 少于3个，无法检测粗-细-粗模式，判定为正常")
                .arg(widthInfo.size()));
        }
        return true; // 少于3个点无法判断粗-细-粗模式
    }

    logTextEdit->append(QString("宽度检查: 开始分析 %1 个宽度点的变化模式").arg(widthInfo.size()));

    // 输出所有宽度点的详细信息
    for (size_t i = 0; i < widthInfo.size(); i++) {
        logTextEdit->append(QString("  宽度点%1: 宽度=%2米, 距离=%3米")
            .arg(i + 1).arg(widthInfo[i].width, 0, 'f', 2).arg(widthInfo[i].distance, 0, 'f', 2));
    }

    double significantChangeRatio = 0.3; // 30%的变化被认为是显著的
    logTextEdit->append(QString("宽度检查: 显著变化阈值 = %1 (%2%)")
        .arg(significantChangeRatio, 0, 'f', 2).arg(significantChangeRatio * 100, 0, 'f', 0));

    // 检查粗-细-粗模式
    for (size_t i = 1; i < widthInfo.size() - 1; i++) {
        double prevWidth = widthInfo[i - 1].width;
        double currWidth = widthInfo[i].width;
        double nextWidth = widthInfo[i + 1].width;

        // 检查是否存在显著的细化然后粗化
        if (currWidth < prevWidth * (1 - significantChangeRatio) &&
            nextWidth > currWidth * (1 + significantChangeRatio)) {

            logTextEdit->append(QString("宽度检查: 发现粗-细-粗模式在位置%1")
                .arg(i + 1));
            logTextEdit->append(QString("  前一点宽度: %1米 -> 当前点宽度: %2米 -> 下一点宽度: %3米")
                .arg(prevWidth, 0, 'f', 2).arg(currWidth, 0, 'f', 2).arg(nextWidth, 0, 'f', 2));
            logTextEdit->append(QString("  细化比例: %1%, 粗化比例: %2%")
                .arg((1 - currWidth / prevWidth) * 100, 0, 'f', 1)
                .arg((nextWidth / currWidth - 1) * 100, 0, 'f', 1));

            return false;
        }
    }

    // 检查连续的显著宽度变化
    int consecutiveChanges = 0;
    for (size_t i = 1; i < widthInfo.size(); i++) {
        double prevWidth = widthInfo[i - 1].width;
        double currWidth = widthInfo[i].width;
        double changeRatio = std::abs(currWidth - prevWidth) / prevWidth;

        if (changeRatio > significantChangeRatio) {
            consecutiveChanges++;
            logTextEdit->append(QString("宽度检查: 第%1-%2段发现显著变化，变化比例: %3%")
                .arg(i).arg(i + 1).arg(changeRatio * 100, 0, 'f', 1));
        }
        else {
            if (consecutiveChanges > 0) {
                logTextEdit->append(QString("宽度检查: 连续显著变化段结束，共%1段").arg(consecutiveChanges));
            }
            consecutiveChanges = 0;
        }
    }

    if (consecutiveChanges >= 3) {
        logTextEdit->append(QString("宽度检查: 发现过多连续显著变化 (%1段)，判定为异常").arg(consecutiveChanges));
        return false;
    }

    // 检查整体趋势（不合理的细化）
    double startWidth = widthInfo.front().width;
    double endWidth = widthInfo.back().width;
    double overallChangeRatio = (endWidth - startWidth) / startWidth;

    logTextEdit->append(QString("宽度检查: 整体趋势分析 - 起始宽度: %1米, 结束宽度: %2米, 整体变化: %3%")
        .arg(startWidth, 0, 'f', 2).arg(endWidth, 0, 'f', 2).arg(overallChangeRatio * 100, 0, 'f', 1));

    if (overallChangeRatio < -0.5) { // 整体细化超过50%
        logTextEdit->append(QString("宽度检查: 整体细化过度 (%1%)，判定为异常").arg(-overallChangeRatio * 100, 0, 'f', 1));
        return false;
    }

    logTextEdit->append("宽度检查: 宽度变化模式正常");
    return true;
}
//创建水系走向错误几何体
QgsGeometry ReadShpDialog::createWaterDirectionErrorGeometry(const QgsPointXY& intersectionPoint, double bufferRadius)
{
    return QgsGeometry::fromPointXY(intersectionPoint).buffer(bufferRadius, 8);
}
//快速相似度计算使用长度和端点距离，为库体要素候选匹配的几何快速评分
double ReadShpDialog::calculateQuickSimilarity(const QgsGeometry& geom1, const QgsGeometry& geom2)
{
    // 快速长度比较
    double len1 = geom1.length();
    double len2 = geom2.length();

    if (len1 == 0 || len2 == 0) {
        return 0.0;
    }

    // 长度相似度
    double lengthRatio = std::min(len1, len2) / std::max(len1, len2);

    // 如果长度差异太大，直接返回低分
    if (lengthRatio < 0.3) {
        return lengthRatio * 0.5;
    }

    // 简化的距离计算 - 只计算中心点距离
    QgsPointXY center1 = geom1.centroid().asPoint();
    QgsPointXY center2 = geom2.centroid().asPoint();
    double centerDistance = center1.distance(center2);

    // 距离评分
    double maxLen = std::max(len1, len2);
    double distanceScore = std::max(0.0, 1.0 - (centerDistance / maxLen));

    // 综合评分（长度权重0.6，距离权重0.4）
    return lengthRatio * 0.6 + distanceScore * 0.4;
}
//获取与指定要素相交的要素列表
std::vector<QgsFeature> ReadShpDialog::getIntersectingFeatures(const QgsFeature& targetFeature, QgsVectorLayer* layer)
{
    std::vector<QgsFeature> intersectingFeatures;

    if (!layer || !targetFeature.hasGeometry()) {
        return intersectingFeatures;
    }

    QgsGeometry targetGeom = targetFeature.geometry();
    QgsRectangle searchBounds = targetGeom.boundingBox().buffered(10.0); // 10米缓冲区

    QgsFeatureRequest request;
    request.setFilterRect(searchBounds);

    QgsFeatureIterator iter = layer->getFeatures(request);
    QgsFeature feature;

    while (iter.nextFeature(feature)) {
        if (feature.id() == targetFeature.id()) {
            continue; // 跳过自身
        }

        if (feature.hasGeometry() && feature.geometry().type() == QgsWkbTypes::LineGeometry) {
            // 检查是否相交
            if (targetGeom.intersects(feature.geometry())) {
                intersectingFeatures.push_back(feature);
            }
        }
    }

    return intersectingFeatures;
}
//计算拓扑一致性评分
double ReadShpDialog::calculateTopologyConsistency(const QgsFeature& candidateFeature, QgsVectorLayer* referenceLayer, const std::vector<QgsFeature>& parsedIntersectingFeatures)
{
    if (!candidateFeature.hasGeometry()) {
        return 0.0;
    }

    // 获取候选要素在库体中的相交要素
    std::vector<QgsFeature> candidateIntersectingFeatures = getIntersectingFeatures(candidateFeature, referenceLayer);

    // 比较相交要素数量
    int parsedIntersectionCount = parsedIntersectingFeatures.size();
    int candidateIntersectionCount = candidateIntersectingFeatures.size();

    // 如果两者都没有相交要素，拓扑一致性很高
    if (parsedIntersectionCount == 0 && candidateIntersectionCount == 0) {
        return 1.0;
    }

    // 如果一个有相交要素而另一个没有，拓扑一致性很低
    if ((parsedIntersectionCount == 0) != (candidateIntersectionCount == 0)) {
        return 0.1; // 给一个很低的分数，但不是0，以免完全排除
    }

    // 计算相交数量的相似度
    double countSimilarity = 0.0;
    if (parsedIntersectionCount > 0 && candidateIntersectionCount > 0) {
        int maxCount = std::max(parsedIntersectionCount, candidateIntersectionCount);
        int minCount = std::min(parsedIntersectionCount, candidateIntersectionCount);
        countSimilarity = static_cast<double>(minCount) / maxCount;
    }

    // 计算相交角度的相似度
    double angleSimilarity = calculateIntersectionAngleSimilarity(
        candidateFeature, candidateIntersectingFeatures,
        parsedIntersectingFeatures);

    // 综合评分：数量相似度60% + 角度相似度40%
    return countSimilarity * 0.6 + angleSimilarity * 0.4;
}
//计算相交角度相似度
double ReadShpDialog::calculateIntersectionAngleSimilarity(const QgsFeature& candidateFeature, const std::vector<QgsFeature>& candidateIntersections, const std::vector<QgsFeature>& parsedIntersections)
{
    if (candidateIntersections.empty() || parsedIntersections.empty()) {
        return candidateIntersections.empty() && parsedIntersections.empty() ? 1.0 : 0.0;
    }

    QgsGeometry candidateGeom = candidateFeature.geometry();

    // 计算候选要素的相交角度
    std::vector<double> candidateAngles;
    for (const auto& intersection : candidateIntersections) {
        std::vector<QgsPointXY> intersectionPoints = findIntersectionPoints(candidateGeom, intersection.geometry());
        for (const auto& point : intersectionPoints) {
            double angle = calculateIntersectionAngle(candidateGeom, intersection.geometry(), point);
            if (angle >= 0) {
                candidateAngles.push_back(angle);
            }
        }
    }

    // 对于解析要素的相交，我们需要估算角度（因为我们只有相交要素列表）
    // 这里简化处理：如果相交数量相近，给予较高分数
    if (candidateAngles.empty()) {
        return parsedIntersections.empty() ? 1.0 : 0.5;
    }

    // 基于相交角度的多样性给分
    // 如果有多种角度，说明拓扑结构复杂，更可能是正确匹配
    std::sort(candidateAngles.begin(), candidateAngles.end());

    double angleVariance = 0.0;
    if (candidateAngles.size() > 1) {
        double sum = 0.0;
        for (double angle : candidateAngles) {
            sum += angle;
        }
        double mean = sum / candidateAngles.size();

        double varianceSum = 0.0;
        for (double angle : candidateAngles) {
            varianceSum += (angle - mean) * (angle - mean);
        }
        angleVariance = varianceSum / candidateAngles.size();
    }

    // 角度方差越大，拓扑结构越复杂，匹配可信度越高
    return std::min(1.0, angleVariance / 1000.0 + 0.5);
}
//查找两个几何体的相交点
std::vector<QgsPointXY> ReadShpDialog::findIntersectionPoints(const QgsGeometry& geom1, const QgsGeometry& geom2)
{
    std::vector<QgsPointXY> points;

    try {
        QgsGeometry intersection = geom1.intersection(geom2);
        if (!intersection.isEmpty()) {
            if (intersection.type() == QgsWkbTypes::PointGeometry) {
                if (intersection.isMultipart()) {
                    QgsMultiPointXY multiPoint = intersection.asMultiPoint();
                    for (const auto& point : multiPoint) {
                        points.push_back(point);
                    }
                }
                else {
                    points.push_back(intersection.asPoint());
                }
            }
        }
    }
    catch (...) {
        // 忽略几何计算错误
    }

    return points;
}
//计算在指定点处两个几何体的相交角度
double ReadShpDialog::calculateIntersectionAngle(const QgsGeometry& geom1, const QgsGeometry& geom2, const QgsPointXY& intersectionPoint)
{
    const QgsLineString* line1 = getFirstLineString(geom1);
    const QgsLineString* line2 = getFirstLineString(geom2);

    if (!line1 || !line2) {
        return -1.0;
    }

    // 找到相交点附近的线段方向
    QgsVector direction1 = getLineDirectionAtPoint(line1, intersectionPoint);
    QgsVector direction2 = getLineDirectionAtPoint(line2, intersectionPoint);

    if (direction1.length() == 0 || direction2.length() == 0) {
        return -1.0;
    }

    // 计算两个方向向量的夹角
    double dotProduct = direction1.x() * direction2.x() + direction1.y() * direction2.y();
    double magnitude1 = direction1.length();
    double magnitude2 = direction2.length();

    double cosAngle = dotProduct / (magnitude1 * magnitude2);
    cosAngle = std::max(-1.0, std::min(1.0, cosAngle)); // 确保在有效范围内

    double angle = std::acos(cosAngle) * 180.0 / M_PI;

    // 对于河流交叉，返回实际的交叉角度（可能是锐角或钝角）

    return angle;
}
//显示质检结果
void ReadShpDialog::showQualityResults(const std::vector<QualityCheckResult>& results)
{
    // 保存结果到成员变量
    lastCheckResults = results;

    // 清空表格
    tableWidget->clear();
    tableWidget->setRowCount(0);

    // 设置表头
    QStringList headers;
    headers << "状态" << "错误类型" << "描述" << "计算值" << "标准值" << "要素ID" << "源文件";
    tableWidget->setColumnCount(headers.size());
    tableWidget->setHorizontalHeaderLabels(headers);

    // 填充表格
    int rowIndex = 0;
    bool onlyShowErrors = onlyErrorsCheckBox->isChecked();

    for (const auto& result : results) {
        // 如果只显示错误，则跳过通过的项
        if (onlyShowErrors && result.passed) {
            continue;
        }

        tableWidget->insertRow(rowIndex);

        // 状态
        QTableWidgetItem* statusItem = new QTableWidgetItem();
        statusItem->setTextAlignment(Qt::AlignCenter);
        if (result.passed) {
            statusItem->setText("通过");
            statusItem->setBackground(QBrush(QColor(200, 255, 200)));
        }
        else {
            statusItem->setText("错误");
            statusItem->setBackground(QBrush(QColor(255, 200, 200)));
        }
        tableWidget->setItem(rowIndex, 0, statusItem);

        // 错误类型 - 根据checkType显示大分类
        QString errorTypeDisplay;
        switch (result.checkType) {
        case CheckType::DIRECTION_ERROR:
            errorTypeDisplay = "走向错误";
            break;
        case CheckType::POLYGON_TOPOLOGY_ERROR:
            errorTypeDisplay = "面要素拓扑错误";
            break;
        case CheckType::LINE_TOPOLOGY_ERROR:
            errorTypeDisplay = "线要素拓扑错误";
            break;
        case CheckType::NETWORK_CONNECTIVITY_ERROR:
            errorTypeDisplay = "路网通达性错误";
            break;
        case CheckType::RIGHT_ANGLE_THRESHOLD_ERROR:
            errorTypeDisplay = "直角化角度阈值控制错误";
            break;
        default:
            errorTypeDisplay = "未知错误类型";
            break;
        }
        QTableWidgetItem* typeItem = new QTableWidgetItem(errorTypeDisplay);
        typeItem->setTextAlignment(Qt::AlignCenter);
        tableWidget->setItem(rowIndex, 1, typeItem);

        // 描述 - 包含具体的错误类型和详细描述
        QString descriptionText = result.errorType + " - " + result.description;
        QTableWidgetItem* descItem = new QTableWidgetItem(descriptionText);
        tableWidget->setItem(rowIndex, 2, descItem);

        // 计算值
        QTableWidgetItem* calcItem = new QTableWidgetItem(QString::number(result.calculatedValue, 'f', 2));
        calcItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        tableWidget->setItem(rowIndex, 3, calcItem);

        // 标准值
        QTableWidgetItem* stdItem = new QTableWidgetItem(QString::number(result.standardValue, 'f', 2));
        stdItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        tableWidget->setItem(rowIndex, 4, stdItem);

        // 要素ID
        QTableWidgetItem* idItem = new QTableWidgetItem(QString::number(result.featureId));
        idItem->setTextAlignment(Qt::AlignCenter);
        tableWidget->setItem(rowIndex, 5, idItem);

        // 源文件
        QTableWidgetItem* sourceItem = new QTableWidgetItem(result.sourceFile);
        sourceItem->setTextAlignment(Qt::AlignCenter);
        tableWidget->setItem(rowIndex, 6, sourceItem);

        rowIndex++;
    }

    // 调整列宽
    tableWidget->setColumnWidth(0, 60);  // 状态
    tableWidget->setColumnWidth(1, 120); // 错误类型
    tableWidget->setColumnWidth(2, 350); // 描述（增加宽度以显示完整信息）
    tableWidget->setColumnWidth(3, 80);  // 计算值
    tableWidget->setColumnWidth(4, 80);  // 标准值
    tableWidget->setColumnWidth(5, 80);  // 要素ID
    tableWidget->setColumnWidth(6, 150); // 源文件

    // 自动调整最后一列宽度
    tableWidget->horizontalHeader()->setStretchLastSection(true);

    // 显示结果统计
    int totalCount = results.size();
    int errorCount = 0;
    for (const auto& result : results) {
        if (!result.passed) {
            errorCount++;
        }
    }

    logTextEdit->append(QString("检查结果统计: 总计 %1 项，通过 %2 项，错误 %3 项")
        .arg(totalCount)
        .arg(totalCount - errorCount)
        .arg(errorCount));
}
//输出错误结果
void ReadShpDialog::outputToFile()
{

    QString format = outputFormatCombo->currentText();
    if (format != "log" && lastCheckResults.empty()) {
        QMessageBox::warning(this, "警告", "没有可输出的检查结果");
        return;
    }

    QString filter;
    QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString fileName;

    // 根据格式设置文件过滤器
    if (format == "shp") {
        filter = "Shapefile (*.shp)";
    }
    else if (format == "json") {
        filter = "JSON Files (*.json)";
    }
    else if (format == "xlsx") {
        filter = "Excel Files (*.xlsx)";
    }
    else {
        filter = "Text Files (*.txt)";
    }
    // 仅对需要文件名的格式弹保存框；shp/log 选择目录
    if (format != "shp" && format != "log") {
        fileName = QFileDialog::getSaveFileName(this, "保存检查结果", defaultPath, filter);
        if (fileName.isEmpty()) {
            return;
        }
    }

    if (format == "shp") {
        // 选择父目录 → 创建“质检结果”子文件夹
        QString parentDir = m_qualityResultsDir;
        if (parentDir.isEmpty()) {
            parentDir = QFileDialog::getExistingDirectory(this, QString::fromUtf8("选择输出父目录"), defaultPath);
            if (parentDir.isEmpty()) return;
        }
        QDir base(parentDir);
        QString resultsDir = base.filePath(QString::fromUtf8("质检结果"));
        QDir results(resultsDir);
        if (!results.exists()) {
            if (!QDir().mkpath(results.absolutePath())) {
                QMessageBox::critical(this, "错误", QString("无法创建质检结果文件夹: %1").arg(results.absolutePath()));
                return;
            }
        }
        m_qualityResultsDir = resultsDir; // 记忆具体子文件夹路径

        if (exportErrorsToShp(resultsDir)) {
            logTextEdit->append(QString("错误要素已保存到质检结果文件夹: %1").arg(resultsDir));
            QMessageBox::information(this, "保存成功", QString("错误要素已保存到:\n%1").arg(resultsDir));
        }
        else {
            QMessageBox::critical(this, "错误", "导出shp文件失败");
        }
    }
    else if (format == "log") {
        // 选择父目录 → 创建“质检日志”子文件夹
        QString parentDir = m_qualityLogsDir;
        if (parentDir.isEmpty()) {
            parentDir = QFileDialog::getExistingDirectory(this, QString::fromUtf8("选择输出父目录"), defaultPath);
            if (parentDir.isEmpty()) return;
        }
        QDir base(parentDir);
        QString logsDir = base.filePath(QString::fromUtf8("质检日志"));
        QDir logs(logsDir);
        if (!logs.exists()) {
            if (!QDir().mkpath(logs.absolutePath())) {
                QMessageBox::critical(this, "错误", QString("无法创建质检日志文件夹: %1").arg(logs.absolutePath()));
                return;
            }
        }
        m_qualityLogsDir = logsDir; // 记忆具体子文件夹路径

        if (exportLogsToFolder(logsDir)) {
            logTextEdit->append(QString("运行日志已保存到质检日志文件夹: %1").arg(logsDir));
            QMessageBox::information(this, "保存成功", QString("运行日志已保存到:\n%1").arg(logsDir));
        }
        else {
            QMessageBox::critical(this, "错误", "导出日志失败");
        }
    
    }
    else if (format == "json") {
        QJsonObject root;

        const bool directionSelected = directionErrorCheckBox && directionErrorCheckBox->isChecked();
        const bool polygonSelected = polygonOverlapGapCheckBox && polygonOverlapGapCheckBox->isChecked();
        const bool lineTopoSelected = lineTopologyCheckBox && lineTopologyCheckBox->isChecked();
        const bool rightAngleSelected = rightAngleCheckBox && rightAngleCheckBox->isChecked();

        QList<QualityCheckResult> lineTopoErrors;
        QList<QualityCheckResult> directionErrors;
        QList<QualityCheckResult> polygonErrors;
        QList<QualityCheckResult> rightAngleErrors;
        bool lineTopoHasInvalidGeometry = false;
        bool dirHasInvalidGeometry = false;
        bool polyHasInvalidGeometry = false;
        bool rightHasInvalidGeometry = false;

        for (const auto& r : lastCheckResults) {
            if (r.checkType == CheckType::LINE_TOPOLOGY_ERROR || r.checkType == CheckType::NETWORK_CONNECTIVITY_ERROR) {
                if (!r.passed) {
                    lineTopoErrors.append(r);
                    if (r.errorType.contains(QString::fromUtf8("几何体无效"))) lineTopoHasInvalidGeometry = true;
                }
            }
            else if (r.checkType == CheckType::DIRECTION_ERROR) {
                if (!r.passed) {
                    directionErrors.append(r);
                    if (r.errorType.contains(QString::fromUtf8("几何体无效"))) dirHasInvalidGeometry = true;
                }
            }
            else if (r.checkType == CheckType::POLYGON_TOPOLOGY_ERROR) {
                if (!r.passed) {
                    polygonErrors.append(r);
                    if (r.errorType.contains(QString::fromUtf8("几何体无效"))) polyHasInvalidGeometry = true;
                }
            }
            else if (r.checkType == CheckType::RIGHT_ANGLE_THRESHOLD_ERROR) {
                if (!r.passed) {
                    rightAngleErrors.append(r);
                    if (r.errorType.contains(QString::fromUtf8("几何体无效"))) rightHasInvalidGeometry = true;
                }
            }
        }

        auto numOrNull = [](double v) -> QJsonValue {
            return std::isfinite(v) ? QJsonValue(v) : QJsonValue(QJsonValue::Null);
        };

        auto buildCheckItemDetailed = [&](const QString& id, const QString& name,
            bool selected, const QList<QualityCheckResult>& errors,
            bool hasInvalid, bool executed) -> QJsonObject {
                QJsonObject item;
                item.insert("id", id);
                item.insert("name", name);

                // 新语义：按是否执行算法来设置状态
                const QString status = !selected
                    ? QString::fromUtf8("未质检")
                    : (executed ? QString::fromUtf8("质检成功") : QString::fromUtf8("质检失败"));
                item.insert("status", status);

                // err_exist 保持字符串以兼容现有解析器
                item.insert("err_exist", (selected && !errors.isEmpty()) ? "true" : "false");

                // info 对象：summary + error_count + error_types[]
                QJsonObject infoObj;
                QJsonArray typeArr;
                QSet<QString> uniqTypes;
                for (const auto& e : errors) {
                    if (!e.errorType.isEmpty()) uniqTypes.insert(e.errorType);
                }
                for (const auto& t : uniqTypes) typeArr.append(t);

                if (!selected) {
                    infoObj.insert("summary", QString::fromUtf8("未质检"));
                    infoObj.insert("error_count", 0);
                    infoObj.insert("error_types", QJsonArray{});
                }
                else if (!executed) {
                    // 执行失败场景，给出简要说明
                    infoObj.insert("summary", hasInvalid
                        ? QString::fromUtf8("执行失败：存在无效几何或图层未正确加载")
                        : QString::fromUtf8("执行失败：未能加载有效图层或未匹配到有效组合"));
                    infoObj.insert("error_count", 0);
                    infoObj.insert("error_types", QJsonArray{});
                }
                else {
                    if (!errors.isEmpty()) {
                        const QString summary = uniqTypes.isEmpty()
                            ? QString::fromUtf8("发现错误，共%1项").arg(errors.size())
                            : QString::fromUtf8("存在%1，共%2项")
                            .arg(QStringList(uniqTypes.begin(), uniqTypes.end()).join(QString::fromUtf8("、")))
                            .arg(errors.size());
                        infoObj.insert("summary", summary);
                        infoObj.insert("error_count", errors.size());
                        infoObj.insert("error_types", typeArr);
                    }
                    else {
                        infoObj.insert("summary", QString::fromUtf8("未发现错误"));
                        infoObj.insert("error_count", 0);
                        infoObj.insert("error_types", QJsonArray{});
                    }
                }
                item.insert("info", infoObj);

                // errors 改为对象数组，每条错误一个对象
                QJsonArray errorsArr;
                if (selected && !errors.isEmpty()) {
                    for (const auto& e : errors) {
                        QJsonObject errObj;
                        errObj.insert("error_type", e.errorType);
                        errObj.insert("description", e.description);
                        errObj.insert("calc_value", numOrNull(e.calculatedValue));
                        errObj.insert("std_value", numOrNull(e.standardValue));
                        qint64 fid = e.featureId != 0 ? e.featureId : e.feature.id();
                        errObj.insert("feature_id", static_cast<double>(fid));
                        errObj.insert("source_file", e.sourceFile);
                        errObj.insert("check_type", static_cast<int>(e.checkType));
                        errorsArr.append(errObj);
                    }
                }
                item.insert("errors", errorsArr);

                return item;
        };

        // 顶层四个检查项，加入 executed 标志以设置 status
        root.insert("check_item1",
            buildCheckItemDetailed("112", QString::fromUtf8("线要素拓扑检查"),
                lineTopoSelected, lineTopoErrors, lineTopoHasInvalidGeometry, executedLineTopology));
        root.insert("check_item2",
            buildCheckItemDetailed("105", QString::fromUtf8("几何走向异常检查"),
                directionSelected, directionErrors, dirHasInvalidGeometry, executedDirection));
        root.insert("check_item3",
            buildCheckItemDetailed("111", QString::fromUtf8("面要素重叠/间隙检查"),
                polygonSelected, polygonErrors, polyHasInvalidGeometry, executedPolygon));
        root.insert("check_item4",
            buildCheckItemDetailed("109", QString::fromUtf8("直角化角度阈值控制"),
                rightAngleSelected, rightAngleErrors, rightHasInvalidGeometry, executedRightAngle));

        if (exportResultsToJson(fileName, root)) {
            logTextEdit->append(QString("JSON检查结果已保存到: %1").arg(fileName));
            QMessageBox::information(this, "保存成功", QString("JSON检查结果已保存到:\n%1").arg(fileName));
        }
        else {
            QMessageBox::critical(this, "错误", "导出JSON文件失败");
        }
    }
    else if (format == "txt") {
        // 导出为txt格式，包含工作日志和检查结果
        QFile file(fileName);
        if (!file.open(QIODevice::WriteOnly)) {  // 移除QIODevice::Text标志
            QMessageBox::critical(this, "错误", "无法创建输出文件");
            return;
        }

        // 直接写入UTF-8 BOM
        file.write("\xEF\xBB\xBF");

        QTextStream out(&file);
        out.setCodec("UTF-8");

        // 写入标题和基本信息
        out << QString::fromUtf8("=================================================================\n");
        out << QString::fromUtf8("                SHP文件质量检查完整报告\n");
        out << QString::fromUtf8("=================================================================\n");
        out << QString::fromUtf8("生成时间: ") << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n";
        out << QString::fromUtf8("检查工具: SHP文件走向错误检查工具\n");
        out << QString::fromUtf8("=================================================================\n\n");

        // 第一部分：工作日志信息
        out << QString::fromUtf8("=== 第一部分：检查过程工作日志 ===\n\n");

        // 交互与批处理都从同一日志缓冲读取，避免依赖 QTextEdit。
        QString logContent = logTextEdit->toPlainText();
        if (!logContent.isEmpty()) {
            // 确保内容以UTF-8编码写入
            QByteArray logBytes = logContent.toUtf8();
            QString logUtf8 = QString::fromUtf8(logBytes);
            out << logUtf8 << "\n\n";
        }
        else {
            out << QString::fromUtf8("无工作日志信息\n\n");
        }

        out << QString::fromUtf8("=================================================================\n\n");

        // 第二部分：检查结果统计
        out << QString::fromUtf8("=== 第二部分：检查结果统计 ===\n\n");

        int totalCount = lastCheckResults.size();
        int errorCount = 0;
        QMap<QString, int> errorTypeCount; // 按错误类型统计
        QMap<QString, int> sourceFileCount; // 按源文件统计

        for (const auto& result : lastCheckResults) {
            if (!result.passed) {
                errorCount++;
                errorTypeCount[result.errorType]++;
                sourceFileCount[result.sourceFile]++;
            }
        }

        out << QString::fromUtf8("总体统计:\n");
        out << QString::fromUtf8("  总检查项目: ") << totalCount << QString::fromUtf8(" 项\n");
        out << QString::fromUtf8("  检查通过: ") << (totalCount - errorCount) << QString::fromUtf8(" 项\n");
        out << QString::fromUtf8("  发现错误: ") << errorCount << QString::fromUtf8(" 项\n");
        out << QString::fromUtf8("  通过率: ") << QString::number((totalCount - errorCount) * 100.0 / totalCount, 'f', 2) << "%\n\n";

        // 按错误类型统计
        if (!errorTypeCount.isEmpty()) {
            out << QString::fromUtf8("错误类型统计:\n");
            for (auto it = errorTypeCount.begin(); it != errorTypeCount.end(); ++it) {
                out << QString::fromUtf8("  ") << it.key() << QString::fromUtf8(": ") << it.value() << QString::fromUtf8(" 项\n");
            }
            out << "\n";
        }

        // 按源文件统计
        if (!sourceFileCount.isEmpty()) {
            out << QString::fromUtf8("源文件错误统计:\n");
            for (auto it = sourceFileCount.begin(); it != sourceFileCount.end(); ++it) {
                out << QString::fromUtf8("  ") << it.key() << QString::fromUtf8(": ") << it.value() << QString::fromUtf8(" 项\n");
            }
            out << "\n";
        }

        out << QString::fromUtf8("=================================================================\n\n");

        // 第三部分：详细检查结果
        out << QString::fromUtf8("=== 第三部分：详细检查结果 ===\n\n");

        // 使用更简单的格式，避免复杂的制表符对齐
        out << QString::fromUtf8("状态\t错误类型\t描述\t计算值\t标准值\t要素ID\t源文件\n");
        out << QString("-").repeated(100) << "\n";

        // 详细结果数据
        for (const auto& result : lastCheckResults) {
            QString status = result.passed ? QString::fromUtf8("通过") : QString::fromUtf8("错误");
            QString description = result.description.length() > 40 ? result.description.left(37) + "..." : result.description;
            QString sourceFileName = QFileInfo(result.sourceFile).baseName();

            out << status << "\t"
                << result.errorType << "\t"
                << description << "\t"
                << QString::number(result.calculatedValue, 'f', 2) << "\t"
                << QString::number(result.standardValue, 'f', 2) << "\t"
                << QString::number(result.feature.id()) << "\t"
                << sourceFileName << "\n";

            // 如果有错误，添加详细坐标信息
            if (!result.passed) {
                QString coordinateInfo = getDetailedCoordinateInfo(result);
                if (!coordinateInfo.isEmpty()) {
                    out << QString::fromUtf8("    坐标详情: ") << coordinateInfo << "\n";
                }
                out << "\n";
            }
        }

        out << QString::fromUtf8("\n=================================================================\n");
        out << QString::fromUtf8("报告结束\n");
        out << QString::fromUtf8("=================================================================\n");

        file.close();
        logTextEdit->append(QString("完整检查报告已保存到: %1").arg(fileName));

        // 显示保存成功的消息框
        QMessageBox::information(this, "保存成功",
            QString("完整检查报告已保存到:\n%1\n\n报告包含:\n- 检查过程工作日志\n- 检查结果统计\n- 详细检查结果").arg(fileName));
    }
    else if (format == "xlsx") {
        // 这里需要添加Excel导出功能，可以使用第三方库如QXlsx或QtXlsx
        QMessageBox::information(this, "提示", "Excel导出功能尚未实现，请选择txt格式导出");
    }
}
//输出错误为shp图层
bool ReadShpDialog::exportErrorsToShp(const QString& filePath)
{
    QFileInfo fileInfo(filePath);
    const bool isExplicitFile = fileInfo.suffix().compare("shp", Qt::CaseInsensitive) == 0;
    const bool looksLikeDir = fileInfo.isDir() || filePath.endsWith('/') || filePath.endsWith('\\') || !isExplicitFile;

    QDir outDir = looksLikeDir ? QDir(filePath) : fileInfo.absoluteDir();
    if (!outDir.exists()) {
        if (!QDir().mkpath(outDir.absolutePath())) {
            logTextEdit->append(QString("错误：无法创建目录 %1").arg(outDir.absolutePath()));
            return false;
        }
    }

    // 检查文件是否被占用（仅当传入的是具体文件路径）
    if (!looksLikeDir && QFile::exists(filePath)) {
        QFile testFile(filePath);
        if (!testFile.open(QIODevice::WriteOnly)) {
            logTextEdit->append(QString("错误：文件被占用或无写入权限 %1").arg(filePath));
            return false;
        }
        testFile.close();
    }
    // 当按目录输出时，用源文件名或默认名作为基础名
    QString dirBaseName;
    if (looksLikeDir) {
        if (!lastCheckResults.empty() && !lastCheckResults.front().sourceFile.isEmpty()) {
            dirBaseName = QFileInfo(lastCheckResults.front().sourceFile).completeBaseName();
        }
        if (dirBaseName.isEmpty()) {
            dirBaseName = "errors";
        }
    }
    // 标准图层名（优先取下拉框；为空时回退源文件名）
    auto safeName = [](QString s) {
        s.replace("/", "_").replace("\\", "_").replace(":", "_").replace("*", "_")
            .replace("?", "_").replace("\"", "_").replace("<", "_").replace(">", "_").replace("|", "_");
        return s.trimmed().isEmpty() ? QString::fromUtf8("未命名图层") : s.trimmed();
    };
    QString standardName = property("quality/output_standard_layer").toString().trimmed();
    if (standardName.isEmpty()) standardName = standardLayerCombo ? standardLayerCombo->currentText() : "";
    if (standardName.trimmed().isEmpty()) {
        if (!lastCheckResults.empty() && !lastCheckResults.front().sourceFile.isEmpty()) {
            standardName = QFileInfo(lastCheckResults.front().sourceFile).completeBaseName();
        }
    }
    QString safeStandardName = safeName(standardName);

    // 分组：按质检项 + 几何类型
    struct GroupKey {
        int checkType; // 靠枚举值即可
        QgsWkbTypes::GeometryType geomType;
        bool operator<(const GroupKey& other) const {
            if (checkType != other.checkType) return checkType < other.checkType;
            return static_cast<int>(geomType) < static_cast<int>(other.geomType);
        }
    };
    std::map<GroupKey, std::vector<QualityCheckResult>> grouped;
    std::map<QgsWkbTypes::GeometryType, std::vector<QualityCheckResult>> geometryGroups;
    int skippedCount = 0;

    for (const auto& result : lastCheckResults) {
        if (!result.passed) {
            // 关键修复：分组与写出均使用 errorGeometry（避免点被按线导出）
            QgsGeometry geom = result.errorGeometry;

            if (geom.isNull() || geom.isEmpty()) {
                // 对于特殊的汇总性错误（如路网通达性问题），尝试使用 geometry 字段
                if (!result.geometry.isNull() && !result.geometry.isEmpty()) {
                    geom = result.geometry;
                }
                else {
                    skippedCount++;
                    if (skippedCount <= 5) { // 限制警告信息的数量
                        logTextEdit->append(QString("警告：跳过空错误几何体，错误类型: %1").arg(result.errorType));
                    }
                    else if (skippedCount == 6) {
                        logTextEdit->append(QString("警告：已跳过多个空几何体，后续类似警告将被省略..."));
                    }
                    continue;
                }
            }

            // 验证和修复几何体
            if (!geom.isGeosValid()) {
                logTextEdit->append(QString("警告：修复无效错误几何体，错误类型: %1").arg(result.errorType));
                geom = geom.makeValid();
                if (geom.isEmpty() || !geom.isGeosValid()) {
                    logTextEdit->append(QString("警告：跳过无法修复的错误几何体，错误类型: %1").arg(result.errorType));
                    continue;
                }
            }

            // makeValid() may return a collection containing both polygon and
            // residual linework. Export every supported component with its source ID.
            std::vector<QgsGeometry> pending { geom };
            while (!pending.empty()) {
                const QgsGeometry component = pending.back();
                pending.pop_back();
                if (component.isNull() || component.isEmpty()) continue;
                const auto geomType = QgsWkbTypes::geometryType(component.wkbType());
                if (geomType == QgsWkbTypes::UnknownGeometry) {
                    const auto children = component.asGeometryCollection();
                    if (children.isEmpty()) {
                        logTextEdit->append(QStringLiteral("警告：错误几何类型不支持导出，FID=%1，类型=%2")
                            .arg(result.featureId).arg(QgsWkbTypes::displayString(component.wkbType())));
                    }
                    for (const auto& child : children) pending.push_back(child);
                    continue;
                }
                QualityCheckResult fixedResult = result;
                fixedResult.errorGeometry = component;
                if (result.checkType == CheckType::RIGHT_ANGLE_THRESHOLD_ERROR &&
                    result.errorGeometry.type() == QgsWkbTypes::PolygonGeometry &&
                    geomType == QgsWkbTypes::LineGeometry) {
                    fixedResult.errorType = QStringLiteral("源面无效定位线");
                    fixedResult.description = QStringLiteral("源面FID %1的修复残留定位线（长度%2米），关联原面几何无效记录；原问题：%3")
                        .arg(result.featureId).arg(component.length(), 0, 'f', 3).arg(result.description);
                }
                geometryGroups[geomType].push_back(fixedResult);
                const CheckType outputCheckType = fixedResult.checkType == CheckType::NETWORK_CONNECTIVITY_ERROR
                    ? CheckType::LINE_TOPOLOGY_ERROR : fixedResult.checkType;
                GroupKey gk{ static_cast<int>(outputCheckType), geomType };
                grouped[gk].push_back(fixedResult);
            }
        }
    }
    if (skippedCount > 0) {
        logTextEdit->append(QString("总计跳过了 %1 个空几何体").arg(skippedCount));
    }

    if (geometryGroups.empty()) {
        QMessageBox::information(this, "提示", "没有错误要素需要导出");
        return false;
    }

    // 创建字段定义
    QgsFields fields;
    fields.append(QgsField("error_type", QVariant::String, "String", 200));
    fields.append(QgsField("descriptio", QVariant::String, "String", 254));
    fields.append(QgsField("calc_value", QVariant::Double, "Real", 15, 6));
    fields.append(QgsField("std_value", QVariant::Double, "Real", 15, 6));
    fields.append(QgsField("feature_id", QVariant::Int, "Integer", 10));
    fields.append(QgsField("source_fil", QVariant::String, "String", 254));
    fields.append(QgsField("check_type", QVariant::String, "String", 50));
    fields.append(QgsField("ref_class", QVariant::String, "String", 80));
    fields.append(QgsField("ref_value", QVariant::String, "String", 80));
    fields.append(QgsField("ref_source", QVariant::String, "String", 254));
    fields.append(QgsField("ref_fid", QVariant::LongLong));

    std::size_t descriptionPartCount = 1;
    for (const auto& group : geometryGroups)
        for (const QualityCheckResult& result : group.second)
            descriptionPartCount = std::max(descriptionPartCount,
                LineTopologyIssueAggregation::splitUtf8(result.description.toUtf8().toStdString()).size());
    for (std::size_t part = 2; part <= descriptionPartCount; ++part)
        fields.append(QgsField(QStringLiteral("detail_%1").arg(static_cast<int>(part)), QVariant::String, "String", 254));

    auto resultAttributes = [descriptionPartCount](const QualityCheckResult& result) {
        const auto parts = LineTopologyIssueAggregation::splitUtf8(result.description.toUtf8().toStdString());
        QgsAttributes attributes;
        attributes << result.errorType << QString::fromUtf8(parts.front().c_str())
            << result.calculatedValue << result.standardValue << static_cast<int>(result.featureId)
            << result.sourceFile << QString::number(static_cast<int>(result.checkType));
        attributes << result.referenceCategory << result.referenceValue << result.referenceSource
                   << result.referenceFeatureId;
        for (std::size_t part = 1; part < descriptionPartCount; ++part)
            attributes << (part < parts.size() ? QString::fromUtf8(parts[part].c_str()) : QString());
        return attributes;
    };

    // 获取坐标系：优先从解析/库体路径快速读取，失败再走原有候选与兜底
    QgsCoordinateReferenceSystem crs;

    auto loadCrsQuick = [&](const QString& path) -> QgsCoordinateReferenceSystem {
        QString name = QFileInfo(path).baseName();
        QgsVectorLayer* tmp = new QgsVectorLayer(path, name, "ogr");
        if (tmp && tmp->isValid()) {
            QgsCoordinateReferenceSystem c = tmp->crs();
            delete tmp;
            return c;
        }
        if (tmp) delete tmp;
        return QgsCoordinateReferenceSystem();
    };

    // 1) 从解析图层路径读取 CRS
    if (!parseFilePaths.isEmpty()) {
        crs = loadCrsQuick(parseFilePaths.front());
        if (crs.isValid()) {
            logTextEdit->append(QString("使用解析图层路径的坐标系：%1").arg(crs.authid()));
        }
    }

    // 2) 若无效，尝试库体图层路径
    if (!crs.isValid() && !referenceFilePaths.isEmpty()) {
        crs = loadCrsQuick(referenceFilePaths.front());
        if (crs.isValid()) {
            logTextEdit->append(QString("使用库体图层路径的坐标系：%1").arg(crs.authid()));
        }
    }

    // 3) 若仍无效，保留原先“Asia_North_Albers_Equal_Area_Conic”候选逻辑
    if (!crs.isValid()) {
        QStringList possibleEpsgCodes = { "EPSG:102025", "EPSG:3395", "EPSG:54030" };
        for (const QString& epsgCode : possibleEpsgCodes) {
            QgsCoordinateReferenceSystem cand(epsgCode);
            if (cand.isValid() && cand.description().contains("Asia_North_Albers_Equal_Area_Conic")) {
                crs = cand;
                logTextEdit->append("使用EPSG候选坐标系：" + epsgCode + " - " + crs.description());
                break;
            }
        }
    }

    // 4) 若仍无效，继续用 3857 / 4326 兜底
    if (!crs.isValid()) {
        crs = QgsCoordinateReferenceSystem("EPSG:3857");
        if (crs.isValid()) {
            logTextEdit->append("使用Web Mercator作为备选：" + crs.description());
        }
        else {
            crs = QgsCoordinateReferenceSystem("EPSG:4326");
            logTextEdit->append("警告：使用WGS84作为最后备选");
        }
    }

    // 输出坐标系调试信息
    if (crs.isValid()) {
        logTextEdit->append("最终使用的坐标系：");
        logTextEdit->append(" 名称: " + crs.description());
        logTextEdit->append(" EPSG代码: " + crs.authid());
        logTextEdit->append(" 是否有效: " + QString(crs.isValid() ? "是" : "否"));
        QString wkt = crs.toWkt(QgsCoordinateReferenceSystem::WKT_PREFERRED);
        if (!wkt.isEmpty()) {
            logTextEdit->append("  WKT定义（前200字符）：" + wkt.left(200) + "...");
        }
    }

    bool hasSuccess = false;
    int totalExported = 0;
    auto checkTypeLabel = [&](int ct) -> QString {
        switch (static_cast<CheckType>(ct)) {
        case CheckType::LINE_TOPOLOGY_ERROR:
        case CheckType::NETWORK_CONNECTIVITY_ERROR:
            return QString::fromUtf8("线要素拓扑检查");
        case CheckType::DIRECTION_ERROR:
            return QString::fromUtf8("几何走向异常检查");
        case CheckType::POLYGON_TOPOLOGY_ERROR:
            return QString::fromUtf8("面要素重叠/间隙检查");
        case CheckType::RIGHT_ANGLE_THRESHOLD_ERROR:
            return QString::fromUtf8("直角化角度阈值控制");
        default:
            return QString::fromUtf8("未知检查项");
        }
    };
    for (const auto& kv : grouped) {
        const GroupKey& k = kv.first;
        const std::vector<QualityCheckResult>& results = kv.second;
        if (results.empty()) continue;

        // 类型映射（沿用你原有逻辑）
        QgsWkbTypes::Type wkbType;
        QString geometryTypeStr;
        QString typeSuffix;
        switch (k.geomType) {
        case QgsWkbTypes::PointGeometry: wkbType = QgsWkbTypes::Point; geometryTypeStr = "Point"; typeSuffix = "_points"; break;
        case QgsWkbTypes::LineGeometry:  wkbType = QgsWkbTypes::LineString; geometryTypeStr = "LineString"; typeSuffix = "_lines"; break;
        case QgsWkbTypes::PolygonGeometry:wkbType = QgsWkbTypes::Polygon; geometryTypeStr = "Polygon"; typeSuffix = "_polygons"; break;
        default: continue;
        }

        // 基础名：质检项_标准图层名_errors；几何类型后缀用于区分点、线、面错误图层。
        QString baseName = safeName(QString("%1_%2_errors").arg(checkTypeLabel(k.checkType)).arg(safeStandardName));
        QString typeFilePath = outDir.filePath(QString("%1%2.%3").arg(baseName).arg(typeSuffix).arg("shp"));

        // 创建内存图层与字段（保持不变）
        QString memoryLayerUri = QString("%1?crs=%2").arg(geometryTypeStr).arg(crs.authid());
        QgsVectorLayer* memoryLayer = new QgsVectorLayer(memoryLayerUri, "temp", "memory");
        if (!memoryLayer->isValid()) {
            logTextEdit->append(QString("错误：无法创建%1类型内存图层").arg(geometryTypeStr));
            delete memoryLayer;
            continue;
        }
        QgsVectorDataProvider* provider = memoryLayer->dataProvider();
        if (!provider->addAttributes(fields.toList())) {
            logTextEdit->append(QString("错误：无法添加字段到%1类型内存图层").arg(geometryTypeStr));
            delete memoryLayer;
            continue;
        }
        memoryLayer->updateFields();

        // 添加要素（沿用你原有属性顺序）
        QList<QgsFeature> features;
        for (const auto& result : results) {
            QgsFeature newFeature;
            newFeature.setGeometry(result.errorGeometry);
            newFeature.setAttributes(resultAttributes(result));
            features.append(newFeature);
        }

        if (!provider->addFeatures(features)) {
            logTextEdit->append(QString("错误：无法添加要素到%1类型内存图层").arg(geometryTypeStr));
            // 保留你原有的逐要素诊断逻辑（省略展开）
            // ... existing code ...
            delete memoryLayer;
            continue;
        }

        // 写入 shp（使用你原有 V3 与备用方法的双重写出）
        memoryLayer->setCrs(crs);
        QgsCoordinateTransformContext transformContext;
        QString errorMessage;
        QgsVectorFileWriter::SaveVectorOptions options;
        options.driverName = "ESRI Shapefile";
        options.fileEncoding = "UTF-8";
        options.actionOnExistingFile = QgsVectorFileWriter::CreateOrOverwriteFile;
        options.symbologyExport = QgsVectorFileWriter::NoSymbology;

        QgsVectorFileWriter::WriterError error = QgsVectorFileWriter::writeAsVectorFormatV3(
            memoryLayer, typeFilePath, transformContext, options, &errorMessage);

        if (error != QgsVectorFileWriter::NoError) {
            logTextEdit->append(QString("写入%1类型shp文件失败: %2").arg(geometryTypeStr).arg(errorMessage));
            QgsVectorFileWriter writer(typeFilePath, "UTF-8", fields, wkbType, crs, "ESRI Shapefile");
            if (writer.hasError() != QgsVectorFileWriter::NoError) {
                logTextEdit->append(QString("备用方法写入%1类型失败: %2").arg(geometryTypeStr).arg(writer.errorMessage()));
                delete memoryLayer;
                continue;
            }
            // 从内存图层读要素写入（保留你原逻辑）
            QgsFeatureIterator iter = memoryLayer->getFeatures();
            QgsFeature feature;
            int writeCount = 0;
            while (iter.nextFeature(feature)) {
                if (!writer.addFeature(feature)) {
                    logTextEdit->append(QString("添加%1类型要素失败: ID %2").arg(geometryTypeStr).arg(feature.id()));
                }
                else {
                    writeCount++;
                }
            }
            logTextEdit->append(QString("备用方法成功写入 %1 个%2类型要素").arg(writeCount).arg(geometryTypeStr));
        }
        delete memoryLayer;

        // 验证输出（保留不变）
        QgsVectorLayer* testLayer = new QgsVectorLayer(typeFilePath, "test", "ogr");
        if (testLayer->isValid()) {
            int actualFeatureCount = testLayer->featureCount();
            logTextEdit->append(QString("成功导出 %1 个%2类型错误要素到: %3")
                .arg(actualFeatureCount).arg(geometryTypeStr).arg(typeFilePath));
            totalExported += actualFeatureCount;
            hasSuccess = true;
        }
        else {
            logTextEdit->append(QString("警告：%1类型文件验证失败").arg(geometryTypeStr));
        }
        delete testLayer;
    }
    if (!hasSuccess) {
    for (const auto& group : geometryGroups) {
        QgsWkbTypes::GeometryType geomType = group.first;
        const std::vector<QualityCheckResult>& results = group.second;

        if (results.empty()) continue;

        // 确定具体的几何类型
        QgsWkbTypes::Type wkbType;
        QString geometryTypeStr;
        QString typeSuffix;

        switch (geomType) {
        case QgsWkbTypes::PointGeometry:
            wkbType = QgsWkbTypes::Point;
            geometryTypeStr = "Point";
            typeSuffix = "_points";
            break;
        case QgsWkbTypes::LineGeometry:
            wkbType = QgsWkbTypes::LineString;
            geometryTypeStr = "LineString";
            typeSuffix = "_lines";
            break;
        case QgsWkbTypes::PolygonGeometry:
            wkbType = QgsWkbTypes::Polygon;
            geometryTypeStr = "Polygon";
            typeSuffix = "_polygons";
            break;
        default:
            continue;
        }

        // 生成分类型的文件名（目录/文件两种模式）
        QString typeFilePath;
        if (looksLikeDir) {
            typeFilePath = outDir.filePath(QString("%1%2.%3")
                .arg(dirBaseName)
                .arg(typeSuffix)
                .arg("shp"));
        }
        else {
            QString tmp = filePath;
            if (geometryGroups.size() > 1) {
                QFileInfo info(filePath);
                tmp = QString("%1/%2%3.%4")
                    .arg(info.absolutePath())
                    .arg(info.baseName())
                    .arg(typeSuffix)
                    .arg(info.suffix());
            }
            typeFilePath = tmp;
        }

        // 创建要素列表
        QList<QgsFeature> features;
        for (const auto& result : results) {
            QgsFeature newFeature;
            newFeature.setGeometry(result.errorGeometry);

            // 设置属性
            newFeature.setAttributes(resultAttributes(result));
            features.append(newFeature);
        }

        if (features.isEmpty()) {
            logTextEdit->append(QString("警告：%1类型没有有效要素可导出").arg(geometryTypeStr));
            continue;
        }

        // 创建内存图层
        QString memoryLayerUri = QString("%1?crs=%2")
            .arg(geometryTypeStr)
            .arg(crs.authid());
        QgsVectorLayer* memoryLayer = new QgsVectorLayer(memoryLayerUri, "temp", "memory");

        if (!memoryLayer->isValid()) {
            logTextEdit->append(QString("错误：无法创建%1类型内存图层").arg(geometryTypeStr));
            delete memoryLayer;
            continue;
        }

        QgsVectorDataProvider* provider = memoryLayer->dataProvider();
        if (!provider->addAttributes(fields.toList())) {
            logTextEdit->append(QString("错误：无法添加字段到%1类型内存图层").arg(geometryTypeStr));
            delete memoryLayer;
            continue;
        }
        memoryLayer->updateFields();

        // 添加要素
        if (!provider->addFeatures(features)) {
            logTextEdit->append(QString("错误：无法添加要素到%1类型内存图层").arg(geometryTypeStr));
            // 清空内存图层中可能已经添加的要素
            provider->deleteFeatures(provider->allFeatureIds());

            // 如果是LineString几何体，尝试逐个添加要素并进行详细诊断
            if (geomType == QgsWkbTypes::LineGeometry) {
                bool partialSuccess = false;
                int successCount = 0;
                int totalCount = features.size();
                QList<QgsFeature> successFeatures;

                for (int i = 0; i < features.size(); ++i) {
                    QgsFeature feature = features[i];
                    QgsGeometry geom = feature.geometry();

                    // 详细诊断要素问题
                    logTextEdit->append(QString("诊断要素 %1:").arg(i + 1));

                    // 检查几何体基本信息
                    if (geom.isNull()) {
                        logTextEdit->append(QString("  - 几何体为null，跳过"));
                        continue;
                    }

                    if (geom.isEmpty()) {
                        logTextEdit->append(QString("  - 几何体为空，跳过"));
                        continue;
                    }

                    // 检查几何体类型
                    QgsWkbTypes::Type wkbType = geom.wkbType();
                    logTextEdit->append(QString("  - WKB类型: %1").arg(QgsWkbTypes::displayString(wkbType)));

                    if (geom.type() != QgsWkbTypes::LineGeometry) {
                        logTextEdit->append(QString("  - 几何体类型不匹配，期望LineGeometry，实际为%1，跳过")
                            .arg(QgsWkbTypes::displayString(wkbType)));
                        continue;
                    }

                    // 检查几何体有效性
                    if (!geom.isGeosValid()) {
                        logTextEdit->append(QString("  - 几何体无效，尝试修复"));
                        QgsGeometry fixedGeom = geom.makeValid();
                        if (!fixedGeom.isNull() && !fixedGeom.isEmpty() && fixedGeom.isGeosValid()) {
                            geom = fixedGeom;
                            logTextEdit->append(QString("  - 几何体修复成功"));
                        }
                        else {
                            logTextEdit->append(QString("  - 几何体修复失败，跳过"));
                            continue;
                        }
                    }
                    else {
                        logTextEdit->append(QString("  - 几何体有效"));
                    }

                    // 处理MultiLineString
                    if (geom.isMultipart()) {
                        logTextEdit->append(QString("  - 检测到MultiLineString，转换为单个LineString"));
                        const QgsMultiLineString* multiLine = qgsgeometry_cast<const QgsMultiLineString*>(geom.constGet());
                        if (multiLine && multiLine->numGeometries() > 0) {
                            const QgsLineString* firstLine = qgsgeometry_cast<const QgsLineString*>(multiLine->geometryN(0));
                            if (firstLine && firstLine->numPoints() >= 2) {
                                QgsGeometry singleLineGeom(firstLine->clone());
                                if (singleLineGeom.isGeosValid() && !singleLineGeom.isEmpty()) {
                                    geom = singleLineGeom;
                                    logTextEdit->append(QString("  - MultiLineString转换成功"));
                                }
                                else {
                                    logTextEdit->append(QString("  - MultiLineString转换后无效，跳过"));
                                    continue;
                                }
                            }
                            else {
                                logTextEdit->append(QString("  - MultiLineString第一个线段无效，跳过"));
                                continue;
                            }
                        }
                        else {
                            logTextEdit->append(QString("  - MultiLineString为空，跳过"));
                            continue;
                        }
                    }
                    else {
                        // 检查单个LineString的点数
                        const QgsLineString* lineString = qgsgeometry_cast<const QgsLineString*>(geom.constGet());
                        if (lineString) {
                            int pointCount = lineString->numPoints();
                            logTextEdit->append(QString("  - LineString点数: %1").arg(pointCount));
                            if (pointCount < 2) {
                                logTextEdit->append(QString("  - 点数不足，跳过"));
                                continue;
                            }
                        }
                        else {
                            logTextEdit->append(QString("  - 无法转换为LineString，跳过"));
                            continue;
                        }
                    }

                    // 确保几何体坐标系匹配
                    if (geom.constGet()->is3D()) {
                        logTextEdit->append(QString("  - 检测到3D几何体，转换为2D"));
                        QgsGeometry geom2D = geom.convertToType(QgsWkbTypes::LineGeometry, false);
                        if (!geom2D.isNull() && geom2D.isGeosValid()) {
                            geom = geom2D;
                        }
                        else {
                            logTextEdit->append(QString("  - 3D到2D转换失败，跳过"));
                            continue;
                        }
                    }

                    // 创建新要素并设置几何体
                    QgsFeature newFeature;
                    newFeature.setGeometry(geom);

                    // 确保属性字段数量匹配
                    QgsAttributes attributes = feature.attributes();
                    if (attributes.size() != fields.size()) {
                        logTextEdit->append(QString("  - 属性字段数量不匹配，调整中"));
                        // 调整属性数量以匹配字段定义
                        while (attributes.size() < fields.size()) {
                            attributes.append(QVariant());
                        }
                        while (attributes.size() > fields.size()) {
                            attributes.removeLast();
                        }
                    }
                    newFeature.setAttributes(attributes);

                    // 最终验证
                    if (!newFeature.geometry().isNull() &&
                        !newFeature.geometry().isEmpty() &&
                        newFeature.geometry().isGeosValid() &&
                        newFeature.geometry().type() == QgsWkbTypes::LineGeometry) {

                        // 尝试添加要素
                        if (provider->addFeature(newFeature)) {
                            successCount++;
                            successFeatures.append(newFeature);
                            partialSuccess = true;
                            logTextEdit->append(QString("  - 要素添加成功"));
                        }
                        else {
                            logTextEdit->append(QString("  - 要素添加失败，原因未知"));
                            // 获取更详细的错误信息
                            if (provider->hasErrors()) {
                                QStringList errors = provider->errors();
                                for (const QString& error : errors) {
                                    logTextEdit->append(QString("    错误: %1").arg(error));
                                }
                                provider->clearErrors();
                            }
                        }
                    }
                    else {
                        logTextEdit->append(QString("  - 最终验证失败，跳过"));
                    }
                }

                if (partialSuccess) {
                    logTextEdit->append(QString("部分成功: %1/%2 个要素添加成功").arg(successCount).arg(totalCount));
                }
                else {
                    logTextEdit->append(QString("所有要素添加失败"));
                    delete memoryLayer;
                    continue;
                }
            }

            else {
                delete memoryLayer;
                continue;
            }

        }
        // 写入shp文件
        QgsVectorFileWriter::SaveVectorOptions options;
        options.driverName = "ESRI Shapefile";
        options.fileEncoding = "UTF-8";
        options.actionOnExistingFile = QgsVectorFileWriter::CreateOrOverwriteFile;
        // 设置坐标系 - 使用正确的API
        options.symbologyExport = QgsVectorFileWriter::NoSymbology;

        // 确保内存图层使用正确的坐标系
        memoryLayer->setCrs(crs);

        QgsCoordinateTransformContext transformContext;
        QString errorMessage;

        QgsVectorFileWriter::WriterError error = QgsVectorFileWriter::writeAsVectorFormatV3(
            memoryLayer,
            typeFilePath,
            transformContext,
            options,
            &errorMessage
        );

        if (error != QgsVectorFileWriter::NoError) {
            logTextEdit->append(QString("写入%1类型shp文件失败: %2").arg(geometryTypeStr).arg(errorMessage));

            // 使用传统的构造函数方法，只写入内存图层中的要素
            QgsVectorFileWriter writer(typeFilePath, "UTF-8", fields, wkbType, crs, "ESRI Shapefile");
            if (writer.hasError() != QgsVectorFileWriter::NoError) {
                logTextEdit->append(QString("备用方法写入%1类型失败: %2").arg(geometryTypeStr).arg(writer.errorMessage()));
                delete memoryLayer;
                continue;
            }
            // 从内存图层中读取要素，而不是使用原始features列表
            QgsFeatureIterator iter = memoryLayer->getFeatures();
            QgsFeature feature;
            int writeCount = 0;
            while (iter.nextFeature(feature)) {
                if (!writer.addFeature(feature)) {
                    logTextEdit->append(QString("添加%1类型要素失败: ID %2").arg(geometryTypeStr).arg(feature.id()));
                }
                else {
                    writeCount++;
                }
            }
            logTextEdit->append(QString("备用方法成功写入 %1 个%2类型要素").arg(writeCount).arg(geometryTypeStr));

        }
        delete memoryLayer;

        // 验证输出文件
        QgsVectorLayer* testLayer = new QgsVectorLayer(typeFilePath, "test", "ogr");
        if (testLayer->isValid()) {
            int actualFeatureCount = testLayer->featureCount();
            logTextEdit->append(QString("成功导出 %1 个%2类型错误要素到: %3")
                .arg(actualFeatureCount).arg(geometryTypeStr).arg(typeFilePath));
            totalExported += actualFeatureCount;
            hasSuccess = true;
        }
        else {
            logTextEdit->append(QString("警告：%1类型文件验证失败").arg(geometryTypeStr));
        }
        delete testLayer;
    }
}
    if (hasSuccess) {
        logTextEdit->append(QString("=== 导出完成，总计导出 %1 个错误要素 ===").arg(totalExported));
        if (geometryGroups.size() > 1) {
            logTextEdit->append("注意：由于存在混合几何类型，已分别导出为不同文件");
        }
        return true;
    }
    else {
        logTextEdit->append("错误：所有几何类型导出均失败");
        return false;
    }
}
bool ReadShpDialog::exportLogsToFolder(const QString& dirPath)
{
    QDir outDir(dirPath);
    if (!outDir.exists()) {
        if (!QDir().mkpath(outDir.absolutePath())) {
            logTextEdit->append(QString("错误：无法创建目录 %1").arg(outDir.absolutePath()));
            return false;
        }
    }

    auto safeName = [](QString s) {
        s.replace("/", "_").replace("\\", "_").replace(":", "_").replace("*", "_")
            .replace("?", "_").replace("\"", "_").replace("<", "_").replace(">", "_").replace("|", "_");
        return s.trimmed().isEmpty() ? QString::fromUtf8("未命名图层") : s.trimmed();
    };
    QString standardName = standardLayerCombo ? standardLayerCombo->currentText() : "";
    if (standardName.trimmed().isEmpty()) {
        if (!lastCheckResults.empty() && !lastCheckResults.front().sourceFile.isEmpty()) {
            standardName = QFileInfo(lastCheckResults.front().sourceFile).completeBaseName();
        }
    }
    QString safeStandardName = safeName(standardName);
    QString content = logTextEdit ? logTextEdit->toPlainText() : QString();

    auto writeOne = [&](const QString& itemLabel) -> bool {
        QString base = safeName(QString("%1_%2").arg(safeStandardName).arg(itemLabel));
        QString path = outDir.filePath(base + ".log");
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            logTextEdit->append(QString("错误：无法写入日志文件 %1").arg(path));
            return false;
        }
        QTextStream out(&f);
        out.setCodec("UTF-8");
        out << content;
        f.close();
        return true;
    };

    // 仅按“实际执行”的检查导出对应日志；不再回退到复选框
    bool any = false;
    if (executedLineTopology) any |= writeOne(QString::fromUtf8(u8"线要素拓扑检查"));
    if (executedDirection)    any |= writeOne(QString::fromUtf8(u8"几何走向异常检查"));
    if (executedPolygon)      any |= writeOne(QString::fromUtf8(u8"面要素重叠/间隙检查"));
    if (executedRightAngle)   any |= writeOne(QString::fromUtf8(u8"直角化角度阈值控制"));
    return any;
}
//输出错误为json结果
bool ReadShpDialog::exportResultsToJson(const QString& filePath, const QJsonObject& root)
{

    QFileInfo fi(filePath);
    QDir dir = fi.absoluteDir();
    if (!dir.exists()) {
        if (!dir.mkpath(dir.absolutePath())) {
            logTextEdit->append(QString("错误：无法创建目录 %1").arg(dir.absolutePath()));
            return false;
        }
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        logTextEdit->append(QString("错误：无法写入 JSON 文件 %1").arg(filePath));
        return false;
    }

    QJsonObject annotatedRoot = root;
    const QJsonArray diagnostics = property("quality/right_angle_diagnostics").toJsonArray();
    if (!diagnostics.isEmpty()) {
        int evaluated = 0, missing = 0, local = 0, eligible = 0, invalidReferences = 0, invalidSources = 0;
        bool allMatchedPairsHaveNoApplicableCorners = true;
        for (const auto& entry : diagnostics) {
            const auto layer = entry.toObject();
            evaluated += layer.value(QStringLiteral("evaluated_angle_count")).toInt();
            missing += layer.value(QStringLiteral("missing_corner_count")).toInt();
            local += layer.value(QStringLiteral("local_corner_count")).toInt();
            eligible += layer.value(QStringLiteral("length_eligible_corner_count")).toInt();
            invalidReferences += layer.value(QStringLiteral("invalid_reference_geometry_count")).toInt();
            invalidSources += layer.value(QStringLiteral("invalid_geometry_count")).toInt();
            if (layer.value(QStringLiteral("matched_feature_count")).toInt() <= 0 ||
                layer.value(QStringLiteral("length_eligible_corner_count")).toInt() > 0 ||
                layer.value(QStringLiteral("invalid_geometry_count")).toInt() > 0 ||
                layer.value(QStringLiteral("invalid_reference_geometry_count")).toInt() > 0)
                allMatchedPairsHaveNoApplicableCorners = false;
        }
        for (auto it = annotatedRoot.begin(); it != annotatedRoot.end(); ++it) {
            QJsonObject item = it.value().toObject();
            if (item.value(QStringLiteral("id")).toString() != QStringLiteral("109")) continue;
            const QString originalStatus = item.value(QStringLiteral("status")).toString();
            if (originalStatus != QStringLiteral("质检成功") && originalStatus != QStringLiteral("质检完成")) continue;
            QJsonObject info = item.value(QStringLiteral("info")).toObject();
            info.insert(QStringLiteral("layer_diagnostics"), diagnostics);
            info.insert(QStringLiteral("evaluated_angle_count"), evaluated);
            info.insert(QStringLiteral("missing_corner_count"), missing);
            info.insert(QStringLiteral("local_corner_count"), local);
            info.insert(QStringLiteral("length_eligible_corner_count"), eligible);
            info.insert(QStringLiteral("invalid_reference_geometry_count"), invalidReferences);
            info.insert(QStringLiteral("source_geometry_error_count"), invalidSources);
            info.insert(QStringLiteral("algorithm_executed"), true);
            info.insert(QStringLiteral("verification_scope"), QStringLiteral("仅已匹配且满足有效边长和局部覆盖条件的角点"));
            const bool verified = evaluated + missing > 0;
            info.insert(QStringLiteral("angle_check_completed"), verified);
            if (invalidReferences > 0) {
                item.insert(QStringLiteral("status"), QStringLiteral("质检失败"));
                info.insert(QStringLiteral("rule_applicability"), QStringLiteral("insufficient_evidence"));
                info.insert(QStringLiteral("coverage_status"), QStringLiteral("库体参考几何无效"));
                const QString warning = QStringLiteral("匹配的库体参考要素存在无效几何，请先修复库体后再检查，不能据此判断整体角度合格");
                const QString summary = info.value(QStringLiteral("error_count")).toInt() > 0
                    ? info.value(QStringLiteral("summary")).toString() + QStringLiteral("；") + warning : warning;
                info.insert(QStringLiteral("summary"), summary);
            }
            else if (!verified && invalidSources > 0 && info.value(QStringLiteral("error_count")).toInt() > 0) {
                // Execution completed and found source defects. This does not
                // certify angles whose source geometry or coverage is unavailable.
                item.insert(QStringLiteral("status"), QStringLiteral("质检完成"));
                info.insert(QStringLiteral("rule_applicability"), QStringLiteral("blocked_by_source_geometry"));
                info.insert(QStringLiteral("coverage_status"), QStringLiteral("已检出源几何错误，角度未核验"));
                const QString warning = QStringLiteral("已完成数据扫描并记录源几何问题；尚无有效角点进入角度核验，不能判定角度合格。请先修复源几何，并核对各图层筛选及局部覆盖统计");
                const QString summary = info.value(QStringLiteral("summary")).toString() + QStringLiteral("；") + warning;
                info.insert(QStringLiteral("summary"), summary);
            }
            else if (!verified && allMatchedPairsHaveNoApplicableCorners &&
                info.value(QStringLiteral("error_count")).toInt() == 0) {
                item.insert(QStringLiteral("status"), QStringLiteral("质检完成"));
                info.insert(QStringLiteral("rule_applicability"), QStringLiteral("not_applicable"));
                info.insert(QStringLiteral("coverage_status"), QStringLiteral("已匹配范围内无适用角点"));
                info.insert(QStringLiteral("summary"), QStringLiteral("已执行检查；按当前角度及最小有效边长条件，已匹配库体范围内无适用角点，按规则跳过角度核验，不作角度合格判定"));
            }
            else if (!verified) {
                // The task manager counts this existing status as uncompleted.
                item.insert(QStringLiteral("status"), QStringLiteral("质检失败"));
                info.insert(QStringLiteral("rule_applicability"), QStringLiteral("insufficient_evidence"));
                info.insert(QStringLiteral("coverage_status"), QStringLiteral("无可核查角点"));
                const QString warning = QStringLiteral("未进入有效角点核查，不能据此判定角度合格；请查看各图层的筛选统计和原因");
                QString summary = info.value(QStringLiteral("summary")).toString();
                if (info.value(QStringLiteral("error_count")).toInt() == 0) summary = warning;
                else summary += QStringLiteral("；") + warning;
                info.insert(QStringLiteral("summary"), summary);
            }
            else {
                info.insert(QStringLiteral("rule_applicability"), QStringLiteral("applicable"));
                info.insert(QStringLiteral("coverage_status"), QStringLiteral("存在已核查角点"));
            }
            item.insert(QStringLiteral("info"), info);
            it.value() = item;
        }
    }
    QJsonDocument doc(annotatedRoot);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    return true;

}
// ========================= 配置与UI（加载/更新） =========================
//更新ui上的xml配置参数，XML 调试与检查项日志
void ReadShpDialog::updateUIFromConfig()
{
    if (scaleComboBox) {
        scaleComboBox->blockSignals(true);
        if (m_mapScale > 0) {
            scaleComboBox->setCurrentText(QString::number(m_mapScale));
        }
        else {
            scaleComboBox->setCurrentText("");
        }
        scaleComboBox->blockSignals(false);
    }

    logXMLDebugInfo(QString("读取比例尺: %1").arg(m_mapScale > 0 ? QString::number(m_mapScale) : "未设置"));
}
void ReadShpDialog::logXMLDebugInfo(const QString& message)
{
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    xmlDebugTextEdit->append(QString("[%1] %2").arg(timestamp).arg(message));
}
void ReadShpDialog::displayXMLConfigInfo()
{
    if (!m_config) return;

    logXMLDebugInfo("=== 配置文件信息 ===");

    auto allItems = m_config->getAllCheckItems();
    for (const auto& item : allItems) {
        logXMLDebugInfo(QString("检查项: %1 (%2) - %3")
            .arg(item.name)
            .arg(item.type)
            .arg(item.enabled ? "启用" : "禁用"));

        // 显示该检查项的所有参数
        if (!item.parameters.isEmpty()) {
            logXMLDebugInfo("  参数详情:");
            for (auto it = item.parameters.begin(); it != item.parameters.end(); ++it) {
                QString paramName = it.key();
                QVariant paramValue = it.value();

                // 跳过单位信息（以_unit结尾的参数）
                if (paramName.endsWith("_unit")) {
                    continue;
                }

                // 获取对应的单位信息
                QString unitKey = paramName + "_unit";
                QString unit = item.parameters.contains(unitKey) ? item.parameters[unitKey].toString() : "";

                // 扩展面积判断：根据参数名或单位字符串判定
                bool isArea = paramName.contains("area", Qt::CaseInsensitive)
                    || paramName.contains(QStringLiteral("面积"))
                    || unit.contains(QStringLiteral("平方"))
                    || unit.contains("m2")
                    || unit.contains(QStringLiteral("㎡"))
                    || unit.contains("cm2")
                    || unit.contains("mm2");

                // 非数值参数跳过转换
                bool ok = false;
                double rawDouble = paramValue.toString().toDouble(&ok);
                if (!ok) {
                    logXMLDebugInfo(QString("    %1: %2 %3 (非数值参数，不转换)")
                        .arg(paramName)
                        .arg(paramValue.toString())
                        .arg(unit));
                    continue;
                }

                // 计算转换后的值（按比例尺）
                double converted = convertUnitValue(rawDouble, unit, isArea);
                QString convertedUnit = getConvertedUnitLabel(unit, isArea);

                // 同时显示原值与转换后值（单位准确）
                logXMLDebugInfo(QString("    %1: %2 %3 -> 转换后: %4 %5")
                    .arg(paramName)
                    .arg(paramValue.toString())
                    .arg(unit)
                    .arg(QString::number(converted))
                    .arg(convertedUnit));
            }
        }
        logXMLDebugInfo(""); // 空行分隔不同检查项
    }

    logXMLDebugInfo("=== 配置信息显示完成 ===");
}
void ReadShpDialog::logCheckItemStart(const QString& checkType)
{
    logXMLDebugInfo(QString("开始执行检查: %1").arg(checkType));
}
void ReadShpDialog::logCheckItemDetails(const QString& checkType)
{
    if (!m_config) return;

    auto config = m_config->getCheckItemConfig(checkType);
    if (config.name.isEmpty()) return;

    logXMLDebugInfo(QString("检查项详情: %1").arg(config.name));
    logXMLDebugInfo(QString("描述: %1").arg(config.description));

    for (auto it = config.parameters.begin(); it != config.parameters.end(); ++it) {
        QString paramName = it.key();
        QVariant paramValue = it.value();

        // 跳过单位信息（以_unit结尾的参数）
        if (paramName.endsWith("_unit")) {
            continue;
        }

        // 获取对应的单位信息
        QString unitKey = paramName + "_unit";
        QString unit = config.parameters.contains(unitKey) ? config.parameters[unitKey].toString() : "";

        logXMLDebugInfo(QString("参数 %1: %2 %3")
            .arg(paramName)
            .arg(paramValue.toString())
            .arg(unit));
    }
}
// 新增：单位转换实现（根据单位字符串 + m_mapScale）
double ReadShpDialog::convertUnitValue(double value, const QString& unit, bool isArea)
{
    if (m_mapScale <= 0) {
        logXMLDebugInfo("比例尺未设置，参数保持原值");
        return value;
    }

    QString u = unit.trimmed().toLower();
    u.replace(" ", "");

    if (isArea) {
        // 地面平方米直接返回
        if (u.isEmpty() || u.contains("平方米") || u.contains("m2") || u.contains("㎡")) {
            return value;
        }
        // 图上平方厘米 -> 地面平方米
        if (u.contains("平方厘米") || u.contains("cm2") || u.contains("㎝²")) {
            return value * static_cast<double>(m_mapScale) * static_cast<double>(m_mapScale) / 10000.0;
        }
        // 图上平方毫米 -> 地面平方米
        if (u.contains("平方毫米") || u.contains("mm2") || u.contains("㎜²")) {
            return value * static_cast<double>(m_mapScale) * static_cast<double>(m_mapScale) / 1000000.0;
        }
        // 兜底：未知面积单位，保持原值
        return value;
    }
    else {
        // 地面米直接返回
        if (u.isEmpty() || u == "m" || u == QStringLiteral("米")) {
            return value;
        }
        // 图上厘米 -> 地面米
        if (u == "cm" || u.contains(QStringLiteral("厘米"))) {
            return value * static_cast<double>(m_mapScale) / 100.0;
        }
        // 图上毫米 -> 地面米
        if (u == "mm" || u.contains(QStringLiteral("毫米"))) {
            return value * static_cast<double>(m_mapScale) / 1000.0;
        }
        // 公里 -> 米（非图上单位，但常见）
        if (u == "km" || u.contains(QStringLiteral("公里"))) {
            return value * 1000.0;
        }
        // 兜底：未知长度单位，保持原值
        return value;
    }
}
QString ReadShpDialog::getConvertedUnitLabel(const QString& unit, bool isArea)
{
    QString u = unit.trimmed().toLower().replace(" ", "");
    if (isArea) {
        if (u.contains(QStringLiteral("平方"))) return QStringLiteral("平方米");
        if (u.contains("m2") || u.contains(QStringLiteral("㎡"))) return QStringLiteral("平方米");
        return unit;
    }
    else {
        if (u == "mm" || u.contains(QStringLiteral("毫米")) || u == "cm" || u.contains(QStringLiteral("厘米"))) {
            return QStringLiteral("米");
        }
        if (u == "m" || u == QStringLiteral("米") || u == "km" || u.contains(QStringLiteral("公里"))) {
            return QStringLiteral("米");
        }
        if (u.contains(QStringLiteral("度")) || u == "deg" || u == "degree" || u == "°") {
            return QStringLiteral("度");
        }
        if (u.contains(QStringLiteral("百分比")) || u == "percent" || u.contains("%") || u.contains("％")) {
            return QStringLiteral("百分比");
        }
        if (u.contains(QStringLiteral("比例")) || u == "ratio") {
            return QStringLiteral("比例");
        }
        return unit;
    }
}
// 空间匹配容差使用数据坐标单位，不参与图上长度的比例尺换算。
// 直角化最小线段长度仍通过 getConvertedParameter 按比例尺转换。
double ReadShpDialog::getUnscaledDistanceParameter(const QString& checkType, const QString& paramName)
{
    if (!m_config) return 0.0;
    const double raw = m_config->getParameterDouble(checkType, paramName);
    const QString unit = m_config->getParameterString(checkType, paramName + "_unit", "");
    if (!std::isfinite(raw) || raw < 0.0) {
        logXMLDebugInfo(QString("空间匹配参数无效: %1/%2=%3 %4").arg(checkType, paramName, QString::number(raw), unit));
        return 0.0;
    }
    logXMLDebugInfo(QString("空间匹配参数: %1/%2=%3 %4；不参与比例尺换算，实际使用=%5米").arg(checkType, paramName, QString::number(raw), unit).arg(QString::number(raw)));
    return raw;
}

// 新增：带checkType/paramName的获取 + 转换
double ReadShpDialog::getConvertedParameter(const QString& checkType, const QString& paramName)
{
    if (!m_config) return 0.0;

    double raw = m_config->getParameterDouble(checkType, paramName);
    QString unit = m_config->getParameterString(checkType, paramName + "_unit", "");

    bool isArea = paramName.contains("area", Qt::CaseInsensitive)
        || paramName.contains(QStringLiteral("面积"))
        || unit.contains(QStringLiteral("平方"))
        || unit.contains("m2")
        || unit.contains(QStringLiteral("㎡"))
        || unit.contains("cm2")
        || unit.contains("mm2");

    double converted = convertUnitValue(raw, unit, isArea);

    logXMLDebugInfo(QString("参数转换: %1/%2 原值=%3 单位=%4 => 转换后=%5 (比例尺 1:%6)")
        .arg(checkType)
        .arg(paramName)
        .arg(QString::number(raw))
        .arg(unit)
        .arg(QString::number(converted))
        .arg(m_mapScale > 0 ? QString::number(m_mapScale) : "未设置"));

    return converted;
}


void ReadShpDialog::updateParametersFromConfig()
{
    if (!m_config) {
        logTextEdit->append("配置对象为空，无法更新参数");
        return;
    }

    // 走向错误检查（长度类，按比例尺转换）
    m_directionDatabaseMatchTolerance = getConvertedParameter(QStringLiteral("几何走向异常检查"), QStringLiteral("direction_database_match_tolerance"));
    m_minRiverLength = getConvertedParameter(QStringLiteral("几何走向异常检查"), QStringLiteral("min_river_length"));
    m_intersectionDetectionTolerance = getConvertedParameter(QStringLiteral("几何走向异常检查"), QStringLiteral("intersection_detection_tolerance"));
    m_directionDanglingDetectionTolerance = getConvertedParameter(QStringLiteral("几何走向异常检查"), QStringLiteral("direction_dangling_detection_tolerance"));

    // 线要素拓扑检查（连通性容差为长度类）
    m_distanceTolerance = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("connectivity_tolerance"));
    // 存储经比例尺转换后的线拓扑参数，便于统一使用与显示
    m_danglingTolerance = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("dangling_tolerance"));
    m_minLineLength = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("min_segment_length"));
    m_connectivityTolerance = m_distanceTolerance;
    // 面要素重叠/间隙检查（长度与面积）
    m_overlapTolerance = getConvertedParameter(QStringLiteral("面要素重叠/间隙检查"), QStringLiteral("overlap_tolerance"));
    m_gapTolerance = getConvertedParameter(QStringLiteral("面要素重叠/间隙检查"), QStringLiteral("gap_tolerance"));
    m_minOverlapArea = getConvertedParameter(QStringLiteral("面要素重叠/间隙检查"), QStringLiteral("min_overlap_area"));

    // 直角化相关：最小线段长度按比例尺转换；空间匹配容差独立使用数据坐标单位。
    rightAngleLowerLimit = m_config->getRightAngleLowerLimit();
    rightAngleUpperLimit = m_config->getRightAngleUpperLimit();
    rightAngleDatabaseMatchTolerance = getUnscaledDistanceParameter(QStringLiteral("直角化角度阈值控制"), QStringLiteral("right_angle_database_match_tolerance"));
    rightAngleGeometrySimilarityThreshold = m_config->getRightAngleGeometrySimilarityThreshold();
    rightAngleTurningPointTolerance = m_config->getRightAngleTurningPointTolerance();
    rightAngleTurningPointMatchTolerance = getUnscaledDistanceParameter(QStringLiteral("直角化角度阈值控制"), QStringLiteral("right_angle_turning_point_match_tolerance"));
    rightAngleMinSegmentLength = getConvertedParameter(QStringLiteral("直角化角度阈值控制"), QStringLiteral("right_angle_min_segment_length"));

    // 更新UI显示
    updateUIFromConfig();

    // 显示所有配置参数（含转换后值）
    logTextEdit->append("=== 已从配置文件更新参数（含转换日志） ===");

    auto allItems = m_config->getAllCheckItems();
    for (const auto& item : allItems) {
        if (!item.parameters.isEmpty()) {
            logTextEdit->append(QString("【%1】:").arg(item.name));

            for (auto it = item.parameters.begin(); it != item.parameters.end(); ++it) {
                QString paramName = it.key();
                QVariant paramValue = it.value();

                // 跳过单位信息（以_unit结尾的参数）
                if (paramName.endsWith("_unit")) {
                    continue;
                }

                // 获取对应的单位信息
                QString unitKey = paramName + "_unit";
                QString unit = item.parameters.contains(unitKey) ? item.parameters[unitKey].toString() : "";

                // 扩展面积判断：根据参数名或单位字符串判定
                bool isArea = paramName.contains("area", Qt::CaseInsensitive)
                    || paramName.contains(QStringLiteral("面积"))
                    || unit.contains(QStringLiteral("平方"))
                    || unit.contains("m2")
                    || unit.contains(QStringLiteral("㎡"))
                    || unit.contains("cm2")
                    || unit.contains("mm2");

                // 非数值参数跳过转换
                bool ok = false;
                double rawDouble = paramValue.toString().toDouble(&ok);
                if (!ok) {
                    logTextEdit->append(QString("  %1: %2%3 (非数值参数，不转换)")
                        .arg(paramName)
                        .arg(paramValue.toString())
                        .arg(unit.isEmpty() ? "" : unit));
                    continue;
                }

                double converted = convertUnitValue(rawDouble, unit, isArea);
                QString convertedUnit = getConvertedUnitLabel(unit, isArea);

                // 格式化参数名显示（保留你的现有映射）
                QString displayName = paramName;
                if (paramName == "direction_database_match_tolerance") displayName = "库体数据匹配容差";
                else if (paramName == "river_width_change_check_enabled") displayName = "河流宽度变化检查";
                else if (paramName == "min_river_length") displayName = "最小河流长度";
                else if (paramName == "intersection_detection_tolerance") displayName = "交叉点检测容差";
                else if (paramName == "direction_dangling_detection_tolerance") displayName = "悬挂线检测容差";
                else if (paramName == "min_segment_length") displayName = "最小线段长度";
                else if (paramName == "overlap_tolerance") displayName = "重叠容差";
                else if (paramName == "gap_tolerance") displayName = "间隙容差";
                else if (paramName == "min_overlap_area") displayName = "最小重叠面积";
                else if (paramName == "dangling_tolerance") displayName = "悬挂容差";
                else if (paramName == "min_line_length") displayName = "最小线长度";
                else if (paramName == "connectivity_tolerance") displayName = "连通性容差";
                else if (paramName == "river_dangling_tolerance") displayName = "河流悬挂容差";
                else if (paramName == "river_isolation_distance") displayName = "河流孤立距离";
                else if (paramName == "river_overlap_tolerance") displayName = "河流重叠容差";
                else if (paramName == "river_intersection_tolerance") displayName = "河流交叉容差";
                else if (paramName == "river_exceeds_tolerance") displayName = "河流超出容差";
                else if (paramName == "database_match_tolerance") displayName = "数据库匹配容差";

                // 同时显示原值与转换后值（单位准确）
                logTextEdit->append(QString("  %1: %2%3 => 转换后: %4 %5")
                    .arg(displayName)
                    .arg(paramValue.toString())
                    .arg(unit.isEmpty() ? "" : unit)
                    .arg(QString::number(converted))
                    .arg(convertedUnit));
            }
        }
    }
    logTextEdit->append("=== 参数更新完成 ===");
}
void ReadShpDialog::loadConfigFromXML()
{
    if (!m_config) {
        logTextEdit->append("配置对象为空，无法加载XML配置");
        return;
    }

    // 获取配置文件路径 - 使用成员变量而不是configLineEdit
    QString configPath = m_configFilePath.trimmed();
    if (configPath.isEmpty()) {
        logTextEdit->append("请先选择配置文件");
        return;
    }

    if (m_config->loadFromFile(configPath)) {
        logTextEdit->append(QString("成功加载配置文件: %1").arg(configPath));

        int parsedScale = 0;
        QStringList scaleValues;

        QFile f(configPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QDomDocument doc;
            QString err; int line = 0, col = 0;
            if (doc.setContent(&f, &err, &line, &col)) {
                QDomElement root = doc.documentElement(); // <check_list>
                QDomNodeList scaleNodes = root.elementsByTagName("scale");
                for (int i = 0; i < scaleNodes.count(); ++i) {
                    QDomElement scaleElem = scaleNodes.at(i).toElement();
                    QString vText = scaleElem.firstChildElement("value").text().trimmed();

                    // 允许 "1:100000" 或纯数字 "100000"
                    QString normalized = vText;
                    int colonPos = normalized.indexOf(':');
                    if (colonPos != -1) normalized = normalized.mid(colonPos + 1);

                    bool ok = false;
                    int v = normalized.toInt(&ok);
                    if (ok && v > 0) {
                        QString asText = QString::number(v);
                        if (!scaleValues.contains(asText)) scaleValues << asText;
                    }
                }
                if (!scaleValues.isEmpty()) {
                    parsedScale = scaleValues.first().toInt();
                }
            }
            else {
                logTextEdit->append(QString("比例尺解析失败: XML错误(行%1,列%2): %3").arg(line).arg(col).arg(err));
            }
            f.close();
        }
        else {
            logTextEdit->append("比例尺解析失败: 无法打开配置文件");
        }

        // 更新比例尺下拉框
        if (scaleComboBox) {
            scaleComboBox->blockSignals(true);
            scaleComboBox->clear();
            for (const QString& vText : scaleValues) {
                scaleComboBox->addItem(vText);
            }
            if (parsedScale > 0) {
                scaleComboBox->setCurrentText(QString::number(parsedScale));
            }
            scaleComboBox->blockSignals(false);
        }

        m_mapScale = parsedScale;

        updateParametersFromConfig();  // 会调用 updateUIFromConfig
        displayXMLConfigInfo();
    }
    else {
        logTextEdit->append(QString("加载配置文件失败: %1").arg(configPath));
    }

}
//比例尺变化时更新
void ReadShpDialog::onScaleSelectionChanged(int index)
{
    if (!scaleComboBox) return;
    bool ok = false;
    int v = scaleComboBox->itemText(index).trimmed().toInt(&ok);
    m_mapScale = ok ? v : 0;

    logXMLDebugInfo(QString("比例尺选择变更: %1").arg(m_mapScale > 0 ? QString("1:%1").arg(m_mapScale) : "未设置"));
    updateParametersFromConfig();
    displayXMLConfigInfo();
}

void ReadShpDialog::onScaleTextChanged(const QString& text)
{
    bool ok = false;
    int v = text.trimmed().toInt(&ok);
    m_mapScale = ok ? v : 0;

    logXMLDebugInfo(QString("比例尺输入变更: %1").arg(m_mapScale > 0 ? QString("1:%1").arg(m_mapScale) : "未设置"));
    updateParametersFromConfig();
    displayXMLConfigInfo();
}
//输出错误完整报告
QString ReadShpDialog::getDetailedCoordinateInfo(const QualityCheckResult& result)
{
    QString info;

    // 获取错误几何的边界框信息
    if (!result.errorGeometry.isEmpty()) {
        QgsRectangle bbox = result.errorGeometry.boundingBox();
        info += QString("错误几何边界框: [%1, %2, %3, %4]\n")
            .arg(bbox.xMinimum(), 0, 'f', 6)
            .arg(bbox.yMinimum(), 0, 'f', 6)
            .arg(bbox.xMaximum(), 0, 'f', 6)
            .arg(bbox.yMaximum(), 0, 'f', 6);
    }

    // 获取要素几何的边界框信息
    if (!result.feature.geometry().isEmpty()) {
        QgsRectangle bbox = result.feature.geometry().boundingBox();
        info += QString("要素几何边界框: [%1, %2, %3, %4]\n")
            .arg(bbox.xMinimum(), 0, 'f', 6)
            .arg(bbox.yMinimum(), 0, 'f', 6)
            .arg(bbox.xMaximum(), 0, 'f', 6)
            .arg(bbox.yMaximum(), 0, 'f', 6);
    }

    return info;
}

// ========================= 地图显示与错误标记 =========================
void ReadShpDialog::initializeMapCanvas()
{
    try {
        mapCanvas = new QgsMapCanvas();
        mapCanvas->setMinimumSize(400, 300);
        mapCanvas->setCanvasColor(QColor(255, 255, 255));
        mapCanvas->enableAntiAliasing(true);

        logTextEdit->append("地图画布初始化成功");
    }
    catch (const std::exception& e) {
        logTextEdit->append(QString("地图画布初始化失败: %1").arg(e.what()));
        mapCanvas = nullptr;
    }
}
void ReadShpDialog::loadLayerToMap(QgsVectorLayer* layer)
{
    if (!mapCanvas || !layer || !layer->isValid()) {
        return;
    }

    try {
        // 设置图层样式 - 默认为解析图层（黄色）
        setLayerStyle(layer, "yellow", 2.0);

        // 添加到显示图层列表
        if (!allDisplayLayers.contains(layer)) {
            allDisplayLayers << layer;
        }

        // 更新地图显示
        mapCanvas->setLayers(allDisplayLayers);
        mapCanvas->refresh();

        logTextEdit->append("图层已加载到地图");
    }
    catch (const std::exception& e) {
        logTextEdit->append(QString("加载图层到地图失败: %1").arg(e.what()));
    }
}
void ReadShpDialog::zoomToLayer()
{
    if (!mapCanvas || !currentLayer || !currentLayer->isValid()) {
        return;
    }

    QgsRectangle extent = currentLayer->extent();
    mapCanvas->setExtent(extent);
    mapCanvas->refresh();
}
void ReadShpDialog::zoomToErrors()
{
    if (!mapCanvas || lastCheckResults.empty()) {
        return;
    }

    QgsRectangle combinedExtent;
    bool first = true;

    for (const auto& result : lastCheckResults) {
        if (!result.passed && !result.errorGeometry.isEmpty()) {
            QgsRectangle bbox = result.errorGeometry.boundingBox();
            if (first) {
                combinedExtent = bbox;
                first = false;
            }
            else {
                combinedExtent.combineExtentWith(bbox);
            }
        }
    }

    if (!first) {
        combinedExtent.scale(1.2);  // 稍微放大一点
        mapCanvas->setExtent(combinedExtent);
        mapCanvas->refresh();
    }
}
void ReadShpDialog::clearErrorMarkers()
{
    for (auto* marker : errorMarkers) {
        if (marker) {
            delete marker;
        }
    }
    errorMarkers.clear();
}
void ReadShpDialog::addErrorMarkersToMap(const std::vector<QualityCheckResult>& results)
{
    if (!mapCanvas) {
        return;
    }

    clearErrorMarkers();

    for (const auto& result : results) {
        if (!result.passed && !result.errorGeometry.isEmpty()) {
            QgsRubberBand* marker = nullptr;

            // 根据错误几何体类型选择合适的标记方式
            if (result.errorGeometry.type() == QgsWkbTypes::LineGeometry) {
                // 线段错误：使用线标记，加粗显示
                marker = new QgsRubberBand(mapCanvas, QgsWkbTypes::LineGeometry);
                marker->setToGeometry(result.errorGeometry, nullptr);
                marker->setColor(QColor(255, 0, 0, 200));
                marker->setWidth(8);  // 更粗的线条突出显示错误线段
            }
            else if (result.errorGeometry.type() == QgsWkbTypes::PointGeometry) {
                // 点错误：使用点标记
                marker = new QgsRubberBand(mapCanvas, QgsWkbTypes::PolygonGeometry);
                marker->setToGeometry(result.errorGeometry, nullptr);
                marker->setColor(QColor(255, 0, 0, 180));
                marker->setFillColor(QColor(255, 0, 0, 100));
                marker->setWidth(2);
            }
            else {
                // 其他类型：使用默认标记
                marker = new QgsRubberBand(mapCanvas, QgsWkbTypes::LineGeometry);
                marker->setToGeometry(result.errorGeometry, nullptr);
                marker->setColor(QColor(255, 0, 0, 128));
                marker->setWidth(4);
            }

            if (marker) {
                errorMarkers.push_back(marker);
            }
        }
    }

    mapCanvas->refresh();
}
// ========================= 日志输出与辅助 =========================
void ReadShpDialog::logCoordinateInfo(const QString& functionName, const QgsLineString* line)
{
    if (!line) return;

    logTextEdit->append(QString("%1 - 线要素信息:").arg(functionName));
    logTextEdit->append(QString("  点数量: %1").arg(line->numPoints()));

    if (line->numPoints() > 0) {
        QgsPoint start = line->startPoint();
        QgsPoint end = line->endPoint();
        logTextEdit->append(QString("  起点: (%.6f, %.6f)").arg(start.x()).arg(start.y()));
        logTextEdit->append(QString("  终点: (%.6f, %.6f)").arg(end.x()).arg(end.y()));
    }
}
void ReadShpDialog::logPointInfo(const QString& context, const QgsPoint& point, int index)
{
    QString indexStr = (index >= 0) ? QString("[%1]").arg(index) : "";
    logTextEdit->append(QString("%1 %2: (%.6f, %.6f)")
        .arg(context).arg(indexStr).arg(point.x()).arg(point.y()));
}
void ReadShpDialog::logDirectionInfo(const QString& context, double direction, const QgsPoint& p1, const QgsPoint& p2)
{
    logTextEdit->append(QString("%1: %.2f度 从(%.6f,%.6f)到(%.6f,%.6f)")
        .arg(context).arg(direction)
        .arg(p1.x()).arg(p1.y())
        .arg(p2.x()).arg(p2.y()));
}

// ========================= 面要素：重叠与间隙检查 =========================
//面要素主检查函数
std::vector<QualityCheckResult> ReadShpDialog::checkPolygonOverlapGap(QgsVectorLayer* parsedLayer, const QString& sourceFile)
{
    std::vector<QualityCheckResult> results;
    // 从配置读取参数，替换硬编码
    double overlapTolerance = m_config ? m_config->getOverlapTolerance() : 0.1;
    double gapTolerance = m_config ? m_config->getGapTolerance() : 0.5;
    double minOverlapArea = m_config ? m_config->getMinOverlapArea() : 0.1;
    if (!parsedLayer) {
        executedPolygon = false;
        return results;
    }
    executedPolygon = true;

    //几何体有效性校验，发现无效立即记录并继续，不终止
    {
        QgsFeatureIterator it = parsedLayer->getFeatures();
        QgsFeature f;
        while (it.nextFeature(f)) {
            if (!f.hasGeometry()) {
                QualityCheckResult r;
                r.passed = false;
                r.errorType = QString::fromUtf8("几何体无效");
                r.description = QString::fromUtf8("要素缺失几何或为空，已跳过该要素继续检查");
                r.feature = f;
                r.errorGeometry = QgsGeometry();
                r.sourceFile = sourceFile;
                r.checkType = CheckType::POLYGON_TOPOLOGY_ERROR;
                r.featureId = f.id();
                r.severity = QString::fromUtf8("ERROR");
                results.push_back(r);
                continue;
            }
            const QgsGeometry g = f.geometry();
            if (g.isNull() || !g.isGeosValid()) {
                QualityCheckResult r;
                r.passed = false;
                r.errorType = QString::fromUtf8("几何体无效");
                r.description = QString::fromUtf8("几何体无效（GEOS 校验失败），已跳过该要素继续检查");
                r.feature = f;
                r.errorGeometry = g;
                r.sourceFile = sourceFile;
                r.checkType = CheckType::POLYGON_TOPOLOGY_ERROR;
                r.featureId = f.id();
                r.severity = QString::fromUtf8("ERROR");
                results.push_back(r);
                continue;
            }
        }
    }
    
    // 获取所有要素并转换为面要素（如果需要）
    std::vector<QgsFeature> polygonFeatures;
    QgsFeatureIterator iter = parsedLayer->getFeatures();
    QgsFeature feature;

    while (iter.nextFeature(feature)) {
        if (!feature.hasGeometry()) continue;

        QgsGeometry geom = feature.geometry();

        // 如果是面要素，直接添加
        if (geom.type() == QgsWkbTypes::PolygonGeometry) {
            polygonFeatures.push_back(feature);
        }
        // 如果是线要素，转换为面要素（缓冲区）
        else if (geom.type() == QgsWkbTypes::LineGeometry) {
            // 使用小的缓冲区将线要素转换为面要素
            double bufferDistance = 1.0; // 1米缓冲区，可以根据需要调整
            QgsGeometry bufferedGeom = geom.buffer(bufferDistance, 8);
            if (!bufferedGeom.isNull() && bufferedGeom.type() == QgsWkbTypes::PolygonGeometry) {
                // 创建新的要素，使用缓冲后的几何体
                QgsFeature bufferedFeature = feature;
                bufferedFeature.setGeometry(bufferedGeom);
                polygonFeatures.push_back(bufferedFeature);
            }
        }
    }

    // 对解析文件内的所有面要素进行两两比较
    for (size_t i = 0; i < polygonFeatures.size(); ++i) {
        const QgsFeature& feature1 = polygonFeatures[i];
        QgsGeometry geom1 = feature1.geometry();

        if (geom1.isNull()) continue;

        for (size_t j = i + 1; j < polygonFeatures.size(); ++j) {
            const QgsFeature& feature2 = polygonFeatures[j];
            QgsGeometry geom2 = feature2.geometry();
            if (geom2.isNull()) continue;

            // 检查重叠
            if (isPolygonOverlapping(geom1, geom2)) {
                // 为第一个要素创建错误记录
                QualityCheckResult result1;
                result1.passed = false;
                result1.checkType = CheckType::POLYGON_TOPOLOGY_ERROR;
                result1.errorType = "面要素重叠";
                result1.sourceFile = sourceFile;
                result1.featureId = feature1.id();
                result1.geometry = geom1;
                // 使用原始几何体作为错误几何体，确保导出的是完整的面要素
                result1.errorGeometry = geom1;
                result1.feature = feature1;
                result1.pointIndex = -1;

                // 计算重叠面积
                QgsGeometry intersection = geom1.intersection(geom2);
                double overlapArea = intersection.isNull() ? 0.0 : intersection.area();
                result1.calculatedValue = overlapArea;
                result1.standardValue = m_overlapTolerance;

                // 根据原始几何类型调整描述
                QString originalType1 = (parsedLayer->geometryType() == QgsWkbTypes::LineGeometry) ? "线要素" : "面要素";
                result1.description = QString("%1存在重叠，重叠面积 %2 平方米，超过阈值 %3 平方米")
                    .arg(originalType1)
                    .arg(result1.calculatedValue, 0, 'f', 2)
                    .arg(m_overlapTolerance, 0, 'f', 2);
                results.push_back(result1);

                // 为第二个要素也创建错误记录
                QualityCheckResult result2;
                result2.passed = false;
                result2.checkType = CheckType::POLYGON_TOPOLOGY_ERROR;
                result2.errorType = "面要素重叠";
                result2.sourceFile = sourceFile;
                result2.featureId = feature2.id();
                result2.geometry = geom2;
                result2.errorGeometry = geom2;
                result2.feature = feature2;
                result2.pointIndex = -1;
                result2.calculatedValue = overlapArea;
                result2.standardValue = m_overlapTolerance;
                result2.description = QString("%1存在重叠，重叠面积 %2 平方米，超过阈值 %3 平方米")
                    .arg(originalType1)
                    .arg(result2.calculatedValue, 0, 'f', 2)
                    .arg(m_overlapTolerance, 0, 'f', 2);
                results.push_back(result2);
            }

            // 检查间隙（只对相邻但不重叠的要素检查）
            if (!geom1.intersects(geom2) && hasAdministrativeGap(geom1, geom2)) {
                // 为第一个要素创建间隙错误记录
                QualityCheckResult result1;
                result1.passed = false;
                result1.checkType = CheckType::POLYGON_TOPOLOGY_ERROR;
                result1.errorType = "面要素间隙";
                result1.sourceFile = sourceFile;
                result1.featureId = feature1.id();
                result1.geometry = geom1;
                result1.errorGeometry = geom1;
                result1.feature = feature1;
                result1.pointIndex = -1;
                result1.calculatedValue = geom1.distance(geom2);
                result1.standardValue = m_gapTolerance;

                // 根据原始几何类型调整描述
                QString originalType1 = (parsedLayer->geometryType() == QgsWkbTypes::LineGeometry) ? "线要素" : "面要素";
                result1.description = QString("%1存在不合理间隙，间隙距离 %2 米，超过阈值 %3 米")
                    .arg(originalType1)
                    .arg(result1.calculatedValue, 0, 'f', 2)
                    .arg(m_gapTolerance, 0, 'f', 2);
                results.push_back(result1);

                // 为第二个要素也创建间隙错误记录
                QualityCheckResult result2;
                result2.passed = false;
                result2.checkType = CheckType::POLYGON_TOPOLOGY_ERROR;
                result2.errorType = "面要素间隙";
                result2.sourceFile = sourceFile;
                result2.featureId = feature2.id();
                result2.geometry = geom2;
                result2.errorGeometry = geom2;
                result2.feature = feature2;
                result2.pointIndex = -1;
                result2.calculatedValue = geom1.distance(geom2);
                result2.standardValue = m_gapTolerance;
                result2.description = QString("%1存在不合理间隙，间隙距离 %2 米，超过阈值 %3 米")
                    .arg(originalType1)
                    .arg(result2.calculatedValue, 0, 'f', 2)
                    .arg(m_gapTolerance, 0, 'f', 2);
                results.push_back(result2);
            }
        }
    }

    return results;
}
//面重叠
bool ReadShpDialog::isPolygonOverlapping(const QgsGeometry& geom1, const QgsGeometry& geom2)
{
    // 修改isValid()调用方式
    if (geom1.isNull() || geom2.isNull() || !geom1.isGeosValid() || !geom2.isGeosValid()) return false;

    QgsGeometry intersection = geom1.intersection(geom2);
    if (intersection.isEmpty() || intersection.isNull()) return false;

    double overlapArea = intersection.area();
    // 使用配置中的最小重叠面积阈值，避免微小重叠被误判
    return overlapArea > m_overlapTolerance && overlapArea > m_minOverlapArea;
}
//面间隙
bool ReadShpDialog::hasAdministrativeGap(const QgsGeometry& geom1, const QgsGeometry& geom2)
{
    // 修改isValid()调用方式
    if (geom1.isNull() || geom2.isNull() || !geom1.isGeosValid() || !geom2.isGeosValid()) return false;

    double distance = geom1.distance(geom2);
    bool areAdjacent = geom1.touches(geom2);

    // 距离大于间隙容差且不相邻时判定为错误
    if (distance > m_gapTolerance && !areAdjacent) {
        return true;
    }

    return false;
}

// ========================= 道路线：拓扑与网络连通检查 =========================
//道路线要素拓扑检查主函数
std::vector<QualityCheckResult> ReadShpDialog::checkRoadLineTopology(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile)
{
    if (RoadTopology::family(sourceFile) == RoadTopology::Road ||
        RoadTopology::baseName(sourceFile) == QStringLiteral("铁路隧道"))
        return RoadTopology::check(*this, parsedLayer, sourceFile);

    std::vector<QualityCheckResult> results;
    executedLineTopology = (parsedLayer != nullptr);


    // 统一前置无效几何体检查：记录错误
    if (collectInvalidGeometry(parsedLayer, sourceFile, CheckType::LINE_TOPOLOGY_ERROR, results)) {
        logTextEdit->append(QString::fromUtf8("发现解析图层存在无效几何体，已记录并跳过对应要素，继续检查"));
    }
    if (referenceLayer) {
        QString refSource = referenceFilePath.isEmpty() ? referenceLayer->name() : referenceFilePath;
        if (collectInvalidGeometry(referenceLayer, refSource, CheckType::LINE_TOPOLOGY_ERROR, results)) {
            logTextEdit->append(QString::fromUtf8("发现库体图层存在无效几何体，已记录并跳过对应要素，继续检查"));
        }
    }

    // 悬挂/独立始终先检查
    // 村道等道路可以合理终止于居民地，居民地端点不应判为悬挂或独立。
    std::vector<QgsVectorLayer*> settlementLayers;
    const QString settlementDir = parsedLayer ? QFileInfo(parsedLayer->source()).absolutePath() : QString();
    const QString referenceSettlementDir = referenceLayer ? QFileInfo(referenceLayer->source()).absolutePath() : QString();
    QStringList settlementSearchDirs;
    auto addSettlementSearchDir = [&](const QString& dir) {
        if (!dir.isEmpty() && !settlementSearchDirs.contains(dir)) settlementSearchDirs.push_back(dir);
    };
    addSettlementSearchDir(settlementDir);
    addSettlementSearchDir(referenceSettlementDir);
    // 批量线要素检查时，所有解析/参考道路SH​​P所在目录都可能包含居民地层。
    for (const QString& path : parseFilePaths) {
        addSettlementSearchDir(QFileInfo(path).absolutePath());
    }
    for (const QString& path : referenceFilePaths) {
        addSettlementSearchDir(QFileInfo(path).absolutePath());
    }
    const QStringList settlementNames = {
        QStringLiteral("自然村.shp"), QStringLiteral("行政村.shp"),
        QStringLiteral("乡镇级驻地.shp"), QStringLiteral("县级驻地.shp"),
        QStringLiteral("市级驻地.shp"), QStringLiteral("省外乡镇驻地.shp"),
        QStringLiteral("其他居民地.shp"), QStringLiteral("街区面.shp"),
        // 部分数据直接使用GDB标准层名导出为SHP。
        QStringLiteral("RESA.shp")
    };
    for (const QString& searchDir : settlementSearchDirs) {
        for (const QString& name : settlementNames) {
            const QString path = QDir(searchDir).filePath(name);
            if (!QFileInfo::exists(path)) continue;
            QgsVectorLayer* layer = new QgsVectorLayer(path, QFileInfo(path).baseName(), "ogr");
            if (layer->isValid()) {
                settlementLayers.push_back(layer);
                logTextEdit->append(QString("[DEBUG] 加载居民地图层: %1 CRS=%2")
                    .arg(path).arg(layer->crs().authid()));
            }
            else delete layer;
        }
    }
    logTextEdit->append(QString("[DEBUG] settlement layers loaded from parsed directory/reference directories=%1 dirs=%2")
        .arg(settlementLayers.size()).arg(settlementSearchDirs.join(";")));

    const double settlementTolerance = std::max(
        getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("dangling_tolerance")),
        getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("connectivity_tolerance")));
    auto endpointNearSettlement = [&](const QgsGeometry& lineGeom) {
        if (settlementLayers.empty() || lineGeom.isNull()) return false;
        const QgsCoordinateReferenceSystem parsedCrs =
            parsedLayer ? parsedLayer->crs() : QgsCoordinateReferenceSystem();
        const std::vector<const QgsLineString*> roadLines = extractLineStrings(lineGeom);
        if (roadLines.empty()) return false;

        for (QgsVectorLayer* settlementLayer : settlementLayers) {
            if (!settlementLayer) continue;
            const bool needsSettlementTransform =
                parsedCrs.isValid() && settlementLayer->crs().isValid() &&
                settlementLayer->crs() != parsedCrs;
            QgsCoordinateTransform settlementTransform;
            if (needsSettlementTransform) {
                settlementTransform = QgsCoordinateTransform(
                    settlementLayer->crs(), parsedCrs, QgsProject::instance());
                logTextEdit->append(QString("[DEBUG] 居民地CRS转换: %1 -> %2")
                    .arg(settlementLayer->crs().authid()).arg(parsedCrs.authid()));
            }

            QgsFeatureIterator settlementIter = settlementLayer->getFeatures();
            QgsFeature settlementFeature;
            while (settlementIter.nextFeature(settlementFeature)) {
                if (!settlementFeature.hasGeometry()) continue;
                QgsGeometry settlement = settlementFeature.geometry();
                if (needsSettlementTransform) {
                    try {
                        settlement.transform(settlementTransform);
                    }
                    catch (const QgsCsException& e) {
                        logTextEdit->append(QString("[DEBUG] 居民地CRS转换失败: %1").arg(e.what()));
                        continue;
                    }
                }

                for (const QgsLineString* line : roadLines) {
                    if (!line || line->numPoints() < 2) continue;
                    const QgsGeometry start = QgsGeometry::fromPointXY(line->startPoint());
                    const QgsGeometry end = QgsGeometry::fromPointXY(line->endPoint());
                    const QgsGeometry startBuffer = start.buffer(settlementTolerance, 8);
                    const QgsGeometry endBuffer = end.buffer(settlementTolerance, 8);
                    // 端点可直接落在线居民地上，也可落在闭合线环内部。
                    if (settlement.contains(start) || settlement.contains(end)) {
                        return true;
                    }
                    for (const QgsLineString* settlementLine : extractLineStrings(settlement)) {
                        if (!settlementLine || !settlementLine->isClosed() || settlementLine->numPoints() < 4) {
                            continue;
                        }
                        QgsPolylineXY ring;
                        ring.reserve(settlementLine->numPoints());
                        for (int i = 0; i < settlementLine->numPoints(); ++i) {
                            ring.push_back(QgsPointXY(settlementLine->pointN(i)));
                        }
                        QgsPolygonXY polygon;
                        polygon.push_back(ring);
                        const QgsGeometry settlementArea = QgsGeometry::fromPolygonXY(polygon);
                        if (settlementArea.contains(start) || settlementArea.contains(end)) {
                            return true;
                        }
                    }
                    if (settlement.intersects(startBuffer) ||
                        settlement.intersects(endBuffer) ||
                        settlement.distance(start) <= settlementTolerance ||
                        settlement.distance(end) <= settlementTolerance) {
                        return true;
                    }
                }
            }
        }
        return false;
    };

    std::vector<QgsVectorLayer*> roadNetworkLayers;
    std::vector<QgsVectorLayer*> ownedRoadLayers;
    auto addRoadNetworkLayer = [&](QgsVectorLayer* layer) {
        if (!layer) return;
        if (std::find(roadNetworkLayers.begin(), roadNetworkLayers.end(), layer) == roadNetworkLayers.end()) {
            roadNetworkLayers.push_back(layer);
        }
    };
    addRoadNetworkLayer(parsedLayer);
    addRoadNetworkLayer(referenceLayer);

    // 同目录下的其他道路成果也纳入空间独立判断，避免跨实际道路图层误报。
    if (!settlementDir.isEmpty()) {
        const QString parsedCanonical = parsedLayer ? QFileInfo(parsedLayer->source()).canonicalFilePath() : QString();
        const QString referenceCanonical = referenceLayer ? QFileInfo(referenceLayer->source()).canonicalFilePath() : QString();
        const QFileInfoList roadFiles = QDir(settlementDir).entryInfoList(QStringList() << QStringLiteral("*.shp"), QDir::Files);
        for (const QFileInfo& roadFile : roadFiles) {
            const QString canonical = roadFile.canonicalFilePath();
            if (canonical == parsedCanonical || canonical == referenceCanonical || !isRoadFile(roadFile.fileName())) continue;
            QgsVectorLayer* roadLayer = new QgsVectorLayer(roadFile.absoluteFilePath(), roadFile.baseName(), "ogr");
            if (roadLayer->isValid()) {
                ownedRoadLayers.push_back(roadLayer);
                addRoadNetworkLayer(roadLayer);
            }
            else {
                delete roadLayer;
            }
        }
    }

    auto danglingResults = checkRoadDangling(parsedLayer, sourceFile);
    danglingResults.erase(std::remove_if(danglingResults.begin(), danglingResults.end(),
        [&](const QualityCheckResult& r) {
            return r.errorType == QStringLiteral("道路悬挂") && endpointNearSettlement(r.geometry);
        }), danglingResults.end());

    auto isolationResults = checkRoadIsolation(parsedLayer, referenceLayer, sourceFile, roadNetworkLayers);

    // 居民地只豁免合理终止造成的悬挂，不取消整条道路的独立性判断。
    results.insert(results.end(), danglingResults.begin(), danglingResults.end());
    results.insert(results.end(), isolationResults.begin(), isolationResults.end());

    // 收集已在悬挂/独立中报错的要素ID，避免重复
    QSet<QgsFeatureId> badFeatureIds;
    for (const auto& r : danglingResults) {
        if (r.featureId >= 0) badFeatureIds.insert(r.featureId);
    }
    for (const auto& r : isolationResults) {
        if (r.featureId >= 0) badFeatureIds.insert(r.featureId);
    }

    // 改动点：只要存在参考图层，就执行跨图层路网连通性检查，并对已报错要素去重
    if (referenceLayer) {
        logTextEdit->append(QString("[DEBUG] 准备调用路网通达性检查: 解析要素=%1, 参考要素=%2")
            .arg(parsedLayer ? parsedLayer->featureCount() : 0)
            .arg(referenceLayer ? referenceLayer->featureCount() : 0));

        auto connectionResults = checkRoadNetworkConnection(parsedLayer, referenceLayer, sourceFile);

        std::vector<QualityCheckResult> filteredConnectionResults;
        filteredConnectionResults.reserve(connectionResults.size());
        int settlementExemptedConnectionCount = 0;
        for (const auto& cr : connectionResults) {
            // 道路端点合理终止于居民地时，不应再报“路网通达性-悬挂”。
            // 该结果由路网通达性检查单独生成，不能只依赖前面的“道路悬挂”过滤。
            const bool isSettlementExemptedEndpointResult =
                (cr.errorType == QStringLiteral("路网通达性-悬挂") ||
                 cr.errorType == QStringLiteral("路网通达性-独立")) &&
                endpointNearSettlement(cr.geometry);
            if (isSettlementExemptedEndpointResult) {
                ++settlementExemptedConnectionCount;
                continue;
            }
            if (!badFeatureIds.contains(cr.featureId)) {
                filteredConnectionResults.push_back(cr);
            }
        }

        logTextEdit->append(QString("[DEBUG] 路网通达性返回结果=%1（去重后=%2）")
            .arg(connectionResults.size())
            .arg(filteredConnectionResults.size()));
        logTextEdit->append(QString("[DEBUG] settlement endpoint exemptions=%1")
            .arg(settlementExemptedConnectionCount));

        results.insert(results.end(), filteredConnectionResults.begin(), filteredConnectionResults.end());
    }
    else {
        logTextEdit->append("[DEBUG] 参考图层为空，跳过路网通达性检查");
    }



    for (QgsVectorLayer* layer : settlementLayers) delete layer;
    for (QgsVectorLayer* layer : ownedRoadLayers) delete layer;
    return results;
}
//检查悬挂线段（只在解析文件内部）
bool ReadShpDialog::isDanglingLineInternal(const QgsGeometry& lineGeom, QgsVectorLayer* parsedLayer)
{
    if (lineGeom.isNull() || !lineGeom.isGeosValid() || lineGeom.type() != QgsWkbTypes::LineGeometry) return false;

    const QgsLineString* line = qgsgeometry_cast<const QgsLineString*>(lineGeom.constGet());
    if (!line || line->numPoints() < 2) return false;

    QgsPoint startPoint = line->startPoint();
    QgsPoint endPoint = line->endPoint();

  // 使用已转换后的悬挂容差（由 updateParametersFromConfig 计算并缓存）
    double danglingTolerance = m_danglingTolerance;

    // 检查起点和终点是否与解析图层中的其他线要素连接
    QgsFeatureIterator iter = parsedLayer->getFeatures();
    QgsFeature feature;

    bool startConnected = false, endConnected = false;

    while (iter.nextFeature(feature)) {
        if (!feature.hasGeometry()) continue;

        QgsGeometry geom = feature.geometry();
        if (geom.type() != QgsWkbTypes::LineGeometry) continue;

        // 跳过自身
        if (geom.equals(lineGeom)) continue;

        // 检查距离
        if (!startConnected && geom.distance(QgsGeometry::fromPointXY(startPoint)) < danglingTolerance) {
            startConnected = true;
        }
        if (!endConnected && geom.distance(QgsGeometry::fromPointXY(endPoint)) < danglingTolerance) {
            endConnected = true;
        }

        if (startConnected && endConnected) break;
    }

    return !startConnected || !endConnected;
}
//检查孤立线段（只在解析文件内部）
bool ReadShpDialog::isIsolatedLineInternal(const QgsGeometry& lineGeom, QgsVectorLayer* parsedLayer, const std::vector<QgsVectorLayer*>& roadNetworkLayers)
{
    if (lineGeom.isNull() || !lineGeom.isGeosValid()) return false;

    // 从XML配置获取参数
    double minLineLength = m_minLineLength;
    // 使用已转换后的孤立距离（直接调用转换助手）
    double isolationDistance = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("isolation_distance"));
    // 检查线段长度
    if (lineGeom.length() < minLineLength) {
        return true;
    }

    // 检查所有已汇总道路图层，空间连接只负责排除“独立”分类。
    for (QgsVectorLayer* roadLayer : roadNetworkLayers) {
        if (!roadLayer) continue;
        QgsFeatureIterator roadIter = roadLayer->getFeatures();
        QgsFeature feature;
        while (roadIter.nextFeature(feature)) {
            if (!feature.hasGeometry()) continue;
            const QgsGeometry geom = feature.geometry();
            if (geom.type() != QgsWkbTypes::LineGeometry) continue;
            if (roadLayer == parsedLayer && geom.equals(lineGeom)) continue;
            if (lineGeom.touches(geom) || lineGeom.intersects(geom) ||
                lineGeom.distance(geom) < isolationDistance) {
                return false;
            }
        }
    }

    return true;  // 没有找到连接，是孤立线段
}
//线要素拓扑检查（道路河流）,从文件名识别道路类型
int ReadShpDialog::getRoadTypeFromFileName(const QString& fileName)
{
    QString baseName = QFileInfo(fileName).baseName().toLower();

    if (baseName.contains("高速公路") || baseName.contains("高速")) {
        return 1; // 高速公路
    }
    else if (baseName.contains("国道")) {
        return 2; // 国道
    }
    else if (baseName.contains("省道")) {
        return 3; // 省道
    }
    else if (baseName.contains("县道")) {
        return 4; // 县道
    }
    else if (baseName.contains("乡道")) {
        return 5; // 乡道
    }
    else if (baseName.contains("村道")) {
        return 8; // 村道
    }
    else if (baseName.contains("快速路") || baseName.contains("快速")) {
        return 6; // 快速路
    }
    else if (baseName.contains("连接道") || baseName.contains("连接")) {
        return 7; // 连接道
    }

    return 0; // 未知类型
}
//从属性表获取道路类型
int ReadShpDialog::getRoadTypeFromAttribute(const QgsFeature& feature)
{
    QgsFields fields = feature.fields();
    // CLASID是XML/图面配置使用的标准分类字段，优先于通用TYPE字段。
    const int clasidIndex = fields.indexFromName("CLASID");
    if (clasidIndex != -1) {
        const QString clasid = feature.attribute(clasidIndex).toString().trimmed();
        if (clasid == "420000") return 7; // 连接道
        if (clasid == "420101") return 2; // 国道
        if (clasid == "420201") return 3; // 省道
        if (clasid == "420301") return 4; // 县道
        if (clasid == "420400") return 5; // 乡道
        if (clasid == "420901") return 1; // 高速公路
    }
    int typeIndex = fields.indexFromName("TYPE");

    if (typeIndex != -1) {
        QVariant raw = feature.attribute(typeIndex);

        // 尝试数字代码
        bool ok = false;
        int numeric = raw.toString().trimmed().toInt(&ok);
        if (ok && numeric > 0) {
            return numeric;
        }

        // 文本映射（中文/英文）
        QString s = raw.toString().toLower().trimmed();
        if (s.contains("连接道") || s.contains("连接") || s.contains("connector") || s.contains("link")) return 7;
        if (s.contains("高速公路") || s.contains("高速") || s.contains("highway") || s.contains("expressway")) return 1;
        if (s.contains("国道") || s.contains("national")) return 2;
        if (s.contains("省道") || s.contains("provincial")) return 3;
        if (s.contains("县道") || s.contains("county")) return 4;
        if (s.contains("乡道") || s.contains("township")) return 5;
        if (s.contains("村道") || s.contains("village")) return 8;
        if (s.contains("快速路") || s.contains("快速") || s.contains("fast")) return 6;
    }

    // 兜底：其他常见字段
    QStringList levelFields = { "ROAD_LEVEL", "道路等级", "DLDJMC", "LEVEL", "CLASS" };
    for (const QString& fieldName : levelFields) {
        int idx = fields.indexFromName(fieldName);
        if (idx != -1) {
            QString v = feature.attribute(idx).toString().toLower().trimmed();

            bool ok2 = false;
            int num2 = v.toInt(&ok2);
            if (ok2 && num2 > 0) return num2;

            if (v.contains("连接道") || v.contains("连接") || v.contains("connector") || v.contains("link")) return 7;
            if (v.contains("高速公路") || v.contains("高速") || v.contains("highway") || v.contains("expressway")) return 1;
            if (v.contains("国道") || v.contains("national")) return 2;
            if (v.contains("省道") || v.contains("provincial")) return 3;
            if (v.contains("县道") || v.contains("county")) return 4;
            if (v.contains("乡道") || v.contains("township")) return 5;
            if (v.contains("村道") || v.contains("village")) return 8;
            if (v.contains("快速路") || v.contains("快速") || v.contains("fast")) return 6;
        }
    }

    return 0; // 未知类型
}
//判断是否为道路文件
bool ReadShpDialog::isRoadFile(const QString& fileName)
{
    return RoadTopology::family(fileName) != RoadTopology::None;
}
//道路库体文件识别函数
bool ReadShpDialog::isLRDLFile(const QString& fileName)
{
    QString baseName = QFileInfo(fileName).baseName();
    return baseName.contains("LRDL", Qt::CaseInsensitive)
        || baseName.contains("LRRL", Qt::CaseInsensitive)
        || baseName.contains("LFCL", Qt::CaseInsensitive)
        || baseName.contains("GWL", Qt::CaseInsensitive)
        || baseName.contains("LVLL", Qt::CaseInsensitive);
}
//检查道路悬挂（仅针对解析文件内部）
std::vector<QualityCheckResult> ReadShpDialog::checkRoadDangling(QgsVectorLayer* parsedLayer, const QString& sourceFile)
{
    std::vector<QualityCheckResult> results;
    // 从配置读取参数，替换硬编码
    double danglingTolerance = m_config ? m_config->getLineTopologyDanglingTolerance() : 0.5;
    if (!parsedLayer) {
        return results;
    }

    QgsFeatureIterator iter = parsedLayer->getFeatures();
    QgsFeature feature;

    while (iter.nextFeature(feature)) {
        if (!feature.hasGeometry()) continue;

        QgsGeometry geom = feature.geometry();
        if (geom.type() != QgsWkbTypes::LineGeometry) continue;

        // 检查是否为悬挂线
        if (isDanglingLineInternal(geom, parsedLayer)) {
            QualityCheckResult result;
            result.passed = false;
            result.checkType = CheckType::LINE_TOPOLOGY_ERROR;
            result.errorType = "道路悬挂";
            result.sourceFile = sourceFile;
            result.featureId = feature.id();
            result.geometry = geom;
            result.errorGeometry = geom;
            result.feature = feature;
            result.pointIndex = -1;
            result.calculatedValue = 0.0;
            // 显示经比例尺转换后的配置阈值（悬挂容差）
            result.standardValue = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("dangling_tolerance"));
            result.description = QString("道路存在悬挂，未与其他道路正确连接");

            results.push_back(result);
        }
    }

    return results;
}
//检查道路不合理独立（仅针对解析文件内部）
std::vector<QualityCheckResult> ReadShpDialog::checkRoadIsolation(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile, const std::vector<QgsVectorLayer*>& roadNetworkLayers)
{
    std::vector<QualityCheckResult> results;
    // 从配置读取参数，替换硬编码
    double minLineLength = m_minLineLength;
    double isolationDistance = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("isolation_distance"));
    if (!parsedLayer) {
        return results;
    }

    QgsFeatureIterator iter = parsedLayer->getFeatures();
    QgsFeature feature;

    while (iter.nextFeature(feature)) {
        if (!feature.hasGeometry()) continue;

        QgsGeometry geom = feature.geometry();
        if (geom.type() != QgsWkbTypes::LineGeometry) continue;

        // 检查是否为不合理独立线
        if (isIsolatedLineInternal(geom, parsedLayer, roadNetworkLayers)) {
            QualityCheckResult result;
            result.passed = false;
            result.checkType = CheckType::LINE_TOPOLOGY_ERROR;
            result.errorType = "道路不合理独立";
            result.sourceFile = sourceFile;
            result.featureId = feature.id();
            result.geometry = geom;
            result.errorGeometry = geom;
            result.feature = feature;
            result.pointIndex = -1;
            result.calculatedValue = calculateMinDistanceToOtherLines(geom, parsedLayer, feature.id());
            // 标准值改为“孤立距离”配置（经比例尺转换）
            result.standardValue = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("isolation_distance"));
            result.description = QString("道路不合理独立，与最近道路距离 %1 米")
                .arg(result.calculatedValue, 0, 'f', 2);

            results.push_back(result);
        }
    }

    return results;
}
//检查道路网连接情况（与库体数据对比）路网连通性主函数
std::vector<QualityCheckResult> ReadShpDialog::checkRoadNetworkConnection(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile)
{
    std::vector<QualityCheckResult> results;

    if (!parsedLayer || !referenceLayer) {
        return results;
    }

    logTextEdit->append(QString("[DEBUG] 进入路网通达性检查: 解析='%1' 参考='%2' source='%3'")
        .arg(parsedLayer ? parsedLayer->name() : "null")
        .arg(referenceLayer ? referenceLayer->name() : "null")
        .arg(sourceFile));

    // 门控：只有“按文件名识别为道路”才执行
    if (!isRoadFile(sourceFile)) {
        logTextEdit->append(QString("[DEBUG] 跳过路网通达性检查：解析文件非道路 -> %1").arg(QFileInfo(sourceFile).baseName()));
        return results;
    }

    // 构建库体道路网络图（按属性分组）
    std::map<int, std::vector<QgsFeature>> referenceRoadsByType;
    const int configuredReferenceType = resolveConfiguredReferenceRoadType(referenceLayer);
    if (configuredReferenceType > 0) {
        logTextEdit->append(QString("[DEBUG] 库体图层按标准图层语义统一识别: %1 -> %2")
            .arg(referenceLayer->name()).arg(getRoadTypeName(configuredReferenceType)));
    }
    QgsFeatureIterator refIter = referenceLayer->getFeatures();
    QgsFeature refFeature;
    int totalRefFeatures = 0;
    while (refIter.nextFeature(refFeature)) {
        if (!refFeature.hasGeometry()) continue;
        ++totalRefFeatures;
        int roadType = getRoadTypeFromAttribute(refFeature);
        if (configuredReferenceType > 0) roadType = configuredReferenceType;
        if (roadType > 0) {
            referenceRoadsByType[roadType].push_back(refFeature);
        }
    }
    if (referenceRoadsByType.empty()) {
        QgsFeatureIterator refIter2 = referenceLayer->getFeatures();
        QgsFeature refFeature2;
        while (refIter2.nextFeature(refFeature2)) {
            if (!refFeature2.hasGeometry()) continue;
            referenceRoadsByType[7].push_back(refFeature2);
            ++totalRefFeatures;
        }
    }
    int groupedTypeCount = static_cast<int>(referenceRoadsByType.size());
    long long groupedFeatureSum = 0;
    for (const auto& kv : referenceRoadsByType) groupedFeatureSum += kv.second.size();
    logTextEdit->append(QString("[DEBUG] 库体网络分组完成: 类型组=%1, 分组总要素=%2, 原始要素总数(估算)=%3")
        .arg(groupedTypeCount).arg(groupedFeatureSum).arg(totalRefFeatures));

    // 解析侧道路类型：使用“解析文件名”识别（整个文件统一类型）
    int parsedRoadType = getRoadTypeFromFileName(sourceFile);
    if (parsedRoadType == 0) {
        logTextEdit->append(QString("[DEBUG] 解析文件名未识别道路类型，跳过通达性检查 -> %1").arg(QFileInfo(sourceFile).baseName()));
        return results;
    }
    // 现有库体TYPE=6在村道任务中表示村道，统一到村道类型8，避免与快速路混淆。
    if (parsedRoadType == 8) {
        auto villageTypeIt = referenceRoadsByType.find(6);
        if (villageTypeIt != referenceRoadsByType.end() &&
            referenceRoadsByType.find(8) == referenceRoadsByType.end()) {
            referenceRoadsByType[8] = villageTypeIt->second;
            referenceRoadsByType.erase(villageTypeIt);
        }
    }
    logTextEdit->append(QString("[DEBUG] 解析文件名识别道路类型=%1").arg(parsedRoadType));

    // 遍历解析要素并检查端点连接
    QgsFeatureIterator parsedIter = parsedLayer->getFeatures();
    QgsFeature parsedFeature;
    int totalParsed = 0, nonLineCount = 0, noGeomCount = 0;
    int endpointsFailCount = 0, evaluatedCount = 0, emittedErrors = 0;

    while (parsedIter.nextFeature(parsedFeature)) {
        ++totalParsed;
        if (!parsedFeature.hasGeometry()) { ++noGeomCount; continue; }

        QgsGeometry parsedGeom = parsedFeature.geometry();
        if (parsedGeom.type() != QgsWkbTypes::LineGeometry) { ++nonLineCount; continue; }

        QgsPointXY startPoint, endPoint;
        if (!getRoadEndpoints(parsedGeom, startPoint, endPoint)) { ++endpointsFailCount; continue; }

        ++evaluatedCount;

        QString detailedErrorInfo = analyzeRoadConnectionDetails(parsedGeom, parsedRoadType, referenceRoadsByType, startPoint, endPoint);
        if (!detailedErrorInfo.isEmpty()) {
            // 统一使用经比例尺转换后的“连通性容差”
            double connectionTolerance = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("connectivity_tolerance"));
            bool startDisconnected = isPointDisconnectedFromAnyRoad(startPoint, referenceRoadsByType, connectionTolerance);
            bool endDisconnected = isPointDisconnectedFromAnyRoad(endPoint, referenceRoadsByType, connectionTolerance);
            QualityCheckResult result;
            result.passed = false;
            result.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
            result.errorType = (startDisconnected && endDisconnected)
                ? "路网通达性-独立"
                : ((startDisconnected || endDisconnected) ? "路网通达性-悬挂" : "道路网连接错误");
            result.sourceFile = sourceFile;
            result.featureId = parsedFeature.id();
            result.geometry = parsedGeom;
            result.errorGeometry = parsedGeom;
            result.feature = parsedFeature;
            result.pointIndex = -1;
            result.calculatedValue = 0.0;
            // 标准值改为转换后的连通性容差
            result.standardValue = connectionTolerance;
            result.description = detailedErrorInfo;

            results.push_back(result);
            ++emittedErrors;
        }
    }

    logTextEdit->append(QString("[DEBUG] 通达性检查统计: 解析要素总数=%1, 无几何=%2, 非线要素=%3, 端点失败=%4, 参与检查=%5, 产生错误=%6")
        .arg(totalParsed).arg(noGeomCount).arg(nonLineCount).arg(endpointsFailCount).arg(evaluatedCount).arg(emittedErrors));

    return results;

}
//检查路网连通性-悬挂
bool ReadShpDialog::isPointDanglingForRoadNetwork(const QgsPointXY& point, int roadType, const std::map<int, std::vector<QgsFeature>>& referenceRoadsByType, double tolerance)
{
    QgsGeometry pointGeom = QgsGeometry::fromPointXY(point);

    double minAllowedDistance = std::numeric_limits<double>::max();
    bool hasAllowedType = false;

    for (const auto& typeGroup : referenceRoadsByType) {
        int refRoadType = typeGroup.first;
        if (!isValidRoadConnection(roadType, refRoadType)) {
            continue; // 只看允许连接的类型
        }
        hasAllowedType = true;

        const std::vector<QgsFeature>& refRoads = typeGroup.second;
        for (const QgsFeature& refRoad : refRoads) {
            const QgsGeometry& refGeom = refRoad.geometry();
            double dist = refGeom.distance(pointGeom);
            if (dist < minAllowedDistance) {
                minAllowedDistance = dist;
            }
        }
    }

    if (!hasAllowedType) {
        // 类型识别失败或库体缺失时，按悬挂处理以避免误报“连接错误”
        return true;
    }

    // 在允许类型集合下，若最小距离仍大于容差则视为悬挂
    return minAllowedDistance > tolerance;
}
bool ReadShpDialog::isPointDisconnectedFromAnyRoad(const QgsPointXY& point, const std::map<int, std::vector<QgsFeature>>& referenceRoadsByType, double tolerance)
{
    QgsGeometry pointGeom = QgsGeometry::fromPointXY(point);

    double minDistanceAny = std::numeric_limits<double>::max();
    for (const auto& typeGroup : referenceRoadsByType) {
        const std::vector<QgsFeature>& refRoads = typeGroup.second;
        for (const QgsFeature& refRoad : refRoads) {
            const QgsGeometry& refGeom = refRoad.geometry();
            double d = refGeom.distance(pointGeom);
            if (d < minDistanceAny) {
                minDistanceAny = d;
            }
        }
    }

    if (referenceRoadsByType.empty()) {
        return true;
    }
    return minDistanceAny > tolerance;
}
//分析道路连接的详细错误信息
QString ReadShpDialog::analyzeRoadConnectionDetails(const QgsGeometry& roadGeom, int roadType, const std::map<int, std::vector<QgsFeature>>& referenceRoadsByType, const QgsPointXY& startPoint, const QgsPointXY& endPoint)
{
    QString roadTypeName = getRoadTypeName(roadType);
    // 使用经比例尺转换后的“连通性容差”，保持描述与阈值一致
    double connectionTolerance = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("connectivity_tolerance"));
    QString startConnectionInfo = analyzePointConnection(startPoint, roadType, referenceRoadsByType, connectionTolerance, "起点");
    QString endConnectionInfo = analyzePointConnection(endPoint, roadType, referenceRoadsByType, connectionTolerance, "终点");
    QString errorInfo = "";

    if (!startConnectionInfo.isEmpty() || !endConnectionInfo.isEmpty()) {
        errorInfo = QString("道路类型：%1\n").arg(roadTypeName);

        if (!startConnectionInfo.isEmpty()) {
            errorInfo += startConnectionInfo;
        }

        if (!endConnectionInfo.isEmpty()) {
            if (!startConnectionInfo.isEmpty()) {
                errorInfo += "\n";
            }
            errorInfo += endConnectionInfo;
        }

        // 添加连接规则说明
        errorInfo += QString("\n\n连接规则：%1只允许连接同类型道路或连接道").arg(roadTypeName);
    }

    return errorInfo;
}
//分析单个端点的连接情况
QString ReadShpDialog::analyzePointConnection(const QgsPointXY& point, int roadType, const std::map<int, std::vector<QgsFeature>>& referenceRoadsByType, double tolerance, const QString& pointName)
{
    QgsGeometry pointGeom = QgsGeometry::fromPointXY(point);

    // 查找最近的道路连接
    double minDistance = std::numeric_limits<double>::max();
    int nearestRoadType = -1;
    QString nearestRoadInfo = "";
    bool hasValidConnection = false;

    for (const auto& typeGroup : referenceRoadsByType) {
        int refRoadType = typeGroup.first;
        const std::vector<QgsFeature>& refRoads = typeGroup.second;

        for (const QgsFeature& refRoad : refRoads) {
            QgsGeometry refGeom = refRoad.geometry();
            double distance = refGeom.distance(pointGeom);

            if (distance < minDistance) {
                minDistance = distance;
                nearestRoadType = refRoadType;
                nearestRoadInfo = QString("距离最近道路：%1（距离：%2米）")
                    .arg(getRoadTypeName(refRoadType))
                    .arg(distance, 0, 'f', 2);
            }

            // 检查是否在容差范围内且连接规则有效
            if (distance <= tolerance && isValidRoadConnection(roadType, refRoadType)) {
                hasValidConnection = true;
            }
        }
    }

    if (hasValidConnection) {
        return ""; // 有有效连接，无错误
    }

    QString errorInfo = QString("%1连接错误：").arg(pointName);

    if (minDistance == std::numeric_limits<double>::max()) {
        errorInfo += "未找到任何库体道路";
    }
    else if (minDistance > tolerance) {
        errorInfo += QString("无连接（%1，超出%2米容差范围）")
            .arg(nearestRoadInfo)
            .arg(tolerance, 0, 'f', 2);
    }
    else {
        // 在容差范围内但连接规则不匹配
        QString expectedConnections = getValidConnectionTypes(roadType);
        errorInfo += QString("连接类型不匹配\n  实际连接：%1\n  允许连接：%2")
            .arg(getRoadTypeName(nearestRoadType))
            .arg(expectedConnections);
    }

    return errorInfo;
}
//获取道路类型名称
QString ReadShpDialog::getRoadTypeName(int roadType)
{
    switch (roadType) {
    case 1: return "高速公路";
    case 2: return "国道";
    case 3: return "省道";
    case 4: return "县道";
    case 5: return "乡道";
    case 6: return "快速路";
    case 7: return "连接道";
    case 8: return "村道";
    default: return QString("未知类型(%1)").arg(roadType);
    }
}
//获取有效连接类型列表
QString ReadShpDialog::getValidConnectionTypes(int roadType)
{
    QStringList validTypes;

    // 连接道可以连接任何类型
    if (roadType == 7) {
        validTypes << "高速公路" << "国道" << "省道" << "县道" << "乡道" << "快速路" << "连接道";
    }
    else {
        // 同级道路
        validTypes << getRoadTypeName(roadType);

        // 连接道
        validTypes << "连接道";
    }

    return validTypes.join("、");
}
//检查道路连接的有效性
bool ReadShpDialog::checkRoadConnectionValidity(const QgsGeometry& roadGeom, int roadType, const std::map<int, std::vector<QgsFeature>>& referenceRoadsByType)
{
    // 获取道路的端点 
    QgsPointXY startPoint, endPoint;
    if (!getRoadEndpoints(roadGeom, startPoint, endPoint)) {
        return false;
    }

    // 检查每个端点是否有合理的连接 
    bool startConnected = false, endConnected = false;
    double connectionTolerance = m_config ? m_config->getConnectivityTolerance() : 5.0; // 使用配置容差

    for (const auto& typeGroup : referenceRoadsByType) {
        int refRoadType = typeGroup.first;
        const std::vector<QgsFeature>& refRoads = typeGroup.second;

        // 检查连接规则：同级或相邻级别道路可以连接 
        if (!isValidRoadConnection(roadType, refRoadType)) {
            continue;
        }

        for (const QgsFeature& refRoad : refRoads) {
            QgsGeometry refGeom = refRoad.geometry();

            // 检查起点连接 - 使用距离计算 
            if (!startConnected) {
                QgsGeometry startPointGeom = QgsGeometry::fromPointXY(startPoint);
                double distanceToStart = refGeom.distance(startPointGeom);
                if (distanceToStart <= connectionTolerance) {
                    startConnected = true;
                }
            }

            // 检查终点连接 - 使用距离计算 
            if (!endConnected) {
                QgsGeometry endPointGeom = QgsGeometry::fromPointXY(endPoint);
                double distanceToEnd = refGeom.distance(endPointGeom);
                if (distanceToEnd <= connectionTolerance) {
                    endConnected = true;
                }
            }

            if (startConnected && endConnected) {
                return true;
            }
        }
    }

    return startConnected && endConnected;
}
//获取道路端点
bool ReadShpDialog::getRoadEndpoints(const QgsGeometry& roadGeom, QgsPointXY& startPoint, QgsPointXY& endPoint)
{
    if (roadGeom.type() != QgsWkbTypes::LineGeometry) {
        return false;
    }

    const QgsLineString* line = getFirstLineString(roadGeom);
    if (!line || line->numPoints() < 2) {
        return false;
    }

    startPoint = QgsPointXY(line->pointN(0));
    endPoint = QgsPointXY(line->pointN(line->numPoints() - 1));

    return true;
}
//检查道路连接规则
bool ReadShpDialog::isValidRoadConnection(int roadType1, int roadType2)
{
    // 道路类型：1-高速公路, 2-国道, 3-省道, 4-县道, 5-乡道, 6-快速路, 7-连接道, 8-村道

    // 连接道可以连接任何类型；普通道路只允许同类型连接。
    if (roadType1 == 7 || roadType2 == 7) {
        return true;
    }
    return roadType1 > 0 && roadType1 == roadType2;
}
//解析文件的道路等级识别（通过文件名）
int ReadShpDialog::getRoadLevelFromFileName(const QString& sourceFile)
{
    if (sourceFile.isEmpty()) return 7;

    QString fileName = QFileInfo(sourceFile).baseName().toLower();

    // 通过文件名识别道路等级
    if (fileName.contains("高速") || fileName.contains("highway") || fileName.contains("expressway")) {
        return 1; // 高速公路
    }
    if (fileName.contains("国道") || fileName.contains("national")) {
        return 2; // 国道
    }
    if (fileName.contains("省道") || fileName.contains("provincial")) {
        return 3; // 省道
    }
    if (fileName.contains("县道") || fileName.contains("county")) {
        return 4; // 县道
    }
    if (fileName.contains("乡道") || fileName.contains("township")) {
        return 5; // 乡道
    }
    if (fileName.contains("村道") || fileName.contains("village")) {
        return 6; // 村道
    }
    if (fileName.contains("连接道") || fileName.contains("connector") || fileName.contains("link")) {
        return 5; // 连接道按乡道级别处理
    }

    return 7; // 未知等级
}
//库体数据的道路等级识别（通过TYPE属性）
int ReadShpDialog::getRoadLevelFromAttribute(const QgsFeature& feature)
{
    QgsFields fields = feature.fields();

    // 检查TYPE字段
    int typeIndex = fields.indexFromName("TYPE");
    if (typeIndex != -1) {
        QString typeValue = feature.attribute(typeIndex).toString().toLower();

        // 根据TYPE值判断道路等级
        if (typeValue.contains("高速") || typeValue.contains("highway") || typeValue == "1") {
            return 1; // 高速公路
        }
        if (typeValue.contains("国道") || typeValue.contains("national") || typeValue == "2") {
            return 2; // 国道
        }
        if (typeValue.contains("省道") || typeValue.contains("provincial") || typeValue == "3") {
            return 3; // 省道
        }
        if (typeValue.contains("县道") || typeValue.contains("county") || typeValue == "4") {
            return 4; // 县道
        }
        if (typeValue.contains("乡道") || typeValue.contains("township") || typeValue == "5") {
            return 5; // 乡道
        }
        if (typeValue.contains("村道") || typeValue.contains("village") || typeValue == "6") {
            return 6; // 村道
        }
        if (typeValue.contains("连接道") || typeValue.contains("connector") || typeValue.contains("link")) {
            return 5; // 连接道按乡道级别处理
        }
    }

    // 如果TYPE字段不存在或无法识别，尝试其他字段
    QStringList levelFields = { "ROAD_LEVEL", "道路等级", "DLDJMC", "LEVEL", "CLASS" };
    for (const QString& fieldName : levelFields) {
        int fieldIndex = fields.indexFromName(fieldName);
        if (fieldIndex != -1) {
            QString value = feature.attribute(fieldIndex).toString().toLower();

            if (value.contains("高速") || value.contains("highway")) return 1;
            if (value.contains("国道") || value.contains("national")) return 2;
            if (value.contains("省道") || value.contains("provincial")) return 3;
            if (value.contains("县道") || value.contains("county")) return 4;
            if (value.contains("乡道") || value.contains("township")) return 5;
            if (value.contains("村道") || value.contains("village")) return 6;
        }
    }

    return 7; // 未知等级
}
//统一的道路等级获取函数
int ReadShpDialog::getRoadLevel(const QgsFeature& feature, const QString& sourceFile, bool isReferenceLayer)
{
    if (isReferenceLayer) {
        // 库体数据使用属性表TYPE字段
        return getRoadLevelFromAttribute(feature);
    }
    else {
        // 解析文件优先使用文件名，备选属性字段
        int levelFromFile = getRoadLevelFromFileName(sourceFile);
        if (levelFromFile != 7) {
            return levelFromFile;
        }
        // 如果文件名无法识别，使用属性字段
        return getRoadLevelFromAttribute(feature);
    }
}
//计算到其他线要素的最小距离，线要素内部拓扑
double ReadShpDialog::calculateMinDistanceToOtherLines(const QgsGeometry& lineGeom, QgsVectorLayer* layer, QgsFeatureId excludeId)
{
    double minDistance = std::numeric_limits<double>::max();

    QgsFeatureIterator iter = layer->getFeatures();
    QgsFeature feature;

    while (iter.nextFeature(feature)) {
        if (!feature.hasGeometry() || feature.id() == excludeId) continue;

        QgsGeometry geom = feature.geometry();
        if (geom.type() == QgsWkbTypes::LineGeometry) {
            double dist = lineGeom.distance(geom);
            if (dist < minDistance) {
                minDistance = dist;
            }
        }
    }

    return minDistance;
}

// ========================= 河流线：拓扑与网络连通检查 =========================
//河流线要素拓扑检查,判断是否为河流文件
bool ReadShpDialog::isRiverFile(const QString& fileName)
{
    QString baseName = QFileInfo(fileName).baseName().toLower();
    // 排除包含"_标注"的文件
    if (baseName.contains("_标注")) {
        return false;
    }
    // 只识别水系线要素文件，明确排除面要素
    if (baseName.contains("水系面") || baseName.contains("水面") ||
        baseName.contains("水系面_linestring") ||
        baseName.contains("polygon") || baseName.contains("_polygon")) {
        return false;
    }

    // 明确匹配水系线要素
    QStringList riverLineKeywords = {
        "水系_linestring", "水系线", "河流_linestring", "河流线","沟渠",
        "river_linestring", "stream_linestring", "waterway_linestring"
    };

    for (const QString& keyword : riverLineKeywords) {
        if (baseName.contains(keyword)) {
            return true;
        }
    }

    return false;
}
//河流线要素拓扑检查主函数
std::vector<QualityCheckResult> ReadShpDialog::checkRiverLineTopology(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile)
{
    std::vector<QualityCheckResult> results;
    executedLineTopology = (parsedLayer != nullptr);
    // 统一前置无效几何体检查：记录错误
    if (collectInvalidGeometry(parsedLayer, sourceFile, CheckType::LINE_TOPOLOGY_ERROR, results)) {
        logTextEdit->append(QString::fromUtf8("发现解析图层存在无效几何体，已记录并跳过对应要素，继续检查"));
    }
    if (referenceLayer) {
        QString refSource = referenceFilePath.isEmpty() ? referenceLayer->name() : referenceFilePath;
        if (collectInvalidGeometry(referenceLayer, refSource, CheckType::LINE_TOPOLOGY_ERROR, results)) {
            logTextEdit->append(QString::fromUtf8("发现库体图层存在无效几何体，已记录并跳过对应要素，继续检查"));
        }
    }

    // 从配置读取参数，替换硬编码
    double waterSystemConnectionTolerance = m_config ? m_config->getWaterSystemConnectionToleranceNew() : 0.5;
    // 确保只对水系线要素进行检查
    if (!isRiverFile(sourceFile)) {
        return results;
    }

    logTextEdit->append(QString("开始河流拓扑检查: %1").arg(QFileInfo(sourceFile).baseName()));

    //检查水系_LineString.shp的悬挂和独立问题
    std::vector<QualityCheckResult> danglingResults = checkRiverDangling(parsedLayer, sourceFile);
    std::vector<QualityCheckResult> isolationResults = checkRiverIsolation(parsedLayer, sourceFile);

    // 合并初步检查结果
    std::vector<QualityCheckResult> preliminaryResults;
    preliminaryResults.insert(preliminaryResults.end(), danglingResults.begin(), danglingResults.end());
    preliminaryResults.insert(preliminaryResults.end(), isolationResults.begin(), isolationResults.end());

    logTextEdit->append(QString("初步检查发现 %1 个悬挂问题").arg(danglingResults.size()));
    logTextEdit->append(QString("初步检查发现 %1 个独立问题").arg(isolationResults.size()));

    if (preliminaryResults.empty()) {
        logTextEdit->append("未发现悬挂或独立问题");
        return results;
    }

    //收集所有水系要素进行连接性检查
    std::vector<QgsFeature> waterSystemFeatures = collectWaterSystemFeatures();

    if (waterSystemFeatures.empty()) {
        logTextEdit->append("警告：未找到其他水系要素，无法进行连接性过滤");
        return preliminaryResults;
    }

    // 过滤掉与其他水系要素有连接的问题
    std::vector<QualityCheckResult> finalResults = filterConnectedToWaterSystemStrict(preliminaryResults, waterSystemFeatures);

    logTextEdit->append(QString("连接性过滤后剩余 %1 个真正的错误").arg(finalResults.size()));

    return finalResults;
}
//检查河流悬挂（复用道路算法）
std::vector<QualityCheckResult> ReadShpDialog::checkRiverDangling(QgsVectorLayer* parsedLayer, const QString& sourceFile)
{
    std::vector<QualityCheckResult> results;

    if (!parsedLayer || !parsedLayer->isValid()) {
        logTextEdit->append("图层无效，无法进行河流悬挂检查");
        return results;
    }

    // 从XML配置获取悬挂容差
    double danglingTolerance = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("dangling_tolerance"));
    logTextEdit->append(QString("悬挂检查容差(转换后): %1 米").arg(danglingTolerance));

    QgsFeatureIterator iter = parsedLayer->getFeatures();
    QgsFeature feature;
    int validFeatureCount = 0;
    int danglingCount = 0;

    // 收集所有有效要素用于连接性检查
    std::vector<QgsFeature> allFeatures;
    QgsFeatureIterator collectIter = parsedLayer->getFeatures();
    QgsFeature collectFeature;
    while (collectIter.nextFeature(collectFeature)) {
        if (collectFeature.hasGeometry() &&
            collectFeature.geometry().type() == QgsWkbTypes::LineGeometry) {
            // 验证几何体有效性
            QgsGeometry geom = collectFeature.geometry();
            if (!geom.isGeosValid()) {
                geom = geom.makeValid();
                if (geom.isGeosValid()) {
                    collectFeature.setGeometry(geom);
                    allFeatures.push_back(collectFeature);
                }
            }
            else {
                allFeatures.push_back(collectFeature);
            }
        }
    }

    while (iter.nextFeature(feature)) {
        if (!feature.hasGeometry()) continue;

        QgsGeometry geom = feature.geometry();
        // 只检查线要素
        if (geom.type() != QgsWkbTypes::LineGeometry) continue;

        // 验证几何体有效性
        if (!geom.isGeosValid()) {
            logTextEdit->append(QString("警告: FID %1 的几何体无效，尝试修复").arg(feature.id()));
            geom = geom.makeValid();
            if (!geom.isGeosValid()) {
                logTextEdit->append(QString("错误: FID %1 的几何体无法修复，跳过").arg(feature.id()));
                continue;
            }
        }

        validFeatureCount++;

        // 检查起点/终点连接性，并记录端点坐标
        bool startConnected = false;
        bool endConnected = false;
        double minStartDistance = std::numeric_limits<double>::max();
        double minEndDistance = std::numeric_limits<double>::max();
        QgsPointXY startPoint;
        QgsPointXY endPoint;

        if (geom.isMultipart()) {
            QgsMultiLineString* multiLine = qgsgeometry_cast<QgsMultiLineString*>(geom.get());
            if (multiLine && multiLine->numGeometries() > 0) {
                const QgsLineString* firstLine = qgsgeometry_cast<const QgsLineString*>(multiLine->geometryN(0));
                const QgsLineString* lastLine = qgsgeometry_cast<const QgsLineString*>(multiLine->geometryN(multiLine->numGeometries() - 1));

                if (firstLine && firstLine->numPoints() > 0) {
                    startPoint = QgsPointXY(firstLine->pointN(0));
                    for (const auto& otherFeature : allFeatures) {
                        if (otherFeature.id() == feature.id()) continue;
                        QgsGeometry otherGeom = otherFeature.geometry();
                        if (otherGeom.type() == QgsWkbTypes::LineGeometry) {
                            double distance = otherGeom.distance(QgsGeometry::fromPointXY(startPoint));
                            minStartDistance = std::min(minStartDistance, distance);
                            if (distance <= danglingTolerance) {
                                startConnected = true;
                                break;
                            }
                        }
                    }
                }

                if (lastLine && lastLine->numPoints() > 0) {
                    endPoint = QgsPointXY(lastLine->pointN(lastLine->numPoints() - 1));
                    for (const auto& otherFeature : allFeatures) {
                        if (otherFeature.id() == feature.id()) continue;
                        QgsGeometry otherGeom = otherFeature.geometry();
                        if (otherGeom.type() == QgsWkbTypes::LineGeometry) {
                            double distance = otherGeom.distance(QgsGeometry::fromPointXY(endPoint));
                            minEndDistance = std::min(minEndDistance, distance);
                            if (distance <= danglingTolerance) {
                                endConnected = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
        else {
            const QgsLineString* line = qgsgeometry_cast<const QgsLineString*>(geom.get());
            if (line && line->numPoints() > 1) {
                startPoint = QgsPointXY(line->pointN(0));
                endPoint = QgsPointXY(line->pointN(line->numPoints() - 1));

                for (const auto& otherFeature : allFeatures) {
                    if (otherFeature.id() == feature.id()) continue;

                    QgsGeometry otherGeom = otherFeature.geometry();
                    if (otherGeom.type() == QgsWkbTypes::LineGeometry) {
                        double startDistance = otherGeom.distance(QgsGeometry::fromPointXY(startPoint));
                        double endDistance = otherGeom.distance(QgsGeometry::fromPointXY(endPoint));

                        minStartDistance = std::min(minStartDistance, startDistance);
                        minEndDistance = std::min(minEndDistance, endDistance);

                        if (startDistance <= danglingTolerance) {
                            startConnected = true;
                        }
                        if (endDistance <= danglingTolerance) {
                            endConnected = true;
                        }

                        if (startConnected && endConnected) break;
                    }
                }
            }
        }

        // 记录悬挂点
        if (!startConnected || !endConnected) {
            danglingCount++;

            QString errorDescription = QString("河流悬挂点: FID %1")
                .arg(feature.id());

            if (!startConnected && !endConnected) {
                errorDescription += QString(", 起点和终点都悬挂，最小距离: 起点 %1 米, 终点 %2 米")
                    .arg(minStartDistance, 0, 'f', 2)
                    .arg(minEndDistance, 0, 'f', 2);
            }
            else if (!startConnected) {
                errorDescription += QString(", 起点悬挂，最小距离: %1 米")
                    .arg(minStartDistance, 0, 'f', 2);
            }
            else {
                errorDescription += QString(", 终点悬挂，最小距离: %1 米")
                    .arg(minEndDistance, 0, 'f', 2);
            }

            QualityCheckResult result;
            result.featureId = feature.id();
            result.checkType = CheckType::LINE_TOPOLOGY_ERROR;  // 设置正确的错误类型
            result.errorType = "河流悬挂";
            result.description = errorDescription;
            result.feature = feature;
            result.geometry = geom;
            result.errorGeometry = geom;  // 关键：错误几何设置为线几何，确保按线导出
            result.sourceFile = sourceFile;
            result.passed = false;  // 明确设置为错误
            result.calculatedValue = !startConnected ? minStartDistance : minEndDistance;
            result.standardValue = danglingTolerance;
            results.push_back(result);
        }
    }
    logTextEdit->append(QString("总共 %1 个有效河流要素").arg(validFeatureCount));
    logTextEdit->append(QString("河流悬挂检查完成，发现 %1 个悬挂点").arg(danglingCount));
    logTextEdit->append(QString("初步检查发现 %1 个悬挂问题").arg(results.size()));

    // 收集水系要素进行过滤
    std::vector<QgsFeature> danglingWaterSystemFeatures = collectWaterSystemFeatures();

    if (!danglingWaterSystemFeatures.empty()) {
        logTextEdit->append(QString("开始河流独立检查，最小长度: 5 米, 独立距离: 10 米"));

        // 使用严格的水系连接过滤
        std::vector<QualityCheckResult> filteredResults = filterConnectedToWaterSystemStrict(results, danglingWaterSystemFeatures);;

        logTextEdit->append(QString("初步检查完成: 共检查 %1 个河流要素，发现 %2 个悬挂点")
            .arg(validFeatureCount).arg(filteredResults.size()));

        return filteredResults;
    }
    else {
        logTextEdit->append("警告: 未找到水系要素，无法进行连接性过滤");
        return results;
    }
}
//检查河流不合理独立
std::vector<QualityCheckResult> ReadShpDialog::checkRiverIsolation(QgsVectorLayer* parsedLayer, const QString& sourceFile)
{
    std::vector<QualityCheckResult> results;

    if (!parsedLayer || !isRiverFile(sourceFile)) {
        return results;
    }

    // 使用经过比例尺转换的参数
    double isolationDistance = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("isolation_distance"));

    logTextEdit->append(QString("开始河流独立检查，检查所有有效河流线，独立距离: %1 米")
        .arg(isolationDistance));

    // 参数验证
    if (!std::isfinite(isolationDistance) || isolationDistance < 0) {
        logTextEdit->append("错误：参数配置无效");
        return results;
    }

    QgsFeatureIterator iter = parsedLayer->getFeatures();
    QgsFeature feature;
    std::vector<QgsFeature> allFeatures;
    QgsSpatialIndex index(QgsSpatialIndex::FlagStoreFeatureGeometries);

    // 收集有效河流并建立空间索引
    while (iter.nextFeature(feature)) {
        if (!feature.hasGeometry()) continue;

        QgsGeometry geom = feature.geometry();
        if (geom.isNull() || geom.isEmpty() || geom.type() != QgsWkbTypes::LineGeometry ||
            !geom.isGeosValid()) continue;

        allFeatures.push_back(feature);
        index.addFeature(feature);
    }

    logTextEdit->append(QString("有效河流线要素: %1 个").arg(allFeatures.size()));

    // 检查整条河流与最近河流之间的距离
    std::vector<QualityCheckResult> potentialProblems;

    for (const QgsFeature& riverFeature : allFeatures) {
        QgsGeometry riverGeom = riverFeature.geometry();
        bool isIsolated = true;
        double minDistanceToOther = std::numeric_limits<double>::max();
        int nearbyCount = 0;

        for (const QgsFeatureId otherId : index.nearestNeighbor(riverGeom, 2)) {
            if (otherId == riverFeature.id()) continue;
            const QgsGeometry otherGeom = index.geometry(otherId);
            double distance = riverGeom.distance(otherGeom);
            if (distance < 0) continue;
            minDistanceToOther = std::min(minDistanceToOther, distance);

            logTextEdit->append(QString("距离检查: 河流 FID %1 到 FID %2 距离: %3 米")
                .arg(riverFeature.id()).arg(otherId).arg(distance, 0, 'f', 3));

            if (distance <= isolationDistance) {
                isIsolated = false;
                nearbyCount++;
                logTextEdit->append(QString("发现连接: 距离 %1 米 <= 阈值 %2 米")
                    .arg(distance, 0, 'f', 3).arg(isolationDistance));
            }
        }

        if (isIsolated) {
            logTextEdit->append(QString("发现独立线段: FID %1, 最小距离 %2 米 > 阈值 %3 米")
                .arg(riverFeature.id()).arg(minDistanceToOther, 0, 'f', 3).arg(isolationDistance));

            QualityCheckResult result;
            result.passed = false;
            result.checkType = CheckType::LINE_TOPOLOGY_ERROR;
            result.errorType = "河流独立";
            result.sourceFile = sourceFile;
            result.featureId = riverFeature.id();
            result.geometry = riverGeom;
            result.errorGeometry = riverGeom;
            result.feature = riverFeature;
            result.pointIndex = -1;
            result.calculatedValue = minDistanceToOther == std::numeric_limits<double>::max() ? -1.0 : minDistanceToOther;
            result.standardValue = isolationDistance;
            result.description = minDistanceToOther == std::numeric_limits<double>::max()
                ? QString("河流线段独立，长度%1米，未找到其他有效河流线").arg(riverGeom.length(), 0, 'f', 2)
                : QString("河流线段独立，长度%1米，与最近线段距离%2米 > 阈值%3米")
                    .arg(riverGeom.length(), 0, 'f', 2)
                    .arg(minDistanceToOther, 0, 'f', 2).arg(isolationDistance);

            potentialProblems.push_back(result);
        }
        else {
            logTextEdit->append(QString("线段连接正常: FID %1, 有 %2 个邻近线段")
                .arg(riverFeature.id()).arg(nearbyCount));
        }
    }

    logTextEdit->append(QString("初步检查完成：共检查 %1 个河流要素，发现 %2 个潜在独立问题")
        .arg(allFeatures.size()).arg(potentialProblems.size()));

    // 收集水系要素进行过滤
    std::vector<QgsFeature> isolationWaterSystemFeatures = collectWaterSystemFeatures();
    logTextEdit->append(QString("收集水系要素：共 %1 个水系要素用于连接性检查")
        .arg(isolationWaterSystemFeatures.size()));

    // 使用更严格的过滤逻辑
    results = filterConnectedToWaterSystemStrict(potentialProblems, isolationWaterSystemFeatures);

    logTextEdit->append(QString("过滤完成：原始问题 %1 个，过滤后剩余 %2 个")
        .arg(potentialProblems.size()).arg(results.size()));

    return results;
}
//过滤掉与水系要素连接的问题
std::vector<QualityCheckResult> ReadShpDialog::filterConnectedToWaterSystemStrict(const std::vector<QualityCheckResult>& problemResults, const std::vector<QgsFeature>& waterSystemFeatures)
{
    std::vector<QualityCheckResult> filteredResults;
    // 使用转换后的水系连接容差替代硬编码
    double tolerance = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("water_system_connection_tolerance"));

    logTextEdit->append(QString("开始严格过滤水系连接，容差值：%1米（原容差0.5米），水系要素数量：%2")
        .arg(tolerance).arg(waterSystemFeatures.size()));

    int originalCount = problemResults.size();
    int filteredCount = 0;
    int validWaterSystemCount = 0;
    int invalidGeometryCount = 0;

    // 预处理水系要素，验证几何体有效性
    std::vector<QgsFeature> validWaterSystemFeatures;
    for (const auto& waterFeature : waterSystemFeatures) {
        if (!waterFeature.hasGeometry()) {
            invalidGeometryCount++;
            continue;
        }

        QgsGeometry waterGeom = waterFeature.geometry();
        if (!waterGeom.isGeosValid()) {
            logTextEdit->append(QString("警告: 水系要素 FID %1 几何体无效，尝试修复").arg(waterFeature.id()));
            waterGeom = waterGeom.makeValid();
            if (!waterGeom.isGeosValid()) {
                logTextEdit->append(QString("错误: 水系要素 FID %1 几何体无法修复，跳过").arg(waterFeature.id()));
                invalidGeometryCount++;
                continue;
            }
        }

        // 创建修复后的要素
        QgsFeature validFeature = waterFeature;
        validFeature.setGeometry(waterGeom);
        validWaterSystemFeatures.push_back(validFeature);
        validWaterSystemCount++;
    }

    logTextEdit->append(QString("水系要素验证完成：总数 %1，有效 %2，无效 %3")
        .arg(waterSystemFeatures.size()).arg(validWaterSystemCount).arg(invalidGeometryCount));

    if (validWaterSystemFeatures.empty()) {
        logTextEdit->append("警告：没有有效的水系要素用于距离计算");
        return problemResults; // 返回原始结果
    }

    for (const auto& problem : problemResults) {
        bool isConnectedToWaterSystem = false;
        double minDistanceToWater = std::numeric_limits<double>::max();

        QgsGeometry problemGeom = problem.geometry;

        // 验证问题几何体
        if (!problemGeom.isGeosValid()) {
            logTextEdit->append(QString("警告: 问题要素 FID %1 几何体无效，尝试修复").arg(problem.featureId));
            problemGeom = problemGeom.makeValid();
            if (!problemGeom.isGeosValid()) {
                logTextEdit->append(QString("错误: 问题要素 FID %1 几何体无法修复，保留原问题").arg(problem.featureId));
                filteredResults.push_back(problem);
                continue;
            }
        }

        // 检查与每个有效水系要素的距离
        for (const auto& waterFeature : validWaterSystemFeatures) {
            try {
                QgsGeometry waterGeom = waterFeature.geometry();
                double distance = problemGeom.distance(waterGeom);

                minDistanceToWater = std::min(minDistanceToWater, distance);

                if (distance <= tolerance) {
                    isConnectedToWaterSystem = true;
                    logTextEdit->append(QString("发现连接: 问题 FID %1 与水系要素 FID %2 距离 %3 米 <= 容差 %4 米")
                        .arg(problem.featureId).arg(waterFeature.id()).arg(distance, 0, 'f', 3).arg(tolerance));
                    break;
                }
            }
            catch (const std::exception& e) {
                logTextEdit->append(QString("距离计算异常: 问题 FID %1 与水系要素 FID %2 - %3")
                    .arg(problem.featureId).arg(waterFeature.id()).arg(e.what()));
                continue;
            }
        }

        if (!isConnectedToWaterSystem) {
            // 创建更新的问题结果，包含距离信息
            QualityCheckResult updatedProblem = problem;
            updatedProblem.calculatedValue = minDistanceToWater;
            updatedProblem.description += QString("，与最近水系距离: %1 米").arg(minDistanceToWater, 0, 'f', 2);

            filteredResults.push_back(updatedProblem);
        }
        else {
            filteredCount++;
        }
    }

    logTextEdit->append(QString("过滤完成：原始问题 %1 个，过滤掉连接的 %2 个，剩余 %3 个")
        .arg(originalCount).arg(filteredCount).arg(filteredResults.size()));

    if (filteredResults.size() > 0) {
        logTextEdit->append(QString("警告：%1 个问题未找到有效水系要素进行距离计算").arg(filteredResults.size()));
    }

    return filteredResults;
}
//收集所有解析文件中的水系要素
std::vector<QgsFeature> ReadShpDialog::collectWaterSystemFeatures()
{
    std::vector<QgsFeature> waterSystemFeatures;
    int totalFeatures = 0;

    // 遍历所有解析文件，收集所有水系要素（面、点状水库、拦水坝、井、泉等）
    for (int i = 0; i < parseFilePaths.size(); ++i) {
        QString fileName = parseFilePaths[i];
        QString baseName = QFileInfo(fileName).baseName();

        // 收集所有水系相关要素：水系面、点状水库、拦水坝、井、泉等
        if (isWaterAreaFile(fileName) || isWaterLineFile(fileName)) {
            QgsVectorLayer* layer = new QgsVectorLayer(fileName, baseName, "ogr");
            if (layer && layer->isValid()) {
                QgsFeatureIterator iter = layer->getFeatures();
                QgsFeature feature;
                int fileFeatureCount = 0;

                while (iter.nextFeature(feature)) {
                    if (feature.hasGeometry()) {
                        waterSystemFeatures.push_back(feature);
                        fileFeatureCount++;
                    }
                }

                totalFeatures += fileFeatureCount;
                logTextEdit->append(QString("从文件 %1 收集到 %2 个水系要素用于连接性检查")
                    .arg(baseName).arg(fileFeatureCount));
            }
            else {
                logTextEdit->append(QString("无法加载水系文件: %1").arg(baseName));
            }
            delete layer;
        }
    }

    logTextEdit->append(QString("总共收集到 %1 个水系要素用于连接性检查").arg(totalFeatures));
    return waterSystemFeatures;
}
//判断是否为水系面要素文件
bool ReadShpDialog::isWaterAreaFile(const QString& fileName)
{
    QString baseName = QFileInfo(fileName).baseName().toLower();
    // 排除含有"_标注"的文件名
    if (baseName.contains("_标注")) {
        return false;
    }
    // 排除"水系面_linestring"类型的文件
    if (baseName.contains("水系面_linestring")) {
        return false;
    }
    QStringList waterAreaKeywords = {
        "水系面", "水面", "湖泊", "pond", "lake",
        "sxm", "sm", "水体面", "水域面"
    };

    for (const QString& keyword : waterAreaKeywords) {
        if (baseName.contains(keyword)) {
            return true;
        }
    }

    return false;
}
//判断是否为水系线要素文件（水库、拦水坝、井、泉等）
bool ReadShpDialog::isWaterLineFile(const QString& fileName)
{
    QString baseName = QFileInfo(fileName).baseName().toLower();
    // 排除含有"_标注"的文件名
    if (baseName.contains("_标注")) {
        return false;
    }

    QStringList waterLineKeywords = {
        "水库", "拦水坝", "井", "泉", "reservoir", "dam", "well", "spring",
        "sk", "lsb", "j", "q", "水闸", "gate", "sz"
    };

    for (const QString& keyword : waterLineKeywords) {
        if (baseName.contains(keyword)) {
            return true;
        }
    }

    return false;
}
//河网通达性检查主函数
std::vector<QualityCheckResult> ReadShpDialog::checkRiverNetworkConnectivity(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile) 
{
    std::vector<QualityCheckResult> results;
    std::set<QgsFeatureId> problemFeatureIds;  // 记录有基础问题的要素ID
    std::set<QgsFeatureId> connectedToWaterSystemIds;  // 记录与水系面连接的要素ID

    executedLineTopology = (parsedLayer != nullptr);

    if (!parsedLayer) {
        return results;
    }

    logTextEdit->append("=== 开始河网通达性检查 ===");
    logTextEdit->append(QString("检查文件: %1").arg(QFileInfo(sourceFile).baseName()));

    // 检查水系线悬挂和独立问题
    logTextEdit->append("--- 第一步：检查水系线悬挂和独立问题 ---");
    std::vector<QualityCheckResult> danglingResults = checkRiverDangling(parsedLayer, sourceFile);
    std::vector<QualityCheckResult> isolationResults = checkRiverIsolation(parsedLayer, sourceFile);

    // 记录有基础问题的要素ID
    for (const auto& result : danglingResults) {
        problemFeatureIds.insert(result.featureId);
    }
    for (const auto& result : isolationResults) {
        problemFeatureIds.insert(result.featureId);
    }

    logTextEdit->append(QString("悬挂问题统计: %1 个").arg(danglingResults.size()));
    logTextEdit->append(QString("独立问题统计: %1 个").arg(isolationResults.size()));
    logTextEdit->append(QString("存在基础问题的要素数量: %1 个").arg(problemFeatureIds.size()));

    // 对有悬挂和独立问题的水系线检查与水系面的连接性
    logTextEdit->append("--- 第二步：检查有问题的水系线与水系面的连接性 ---");

    if (!problemFeatureIds.empty()) {
        std::vector<QgsFeature> waterSystemFeatures = collectWaterSystemFeatures();

        if (!waterSystemFeatures.empty()) {
            // 过滤出水系面要素
            std::vector<QgsFeature> waterAreas;
            for (const auto& feature : waterSystemFeatures) {
                QString featureType = getWaterFeatureType(feature, sourceFile);
                if (featureType == "水系面") {
                    waterAreas.push_back(feature);
                }
            }

            logTextEdit->append(QString("收集到 %1 个水系面要素用于连接性检查").arg(waterAreas.size()));

            // 检查有问题的要素是否与水系面连接
            QgsFeatureIterator riverIterator = parsedLayer->getFeatures();
            QgsFeature riverFeature;
            int unconnectedCount = 0;  // 添加未连接计数器
            while (riverIterator.nextFeature(riverFeature)) {
                // 只检查有基础问题的要素
                if (problemFeatureIds.find(riverFeature.id()) == problemFeatureIds.end()) {
                    continue;
                }

                QgsGeometry riverGeom = riverFeature.geometry();
                if (riverGeom.isNull()) continue;

                bool connectedToWaterArea = false;
                // 使用转换后的容差
                double waterSystemConnectionTolerance = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("water_system_connection_tolerance"));
                // 检查是否与任何水系面连接
                for (const auto& waterArea : waterAreas) {
                    if (waterArea.hasGeometry()) {
                        QgsGeometry waterGeom = waterArea.geometry();

                        // 检查河流端点到水系面的距离
                        const QgsLineString* lineString = qgsgeometry_cast<const QgsLineString*>(riverGeom.constGet());
                        if (lineString && lineString->numPoints() >= 2) {
                            QgsPointXY startPoint = lineString->startPoint();
                            QgsPointXY endPoint = lineString->endPoint();

                            double startDistance = waterGeom.distance(QgsGeometry::fromPointXY(startPoint));
                            double endDistance = waterGeom.distance(QgsGeometry::fromPointXY(endPoint));

                            if (startDistance <= waterSystemConnectionTolerance || endDistance <= waterSystemConnectionTolerance) {
                                connectedToWaterArea = true;
                                connectedToWaterSystemIds.insert(riverFeature.id());
                                QString connMsg = QString("线要素%1与水系面%2连接，距离：起点%3米，终点%4米（容差%5米）")
                                    .arg(riverFeature.id())
                                    .arg(waterArea.id())
                                    .arg(startDistance, 0, 'f', 2)
                                    .arg(endDistance, 0, 'f', 2)
                                    .arg(waterSystemConnectionTolerance);
                                logTextEdit->append(connMsg);
                                break;
                            }
                        }
                    }
                }

                if (!connectedToWaterArea) {
                    QString errorMsg = QString("确认错误：线要素%1未连接到任何水系面")
                        .arg(riverFeature.id());
                    logTextEdit->append(errorMsg);
                    unconnectedCount++;  // 增加未连接计数
                }
            }

            logTextEdit->append(QString("第二步检查结果统计:"));
            logTextEdit->append(QString("  - 与水系面连接的问题要素数量: %1 个").arg(connectedToWaterSystemIds.size()));
            logTextEdit->append(QString("  - 未连接到水系面的问题要素数量: %1 个").arg(unconnectedCount));
            logTextEdit->append(QString("  - 总问题要素数量: %1 个").arg(problemFeatureIds.size()));

            if (connectedToWaterSystemIds.size() > 0) {
                logTextEdit->append(QString("注意：%1 个问题要素因连接到水系面而被排除，不作为最终错误").arg(connectedToWaterSystemIds.size()));
            }
        }
        else {
            logTextEdit->append("警告：未找到水系面要素，无法进行连接性检查");
            logTextEdit->append(QString("所有 %1 个问题要素都将被标记为错误").arg(problemFeatureIds.size()));
        }
    }
    else {
        logTextEdit->append("第一步未发现悬挂和独立问题，跳过第二步检查");
    }

    // 将确认的悬挂和独立问题添加到结果中（排除与水系面连接的）
    for (const auto& result : danglingResults) {
        if (connectedToWaterSystemIds.find(result.featureId) == connectedToWaterSystemIds.end()) {
            QualityCheckResult finalResult = result;
            finalResult.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
            finalResult.errorType = "河网通达性-悬挂";
            finalResult.description = "河网通达性检查：" + result.description;
            results.push_back(finalResult);
        }
    }

    for (const auto& result : isolationResults) {
        if (connectedToWaterSystemIds.find(result.featureId) == connectedToWaterSystemIds.end()) {
            QualityCheckResult finalResult = result;
            finalResult.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
            finalResult.errorType = "河网通达性-独立";
            finalResult.description = "河网通达性检查：" + result.description;
            results.push_back(finalResult);
        }
    }

    // 检查无基础问题水系线的空间关系
    logTextEdit->append("--- 第三步：检查无基础问题水系线的空间关系 ---");
    std::vector<QgsFeature> spatialWaterSystemFeatures = collectWaterSystemFeatures();

    if (!spatialWaterSystemFeatures.empty()) {
        logTextEdit->append(QString("收集到 %1 个水系要素用于空间关系检查").arg(spatialWaterSystemFeatures.size()));

        std::vector<QualityCheckResult> spatialResults;
        QgsFeatureIterator riverIterator2 = parsedLayer->getFeatures();
        QgsFeature riverFeature2;

        while (riverIterator2.nextFeature(riverFeature2)) {
            // 只检查无基础问题的要素（第一步无问题的 + 第二步与水系面连接的）
            bool hasBasicProblem = (problemFeatureIds.find(riverFeature2.id()) != problemFeatureIds.end()) &&
                (connectedToWaterSystemIds.find(riverFeature2.id()) == connectedToWaterSystemIds.end());

            if (hasBasicProblem) {
                continue;  // 跳过有基础问题且未连接水系面的要素
            }

            QgsGeometry riverGeom = riverFeature2.geometry();
            if (riverGeom.isNull()) continue;

            // 检查空间关系
            std::vector<QualityCheckResult> featureSpatialResults = checkSingleRiverSpatialRelations(riverFeature2, spatialWaterSystemFeatures, sourceFile);
            spatialResults.insert(spatialResults.end(), featureSpatialResults.begin(), featureSpatialResults.end());
        }

        if (!spatialResults.empty()) {
            results.insert(results.end(), spatialResults.begin(), spatialResults.end());
            logTextEdit->append(QString("发现空间关系问题: %1 个").arg(spatialResults.size()));
        }
        else {
            logTextEdit->append("空间关系检查通过，未发现问题");
        }
    }
    else {
        logTextEdit->append("警告：未找到其他水系要素，跳过空间关系检查");
    }

    // 与库体数据对比（如果有库体数据）
    if (referenceLayer) {
        logTextEdit->append("--- 第四步：与库体数据对比检查 ---");
        std::vector<QualityCheckResult> databaseResults;
        QHash<QgsFeatureId, WaterMatchCacheEntry> referenceCache;
        QgsFeatureIterator riverIterator3 = parsedLayer->getFeatures();
        QgsFeature riverFeature3;

        while (riverIterator3.nextFeature(riverFeature3)) {
            bool hasBasicProblem = (problemFeatureIds.find(riverFeature3.id()) != problemFeatureIds.end()) &&
                (connectedToWaterSystemIds.find(riverFeature3.id()) == connectedToWaterSystemIds.end());
            if (hasBasicProblem) {
                continue;
            }
            QgsGeometry riverGeom = riverFeature3.geometry();
            if (riverGeom.isNull()) continue;

            // 传入 parsedLayer，避免 currentLayer 依赖
            std::vector<QualityCheckResult> featureDatabaseResults =
                compareSingleFeatureWithDatabase(riverFeature3, parsedLayer, referenceLayer, sourceFile, &referenceCache);
            databaseResults.insert(databaseResults.end(), featureDatabaseResults.begin(), featureDatabaseResults.end());
        }
        if (!databaseResults.empty()) {
            results.insert(results.end(), databaseResults.begin(), databaseResults.end());
            logTextEdit->append(QString("发现与库体数据不一致的空间关系: %1 个").arg(databaseResults.size()));
        }
        else {
            logTextEdit->append("与库体数据对比检查通过，所有空间关系一致");
        }
    }
    else {
        logTextEdit->append("警告：未提供有效的库体数据，跳过数据库对比检查");
    }

    // 总结报告
    logTextEdit->append("=== 河网通达性检查总结 ===");
    logTextEdit->append(QString("总计发现问题: %1 个").arg(results.size()));

    // 按问题类型统计
    int danglingCount = 0, isolationCount = 0, spatialCount = 0, databaseCount = 0;
    for (const auto& result : results) {
        if (result.errorType.contains("悬挂")) danglingCount++;
        else if (result.errorType.contains("独立")) isolationCount++;
        else if (result.errorType.contains("超出") || result.errorType.contains("关系异常")) spatialCount++;
        else if (result.errorType.contains("空间关系不一致")) databaseCount++;
    }

    logTextEdit->append(QString("  - 悬挂问题: %1 个").arg(danglingCount));
    logTextEdit->append(QString("  - 独立问题: %1 个").arg(isolationCount));
    logTextEdit->append(QString("  - 空间关系问题: %1 个").arg(spatialCount));
    logTextEdit->append(QString("  - 数据库对比问题: %1 个").arg(databaseCount));

    if (results.empty()) {
        logTextEdit->append("河网通达性检查全部通过！");
    }
    else {
        logTextEdit->append("发现河网通达性问题，需要修复");
    }

    logTextEdit->append("=== 河网通达性检查完成 ===\n");

    return results;
}
//检查单个河流要素的空间关系
std::vector<QualityCheckResult> ReadShpDialog::checkSingleRiverSpatialRelations(const QgsFeature& riverFeature, const std::vector<QgsFeature>& waterSystemFeatures, const QString& sourceFile) {
    std::vector<QualityCheckResult> results;

    QgsGeometry riverGeom = riverFeature.geometry();
    if (riverGeom.isNull()) return results;

    // 使用经过比例尺转换的容差参数
    double exceedsTolerance = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("river_exceeds_tolerance"));
    double intersectionTolerance = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("river_intersection_tolerance"));
    double overlapTolerance = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("river_overlap_tolerance"));

    // 按要素类型分组水系要素
    std::vector<QgsFeature> waterAreas;      // 水系面（面要素）
    std::vector<QgsFeature> dams;            // 拦水坝（线要素）
    std::vector<QgsFeature> wells;           // 井（线要素）
    std::vector<QgsFeature> springs;         // 泉（线要素）
    std::vector<QgsFeature> reservoirs;      // 点状水库（线要素）

    for (const auto& feature : waterSystemFeatures) {
        QString featureType = getWaterFeatureType(feature, sourceFile);

        if (featureType == "水系面") {
            waterAreas.push_back(feature);
        }
        else if (featureType == "拦水坝") {
            dams.push_back(feature);
        }
        else if (featureType == "井") {
            wells.push_back(feature);
        }
        else if (featureType == "泉") {
            springs.push_back(feature);
        }
        else if (featureType == "点状水库") {
            reservoirs.push_back(feature);
        }
    }

    // 检查河流是否超出水系面边界
    for (const auto& waterArea : waterAreas) {
        if (checkRiverExceedsWaterArea(riverGeom, waterArea.geometry(), exceedsTolerance)) {
            QualityCheckResult result;
            result.passed = false;
            result.errorType = "河流超出水系面边界";
            result.description = QString("河流要素超出了水系面的边界范围，容差: %1米").arg(exceedsTolerance);
            result.feature = riverFeature;
            result.errorGeometry = riverGeom;
            result.sourceFile = sourceFile;
            result.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
            result.featureId = riverFeature.id();
            result.geometry = riverGeom;
            result.standardValue = exceedsTolerance;
            results.push_back(result);
        }
    }

    // 检查河流与线状水系要素的关系
    // 检查与泉的关系
    for (const auto& spring : springs) {
        if (checkRiverExceedsLineFeature(riverGeom, spring.geometry(), exceedsTolerance)) {
            QualityCheckResult result;
            result.passed = false;
            result.errorType = "河流超出泉范围";
            result.description = QString("河流要素超出了泉的范围，容差: %1米").arg(exceedsTolerance);
            result.feature = riverFeature;
            result.errorGeometry = riverGeom;
            result.sourceFile = sourceFile;
            result.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
            result.featureId = riverFeature.id();
            result.geometry = riverGeom;
            result.standardValue = exceedsTolerance;
            results.push_back(result);
        }
    }

    // 检查与井的关系
    for (const auto& well : wells) {
        if (checkRiverExceedsLineFeature(riverGeom, well.geometry(), exceedsTolerance)) {
            QualityCheckResult result;
            result.passed = false;
            result.errorType = "河流超出井范围";
            result.description = QString("河流要素超出了井的范围，容差: %1米").arg(exceedsTolerance);
            result.feature = riverFeature;
            result.errorGeometry = riverGeom;
            result.sourceFile = sourceFile;
            result.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
            result.featureId = riverFeature.id();
            result.geometry = riverGeom;
            result.standardValue = exceedsTolerance;
            results.push_back(result);
        }
    }

    // 检查与点状水库的关系
    for (const auto& reservoir : reservoirs) {
        if (checkRiverExceedsLineFeature(riverGeom, reservoir.geometry(), exceedsTolerance)) {
            QualityCheckResult result;
            result.passed = false;
            result.errorType = "河流超出点状水库范围";
            result.description = QString("河流要素超出了点状水库的范围，容差: %1米").arg(exceedsTolerance);
            result.feature = riverFeature;
            result.errorGeometry = riverGeom;
            result.sourceFile = sourceFile;
            result.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
            result.featureId = riverFeature.id();
            result.geometry = riverGeom;
            result.standardValue = exceedsTolerance;
            results.push_back(result);
        }
    }

    // 检查与拦水坝的关系
    for (const auto& dam : dams) {
        if (checkRiverDamRelation(riverGeom, dam.geometry(), intersectionTolerance)) {
            QualityCheckResult result;
            result.passed = false;
            result.errorType = "河流与拦水坝关系异常";
            result.description = QString("河流要素与拦水坝的空间关系异常，容差: %1米").arg(intersectionTolerance);
            result.feature = riverFeature;
            result.errorGeometry = riverGeom;
            result.sourceFile = sourceFile;
            result.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
            result.featureId = riverFeature.id();
            result.geometry = riverGeom;
            result.standardValue = intersectionTolerance;
            results.push_back(result);
        }
    }

    return results;
}
//解析数据中要素与库体数据对比（解析数据中要素的连接关系与库体数据对应要素的连接关系是否一致）
std::vector<QualityCheckResult> ReadShpDialog::compareSingleFeatureWithDatabase(
    const QgsFeature& parsedFeature,
    QgsVectorLayer* parsedLayer,
    QgsVectorLayer* referenceLayer,
    const QString& sourceFile,
    QHash<QgsFeatureId, WaterMatchCacheEntry>* cache
) {
    std::vector<QualityCheckResult> results;

    if (!referenceLayer || !referenceLayer->isValid()) {
        return results;
    }

    // 原来使用 currentLayer 的空指针检查，改为使用传入的 parsedLayer
    if (!parsedLayer || !parsedLayer->isValid()) {
        return results;
    }


    QgsGeometry parsedGeom = parsedFeature.geometry();
    if (parsedGeom.isNull() || !parsedGeom.isGeosValid()) {
        return results;
    }

    // 空间容差设置（改为使用比例尺转换后的值）
    double spatialTolerance = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("connectivity_tolerance"));

    // Match the source line using the same classification rules as result export.
    const QString matchParameter = QStringLiteral("database_match_tolerance");
    const bool hasMatchTolerance = m_config && m_config->getCheckItemConfig(
        QStringLiteral("线要素拓扑检查")).parameters.contains(matchParameter);
    const double matchTolerance = hasMatchTolerance
        ? getConvertedParameter(QStringLiteral("线要素拓扑检查"), matchParameter)
        : spatialTolerance;
    const QString fallbackParameter = QStringLiteral("topology_classification_fallback_tolerance");
    const bool hasFallbackTolerance = m_config && m_config->getCheckItemConfig(
        QStringLiteral("线要素拓扑检查")).parameters.contains(fallbackParameter);
    const double fallbackTolerance = hasFallbackTolerance
        ? getConvertedParameter(QStringLiteral("线要素拓扑检查"), fallbackParameter) : 0;
    auto resolveReference = [&](const QgsFeature& source) {
        auto match = WaterResultClassification::classify(
            source.geometry(), parsedLayer->crs(), referenceLayer, matchTolerance);
        if (match.category.isEmpty() && !match.candidates.isEmpty())
            match = WaterResultClassification::classifyWithNeighbors(
                source, parsedLayer, referenceLayer, matchTolerance, match);
        if (hasFallbackTolerance && match.category.isEmpty() && match.candidates.isEmpty())
            match = WaterResultClassification::classifyDirectionFallback(
                source.geometry(), parsedLayer->crs(), referenceLayer, matchTolerance, fallbackTolerance);
        return match;
    };
    auto resolveWithChain = [&](const QgsFeature& feature) {
        if (cache && cache->contains(feature.id())) return cache->value(feature.id());
        WaterMatchCacheEntry entry;
        entry.match = resolveReference(feature);
        entry.chain = WaterReferenceChain::find(feature.geometry(), parsedLayer->crs(), referenceLayer,
                                                entry.match.category, matchTolerance);
        if (entry.chain.features.isEmpty() && entry.match.referenceId >= 0)
            entry.chain.features.append(referenceLayer->getFeature(entry.match.referenceId));
        if (cache) cache->insert(feature.id(), entry);
        return entry;
    };
    const auto sourceResolved = resolveWithChain(parsedFeature);
    const auto& sourceMatch = sourceResolved.match;
    const auto& sourceChain = sourceResolved.chain;
    const bool foundMatch = !sourceMatch.category.isEmpty() && !sourceChain.features.isEmpty();
    const QgsFeature matchedDbFeature = foundMatch
        ? sourceChain.features.front() : QgsFeature();

    if (!foundMatch) {
        // 如果在库体数据中没有找到匹配的要素，记录为错误
        QualityCheckResult result;
        result.errorType = "库体匹配待确认";
        result.description = sourceMatch.category.isEmpty()
            ? QString("无法唯一确定对应库体河段，连接关系待确认")
            : QString("成果要素%1已确定类别%2，但对应库体FID不唯一，连接关系待确认")
                .arg(parsedFeature.id()).arg(sourceMatch.category);
        result.feature = parsedFeature;
        result.geometry = parsedGeom;
        result.errorGeometry = parsedGeom;
        result.sourceFile = sourceFile;
        result.featureId = parsedFeature.id();
        result.calculatedValue = 0.0;
        result.standardValue = spatialTolerance; // 原先硬编码为 1.0，改为XML配置
        result.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
        result.passed = false;
        result.pointIndex = -1;
        if (!sourceMatch.category.isEmpty()) {
            result.referenceCategory = sourceMatch.category;
            result.referenceValue = sourceMatch.rawValue;
            result.referenceSource = referenceLayer->source();
        }

        results.push_back(result);
        return results;
    }

    // A connection may occur at an endpoint on the other line's interior.
    const auto connectionJunction = [](const QgsGeometry& first, const QgsGeometry& second) {
        const QgsGeometry exact = first.intersection(second);
        if (!exact.isNull() && !exact.isEmpty()) return exact;
        constexpr double nodeTolerance = 0.01;
        const QgsGeometry* lines[2] = {&first, &second};
        for (int side = 0; side < 2; ++side) {
            const QgsGeometry& line = *lines[side];
            const QgsGeometry& other = *lines[1 - side];
            QgsPointXY start, end;
            if (!WaterResultClassification::endpoints(line, start, end)) continue;
            for (const QgsPointXY& node : {start, end}) {
                const QgsGeometry point = QgsGeometry::fromPointXY(node);
                const double distance = point.distance(other);
                if (distance >= 0 && distance <= nodeTolerance) return point;
            }
        }
        return QgsGeometry();
    };
    struct ConnectedRiver {
        QgsFeature feature;
        QgsGeometry junction;
    };
    std::vector<ConnectedRiver> parsedConnectedFeatures;
    {
        QgsFeatureIterator parsedIter = parsedLayer->getFeatures(
            QgsFeatureRequest().setFilterRect(parsedGeom.boundingBox().buffered(0.01)));
        QgsFeature otherParsedFeature;

        while (parsedIter.nextFeature(otherParsedFeature)) {
            if (otherParsedFeature.id() == parsedFeature.id()) {
                continue;
            }
            QgsGeometry otherGeom = otherParsedFeature.geometry();
            if (otherGeom.isNull() || !otherGeom.isGeosValid()) {
                continue;
            }
            try {
                const QgsGeometry junction = connectionJunction(parsedGeom, otherGeom);
                if (!junction.isNull() && !junction.isEmpty())
                    parsedConnectedFeatures.push_back({otherParsedFeature, junction});
            }
            catch (const std::exception&) {
                continue;
            }
        }
    }
    //检查每个连接关系在库体数据中是否也存在
    for (const ConnectedRiver& connection : parsedConnectedFeatures) {
        const QgsFeature& connectedFeature = connection.feature;
        // 为连接的要素在库体数据中找匹配要素
        QgsGeometry connectedGeom = connectedFeature.geometry();
        if (connectedGeom.isNull() || !connectedGeom.isGeosValid()) {
            continue;
        }

        const auto connectedResolved = resolveWithChain(connectedFeature);
        const auto& connectedMatch = connectedResolved.match;
        const auto& connectedChain = connectedResolved.chain;
        const bool foundConnectedMatch = !connectedMatch.category.isEmpty()
            && !connectedChain.features.isEmpty();
        const QgsFeature matchedConnectedDbFeature = foundConnectedMatch
            ? connectedChain.features.front() : QgsFeature();

        if (!foundConnectedMatch) {
            QualityCheckResult pending{};
            pending.errorType = QStringLiteral("库体匹配待确认");
            pending.description = QStringLiteral("相连成果要素 %1 的库体匹配不唯一，连接关系待确认")
                .arg(connectedFeature.id());
            pending.description += QStringLiteral("，成果接点%1").arg(connection.junction.asWkt(2));
            pending.feature = parsedFeature;
            pending.geometry = parsedGeom;
            pending.errorGeometry = parsedGeom;
            pending.sourceFile = sourceFile;
            pending.featureId = parsedFeature.id();
            pending.standardValue = spatialTolerance;
            pending.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
            pending.passed = false;
            pending.pointIndex = -1;
            results.push_back(pending);
            continue;
        }

        if (foundConnectedMatch) {
            // 检查库体数据中对应的两个要素是否也连接
            bool dbFeaturesConnected = false;
            double bestOffset = std::numeric_limits<double>::infinity();
            double localGap = std::numeric_limits<double>::infinity();
            QgsFeatureId bestFirst = -1, bestSecond = -1;
            try {
                const double junctionSearchRadius = matchTolerance * 3;
                const QgsGeometry neighborhood = connection.junction.buffer(junctionSearchRadius, 8);
                for (const QgsFeature& left : sourceChain.features) {
                    const QgsGeometry first = left.geometry();
                    const double firstDistance = connection.junction.distance(first);
                    if (firstDistance < 0 || firstDistance > junctionSearchRadius) continue;
                    for (const QgsFeature& right : connectedChain.features) {
                        const QgsGeometry second = right.geometry();
                        const double secondDistance = connection.junction.distance(second);
                        if (secondDistance < 0 || secondDistance > junctionSearchRadius) continue;
                        if (left.id() == right.id()) {
                            const double offset = std::max(firstDistance, secondDistance);
                            if (offset < bestOffset) {
                                bestOffset = offset; bestFirst = left.id(); bestSecond = right.id();
                            }
                            continue;
                        }
                        const QgsGeometry databaseJunction = connectionJunction(first, second);
                        if (!databaseJunction.isNull() && !databaseJunction.isEmpty()) {
                            const double offset = connection.junction.distance(databaseJunction);
                            if (offset >= 0 && offset < bestOffset) {
                                bestOffset = offset; bestFirst = left.id(); bestSecond = right.id();
                            }
                        } else {
                            const QgsGeometry localFirst = first.intersection(neighborhood);
                            const QgsGeometry localSecond = second.intersection(neighborhood);
                            const double gap = localFirst.distance(localSecond);
                            if (gap >= 0 && gap < localGap) {
                                localGap = gap;
                                if (!std::isfinite(bestOffset)) { bestFirst = left.id(); bestSecond = right.id(); }
                            }
                        }
                    }
                }
                dbFeaturesConnected = bestOffset <= matchTolerance;
            }
            catch (const std::exception& e) {
                continue;
            }

            if (!dbFeaturesConnected) {
                // 解析数据中连接，但库体数据中不连接，记录为错误
                QualityCheckResult result;
                result.errorType = bestFirst < 0 ? QStringLiteral("库体匹配待确认") : QStringLiteral("连接关系不一致");
                result.description = QString("解析要素%1与要素%2连接，库体接点比较FID %3/%4：")
                    .arg(parsedFeature.id()).arg(connectedFeature.id()).arg(bestFirst).arg(bestSecond);
                if (std::isfinite(bestOffset))
                    result.description += QString("接点偏差%1米，超过%2米").arg(bestOffset,0,'f',2).arg(matchTolerance,0,'f',2);
                else if (std::isfinite(localGap))
                    result.description += QString("库体线在接点附近存在%1米断缝").arg(localGap,0,'f',2);
                else
                    result.description += QStringLiteral("接点附近缺少可确认的库体连接");
                result.description += QStringLiteral("，成果接点%1").arg(connection.junction.asWkt(2));
                result.feature = parsedFeature;
                result.geometry = parsedGeom;
                result.errorGeometry = parsedGeom;
                result.sourceFile = sourceFile;
                result.featureId = parsedFeature.id();
                result.calculatedValue = 0.0;
                result.standardValue = spatialTolerance; // 原先硬编码为 1.0，改为XML配置
                result.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
                result.passed = false;
                result.pointIndex = -1;
                const QString field = referenceLayer->fields().indexFromName(QStringLiteral("LEVEL")) >= 0
                    ? QStringLiteral("LEVEL") : QStringLiteral("TYPE");
                result.referenceSource = referenceLayer->source();
                result.referenceFeatureId = bestFirst >= 0 ? bestFirst : matchedDbFeature.id();
                result.referenceValue = matchedDbFeature.attribute(field).toString().trimmed();
                result.referenceCategory = WaterResultClassification::categoryFor(field, result.referenceValue);

                results.push_back(result);
            }
        }
    }

    return results;
}
//检查水系线与其他要素的空间关系
std::vector<QualityCheckResult> ReadShpDialog::checkRiverSpatialRelations(QgsVectorLayer* riverLayer, const std::vector<QgsFeature>& waterSystemFeatures, const QString& sourceFile) {
    std::vector<QualityCheckResult> results;

    // 从配置读取参数，替换硬编码
    double riverOverlapTolerance = m_config ? m_config->getRiverOverlapToleranceNew() : 0.1;
    double riverIntersectionTolerance = m_config ? m_config->getRiverIntersectionToleranceNew() : 0.2;
    double riverExceedsTolerance = m_config ? m_config->getRiverExceedsToleranceNew() : 0.3;
    double databaseMatchTolerance = m_config ? m_config->getDatabaseMatchToleranceNew() : 1.0;
    if (!riverLayer || waterSystemFeatures.empty()) {
        return results;
    }


    // 使用XML中的容差（已按比例尺转换），分别用于不同关系
    double overlapTolerance = m_config ? m_config->getRiverOverlapTolerance() : 0.1;
    double intersectionTolerance = m_config ? m_config->getRiverIntersectionTolerance() : 0.2;
    double exceedsTolerance = m_config ? m_config->getRiverExceedsTolerance() : 0.3;

    logTextEdit->append(QString("开始检查河网空间关系，容差: %1米").arg(overlapTolerance).arg(intersectionTolerance).arg(exceedsTolerance));

    // 按要素类型分组水系要素
    std::vector<QgsFeature> waterAreas;      // 水系面（面要素）
    std::vector<QgsFeature> dams;            // 拦水坝（线要素）
    std::vector<QgsFeature> wells;           // 井（线要素）
    std::vector<QgsFeature> springs;         // 泉（线要素）
    std::vector<QgsFeature> reservoirs;      // 点状水库（线要素）

    for (const auto& feature : waterSystemFeatures) {
        QString featureType = getWaterFeatureType(feature, sourceFile);

        if (featureType == "水系面") {
            waterAreas.push_back(feature);
        }
        else if (featureType == "拦水坝") {
            dams.push_back(feature);
        }
        else if (featureType == "井") {
            wells.push_back(feature);
        }
        else if (featureType == "泉") {
            springs.push_back(feature);
        }
        else if (featureType == "点状水库") {
            reservoirs.push_back(feature);
        }
    }

    logTextEdit->append(QString("水系要素分类完成 - 水系面: %1, 拦水坝: %2, 井: %3, 泉: %4, 点状水库: %5")
        .arg(waterAreas.size()).arg(dams.size()).arg(wells.size()).arg(springs.size()).arg(reservoirs.size()));

    // 遍历河流要素进行空间关系检查
    QgsFeatureIterator riverIterator = riverLayer->getFeatures();
    QgsFeature riverFeature;

    while (riverIterator.nextFeature(riverFeature)) {
        QgsGeometry riverGeom = riverFeature.geometry();
        if (riverGeom.isNull()) continue;

        //  检查河流是否超出水系面边界
        for (const auto& waterArea : waterAreas) {
            if (checkRiverExceedsWaterArea(riverGeom, waterArea.geometry(), exceedsTolerance)) {
                QualityCheckResult result;
                result.passed = false;
                result.errorType = "河流超出水系面边界";
                result.description = QString("河流要素超出了水系面的边界范围，容差: %1米").arg(exceedsTolerance);
                result.feature = riverFeature;
                result.errorGeometry = riverGeom;
                result.sourceFile = sourceFile;
                result.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
                result.featureId = riverFeature.id();
                result.geometry = riverGeom;
                result.standardValue = exceedsTolerance;
                results.push_back(result);

                logTextEdit->append(QString("发现问题: 河流要素ID %1 超出水系面边界").arg(riverFeature.id()));
            }
        }

        // 检查河流与线状水系要素的关系
        // 检查与泉的关系（河流不应超出泉的范围）
        for (const auto& spring : springs) {
            if (checkRiverExceedsLineFeature(riverGeom, spring.geometry(), exceedsTolerance)) {
                QualityCheckResult result;
                result.passed = false;
                result.errorType = "河流超出泉范围";
                result.description = QString("河流要素超出了泉的范围，容差: %1米").arg(exceedsTolerance);
                result.feature = riverFeature;
                result.errorGeometry = riverGeom;
                result.sourceFile = sourceFile;
                result.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
                result.featureId = riverFeature.id();
                result.geometry = riverGeom;
                result.standardValue = exceedsTolerance;
                results.push_back(result);

                logTextEdit->append(QString("发现问题: 河流要素ID %1 超出泉范围").arg(riverFeature.id()));
            }
        }

        // 检查与井的关系
        for (const auto& well : wells) {
            if (checkRiverExceedsLineFeature(riverGeom, well.geometry(), exceedsTolerance)) {
                QualityCheckResult result;
                result.passed = false;
                result.errorType = "河流超出井范围";
                result.description = QString("河流要素超出了井的范围，容差: %1米").arg(exceedsTolerance);
                result.feature = riverFeature;
                result.errorGeometry = riverGeom;
                result.sourceFile = sourceFile;
                result.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
                result.featureId = riverFeature.id();
                result.geometry = riverGeom;
                result.standardValue = exceedsTolerance;
                results.push_back(result);

                logTextEdit->append(QString("发现问题: 河流要素ID %1 超出井范围").arg(riverFeature.id()));
            }
        }

        // 检查与点状水库的关系
        for (const auto& reservoir : reservoirs) {
            if (checkRiverExceedsLineFeature(riverGeom, reservoir.geometry(), exceedsTolerance)) {
                QualityCheckResult result;
                result.passed = false;
                result.errorType = "河流超出点状水库范围";
                result.description = QString("河流要素超出了点状水库的范围，容差: %1米").arg(exceedsTolerance);
                result.feature = riverFeature;
                result.errorGeometry = riverGeom;
                result.sourceFile = sourceFile;
                result.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
                result.featureId = riverFeature.id();
                result.geometry = riverGeom;
                result.standardValue = exceedsTolerance;
                results.push_back(result);

                logTextEdit->append(QString("发现问题: 河流要素ID %1 超出点状水库范围").arg(riverFeature.id()));
            }
        }

        //  检查与拦水坝的关系（河流与拦水坝应该有合理的交叉关系）
        for (const auto& dam : dams) {
            if (checkRiverDamRelation(riverGeom, dam.geometry(), intersectionTolerance)) {
                QualityCheckResult result;
                result.passed = false;
                result.errorType = "河流与拦水坝关系异常";
                result.description = QString("河流要素与拦水坝的空间关系异常，容差: %1米").arg(intersectionTolerance);
                result.feature = riverFeature;
                result.errorGeometry = riverGeom;
                result.sourceFile = sourceFile;
                result.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
                result.featureId = riverFeature.id();
                result.geometry = riverGeom;
                result.standardValue = intersectionTolerance;
                results.push_back(result);

                logTextEdit->append(QString("发现问题: 河流要素ID %1 与拦水坝关系异常").arg(riverFeature.id()));
            }
        }
    }

    logTextEdit->append(QString("河网空间关系检查完成，发现 %1 个问题").arg(results.size()));
    return results;
}
//检查河流是否超出水系面边界（面要素）
bool ReadShpDialog::checkRiverExceedsWaterArea(const QgsGeometry& riverGeom, const QgsGeometry& waterAreaGeom, double tolerance)
{
    if (riverGeom.isNull() || waterAreaGeom.isNull()) {
        return false;
    }

    try {
        // 检查河流是否与水系面相交
        if (!riverGeom.intersects(waterAreaGeom)) {
            return false; // 如果不相交，则不存在超出问题
        }

        // 计算河流与水系面的交集
        QgsGeometry intersection = riverGeom.intersection(waterAreaGeom);
        if (intersection.isNull()) {
            return false;
        }

        // 计算交集长度与河流总长度的比例
        double riverLength = riverGeom.length();
        double intersectionLength = intersection.length();

        if (riverLength <= 0) {
            return false;
        }

        double intersectionRatio = intersectionLength / riverLength;

        // 如果交集比例小于阈值（如0.8），说明河流有部分超出了水系面,阈值可以根据实际需求调整
        double thresholdRatio = 0.8; // 80%的河流应该在水系面内

        bool exceeds = intersectionRatio < thresholdRatio;

        if (exceeds) {
            logTextEdit->append(QString("河流超出水系面：交集比例 %1%, 阈值 %2%")
                .arg(intersectionRatio * 100, 0, 'f', 1)
                .arg(thresholdRatio * 100, 0, 'f', 1));
        }

        return exceeds;
    }
    catch (const std::exception& e) {
        logTextEdit->append(QString("检查河流超出水系面时发生异常: %1").arg(e.what()));
        return false;
    }
}
//检查河流是否超出线状要素范围（井、泉、点状水库）
bool ReadShpDialog::checkRiverExceedsLineFeature(const QgsGeometry& riverGeom, const QgsGeometry& lineFeatureGeom, double tolerance)
{
    if (riverGeom.isNull() || lineFeatureGeom.isNull()) {
        return false;
    }

    try {
        // 计算河流与线状要素的最小距离
        double distance = riverGeom.distance(lineFeatureGeom);

        // 如果距离大于容差，认为超出范围
        return distance > tolerance;
    }
    catch (const std::exception& e) {
        logTextEdit->append(QString("检查河流超出线状要素时发生异常: %1").arg(e.what()));
        return false;
    }
}
//检查河流与拦水坝的空间关系
bool ReadShpDialog::checkRiverDamRelation(const QgsGeometry& riverGeom, const QgsGeometry& damGeom, double tolerance)
{
    if (riverGeom.isNull() || damGeom.isNull()) {
        return false;
    }

    try {
        // 检查河流与拦水坝是否有合理的交叉关系
        if (!riverGeom.intersects(damGeom)) {
            // 如果不相交，检查距离是否在合理范围内
            double distance = riverGeom.distance(damGeom);
            return distance > tolerance * 2; // 拦水坝应该与河流有更紧密的关系
        }

        return false; // 有交叉关系，认为正常
    }
    catch (const std::exception& e) {
        logTextEdit->append(QString("检查河流与拦水坝关系时发生异常: %1").arg(e.what()));
        return false;
    }
}

//获取水系要素类型（根据实际情况：拦水坝、井、泉、点状水库为线要素，水系面为面要素）
QString ReadShpDialog::getWaterFeatureType(const QgsFeature& feature, const QString& fileName)
{
    QString baseName = QFileInfo(fileName).baseName().toLower();
    QgsWkbTypes::Type geomType = feature.geometry().wkbType();

    // 面要素：水系面
    if (QgsWkbTypes::geometryType(geomType) == QgsWkbTypes::PolygonGeometry) {
        if (isWaterAreaFile(fileName)) {
            return "水系面";
        }
        return "水系面"; // 默认面要素都认为是水系面
    }

    // 线要素：根据文件名判断具体类型
    if (QgsWkbTypes::geometryType(geomType) == QgsWkbTypes::LineGeometry) {

        // 拦水坝（线要素）
        if (isDamFile(fileName) || baseName.contains("拦水坝") || baseName.contains("lsb")) {
            return "拦水坝";
        }

        // 井（线要素）
        if (isWellFile(fileName) || baseName.contains("井") || baseName.contains("j")) {
            return "井";
        }

        // 泉（线要素）
        if (isSpringFile(fileName) || baseName.contains("泉") || baseName.contains("q")) {
            return "泉";
        }

        // 点状水库（线要素）
        if (isWaterReservoirFile(fileName) || baseName.contains("水库") || baseName.contains("sk")) {
            return "点状水库";
        }

        // 河流（水系线）
        if (isRiverFile(fileName) || baseName.contains("河流") || baseName.contains("hl")) {
            return "河流";
        }

        // 其他水系线要素的通用判断
        if (isWaterLineFile(fileName)) {
            // 按优先级进行细分判断
            if (baseName.contains("拦水坝") || baseName.contains("lsb") || baseName.contains("dam")) {
                return "拦水坝";
            }
            else if (baseName.contains("井") || baseName.contains("well") || baseName.contains("j")) {
                return "井";
            }
            else if (baseName.contains("泉") || baseName.contains("spring") || baseName.contains("q")) {
                return "泉";
            }
            else if (baseName.contains("水库") || baseName.contains("reservoir") || baseName.contains("sk")) {
                return "点状水库";
            }
            else if (baseName.contains("河") || baseName.contains("river") || baseName.contains("hl")) {
                return "河流";
            }
            else {
                return "河流"; // 默认水系线要素为河流
            }
        }

        // 如果都不匹配，默认为河流
        return "河流";
    }

    // 点要素：处理真正的点状要素
    if (QgsWkbTypes::geometryType(geomType) == QgsWkbTypes::PointGeometry) {
        // 根据文件名判断点要素类型
        if (baseName.contains("井") || baseName.contains("j")) {
            return "井点";
        }
        else if (baseName.contains("泉") || baseName.contains("q")) {
            return "泉点";
        }
        else if (baseName.contains("水库") || baseName.contains("sk")) {
            return "水库点";
        }
        else if (baseName.contains("拦水坝") || baseName.contains("lsb")) {
            return "拦水坝点";
        }
        else {
            return "点状水系要素";
        }
    }

    return "未知要素类型";
}
//检查水系线有没有和其他水系要素重叠
bool ReadShpDialog::checkOverlapRelation(const QgsGeometry& riverGeom, const QgsGeometry& waterFeatureGeom, double tolerance)
{
    if (!riverGeom.isGeosValid() || !waterFeatureGeom.isGeosValid()) {
        return false;
    }

    // 检查是否存在不合理的重叠
    QgsGeometry intersection = riverGeom.intersection(waterFeatureGeom);
    if (!intersection.isEmpty()) {
        // 如果交集长度超过河流总长度的一定比例，认为是不合理重叠
        double intersectionLength = intersection.length();
        double riverLength = riverGeom.length();

        if (intersectionLength > riverLength * 0.8) { // 80%以上重叠认为不合理
            return true;
        }
    }

    return false;
}
//检查水系线有没有和其他水系要素相交
bool ReadShpDialog::checkIntersectionRelation(const QgsGeometry& riverGeom, const QgsGeometry& waterFeatureGeom, double tolerance)
{
    if (!riverGeom.isGeosValid() || !waterFeatureGeom.isGeosValid()) {
        return false;
    }

    // 检查是否存在不合理的交叉
    if (riverGeom.intersects(waterFeatureGeom)) {
        QgsGeometry intersection = riverGeom.intersection(waterFeatureGeom);

        // 如果交集是点或很短的线段，可能是合理的连接
        if (intersection.type() == QgsWkbTypes::PointGeometry) {
            return false; // 点交叉通常是合理的
        }

        if (intersection.type() == QgsWkbTypes::LineGeometry && intersection.length() < tolerance) {
            return false; // 很短的线交叉可能是合理的
        }

        return true; // 其他情况认为是不合理交叉
    }

    return false;
}
//检查水系线有没有和其他水系要素超出
bool ReadShpDialog::checkExceedsRelation(const QgsGeometry& riverGeom, const QgsGeometry& waterFeatureGeom, double tolerance)
{
    if (!riverGeom.isGeosValid() || !waterFeatureGeom.isGeosValid()) {
        return false;
    }

    // 检查河流是否超出水系面等要素的边界
    if (waterFeatureGeom.type() == QgsWkbTypes::PolygonGeometry) {
        QgsGeometry bufferedWater = waterFeatureGeom.buffer(tolerance, 5);
        QgsGeometry difference = riverGeom.difference(bufferedWater);

        if (!difference.isEmpty() && difference.length() > tolerance) {
            return true; // 河流有部分超出了水系面边界
        }
    }

    return false;
}
//判断是不是水库
bool ReadShpDialog::isWaterReservoirFile(const QString& fileName)
{
    QString baseName = QFileInfo(fileName).baseName().toLower();
    QStringList keywords = { "水库", "reservoir", "sk" };

    for (const QString& keyword : keywords) {
        if (baseName.contains(keyword)) {
            return true;
        }
    }
    return false;
}
//判断是不是拦水坝
bool ReadShpDialog::isDamFile(const QString& fileName)
{
    QString baseName = QFileInfo(fileName).baseName().toLower();
    QStringList keywords = { "拦水坝", "dam", "lsb", "水闸", "gate" };

    for (const QString& keyword : keywords) {
        if (baseName.contains(keyword)) {
            return true;
        }
    }
    return false;
}
//判断是不是井
bool ReadShpDialog::isWellFile(const QString& fileName)
{
    QString baseName = QFileInfo(fileName).baseName().toLower();
    QStringList keywords = { "井", "well", "j" };

    for (const QString& keyword : keywords) {
        if (baseName.contains(keyword)) {
            return true;
        }
    }
    return false;
}
//判断是不是泉
bool ReadShpDialog::isSpringFile(const QString& fileName)
{
    QString baseName = QFileInfo(fileName).baseName().toLower();
    QStringList keywords = { "泉", "spring", "q" };

    for (const QString& keyword : keywords) {
        if (baseName.contains(keyword)) {
            return true;
        }
    }
    return false;
}

//批量加载图层到地图
void ReadShpDialog::loadMultipleLayersToMap()
{
    if (!mapCanvas) {
        return;
    }

    try {
        clearAllLayers();

        // 加载库体图层（蓝色）
        if (referenceLayer && referenceLayer->isValid()) {
            setLayerStyle(referenceLayer, "blue", 2.0);
            allDisplayLayers << referenceLayer;
            logTextEdit->append("库体图层已加载（蓝色）");
        }

        // 加载批量库体图层
        for (auto* refLayer : referenceLayers) {
            if (refLayer && refLayer->isValid()) {
                setLayerStyle(refLayer, "blue", 2.0);
                allDisplayLayers << refLayer;
            }
        }

        //  加载解析图层（黄色）
        if (currentLayer && currentLayer->isValid()) {
            setLayerStyle(currentLayer, "yellow", 2.0);
            allDisplayLayers << currentLayer;
            logTextEdit->append("解析图层已加载（黄色）");
        }

        // 加载批量解析图层
        for (auto* parseLayer : parseLayers) {
            if (parseLayer && parseLayer->isValid()) {
                setLayerStyle(parseLayer, "yellow", 2.0);
                allDisplayLayers << parseLayer;
            }
        }

        // 创建并加载错误图层（红色）
        if (!lastCheckResults.empty()) {
            // 从解析图层获取CRS；若当前解析图层不可用，尝试批量解析图层的第一个；最后再兜底用参考图层
            QgsCoordinateReferenceSystem parsedCrs;
            if (currentLayer && currentLayer->isValid()) {
                parsedCrs = currentLayer->crs();
            }
            else if (!parseLayers.empty() && parseLayers.front() && parseLayers.front()->isValid()) {
                parsedCrs = parseLayers.front()->crs();
            }
            else if (referenceLayer && referenceLayer->isValid()) {
                parsedCrs = referenceLayer->crs();
            }

            // 改为分类型创建错误图层，确保点错误以点要素显示
            QList<QgsVectorLayer*> errLayers = createErrorLayers(lastCheckResults, parsedCrs);
            if (!errLayers.isEmpty()) {
                // 将第一层记录到 errorLayer（兼容原有成员），其余直接加入显示栈
                errorLayer = errLayers.first();
                for (QgsVectorLayer* lyr : errLayers) {
                    setLayerStyle(lyr, "red", 3.0);
                    allDisplayLayers << lyr;
                }
                logTextEdit->append("错误要素图层已加载（红色，按点/线/面分层）");
            }
        }

        refreshMapDisplay();
    }
    catch (const std::exception& e) {
        logTextEdit->append(QString("加载多图层失败: %1").arg(e.what()));
    }
}
void ReadShpDialog::setLayerStyle(QgsVectorLayer* layer, const QString& color, double width)
{
    if (!layer || !layer->isValid()) {
        return;
    }

    QgsSymbol* symbol = nullptr;
    if (layer->geometryType() == QgsWkbTypes::LineGeometry) {
        QgsLineSymbol* lineSymbol = QgsLineSymbol::createSimple({
            {"color", color},
            {"width", QString::number(width)}
            });
        symbol = lineSymbol;
    }
    else if (layer->geometryType() == QgsWkbTypes::PolygonGeometry) {
        QgsFillSymbol* fillSymbol = QgsFillSymbol::createSimple({
            {"color", color},
            {"outline_color", color},
            {"outline_width", QString::number(width)}
            });
        symbol = fillSymbol;
    }
    else if (layer->geometryType() == QgsWkbTypes::PointGeometry) {
        QgsMarkerSymbol* markerSymbol = QgsMarkerSymbol::createSimple({
            {"color", color},
            {"size", QString::number(width * 2)}
            });
        symbol = markerSymbol;
    }

    if (symbol) {
        QgsSingleSymbolRenderer* renderer = new QgsSingleSymbolRenderer(symbol);
        layer->setRenderer(renderer);
    }
}
QgsVectorLayer* ReadShpDialog::createErrorLayer(const std::vector<QualityCheckResult>& results, const QgsCoordinateReferenceSystem& crs)
{
    if (results.empty()) {
        return nullptr;
    }

    // 使用通用几何类型定义，后续通过 setCrs 设置坐标系
    QString layerDef = "LineString";
    QgsVectorLayer* memoryLayer = new QgsVectorLayer(layerDef, "错误要素", "memory");

    if (!memoryLayer->isValid()) {
        delete memoryLayer;
        return nullptr;
    }

    // 设置为解析图层一致的CRS（支持自定义/非EPSG投影）
    if (crs.isValid()) {
        memoryLayer->setCrs(crs, true);
    }

    QgsVectorDataProvider* provider = memoryLayer->dataProvider();

    // 添加属性字段
    QList<QgsField> fields;
    fields << QgsField("error_type", QVariant::String);
    fields << QgsField("description", QVariant::String);
    provider->addAttributes(fields);
    memoryLayer->updateFields();

    // 添加错误要素
    QList<QgsFeature> features;
    for (const auto& result : results) {
        if (!result.passed && !result.errorGeometry.isEmpty()) {
            QgsFeature feature;
            feature.setGeometry(result.errorGeometry);
            QgsAttributes attributes;
            attributes << result.errorType << result.description;
            feature.setAttributes(attributes);
            features << feature;
        }
    }

    provider->addFeatures(features);
    memoryLayer->updateExtents();

    return memoryLayer;
}
QList<QgsVectorLayer*> ReadShpDialog::createErrorLayers(const std::vector<QualityCheckResult>& results, const QgsCoordinateReferenceSystem& crs)
{
    // ... existing code ...
    QList<QgsVectorLayer*> createdLayers;
    if (results.empty()) {
        return createdLayers;
    }

    // 先按 errorGeometry 的几何类型分组（严格使用错误几何）
    std::map<QgsWkbTypes::GeometryType, std::vector<QualityCheckResult>> groups;
    for (const auto& r : results) {
        if (!r.passed && !r.errorGeometry.isNull() && !r.errorGeometry.isEmpty()) {
            QgsWkbTypes::GeometryType gt = QgsWkbTypes::geometryType(r.errorGeometry.wkbType());
            groups[gt].push_back(r);
        }
    }
    if (groups.empty()) {
        return createdLayers;
    }

    // 为每种类型创建独立的内存图层
    for (const auto& kv : groups) {
        QgsWkbTypes::GeometryType gt = kv.first;
        const auto& rs = kv.second;

        QString layerDef;
        QString layerName;
        switch (gt) {
        case QgsWkbTypes::PointGeometry:   layerDef = "Point";     layerName = "错误要素_点"; break;
        case QgsWkbTypes::LineGeometry:    layerDef = "LineString"; layerName = "错误要素_线"; break;
        case QgsWkbTypes::PolygonGeometry: layerDef = "Polygon";   layerName = "错误要素_面"; break;
        default: continue;
        }

        QgsVectorLayer* mem = new QgsVectorLayer(layerDef, layerName, "memory");
        if (!mem || !mem->isValid()) {
            if (mem) delete mem;
            continue;
        }
        if (crs.isValid()) {
            mem->setCrs(crs, true);
        }

        QgsVectorDataProvider* provider = mem->dataProvider();
        QList<QgsField> fields;
        fields << QgsField("error_type", QVariant::String);
        fields << QgsField("description", QVariant::String);
        provider->addAttributes(fields);
        mem->updateFields();

        QList<QgsFeature> features;
        for (const auto& r : rs) {
            QgsFeature f;
            f.setGeometry(r.errorGeometry);
            QgsAttributes attrs;
            attrs << r.errorType << r.description;
            f.setAttributes(attrs);
            features << f;
        }

        provider->addFeatures(features);
        mem->updateExtents();

        createdLayers << mem;
    }

    return createdLayers;
    // ... existing code ...
}
void ReadShpDialog::clearAllLayers()
{
    allDisplayLayers.clear();

    if (errorLayer) {
        delete errorLayer;
        errorLayer = nullptr;
    }
}
void ReadShpDialog::refreshMapDisplay()
{
    if (!mapCanvas) {
        return;
    }

    mapCanvas->setLayers(allDisplayLayers);

    // 自动缩放到所有图层
    if (!allDisplayLayers.isEmpty()) {
        QgsRectangle combinedExtent;
        bool first = true;

        for (auto* layer : allDisplayLayers) {
            if (auto* vectorLayer = qobject_cast<QgsVectorLayer*>(layer)) {
                if (vectorLayer->isValid()) {
                    QgsRectangle extent = vectorLayer->extent();
                    if (first) {
                        combinedExtent = extent;
                        first = false;
                    }
                    else {
                        combinedExtent.combineExtentWith(extent);
                    }
                }
            }
        }

        if (!first) {
            combinedExtent.scale(1.1);  // 稍微放大一点
            mapCanvas->setExtent(combinedExtent);
        }
    }

    mapCanvas->refresh();
}
// Resolve a reference layer's configured road type. A one-to-one GDB mapping
// (for example LVLL -> 村道) applies to every valid feature in that layer;
// shared layers such as LRDL must continue to use per-feature attributes.
int ReadShpDialog::resolveConfiguredReferenceRoadType(QgsVectorLayer* referenceLayer)
{
    if (!referenceLayer) return 0;

    auto normalizeLayerKey = [](QString value) {
        value = value.trimmed().replace('\\', '/').toLower();
        const int layerNamePos = value.indexOf("|layername=");
        if (layerNamePos >= 0) {
            value = value.mid(layerNamePos + QString("|layername=").size());
        }
        while (value.endsWith('/')) value.chop(1);
        const int slashPos = value.lastIndexOf('/');
        if (slashPos >= 0) value = value.mid(slashPos + 1);
        return value;
    };

    const QString referenceName = normalizeLayerKey(referenceLayer->name());
    const QString referenceSource = normalizeLayerKey(referenceLayer->source());
    QSet<QString> uniqueStandardLayerNames;
    for (const StandardLayerEntry& entry : m_standardLayers) {
        const QString gdbKey = normalizeLayerKey(entry.gdbShpPath);
        if (gdbKey.isEmpty()) continue;

        const bool matchesLayerName = (gdbKey == referenceName);
        const bool matchesSource = (gdbKey == referenceSource);
        if (matchesLayerName || matchesSource) {
            const QString standardName = entry.standardLayerName.trimmed();
            if (!standardName.isEmpty()) uniqueStandardLayerNames.insert(standardName);
        }
    }

    // Only a unique standard meaning is safe to apply to every feature.
    // LRDL maps to several standards, so it intentionally falls through to
    // per-feature CLASID/TYPE classification.
    if (uniqueStandardLayerNames.size() == 1) {
        const QString standardName = *uniqueStandardLayerNames.constBegin();
        return getRoadTypeFromFileName(standardName);
    }

    // If the CSV mapping is unavailable, retain an explicitly bound semantic
    // as a fallback. A known shared GDB layer with multiple mappings is not
    // allowed to use this fallback and must remain attribute-driven.
    QString configuredSemantic = referenceLayer->customProperty(QStringLiteral("quality/standard_layer_name")).toString().trimmed();
    if (uniqueStandardLayerNames.isEmpty() && !configuredSemantic.isEmpty()) {
        const int configuredType = getRoadTypeFromFileName(configuredSemantic);
        if (configuredType > 0) return configuredType;
    }
    return 0;
}
