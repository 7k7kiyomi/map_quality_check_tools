#pragma once

#include <QDialog>
#include <QVector>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QtGlobal>
#include <QLineEdit>
#include <QLabel>
#include <QTextEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QTreeWidget>
#include <qgsfeature.h>
#include <qgsfields.h>
#include <qgsgeometry.h>
#include <qgscoordinatereferencesystem.h>

#include <functional>

class QgisInterface;
class QGroupBox;
class SeriesSheetOverview;
struct AlgorithmFilePaths;
struct CheckItemResult;

class SeriesMapEdgeMatchTaskDialog : public QDialog
{
    friend struct SeriesMapEdgeMatchDialogTest;
public:
    explicit SeriesMapEdgeMatchTaskDialog(QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags());
    explicit SeriesMapEdgeMatchTaskDialog(QgisInterface* iface, QWidget* parent, Qt::WindowFlags f = Qt::WindowFlags());
    void setReportExportHandler(const std::function<void(const QStringList&)>& handler);

private:
    struct SheetInfo
    {
        qint64 featureId = -1;
        QString displayName;
        QgsGeometry geometry;
        QgsCoordinateReferenceSystem crs;
        QString pdfPath;
        bool previewFromExternalIndex = false;
        int scale = 0; // 比例尺分母，例如 10000, 50000
    };

    struct AdjacentPair
    {
        qint64 leftId = -1;
        qint64 rightId = -1;
        QString leftName;
        QString rightName;
        int leftScale = 0;
        int rightScale = 0;
    };

    struct CoverageGroup
    {
        qint64 smallScaleId = -1;
        QString smallScaleName;
        int smallScale = 0;
        QVector<qint64> largeScaleIds;
        QStringList largeScaleNames;
        int largeScale = 0;
        QString displayName;
    };

    struct TaskChoice {
        QString key;
        bool crossScale = false;
        int index = -1;
        bool ready = false;
    };

    enum class InputDataType {
        Unknown,
        Shapefile,
        GeoPDF
    };

    enum class IndexRelationType {
        None,
        Adjacent,
        Overlap,
        IntersectsOnly
    };

    struct PreprocessResult {
        bool success = false;
        QString pdfPath;
        QString shpDir;
        QString errorStage;
        QString logPath;
        QString errorMessage;
    };

    QString resolvePreprocessedShpDir(const QString& pdfPath, const QString& outputRootDir) const;
    PreprocessResult preprocessGeoPdfIfNeeded(const QString& pdfPath, const QString& outputRootDir);
    bool runPreprocessScript(const QString& scriptName, const QStringList& args, QString* message);
    QString getPreprocessPythonPath() const;
    QString getPreprocessScriptPath(const QString& scriptName) const;

    void buildUi();
    void browseXmlRule();
    void browseIndexFile();
    void browseOutputDirectory();
    void appendRuntimeLog(const QString& text);
    bool loadSheets(bool validateRealFrameRelations = true, QStringList* removedMainMapNames = nullptr);
    QStringList collectConfiguredGeoPdfs() const;
    bool tryUseCachedMergedIndex(const QStringList& pdfPaths, QString* indexPath);
    bool buildMergedIndexFromGeoPdfs(const QStringList& pdfPaths, QString* indexPath);
    bool saveMergedIndexCache(const QStringList& pdfPaths, const QString& indexPath, const QJsonArray& sourceSnapshot);
    QJsonArray mergedIndexSourceSnapshot(const QStringList& pdfPaths, QMap<QString, QString>* resolvedDirs = nullptr) const;
    QString preprocessedShpFingerprint(const QString& shpDir) const;
    void updateLoadRelationProgress(int value, const QString& text);
    QStringList resolveGeoPdfSearchRoots(const QString& indexPath) const;
    bool loadSheetIndex(const QString& indexPath);
    bool readSheetInfosFromIndex(const QString& indexPath, QVector<SheetInfo>* sheetInfos, QgsCoordinateReferenceSystem* crs = nullptr) const;
    void refreshSheetList();
    void clearLoadedSheetRelations();
    void autoPopulateConfigRowsFromSheetInfos(const QStringList& searchRoots);
    void updateSheetScalesFromConfigTable();
    void rebuildPairList();
    void addConfigRow();
    int addEmptyConfigRow();
    void addConfigRowsFromPaths(const QStringList& mainPdfPaths, const QStringList& assocPdfPaths = QStringList());
    void removeConfigRow();
    void browseTableMainMap(int row);
    void browseTableAssocMap(int row);
    void autoFillAssociatedMaps(int row, const QString& mainPdfPath);
    QLineEdit* configLineEdit(int row, int col) const;
    void rebindConfigTreeButtons();
    void refreshConfigTreeChildren();
    void checkTableStatus();
    void updatePairCheckStates();
    void updateSelectedCount();
    void updateOverview(bool allowPreprocess = false);
    void highlightConfigItem(QTreeWidgetItem* item);
    void locateOverviewSheet(qint64 id);
    bool eventFilter(QObject* watched, QEvent* event) override;
    void setTaskChecked(QTreeWidgetItem* item, int column);
    bool hasExternalSheetIndex() const;
    bool hasCompletedConfigRows() const;
    int scaleForGeoPdf(const QString& pdfPath) const;
    bool readPreprocessedFrameGeometry(const QString& shpDir, QgsGeometry* geometry,
        QgsCoordinateReferenceSystem* crs, QString* error) const;
    bool readGeoPdfFrameGeometry(const QString& pdfPath, QgsGeometry* geometry, QString* error);
    bool hasRealGeoPdfFrameRelation(const QString& mainPdfPath, int mainScale, const QString& assocPdfPath, int assocScale, QString* reason);
    bool hasRealGeoPdfFrameRelationFromMergedIndex(const QVector<SheetInfo>& frameSheetInfos, const QString& mainPdfPath, int mainScale, const QString& assocPdfPath, int assocScale, QString* reason) const;
    bool buildRealFrameSheetInfosForConfiguredPdfs(QVector<SheetInfo>* frameSheetInfos, QString* error);
    QStringList filterAssociatedPdfsByRealFrameRelation(
        const QString& mainPdfPath,
        const QStringList& candidateAssocPdfs,
        int mainScaleOverride = 0,
        const QMap<QString, int>& assocScaleOverrides = QMap<QString, int>(),
        const QVector<SheetInfo>* realFrameSheetInfos = nullptr);
    QStringList synchronizeConfigRowsWithRealFrameRelations();
    void selectAllPairs();
    void clearAllPairs();
    void startCheck();
    void exportErrorShp(const QVector<QJsonObject>& records = QVector<QJsonObject>(), const QString& pointFileName = QString(), const QString& lineFileName = QString());
    void exportReport();
    QStringList currentReportResultJsonPaths() const;
    QStringList configuredMainAssocPairs() const;
    QString outputRootDir() const;
    QString phaseOneOutputRootDir() const;
    QString resultOutputRootDir() const;
    QString seriesWorkDir() const;
    bool writeCurrentOutputs(const QStringList& logLines = QStringList());

    QString resolveSheetName(const QgsFeature& feature, const QgsFields& fields) const;
    double readSeriesRuleDouble(const QStringList& ruleNames, double fallback) const;
    double requiredSeriesRuleDouble(const QStringList& ruleNames, bool* ok = nullptr) const;
    QString readSeriesRuleText(const QStringList& ruleNames, const QString& fallback = QString(), int scale = 0) const;
    QStringList seriesCheckTargetLayers(const QString& xmlPath, int scale) const;
    bool confirmSeriesLayerMappings(const QVector<AdjacentPair>& selectedPairs, const QVector<CoverageGroup>& selectedCoverageGroups, const QMap<QString, QString>& pdfBaseNameToShpDir, int defaultScale, QMap<QString, QString>* confirmedLayerMappings);
    QString buildSeriesLayerMappingJson(QTreeWidget* tree, const QString& pairName) const;
    double seriesIndexAdjacencyTolerance() const;
    double seriesCrossScaleOverlapThreshold() const;
    IndexRelationType indexRelationType(const QgsGeometry& left, const QgsGeometry& right) const;
    bool evaluateConfiguredFrameRelation(const QgsGeometry& mainGeometry, int mainScale,
        const QgsGeometry& assocGeometry, int assocScale, QString* reason) const;
    bool isAdjacent(const QgsGeometry& left, const QgsGeometry& right) const;
    bool isConfiguredSheetRelationValid(const SheetInfo& mainInfo, const SheetInfo& assocInfo, int mainScale, int assocScale) const;
    bool validateSeriesCrsAndUnits(const QStringList& shpDirs, QString* error) const;
    bool applyRealFrameScope(const QStringList& leftDirs, const QStringList& rightDirs,
        AlgorithmFilePaths* paths, QJsonObject* scope, QString* error) const;
    void recordPairExecution(const QString& pairName, const CheckItemResult& result);
    QJsonObject executionSummaryForMap(const QString& mapName, const QString& layerName = QString()) const;
    QString buildTaskSummary(const QVector<AdjacentPair>& selectedPairs, const QVector<CoverageGroup>& selectedCoverageGroups) const;

private:
    QLineEdit* m_xmlEdit = nullptr;
    QLineEdit* m_indexFileEdit = nullptr;
    QLineEdit* m_outputDirEdit = nullptr;

    QTreeWidget* m_configTree = nullptr;

    SeriesSheetOverview* m_sheetOverview = nullptr;
    QVector<TaskChoice> m_taskChoices;
    QMap<QString, bool> m_pairSelection;
    bool m_refreshingConfig = false;
    bool m_loadingSheets = false;
    bool m_runningCheck = false;

    QLabel* m_taskCountLabel = nullptr;

    QProgressBar* m_progressBar = nullptr;
    QTextEdit* m_outputEdit = nullptr;

    QPushButton* m_selectAllButton = nullptr;
    QPushButton* m_clearAllButton = nullptr;
    QPushButton* m_startButton = nullptr;
    QPushButton* m_exportErrorButton = nullptr;
    QPushButton* m_exportReportButton = nullptr;
    QVector<SheetInfo> m_sheetInfos;
    QVector<SheetInfo> m_verifiedPreviewSheets;
    QVector<SheetInfo> m_displayedPreviewSheets;
    QMap<QString, SheetInfo> m_cachedPreviewSheets;
    enum class PreviewMode { Index, Configured, Selection };
    PreviewMode m_previewMode = PreviewMode::Index;
    bool m_updatingOverview = false;
    bool m_previewRelationsVerified = false;
    QVector<AdjacentPair> m_adjacentPairs;
    QVector<CoverageGroup> m_coverageGroups;
    QVector<QJsonObject> m_lastErrorRecords;
    QVector<QJsonObject> m_originalErrorRecords;
    QMap<QString, QJsonObject> m_pairRunContext;
    QMap<QString, QSet<QString>> m_checkedLayersByMap;
    std::function<void(const QStringList&)> m_reportExportHandler;
    QJsonObject m_originalStats;
    InputDataType m_inputDataType = InputDataType::Unknown;
    QgsCoordinateReferenceSystem m_sheetIndexCrs;
};
