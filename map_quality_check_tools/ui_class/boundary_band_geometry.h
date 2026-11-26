#pragma once
#include <QString>
#include <QJsonObject>
bool PrepareBoundaryBandGeometry(const QString& sourcePath, const QString& outputPath,
    const QString& colorFieldName, const QString& partName, QJsonObject& diagnostic);

QString CreateBoundaryBandPartRuleXml(const QString& sourceXmlPath, const QString& outputXmlPath, const QString& colorFieldName, const QString& standardColor, double standardWidthMm, double widthToleranceMm);
