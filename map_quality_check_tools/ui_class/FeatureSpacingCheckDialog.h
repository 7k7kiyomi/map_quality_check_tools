#pragma once

#include <QDialog>
#include <QStringList>
#include <QJsonObject>

struct FeatureSpacingRunResult
{
    bool succeeded = false;
    int problemCount = 0;
    QStringList logs;
    QStringList outputPaths;
    QJsonObject errors;
};

class QLabel;
class QgsGeometry;
class QPushButton;
class QSpinBox;
class QTextEdit;
class QWidget;

class FeatureSpacingCheckDialog final : public QDialog
{
public:
    explicit FeatureSpacingCheckDialog(QWidget* parent = nullptr);
    FeatureSpacingRunResult runConfigured(const QStringList& sourcePaths,
                                          const QStringList& associatedPaths,
                                          const QStringList& targetPaths,
                                          const QString& xmlPath, int scale,
                                          const QString& outputShpPath);

private:
    enum class CheckType
    {
        Unknown,
        Spring,
        Well,
        Village,
        River,
        RailwayTunnel,
        RoadTunnel
    };

    struct SpacingRule;
    struct RiverRule;

    QWidget* createFileRow(const QString& title, QLabel*& pathLabel, QStringList& paths,
                           const QString& dialogTitle, bool shapefile);
    QWidget* createOutputPathRow();
    void updateRunEnabled();
    void appendLog(const QString& message, QTextEdit* target = nullptr);
    void updateCheckTypeFromXml(const QString& path);
    bool readRules(const QString& path, SpacingRule& rule, QString& error,
                   bool requireBuffer = true, bool requireSymbolGroup = true) const;
    bool readTunnelRules(const QString& path, double& spacingMeters,
                         QStringList& groupingFields, QString& error) const;
    bool readRiverRules(const QString& path, RiverRule& rule, QString& error) const;
    bool makeSymbolArea(const QgsGeometry& symbolGeometry, QgsGeometry& area) const;
    void runCheck();
    void runWellCheck();
    void runVillageCheck();
    void runRiverCheck();
    void runTunnelCheck();

    QLabel* m_associatedPath = nullptr;
    QLabel* m_targetPath = nullptr;
    QLabel* m_sourcePath = nullptr;
    QLabel* m_outputPathLabel = nullptr;
    QLabel* m_xmlPath = nullptr;
    QStringList m_associatedPaths;
    QStringList m_targetPaths;
    QStringList m_sourcePaths;
    QStringList m_xmlPaths;
    QString m_outputFilePath;
    CheckType m_checkType = CheckType::Unknown;
    QSpinBox* m_scale = nullptr;
    QPushButton* m_run = nullptr;
    QTextEdit* m_log = nullptr;
    bool m_taskManagerMode = false;
    bool m_failed = false;
    bool m_completed = false;
    bool m_outputWritten = false;
    int m_problemCount = 0;
    QStringList m_runLogs;
};

// 可直接在任务入口或菜单回调中调用。
void showFeatureSpacingCheckDialog(QWidget* parent = nullptr);
