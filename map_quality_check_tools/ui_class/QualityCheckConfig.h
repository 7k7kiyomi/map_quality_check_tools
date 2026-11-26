#pragma once
#ifndef QUALITYCHECKCONFIG_H
#define QUALITYCHECKCONFIG_H

#include <QObject>
#include <QString>
#include <QMap>
#include <QList> 
#include <QVariant>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QTextStream>
#include <QDebug>

// Quality check item configuration structure
struct CheckItemConfig {
    QString name;
    QString type;
    bool enabled;
    QMap<QString, QVariant> parameters;
    QString description;
};

class QualityCheckConfig : public QObject
{
    Q_OBJECT

public:
    explicit QualityCheckConfig(QObject* parent = nullptr);
    ~QualityCheckConfig();

    // Load from XML configuration file
    bool loadFromFile(const QString& filePath);

    // Get configuration for specified type
    CheckItemConfig getCheckItemConfig(const QString& type) const;

    // Get all check items
    QList<CheckItemConfig> getAllCheckItems() const;

    // Check if specified type is enabled
    bool isCheckItemEnabled(const QString& type) const;

    // Get parameter values
    double getParameterDouble(const QString& checkType, const QString& paramName, double defaultValue = 0.0) const;
    int getParameterInt(const QString& checkType, const QString& paramName, int defaultValue = 0) const;
    QString getParameterString(const QString& checkType, const QString& paramName, const QString& defaultValue = "") const;

    // ����ר�õĲ�����ȡ����

    double getOverlapTolerance() const;
    double getGapTolerance() const;
    double getMinOverlapArea() const;
    double getDanglingTolerance() const;
    double getMinLineLength() const;
    double getConnectivityTolerance() const;
    // �������ר�ò�����ȡ����
    double getRiverDanglingTolerance() const;
    double getRiverIsolationDistance() const;
    double getRiverMinLineLength() const;
    // ����ͨ���Լ��ר�ò�����ȡ����
    double getRiverOverlapTolerance() const;
    double getRiverIntersectionTolerance() const;
    double getRiverExceedsTolerance() const;
    double getDatabaseMatchTolerance() const;
    double getWaterSystemConnectionTolerance() const;
    //�������

    double getDirectionDatabaseMatchTolerance() const;

    double getMinRiverLength() const;
    double getIntersectionDetectionTolerance() const;
    double getDirectionDanglingDetectionTolerance() const;
    // 系列图模板读取接口
    QString getSeriesBoundaryLineLayer() const;
    QString getSeriesAdminAreaLayer() const;
    bool isSeriesSymmetricDifferenceEnabled() const;
    double getSeriesFeaturePointTolerance() const;
    QString getSeriesFeaturePointToleranceUnit() const;
    double getSeriesMinAreaTolerance() const;
    QString getSeriesMinAreaToleranceUnit() const;
    QStringList getSeriesAttributeConsistencyFields() const;
    QStringList getSeriesLineStyleConsistencyFields() const;
    QStringList getSeriesPolygonStyleConsistencyFields() const;
    QStringList getSeriesPointStyleConsistencyFields() const;
    bool validateSeriesMapTemplate(QStringList* errors = nullptr) const;

    // Error information
    QString getLastError() const { return m_lastError; }

    // ��Ҫ�����˼�����
    double getLineTopologyDanglingTolerance() const;
    double getLineTopologyMinLineLength() const;
    double getLineTopologyIsolationDistance() const;
    double getLineTopologyConnectivityTolerance() const;
    double getNetworkBufferDistance() const;
    double getConnectivityThreshold() const;
    double getWaterSystemConnectionToleranceNew() const;
    double getRiverOverlapToleranceNew() const;
    double getRiverIntersectionToleranceNew() const;
    double getRiverExceedsToleranceNew() const;
    double getDatabaseMatchToleranceNew() const;
    // ֱ�ǻ��Ƕ���ֵ���Ƽ�������ȡ����
    double getRightAngleLowerLimit() const;
    double getRightAngleUpperLimit() const;
    double getRightAngleDatabaseMatchTolerance() const;
    double getRightAngleGeometrySimilarityThreshold() const;
    double getRightAngleTurningPointTolerance() const;
    double getRightAngleTurningPointMatchTolerance() const;
    double getRightAngleMinSegmentLength() const;

private:
    QMap<QString, CheckItemConfig> m_checkItems;
    QString m_lastError;

    // Parse XML elements
    bool parseCheckItem(const QDomElement& element);
    QMap<QString, QVariant> parseParameters(const QDomElement& parametersElement);
};

#endif // QUALITYCHECKCONFIG_H
