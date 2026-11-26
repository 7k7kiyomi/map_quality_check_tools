#ifndef READSHP_H
#define READSHP_H

#include <QtWidgets/QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTableWidget>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QHeaderView>
#include <QTextEdit>
#include <QSplitter>
#include <QGroupBox>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QDebug>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QTabWidget>
#include <QListWidget>
#include <QObject> 
// QGIS ���ͷ�ļ�
#include <qgsvectorlayer.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgslinestring.h>
#include <qgsmultilinestring.h>
#include <qgspoint.h>
#include <qgswkbtypes.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgssymbol.h>
#include <qgsrenderer.h>
#include <qgssingleSymbolrenderer.h>
#include <qgsmarkersymbol.h>
#include <qgslinesymbol.h>
#include <qgsrubberband.h>
#include <qgsmapcanvasitem.h>
#include <qgsgeometryutils.h>
#include <qgsdistancearea.h>
#include <vector>
#include <cmath>
// XML �ļ�����
#include "QualityCheckConfig.h"
#include <QDomDocument>
#include <cmath>
#include <stdexcept>
#include <limits>
#include <set>
#include <map>
#include <QLineEdit>
#include <QIntValidator>
// д�� JSON �ļ�
#include <QJsonObject>


// �ʼ�����ö��
enum class CheckType {
    DIRECTION_ERROR,              // ���������
    POLYGON_TOPOLOGY_ERROR,       // ��Ҫ���ص�/��϶���˴���
    LINE_TOPOLOGY_ERROR,          // ��Ҫ�����˴������ӡ�������
    NETWORK_CONNECTIVITY_ERROR,   // ������ͨ�Դ���
    RIGHT_ANGLE_THRESHOLD_ERROR   // ֱ�ǻ��Ƕ���ֵ����
};

// �߶���Ϣ
struct LineSegmentInfo {
    QgsPointXY startPoint;
    QgsPointXY endPoint;
    double azimuth;  // ��λ�ǣ�0-360�㣩
    int segmentIndex;
    double length;
};

// ˮϵ������Ϣ
struct WaterSystemIntersection {
    QgsPointXY intersectionPoint;
    double inflowAngle;        // ��ˮ�ǣ���Է���
    QgsFeatureId featureId;
    bool isValid;
};

// �ӿ���Ϣ
struct RiverWidthInfo {
    double width;
    double distance;  // �غ��ۼƾ���
    int segmentIndex;
};

// �ڲ�������Ϣ
struct IntersectionAngleInfo {
    QgsPointXY intersectionPoint;
    double intersectionAngle;  // Ŀ�꼸���ڲ����߶��γɵĽǶ�
    QgsFeatureId featureId1;   // ��һ���ཻ�ߵ�Ҫ��ID
    QgsFeatureId featureId2;   // �ڶ����ཻ�ߵ�Ҫ��ID
    bool isValid;
};

// �Ӷ���Ϣ
struct RiverSegmentInfo {
    QgsFeature feature;
    QgsGeometry geometry;

    double length;
    QgsPoint startPoint;
    QgsPoint endPoint;
};

// �ʼ����ṹ
struct QualityCheckResult {
    bool passed;                 // �Ƿ�ͨ��
    QString errorType;           // ������������
    QString description;         // ��������
    QgsFeature feature;          // ���Ҫ��
    QgsGeometry errorGeometry;   // ����λ�ü��Σ����ڿ��ӻ���
    double calculatedValue;      // ʵ��ֵ
    double standardValue;        // ��׼/��ֵ
    int pointIndex;              // ��ص���������ǵ��±꣩
    QString sourceFile;          // ��Դ�ļ���Ϣ��������Դ��
    CheckType checkType;         // �������ö��
    qint64 featureId;            // Ҫ��ID
    QgsGeometry geometry;        // Ҫ�ؼ���
    QString severity;            // ���س̶�
    QString referenceCategory;
    QString referenceValue;
    QString referenceSource;
    qint64 referenceFeatureId = -1;
};

// ֱ�ǻ��ǵ���Ϣ
// Merge repeated line-topology records for the same source feature; returns the removed row count.
int mergeDuplicateLineTopologyErrorRecords(std::vector<QualityCheckResult>& results);

struct RightAnglePoint {
    QgsPointXY position;   // �ǵ�����
    double angle;          // �Ƕ�ֵ
    int segmentIndex1;     // �ǵ�ǰһ������
    int segmentIndex2;     // �ǵ��һ������
    bool isValid;          // �Ƿ���Ч�ǵ�
};

// �����ļ������ݿ��ļ�ӳ��
struct DatabaseFileMapping {
    QString parseFileName;
    QString databaseFileName;
    QString featureType;
};

// �ڽ��ǵ���ҽ��
struct NearCornerResult {
    bool found;
    QgsPointXY corner;
    double angle;
};

// �ǵ��ڽӹ�ϵ
struct CornerNeighbors {
    const QgsLineString* line;
    int prevIndex;
    int cornerIndex;
    int nextIndex;
    bool valid;
};

// ��׼ͼ��ɫö��
enum class StandardRole {
    RiverLine,        // ������
    RoadLine,         // ��·��
    WaterAreaPolygon, // ˮ����
    HydroPoint,       // ˮ����
    Unknown           // δ֪
};

struct WaterMatchCacheEntry;
class ReadShpDialog : public QDialog
{
    Q_OBJECT
public:
    ReadShpDialog(QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
    ~ReadShpDialog();

   
    // =========================��׼ͼ��csv���� =========================
    // ��׼ͼ�㣨CSV��ѡ���� UI
    QGroupBox* standardLayerGroupBox;
    QPushButton* selectCsvButton;
    QLabel* csvPathLabel;
    QComboBox* standardLayerCombo;

    // CSV ӳ����Ŀ
    struct StandardLayerEntry {
        QString standardLayerName;
        QString gdbShpPath;
        QString actualShpPath;
        QString actualAnnoLayerPath;
        QString standardAnnoLayerName;
        QString labelField;
        QString layerSubAttribute;
    };
    QVector<StandardLayerEntry> m_standardLayers;
    QString m_standardCsvFilePath;

    // CSV ������Ӧ��
    bool loadStandardLayersFromCsv(const QString& csvPath);
    void applyStandardLayerEntry(const StandardLayerEntry& entry);
    // ������/���Ž��� .shp ·����isGdb=true �����Ŀ¼��false �����Ŀ¼��
    QString resolveShpPath(const QString& token, bool isGdb);
    QStringList candidateSearchDirs(bool isGdb) const;
    // ========================= UI ������ؼ���Ա�����棩 =========================
    QVBoxLayout* mainLayout;
    QHBoxLayout* buttonLayout;
    QHBoxLayout* topLayout;
    QVBoxLayout* leftLayout;
    QVBoxLayout* rightLayout;

    // �����ļ�ѡ�����б�
    QGroupBox* batchFilesGroupBox;
    QGroupBox* parseFilesGroupBox;
    QGroupBox* referenceFilesGroupBox;
    QListWidget* parseFilesList;
    QListWidget* referenceFilesList;
    QPushButton* addParseFilesButton;
    QPushButton* addReferenceFilesButton;
    QPushButton* removeSelectedParseFileButton;
    QPushButton* removeSelectedReferenceFileButton;
    QPushButton* clearAllParseFilesButton;
    QPushButton* clearAllReferenceFilesButton;
    QLabel* parseFilesCountLabel;
    QLabel* referenceFilesCountLabel;

    // �ļ�ѡ��ť
    QPushButton* selectFileButton;          // ѡ�����SHP�ļ�
    QPushButton* selectReferenceFileButton; // ѡ��ο�SHP�ļ�
    QPushButton* outputButton;
    QPushButton* performCheckButton;

    // �ļ�·����ǩ
    QLabel* currentFileLabel;     // ��ǰ�ļ�·��
    QLabel* referenceFileLabel;   // �ο��ļ�·��
    QLabel* mapLabel;
    // �����������
    QLabel* scaleLabel;
    QComboBox* scaleComboBox;
    // ��������־���
    QTableWidget* tableWidget;
    QTextEdit* logTextEdit;
    QSplitter* splitter;
    QSplitter* mainSplitter;
    QGroupBox* logGroupBox;
    QGroupBox* mapGroupBox;
    QComboBox* outputFormatCombo;
    QGroupBox* tableGroupBox;

    // ���ѡ����أ�
    QGroupBox* checkOptionsGroupBox;
    QCheckBox* directionErrorCheckBox;      // �������
    QCheckBox* polygonOverlapGapCheckBox;   // ��Ҫ���ص�/��϶
    QCheckBox* lineTopologyCheckBox;        // ��Ҫ������
    QCheckBox* rightAngleCheckBox;          // ֱ�ǻ��Ƕ���ֵ
    QCheckBox* onlyErrorsCheckBox;          // ����ʾ����

    // ��־��ǩҳ��������ϸ
    QTabWidget* logTabWidget;
    QTextEdit* xmlDebugTextEdit;
    QString getDetailedCoordinateInfo(const QualityCheckResult& result);

    // ��ͼ��ʾ���
    QgsMapCanvas* mapCanvas;
    QWidget* mapWidget;
    QVBoxLayout* mapLayout;

    // �����Ǽ���
    std::vector<QgsRubberBand*> errorMarkers;

    // ͼ�����ļ�����
    QgsVectorLayer* currentLayer;           // ��ǰͼ��
    QgsVectorLayer* referenceLayer;         // �ο�ͼ��
    QString currentFilePath;                // ��ǰ�ļ�·��
    QString referenceFilePath;              // �ο��ļ�·��
    QStringList parseFilePaths;             // �����ļ�·���б�
    QStringList referenceFilePaths;         // �ο��ļ�·���б�
    QList<QgsMapLayer*> allDisplayLayers;   // ��������ʾͼ��
    QgsVectorLayer* errorLayer;             // ����Ҫ��ͼ��
    std::vector<QgsVectorLayer*> parseLayers;      // ����ͼ���б�
    std::vector<QgsVectorLayer*> referenceLayers;  // �ο�ͼ���б�
    std::vector<QualityCheckResult> lastCheckResults;

    // ���󼸺ι��죨ͨ�ã�
    QgsGeometry createSegmentErrorGeometry(const QgsLineString* line, int startIndex, int endIndex);
    // �ռ���Ч���Σ���/���ཻ�ȣ�����׷�ӵ������������ true ��ʾ������Ч���Σ�����ǰ��ֹ��飩
    bool collectInvalidGeometry(QgsVectorLayer* layer, const QString& sourceFile, CheckType checkType, std::vector<QualityCheckResult>& results);

    // ========================= ˮϵ���������������Ӻ����� =========================
    std::vector<QualityCheckResult> checkWaterSystemDirection(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile);
    std::vector<WaterSystemIntersection> findWaterSystemIntersections(const QgsGeometry& waterGeom, QgsVectorLayer* referenceLayer, double tolerance);
    double calculateInflowAngle(const QgsGeometry& waterGeom, const QgsPointXY& intersectionPoint);
    bool isHangingWaterFeature(const QgsGeometry& waterGeom, QgsVectorLayer* layer, double tolerance);
    bool hasIntersectingWaterFeatures(const QgsGeometry& waterGeom, QgsVectorLayer* layer, double tolerance);
    std::vector<RiverWidthInfo> extractRiverWidthInfo(const QgsFeature& feature);
    bool checkRiverWidthProgression(const std::vector<RiverWidthInfo>& widthInfo);
    QgsGeometry createWaterDirectionErrorGeometry(const QgsPointXY& intersectionPoint, double bufferRadius = 5.0);
    std::vector<IntersectionAngleInfo> findInternalIntersectionAngles(const QgsGeometry& targetGeom, QgsVectorLayer* layer, double tolerance);

    // ���� ����һ�����뼸�η��������ɸ��õ�����/�����ˣ� ����
    double calculateIntersectionAngle(const QgsGeometry& line1, const QgsGeometry& line2, const QgsPointXY& intersectionPoint);
    std::vector<QgsFeature> getIntersectingFeatures(const QgsFeature& targetFeature, QgsVectorLayer* layer);
    double calculateTopologyConsistency(const QgsFeature& candidateFeature, QgsVectorLayer* referenceLayer, const std::vector<QgsFeature>& parsedIntersectingFeatures);
    double calculateIntersectionAngleSimilarity(const QgsFeature& candidateFeature, const std::vector<QgsFeature>& candidateIntersections, const std::vector<QgsFeature>& parsedIntersections);
    std::vector<QgsPointXY> findIntersectionPoints(const QgsGeometry& geom1, const QgsGeometry& geom2);
    QgsVector getLineDirectionAtPoint(const QgsLineString* line, const QgsPointXY& point);

    // ========================= ��Ҫ�أ��ص����϶��� =========================
    std::vector<QualityCheckResult> checkPolygonOverlapGap(QgsVectorLayer* parsedLayer, const QString& sourceFile);
   
    bool isPolygonOverlapping(const QgsGeometry& geom1, const QgsGeometry& geom2); // ���ص�
    bool hasAdministrativeGap(const QgsGeometry& geom1, const QgsGeometry& geom2); // ���϶

    // ========================= ��·�ߣ�������������ͨ��� =========================
    // ���� �ļ�����ʶ����Ҫ�����͸��� ����
    bool isLRDLFile(const QString& fileName);
    bool isRoadFile(const QString& fileName);
    int getRoadTypeFromFileName(const QString& fileName);
    int getRoadTypeFromAttribute(const QgsFeature& feature);
    int resolveConfiguredReferenceRoadType(QgsVectorLayer* referenceLayer);


    // ���� ��·�����˼�飨����/����/�������ˣ� ����
    std::vector<QualityCheckResult> checkRoadLineTopology(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile);
    std::vector<QualityCheckResult> checkRoadDangling(QgsVectorLayer* parsedLayer, const QString& sourceFile);
    std::vector<QualityCheckResult> checkRoadIsolation(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile, const std::vector<QgsVectorLayer*>& roadNetworkLayers);

    // ���� ��·��������ͨ�Լ�飨��ͼ�����磩 ����
    std::vector<QualityCheckResult> checkRoadNetworkConnection(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile);
    bool checkRoadConnectionValidity(const QgsGeometry& roadGeom, int roadType, const std::map<int, std::vector<QgsFeature>>& referenceRoadsByType);
    bool getRoadEndpoints(const QgsGeometry& roadGeom, QgsPointXY& startPoint, QgsPointXY& endPoint);
    bool isValidRoadConnection(int roadType1, int roadType2);
    QgsFeature findBestMatchingFeature(const QgsFeature& parsedFeature, QgsVectorLayer* referenceLayer);
    double calculateLineSimilarity(const QgsGeometry& geom1, const QgsGeometry& geom2);

    // ���� ��Ҫ���ڲ������ж���ͬͼ�㣩 ����
    bool isDanglingLineInternal(const QgsGeometry& lineGeom, QgsVectorLayer* parsedLayer);
    bool isIsolatedLineInternal(const QgsGeometry& lineGeom, QgsVectorLayer* parsedLayer, const std::vector<QgsVectorLayer*>& roadNetworkLayers);
    double calculateMinDistanceToOtherLines(const QgsGeometry& lineGeom, QgsVectorLayer* layer, QgsFeatureId excludeId);

    // ���� ��·�ȼ�ʶ���ļ���/����/�ۺϣ� ����
    int getRoadLevelFromFileName(const QString& sourceFile);
    int getRoadLevelFromAttribute(const QgsFeature& feature);
    int getRoadLevel(const QgsFeature& feature, const QString& sourceFile = QString(), bool isReferenceLayer = false);

 
    // ���� ��·���ӹ�ϵ������˵������ ����
    QString analyzeRoadConnectionDetails(const QgsGeometry& roadGeom, int roadType, const std::map<int, std::vector<QgsFeature>>& referenceRoadsByType, const QgsPointXY& startPoint, const QgsPointXY& endPoint);
    QString analyzePointConnection(const QgsPointXY& point, int roadType, const std::map<int, std::vector<QgsFeature>>& referenceRoadsByType, double tolerance, const QString& pointName);
    QString getRoadTypeName(int roadType);
    QString getValidConnectionTypes(int roadType);
    bool isPointDanglingForRoadNetwork(const QgsPointXY& point, int roadType, const std::map<int, std::vector<QgsFeature>>& referenceRoadsByType, double tolerance);
    bool isPointDisconnectedFromAnyRoad(const QgsPointXY& point, const std::map<int, std::vector<QgsFeature>>& referenceRoadsByType, double tolerance);

    // ========================= ˮϵ�ļ�ʶ�𣨺�/ˮ��/ˮ�ߣ� =========================
    bool isRiverFile(const QString& fileName);
    bool isWaterAreaFile(const QString& fileName);
    bool isWaterLineFile(const QString& fileName);

    // ========================= �����ߣ����˼�飨����/������ =========================
    std::vector<QualityCheckResult> checkRiverLineTopology(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile);
    std::vector<QualityCheckResult> checkRiverDangling(QgsVectorLayer* parsedLayer, const QString& sourceFile);
    std::vector<QualityCheckResult> checkRiverIsolation(QgsVectorLayer* parsedLayer, const QString& sourceFile);

    // ========================= ˮϵҪ�����ӹ��ˣ������� =========================
    std::vector<QgsFeature> collectWaterSystemFeatures();
    std::vector<QualityCheckResult> filterConnectedToWaterSystemStrict(const std::vector<QualityCheckResult>& problemResults, const std::vector<QgsFeature>& waterSystemFeatures);

    // ========================= ����ͨ������ռ��ϵ��� =========================
    std::vector<QualityCheckResult> checkRiverNetworkConnectivity(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile);
    std::vector<QualityCheckResult> checkRiverSpatialRelations(QgsVectorLayer* riverLayer, const std::vector<QgsFeature>& waterSystemFeatures, const QString& sourceFile);
  
    // ���� ��Ҫ�ز�����/�ȶԣ������� ����
    std::vector<QualityCheckResult> checkSingleRiverSpatialRelations(const QgsFeature& riverFeature, const std::vector<QgsFeature>& waterSystemFeatures, const QString& sourceFile);
    std::vector<QualityCheckResult> compareSingleFeatureWithDatabase(const QgsFeature& parsedFeature, QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile, QHash<QgsFeatureId, WaterMatchCacheEntry>* cache = nullptr);

    // ���� �ռ��ϵ�ж����ص�/�ཻ/������ ����
    bool checkOverlapRelation(const QgsGeometry& riverGeom, const QgsGeometry& waterFeatureGeom, double tolerance);
    bool checkIntersectionRelation(const QgsGeometry& riverGeom, const QgsGeometry& waterFeatureGeom, double tolerance);
    bool checkExceedsRelation(const QgsGeometry& riverGeom, const QgsGeometry& waterFeatureGeom, double tolerance);

    // ���� ˮ��Ҫ���ļ�����ʶ�� ����
    bool isWaterReservoirFile(const QString& fileName);
    bool isDamFile(const QString& fileName);
    bool isWellFile(const QString& fileName);
    bool isSpringFile(const QString& fileName);

    // ���� ���ݿ�Ҫ��ƥ����ռ��ϵ�Ա� ����
    bool checkRiverExceedsWaterArea(const QgsGeometry& riverGeom, const QgsGeometry& waterAreaGeom, double tolerance);
    bool checkRiverDamRelation(const QgsGeometry& riverGeom, const QgsGeometry& damGeom, double tolerance);
    bool checkRiverExceedsLineFeature(const QgsGeometry& riverGeom, const QgsGeometry& lineFeatureGeom, double tolerance);
    QString getWaterFeatureType(const QgsFeature& feature, const QString& fileName);
   
    // ========================= ����/ͨ�ù��ߺ��� =========================
    std::vector<const QgsLineString*> extractLineStrings(const QgsGeometry& geom);
    const QgsLineString* getFirstLineString(const QgsGeometry& geom);
    bool isShpLayer(QgsVectorLayer* layer);

    // ========================= ֱ�ǻ��Ƕ���ֵ��飨����ڣ� =========================
    QList<QualityCheckResult> allResults;
    QString checkItemName;
    std::vector<QualityCheckResult> checkRightAngleThreshold(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile);
 

    // ���� ֱ�ǻ��ǵ���ȡ��ƥ����һ�������� ����
    std::vector<RightAnglePoint> extractRightAnglePoints(const QgsGeometry& geometry, double minAngle, double maxAngle, double minSegmentLength = 0.0);
    QgsFeature findMatchingDatabaseFeatureForRightAngle(const QgsFeature& parsedFeature, QgsVectorLayer* parsedLayer, QgsVectorLayer* databaseLayer, double tolerance, double similarityThreshold);  std::vector<RightAnglePoint> checkRightAnglePointsConsistency(const std::vector<RightAnglePoint>& parsedPoints, const std::vector<RightAnglePoint>& databasePoints, double tolerance);
    QgsPointXY findCorrespondingPointImproved(const QgsGeometry& parsedGeom, const QgsGeometry& databaseGeom, const QgsPointXY& databasePoint, double tolerance);
    QgsPointXY getClosestPointOnSegment(const QgsPointXY& point, const QgsPointXY& segStart, const QgsPointXY& segEnd);
    double calculateAngleAtPointImproved(const QgsGeometry& geometry, const QgsPointXY& point, double searchRadius = 1.0);
    CornerNeighbors findCornerNeighbors(const QgsGeometry& parsedGeom, const QgsPointXY& corner, double searchRadius);
    double calculateRightAngleSimilarity(const QgsGeometry& geom1, const QgsGeometry& geom2, double tolerance);
    void analyzeDetailedSimilarity(const QgsGeometry& geom1, const QgsGeometry& geom2, double tolerance, qint64 candidateId);
    QgsGeometry createRightAngleErrorGeometry(const QgsGeometry& parsedGeometry, const QgsPointXY& errorPoint, double tolerance);
    QgsPointXY findCorrespondingPoint(const QgsGeometry& parsedGeom, const QgsGeometry& databaseGeom, const QgsPointXY& databasePoint, double tolerance);
    double calculateAngleAtPoint(const QgsGeometry& geometry, const QgsPointXY& point);
    double calculateVertexAngle(const QgsPointXY& p1, const QgsPointXY& vertex, const QgsPointXY& p2);
    bool isAngleInRange(double angle, double minAngle, double maxAngle);

    // ========================= ��ͼ��ʾ������� =========================
    void initializeMapCanvas();
    void loadLayerToMap(QgsVectorLayer* layer);
    void clearErrorMarkers();
    void addErrorMarkersToMap(const std::vector<QualityCheckResult>& results);
    void zoomToLayer();
    void zoomToErrors();

    // ========================= ��־����븨�� =========================
    void logCoordinateInfo(const QString& functionName, const QgsLineString* line);
    void logPointInfo(const QString& context, const QgsPoint& point, int index = -1);
    void logDirectionInfo(const QString& context, double direction, const QgsPoint& p1, const QgsPoint& p2);

    // ========================= XML ������������־ =========================
    void logXMLDebugInfo(const QString& message);
    void displayXMLConfigInfo();
    void logCheckItemStart(const QString& checkType);
    void logCheckItemDetails(const QString& checkType);

    // ========================= ��������ֵ/�ݲ���Ż����� =========================
    double m_distanceTolerance;                    // �����ݲ�
    double m_directionDatabaseMatchTolerance;       // ���������ݿ�ƥ���ݲ�
    bool m_riverWidthChangeCheckEnabled;            // �Ƿ����úӿ��仯���
    double m_minRiverLength;                        // ��С�Ӷγ���
    double m_intersectionDetectionTolerance;        // �������ݲ�
    double m_directionDanglingDetectionTolerance;   // �������Ӽ���ݲ�
    double m_overlapTolerance;                     // �ص��ж��ݲ�
    double m_gapTolerance;                         // ��϶�ж��ݲ�
    double m_minOverlapArea;                       // ��С�ص������ֵ
    double m_danglingTolerance;                    // �����ж��ݲ�
    double m_minLineLength;                        // ��С�߶γ���
    double m_connectivityTolerance;                // ��ͨ�ж��ݲ�

    // ���� ֱ�ǻ������� ����
    double rightAngleLowerLimit;
    double rightAngleUpperLimit;
    double rightAngleDatabaseMatchTolerance;
    double rightAngleGeometrySimilarityThreshold;
    double rightAngleTurningPointTolerance;
    double rightAngleTurningPointMatchTolerance = 0.0;
    double rightAngleMinSegmentLength;

    // ���� �����Ż����������ƥ�� ����
    int m_maxFeaturesPerLayer = 5000;               // ÿ����ദ��Ҫ����
    int m_maxCandidatesPerFeature = 50;             // ÿҪ������ѡƥ����
    double m_quickDistanceThreshold = 200.0;        // ���پ���ɸѡ��ֵ
    bool m_useQuickMode = true;                     // ���ÿ���ģʽ
    double calculateQuickSimilarity(const QgsGeometry& geom1, const QgsGeometry& geom2);
    // ��ǰ�����ߣ�145000��
    int m_mapScale = 0;
    // ========================= ������UI������/���£� =========================
    QualityCheckConfig* m_config;
    QString m_configFilePath;
    QPushButton* selectConfigButton;
    QLabel* configPathLabel;

    void loadConfigFromXML();
    void updateParametersFromConfig();
    void updateUIFromConfig();
    // ��λת������������ʹ��m_mapScale��
    double convertUnitValue(double value, const QString& unit, bool isArea);
    double getConvertedParameter(const QString& checkType, const QString& paramName);
    double getUnscaledDistanceParameter(const QString& checkType, const QString& paramName);
    QString getConvertedUnitLabel(const QString& unit, bool isArea);
    void updateFileCountLabels();

    // ========================= ��ͼ���������ʽ���� =========================
    void loadMultipleLayersToMap();                                  // ��������ͼ�㵽��ͼ
    void setLayerStyle(QgsVectorLayer* layer, const QString& color, double width = 2.0);  // ����ͼ����ʽ
    QgsVectorLayer* createErrorLayer(const std::vector<QualityCheckResult>& results, const QgsCoordinateReferenceSystem& crs);
    QList<QgsVectorLayer*> createErrorLayers(const std::vector<QualityCheckResult>& results, const QgsCoordinateReferenceSystem& crs);

    // ========================= ����������ͼˢ�� =========================
    bool exportErrorsToShp(const QString& filePath);                 // ��������Ϊ Shp
    void clearAllLayers();                                          // �������ͼ��
    void refreshMapDisplay();                                       // ˢ�µ�ͼ��ʾ
    bool exportResultsToJson(const QString& filePath, const QJsonObject& root);// ���� JSON
    bool exportLogsToFolder(const QString& dirPath);
      // ���ӣ�ִ��״̬��ǣ����� JSON �� status ����
    bool executedLineTopology = false;
    bool executedDirection = false;
    bool executedPolygon = false;
    bool executedRightAngle = false;
    // ���䵼��Ŀ¼���ʼ���/�ʼ���־��
    QString m_qualityResultsDir;
    QString m_qualityLogsDir;
    // ��������ִ�б�ǣ�ÿ��ִ��ǰ���ã�
    void resetExecutionFlags();

public slots:
    // ========================= ������ת��=========================
    void onScaleSelectionChanged(int index);
    void onScaleTextChanged(const QString& text);
   
    // ========================= ��׼ͼ�����=========================
    void selectStandardCsvFile();
    void onStandardLayerChanged(int index);
   
    // ========================= ˽�вۣ����ļ�ѡ��������� =========================
    void selectShpFile();           // ѡ�����SHP�ļ�
    void selectReferenceFile();     // ѡ��ο�SHP�ļ�
    void outputToFile();
    void performQualityCheck();
    void showQualityResults(const std::vector<QualityCheckResult>& results);

    void selectConfigFile();
    void onOnlyErrorsCheckBoxToggled(bool checked);  // ������ʾ���󡱸�ѡ��״̬���

    // ========================= ˽�вۣ������ļ�ѡ����ά�� =========================
    void addParseFiles();           // ���ӽ����ļ�
    void addReferenceFiles();       // ���Ӳο��ļ�
    void removeSelectedParseFile(); // �Ƴ�ѡ�н����ļ�
    void removeSelectedReferenceFile(); // �Ƴ�ѡ�вο��ļ�
    void clearAllParseFiles();      // ������н����ļ�
    void clearAllReferenceFiles();  // ������вο��ļ�
};

#endif // READSHP_H
