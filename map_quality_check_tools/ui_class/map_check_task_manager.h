#pragma once

#include <QtWidgets/QDialog>
#include "ui_map_check_task_manager.h"
#include <QDomDocument>
#include <QProgressBar>
#include <QMenu>
#include <QModelIndex>
#include <QEvent>
#include <QFileDialog>
#include <QDateTime>
#include "map_check_common.h"
#include "map_check_algorithms.h"
#include "AlgorithmThread.h"
#include <QJsonObject> // <--- 替换 "json.hpp"
#include <QStringList>
#include <QVector>
#include <QSet>
#include <QHash>

struct CsvLayerInfo {
    QList<QString> gdbLayerNames;         // gdb图层名列表（支持多个，用|拆分）
    QList<QString> actualLayerNames;      // 实际图层名列表（支持多个，用|拆分）
    QList<QString> actualAnnotLayerNames; // 注记图层名列表（支持多个，用|拆分）
    QString correspondingAnnotLayer;       // 第5列：对应注记层（配置保留，213 不用作分组）
    QString legendField;                  // 第6列：图例字段值
};

using namespace std;
namespace Ui {
    class map_check_task_manager;
}

class map_check_task_manager : public QDialog
{
    Q_OBJECT

public:
    map_check_task_manager(QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
    ~map_check_task_manager(void);

    // 查找质检项节点（用于更新树形控件状态）
    QTreeWidgetItem* findCheckItemByName(const QString& checkItemName);
    // 新增：根据图层名称和质检项名称精确查找质检项节点
    QTreeWidgetItem* findCheckItemByLayerAndName(const QString& layerName, const QString& checkItemName);
    // 新增：CSV解析接口（解析CSV文件，返回"标准图层名→图层信息"映射）
    static  QMap<QString, CsvLayerInfo> parseCsvToLayerMap(const QString& csvPath);
    // 供图幅接边任务读取的一期任务配置快照
    static QJsonObject loadPhaseOneConfigSnapshot();
    //路径拼接
    static void buildShpPaths(AlgorithmFilePaths& filePaths, const CsvLayerInfo& matchedLayers);
    //线程
    void executeAlgorithms(std::atomic<bool>& stopFlag,std::atomic<bool>& pauseFlag,AlgorithmThread* thread);
private slots:
    // "批量添加"按钮点击槽函数
    void on_pushButton_BatchAdd_clicked();
    // "添加路径"按钮点击槽函数
    void on_pushButton_AddGeopdfPath_clicked();
    // “清空路径列表”按钮点击槽函数
    void on_pushButton_ClearGeopdfPath_clicked();
    // “上调地图优先级”按钮点击槽函数
    void on_pushButton_upmap_clicked();
    // “下调地图优先级”按钮点击槽函数
    void on_pushButton_downmap_clicked();
    // “删除地图”按钮点击槽函数
    void on_pushButton_deletemap_clicked();
    // 质检方案设置（文件夹）
    void on_pushButton_loadxmlfiles_clicked();
    //质检方案配置（文件）
    void on_pushButton_loadxmlfile_clicked();
    //“上调子任务优先级”按钮
    void on_pushButton_uptask_clicked();
    //“下调子任务优先级”按钮
    void on_pushButton_downtask_clicked();
    //“删除子任务”按钮
    void on_pushButton_delete_clicked();
    // 新增：处理图标按钮点击事件
    void onLoadButtonClicked(const QModelIndex& index);
    //关闭交互界面
    void on_pushButton_close_clicked();
    // “选择保存路径”按钮点击的槽函数
    void on_Button_SaveLogFilePath_clicked();
    // “开始/暂停”单一按钮点击槽函数
    void on_pushButton_StartPause_clicked();
    // 数据预检查：检查实际图层与对应 GDB 的空间关系，以及实际注记层要素类型和字体样式
    void on_pushButton_DataPrecheck_clicked();
    // “质检报告导出”按钮点击槽函数
    void on_pushButton_ReportExport_clicked();

public slots:
    void appendLog(const QString& msg);
    void updateProgress(int value);
    void refreshTaskTreeByScale(int checkedScale);
    // 切换到指定图幅时重置状态；仅在 GUI 线程调用。
    void beginMapTaskStatus(int taskRow);
    
    // 重置所有质检项的状态图标为未质检状态
    void resetAllCheckItemStatusIcons();


signals:
    // 质检报告导出请求信号
    void reportRequested(const QString& resultJsonPath);

private:
    Ui::map_check_task_manager* ui;

    enum class CompletionStatus {
        NotChecked,   // 未质检
        HasErrors,    // 有错误
        Passed      // 无错误
    };

    // 解析XML文件，获取质检项信息
    void parseXmlFile(const QString& xmlFilePath, QTreeWidgetItem* parentItem);

    // 当前右键点击的单元格位置
    QPoint currentCellPos; // 存储右键点击的单元格位置（必须声明）

    // 新增：创建带图标的单元格部件
    QWidget* createCellWidget(int row, int col, const QString& text);

    // 新增：通用的文件/文件夹加载函数
    void loadFileOrFolderByIndex(const QModelIndex& index);

    // 新增：设置单元格内容
    void setCellContent(int row, int col, const QString& content);

    void setCompletionStatusIcon(QTreeWidgetItem* item, CompletionStatus status);

    // 根据图标获取质检项状态
    QString getCheckItemStatus(const QTreeWidgetItem* item);

    // 生成JSON数据核心函数
    QJsonObject generateJsonData();

    //写入json到文件
    bool writeJsonToFile(const QJsonObject& jsonData, const QString& filePath);
    void savePhaseOneConfigSnapshot() const;

    QString m_saveDir;               // 存储选择的保存路径
    QDateTime m_globalStartTime;     // 全局开始时间
    QDateTime m_globalEndTime;       // 全局结束时间
    QString m_runLogFilePath;          // 本次运行日志的落盘路径

    // 核心成员
    QHash<QString, AlgorithmFunc> m_algorithmMap; // 质检项名称→算法函数映射
    QHash<QString, CheckItemResult> m_checkResults; // 存储所有算法结果
    QJsonObject m_waterClassificationSummary; // 当前地图的水系分类汇总
    int m_currentTaskRow = -1; // 当前执行算法的表格行索引，供 JSON 生成使用
    int m_displayedTaskRow = -1; // 当前树形界面所显示的图幅行，仅在 GUI 线程访问
    std::atomic<int> m_completedCheckCount{0}; // 本次实际执行的质检项数
    std::atomic<int> m_succeededCheckCount{0}; // 正常完成的质检项数（含发现质量问题）
    std::atomic<int> m_failedCheckCount{0};    // 因异常执行失败的质检项数
    std::atomic<int> m_skippedMapCount{0};     // 因输入问题跳过的地图数
    QString m_currentJsonFilePath; // 当前JSON文件路径
    QString m_geopdfPaths;
    QStringList m_xmlPaths; // 存储所有加载的 XML 路径
    // 图层名称 + 质检项名称 -> XML 中配置的严重程度（高 / 中 / 低）
    QHash<QString, QString> m_checkItemSeverity;
    QByteArray m_originalTaskTreeData;  // === 新增：保存原始任务树结构 ===
    bool m_taskTreeFiltered = false;    // 当前显示的任务树是否已按比例尺筛选

    //自动更新json文件中的单个质检结果
    bool updateJsonFileWithResult(int row, const QString& layerName, const QString& checkItemName, const CheckItemResult& result);
    void refreshAnnotationContentCheckResults(int row);
    int archiveErrorLayersBySeverity(int row, const QString& errorResultDir,
                                     const QHash<QString, QString>& layerSeverities,
                                     const QHash<QString, QPair<QString, QString>>& layerSources);
    // 初始化算法映射表
    void initAlgorithmMap();
    // 初始化一次正式质检的界面日志和文件日志。
    void initializeRunLog();
    // 从表格获取指定行的文件路径
    AlgorithmFilePaths getFilePathsByRow(int row);
    // 更新树形控件中质检项的状态图标
    void updateCheckItemStatusIcon(QTreeWidgetItem* checkItem, const CheckItemResult& result);

    //从json中获取比例尺数值
    int getScaleFromJson();

    // === 新增函数声明 ===
    void saveOriginalTaskTree();        // 保存任务树初始状态
    void restoreOriginalTaskTree();     // 还原任务树

    AlgorithmThread* m_algorithmThread = nullptr;
    bool m_isPaused = false;

    // ====== 新增：UI 快照结构，供工作线程使用 ======
public:
    struct LayerTaskSnapshot {
        QString layerName;
        QStringList checkItemNames; // 不含重复项
        QHash<QString, QString> checkItemSeverities; // 质检项名称 -> 高 / 中 / 低
    };

    struct MapRowSnapshot {
        AlgorithmFilePaths paths; // pdf/shp/gdb/legend/csv
        QString mapName;          // 由 pdfPath 提取
    };

private:
    QVector<LayerTaskSnapshot> m_layerTasksSnapshot; // 任务树快照
    QVector<MapRowSnapshot> m_mapRowsSnapshot;       // 表格行快照
    QString m_outputDirSnapshot;                     // 输出目录快照

    // 构建快照（必须在主线程调用）
    QVector<LayerTaskSnapshot> buildTaskSnapshot() const;
    QVector<MapRowSnapshot> buildRowSnapshot() const;

    // 通过名称在主线程更新图标（工作线程发起）。结果必须属于当前显示图幅。
    void updateCheckItemStatusIconByName(int taskRow, const QString& layerName,
                                         const QString& checkItemName,
                                         const CheckItemResult& result);
};
