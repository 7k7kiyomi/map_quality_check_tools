#ifndef SE_PREPROCESS_DIALOG_H
#define SE_PREPROCESS_DIALOG_H

#include <QDialog>
#include <QStringList>
#include <QProcess>
#include <QList>
#include <QComboBox>
#include "ui_preprocess_dialog.h"

enum class SourceDataType
{
    None,
    LocalGdb,
    Postgis
};

// PostGIS 数据源只从主题体系中取数：可处理一个主题的全部内容，或其中一个内容。
enum class PostgisThemeScope
{
    Theme,
    ThemeContent
};

struct PostgisLayerInfo
{
    QString tableName;
    QString geometryColumn;
};

struct PostgisSourceSettings
{
    QString host;
    int port = 5432;
    QString database;
    QString schema = QStringLiteral("public");
    QString user;
    QString password; // 仅保存在当前运行内存中，默认不写入 QSettings。
    PostgisThemeScope themeScope = PostgisThemeScope::Theme;
    qint64 themeId = 0;
    QString themeName;
    qint64 themeContentId = 0;
    QString themeContentName;
    QList<PostgisLayerInfo> layers;
};

// 预处理参数结构
struct PreprocessParams
{
    // GeoPDF设置
    QStringList pdfPaths;
    QString pdfOutputPath;

    // GDB设置
    QString sourceGdbPath;
    SourceDataType sourceDataType = SourceDataType::None;
    PostgisSourceSettings postgisSource;

    // 符号库设置
    QStringList symbolPdfPaths;
    QString symbolOutputPath;
    bool symbolFromShp = false;
    QString symbolShpDir;
};

class CSE_PreprocessDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CSE_PreprocessDialog(QWidget *parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
    ~CSE_PreprocessDialog() override;

    PreprocessParams getPreprocessParams() const;
    void setPreprocessParams(const PreprocessParams &params);

private slots:
    // PDF相关槽函数
    void onAddPDFClicked();
    void onRemovePDFClicked();
    void onClearPDFClicked();
    void onBrowsePDFOutputClicked();

    // GDB相关槽函数
    void onAddGDBClicked();
    void onSourceDataTypeChanged();
    void onConnectPostgisClicked();

    // 符号库相关槽函数
    void onAddSymbolPDFClicked();
    void onRemoveSymbolPDFClicked();
    void onClearSymbolPDFClicked();
    void onBrowseSymbolOutputClicked();

    // 处理控制槽函数
    void onStartProcessClicked();
    void onCancelClicked();
    void onCloseClicked();

    // 进程处理槽函数
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessReadyReadStandardOutput();
    void onProcessReadyReadStandardError();

    // 将 processNextStep 移到 slots 中，使其可以被 QMetaObject::invokeMethod 调用
    void processNextStep();

private:
    void connectSignals();
    bool validateInputParams();
    void updateUIState(bool isRunning);
    void appendLog(const QString &message);
    void startPythonScript(const QString &scriptPath, const QStringList &arguments);
    QString getQgisPythonPath();
    QString getStandardLegendPythonPath();
    QString getScriptFullPath(const QString &scriptName);

    // 地图预处理方法
    void executeLaunchAIForSinglePDF(int pdfIndex);
    void executeGeoJSON2Shp(int pdfIndex);
    void executeGDB2Shp(int pdfIndex);
    void executePostgis2Shp(int pdfIndex);
    bool copyGeoPdfToOutputRoot(int pdfIndex);

    SourceDataType currentSourceDataType() const;
    bool hasSourceData() const;
    int mapStepsPerPdf() const;
    void updateSourceDataUi();
    QString postgisLayersDisplayText() const;
    bool loadPostgisSpatialLayers(QString* errorMessage = nullptr);
    bool clipPostgisLayersByFrame(const QString& frameShpPath, const QString& outputRoot,
                                  const QString& pdfBaseName, QString* errorMessage);
    void completeInternalSourceStep(bool success, const QString& errorMessage = QString());

    // 符号库预处理方法
    void executeSymbolLaunchAI(int symbolIndex);
    void executeSymbolGeoJSON2Shp(int symbolIndex);
    void executeSymbolLegendMatch(int symbolIndex);
    void executeStandardLegendPdfPreprocess();

    void restoreState();
    void saveState();
    bool isProcessingSymbols() const;

private:
    Ui::PreprocessDialog ui;
    QProcess *m_pCurrentProcess;
    PreprocessParams m_params;
    int m_currentStep;
    int m_totalSteps;
    bool m_isSymbolProcessing;
    bool m_cancelRequested = false;
    int m_failedSteps;           // 新增：失败步骤计数
    QStringList m_errorMessages; // 新增：错误信息列表
    PostgisSourceSettings m_postgisSource;
    bool m_rememberPostgisPassword = false;
    QList<PostgisLayerInfo> m_availablePostgisLayers;
    QComboBox *m_symbolInputMode = nullptr;
    QLineEdit *m_symbolShpDir = nullptr;
    QPushButton *m_browseSymbolShp = nullptr;
};

#endif // SE_PREPROCESS_DIALOG_H
