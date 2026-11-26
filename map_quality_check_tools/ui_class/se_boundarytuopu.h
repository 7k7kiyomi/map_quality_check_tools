#pragma once

#include <QtWidgets/QDialog>
#include <QByteArray>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

#include "ogrsf_frmts.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class se_boundarytuopu;
}
QT_END_NAMESPACE

// GDAL dataset deleter.
struct BoundaryTopologyDatasetCloser
{
    void operator()(GDALDataset* dataset) const
    {
        if (dataset != nullptr)
            GDALClose(dataset);
    }
};

using BoundaryTopologyDatasetPtr =
    std::unique_ptr<GDALDataset, BoundaryTopologyDatasetCloser>;

// OGR geometry deleter.
struct BoundaryTopologyGeometryDestroyer
{
    void operator()(OGRGeometry* geometry) const
    {
        if (geometry != nullptr)
            OGRGeometryFactory::destroyGeometry(geometry);
    }
};

using BoundaryTopologyGeometryPtr =
    std::unique_ptr<OGRGeometry, BoundaryTopologyGeometryDestroyer>;

// Input boundary feature and its cloned geometry.
struct BoundaryTopologyLine
{
    QString layerName;
    GIntBig fid = -1;
    OGREnvelope envelope;
    BoundaryTopologyGeometryPtr geometry;

    BoundaryTopologyLine() = default;
    BoundaryTopologyLine(BoundaryTopologyLine&&) noexcept = default;
    BoundaryTopologyLine& operator=(BoundaryTopologyLine&&) noexcept = default;
    BoundaryTopologyLine(const BoundaryTopologyLine&) = delete;
    BoundaryTopologyLine& operator=(const BoundaryTopologyLine&) = delete;
};

// A non-noded line intersection, exported as a point feature.
struct BoundaryTopologyIssue
{
    double x = 0.0;
    double y = 0.0;
    QString type;
    QString layerA;
    GIntBig fidA = -1;
    QString layerB;
    GIntBig fidB = -1;
    QString message;
};

// ui/se_boundarytuopu.ui declares the UI class as Dialog.
class se_boundarytuopu : public QDialog
{
    Q_OBJECT

public:
    explicit se_boundarytuopu(const QString& shpPath, const QString& gdbShpPath, QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
    ~se_boundarytuopu();

private slots:
    void on_pushButton_boundary_clicked();
    void on_pushButton_start_clicked();
    void on_pushButton_shuchu_clicked();

private:
    Ui::se_boundarytuopu* ui;
    QStringList m_boundaryPaths;

    QStringList m_lastCheckedPaths;
    bool m_lastCheckFinished = false;
    int m_lastProblemCount = 0;

    void appendLog(const QString& message);
    void resetCheckState(const QString& reloadMessage);

    // writeOutput false only counts; true also writes the point layer.
    bool runCheck(const QString& outputShpPath, bool writeOutput,
        int& problemCount, QString& message);

    bool collectTopologyIssues(std::vector<BoundaryTopologyIssue>& issues,
        QString& error);
    bool writeProblemLayer(const QString& outputShpPath,
        const std::vector<BoundaryTopologyIssue>& issues, QString& error) const;

    static QStringList normalizeInputPaths(const QStringList& paths);
    static QByteArray toGdalPath(const QString& path);
};
