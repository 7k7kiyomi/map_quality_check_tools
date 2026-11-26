#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <functional>

class QLineEdit;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QgsGeometry;

class SeriesCrossScaleExpressionTaskDialog : public QDialog
{
#ifdef MAP_QUALITY_ENABLE_TEST_ACCESS
    friend struct SeriesCrossScaleExpressionTaskDialogTestAccess;
#endif

public:
    explicit SeriesCrossScaleExpressionTaskDialog(QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags());
    void setReportExportHandler(const std::function<void(const QStringList&)>& handler);

private:
    struct MapTask
    {
        QString pairName;
        QString mainPdfPath;
        QString assocPdfPath;
        QString largeScaleName;
        QString smallScaleName;
        QString largeShpDir;
        QString smallShpDir;
        QJsonArray sourceAudits;
        int mainScale = 0;
        int assocScale = 0;
        int largeScale = 0;
        int smallScale = 0;
    };

    struct PreprocessResult
    {
        bool success = false;
        QString pdfPath;
        QString shpDir;
        QString errorStage;
        QString errorMessage;
    };

    struct CrossScaleRules
    {
        bool valid = false;
        QString errorMessage;
        double spaceToleranceMm = 0.0;
        double candidateSearchToleranceMm = 1.0;
        QString toleranceModel = QStringLiteral("legacy_fixed_m");
        QJsonObject toleranceProfiles;
        int candidateLimit = 0;
        bool outOfRangeError = false;
        double geomToleranceM = 0.0;
        double annotationAnchorToleranceMm = 0.5;
        double matchedThreshold = 0.0;
        double spatialWeight = 0.0;
        double geometryWeight = 0.0;
        double attributeWeight = 0.0;
        double areaBase = 0.0;
        double areaKPower = 0.0;
        double compactnessDiffMax = 0.0;
        double lengthBase = 0.0;
        double lengthKPower = 0.0;
        double directionAngleMax = 0.0;
        double uniqueScoreMargin = 0.10;
        double lineJoinToleranceM = 5.0;
        double lineJoinAngleMax = 75.0;
        double maxUncoveredSegmentLengthMm = -1.0;
        double geomSimThreshold = 0.0;
        double attrSimThreshold = 0.0;
        bool polygonAggregationAllowed = false;
        double maxUncoveredSegmentLength = -1.0;
        QString outputSeverity = QStringLiteral("预警");
        QMap<QString, QString> semanticClassAliases;
        QStringList stableIdentityFields;
        QSet<QString> lineAggregationClasses;
        QSet<QString> pointSymbolClasses;
        QSet<QString> polygonAggregationClasses;
        QSet<QString> strictMissingSupportClasses;
        QSet<QString> mandatoryRetentionClasses;
        QSet<QString> geometryTransitions;
    };

    void buildUi();
    void browseXmlRule();
    void browseOutputDirectory();
    void addConfigRow();
    void addConfigRowsFromPaths(const QStringList& mainPdfPaths, const QStringList& assocPdfPaths = QStringList());
    int addEmptyConfigRow();
    void removeConfigRow();
    void browseTableMainMap(int row);
    void browseTableAssocMap(int row);
    void rebindConfigTreeButtons();
    QLineEdit* configLineEdit(int row, int col) const;
    void autoFillRowScales(int row);
    int scaleForGeoPdf(const QString& pdfPath) const;
    void checkTableStatus();
    void refreshConfigTreeChildren();
    QVector<MapTask> buildConfiguredTasks() const;
    void startCheck();
    void exportReport();
    void appendRuntimeLog(const QString& text);

    QString outputRootDir() const;
    QStringList currentReportResultJsonPaths() const;
    QString getPreprocessPythonPath() const;
    QString getPreprocessScriptPath(const QString& scriptName) const;
    bool runPreprocessScript(const QString& scriptName, const QStringList& args, QString* message);
    QString resolvePreprocessedShpDir(const QString& pdfPath, const QString& outputRootDir) const;
    PreprocessResult preprocessGeoPdfIfNeeded(const QString& pdfPath, const QString& outputRootDir);
    bool prepareTaskShpDirs(QVector<MapTask>* tasks);

    CrossScaleRules readCrossScaleRules(const QString& xmlPath, int scale = 0) const;
    QSet<QString> readExcludedLayerKeys(const QString& xmlPath, int scale = 0) const;
    QMap<QString, QString> readLayerAliases(const QString& xmlPath, int scale) const;
    bool confirmLayerMappings(const QVector<MapTask>& tasks, QMap<QString, QJsonObject>* confirmedLayerMappings);
    QJsonObject buildActualShpLayerMappingForTask(const MapTask& task, const QMap<QString, QString>& aliases, const QSet<QString>& excludedLayerKeys) const;

    QJsonObject runCrossScaleExpressionCheck(const MapTask& task, const QJsonObject& layerMapping) const;
    QJsonObject runLayerCorrespondenceCheck(const MapTask& task, const QString& target, const QStringList& smallPaths, const QStringList& largePaths, const CrossScaleRules& rules, const QgsGeometry& largeCoverage, const QJsonObject& confirmedLayer = QJsonObject(), const QString& checkCrsWkt = QString()) const;
    QVector<QJsonObject> buildOutputRecords(const QVector<QJsonObject>& taskResults, QMap<QString, QSet<QString>>* checkedLayersByMap) const;
    bool writeOutputs(const QVector<QJsonObject>& taskResults, QString* errorDetails = nullptr) const;

private:
    QLineEdit* m_xmlEdit = nullptr;
    QLineEdit* m_outputDirEdit = nullptr;
    QTreeWidget* m_configTree = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QPushButton* m_startButton = nullptr;
    QPushButton* m_reportExportButton = nullptr;
    QStringList m_runtimeLogLines;
    std::function<void(const QStringList&)> m_reportExportHandler;
};


