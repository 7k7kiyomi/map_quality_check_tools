#ifndef ANGLECHECKDIALOG_H
#define ANGLECHECKDIALOG_H

#include <QObject>
#include <QSet>
#include <QTextStream>
#include <QFile>
#include <QDateTime>
#include <vector>
#include <map>
#include <memory>

#include <qgsvectorlayer.h>
#include <qgsgeometry.h>
#include <qgspointxy.h>
#include <qgswkbtypes.h>
#include <qgsvectorfilewriter.h>
#include <qgspolygon.h>
#include <qgslinestring.h>
#include <qgscurve.h>
#include <qgsfields.h>
#include <qgsfeature.h>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransformcontext.h>
#include <qgsmultipolygon.h>
#include <qgsmultilinestring.h>
#include "JsonLogger.h"
#include "GeometryValidator.h"
#include "XmlConfigReader.h"

enum class ErrorType {
    SharpAngle,
    InvalidGeometry,
    InvalidGeometrySkip
};

struct AngleError {
    QgsFeatureId featureId;
    int partIndex;
    int ringIndex;
    int vertexIndex;
    QgsPointXY vertexPosition;
    double angle;
    ErrorType errorType;
    QDateTime detectionTime;
    bool isFromInvalidGeometry;
    QString detectionSource;
    QString errorSubType;
};

struct FeatureErrorSummary
{
    QgsFeatureId featureId;
    int errorCount;
    int sharpAngleCount;
    int invalidGeometryCount;
    double minAngle;
    double maxAngle;
    double thresholdAngle;
    std::vector<AngleError> errors;

    FeatureErrorSummary() : featureId(-1), errorCount(0), sharpAngleCount(0),
        invalidGeometryCount(0), minAngle(180.0), maxAngle(0.0), thresholdAngle(0.0) {}
};

class AngleCheckDialog : public QObject
{
    Q_OBJECT

public:
    // 原110内部的属性筛选和源线缓冲定位，不另设算法文件。
    static constexpr double SOURCE_BUFFER_MM = 2.0;
    static constexpr double MIN_SOURCE_COVERAGE = 0.8;
    struct SourceSelection {
        bool enabled=false;
        QSet<long long> parsedIds;
        int selectedSources=0,matchedSources=0,ambiguousParsed=0;
    };
    static SourceSelection selectSourceFeatures(const QString& parsedPath,
        const QString& sourcePaths,const QString& xmlPath,int scale,
        double bufferMm=SOURCE_BUFFER_MM);

    explicit AngleCheckDialog(QObject* parent = nullptr);
    ~AngleCheckDialog();

    // 使用新接口：直接传递 JSON 路径
    bool executeSharpAngleCheck(
        QgsVectorLayer* layer,
        const QString& xmlConfigPath,
        const QString& logFilePath,
        const QString& outputErrorShpPath,
        const QString& outputJsonPath,
        int scale,
        GeometryValidator* geometryValidator,
        const QgsFeatureIds* checkedIds = nullptr,
        const QString& outputPointPath = QString());

    static bool executeSharpAngleCheckStatic(
        QgsVectorLayer* layer,
        const QString& xmlConfigPath,
        const QString& logFilePath,
        const QString& outputErrorShpPath,
        const QString& outputJsonPath,
        int scale,
        GeometryValidator* geometryValidator,
        const QgsFeatureIds* checkedIds = nullptr,
        const QString& outputPointPath = QString());

    std::vector<AngleError> checkSharpAngles(QgsVectorLayer* layer, double minAngle, double maxAngle);

private:
    const QgsFeatureIds* m_checkedIds = nullptr;
    static bool createAnglePoints(QgsVectorLayer* layer,const QString& path,
        const std::vector<AngleError>& errors,double threshold);
    static constexpr double EPS = 1e-6;

    static double calculateAngle(const QgsPointXY& pPrev, const QgsPointXY& pCurr, const QgsPointXY& pNext, bool isPolygon);
    static bool isContinuousSegment(const QVector<QgsPointXY>& points, int centerIndex);
    static QString pointToString(const QgsPointXY& point);

    static void processPolygonGeometry(
        const QgsPolygon* polygon,
        const QgsFeature& feature,
        int partIdx,
        double minAngle,
        double maxAngle,
        std::vector<AngleError>& errors);

    static void processLineStringGeometry(
        const QgsLineString* line,
        const QgsFeature& feature,
        int partIdx,
        double minAngle,
        double maxAngle,
        std::vector<AngleError>& errors);

    static void processPointsForAngles(
        const QVector<QgsPointXY>& points,
        const QgsFeature& feature,
        int partIdx,
        int ringIdx,
        double minAngle,
        double maxAngle,
        std::vector<AngleError>& errors,
        const QString& geometryType,
        bool isPolygon,
        bool isClockwise = false);

    static void detectAnglesInValidGeometry(const QgsGeometry& geometry, const QgsFeature& feature,
        double minAngle, double maxAngle, std::vector<AngleError>& errors);

    static bool createErrorShapefile(
        QgsVectorLayer* sourceLayer,
        const QString& outputPath,
        const std::vector<FeatureErrorSummary>& summaries);

    QString generateInvalidGeometryOutputPath(const QString& originalErrorShpPath, const QString& layerName);

    void writeLogFile(const QString& logPath, const QString& message);
};

#endif // ANGLECHECKDIALOG_H
