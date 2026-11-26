#include "FeatureSpacingCheckDialog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QMap>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QVariant>
#include <QDomDocument>
#include <QVector>

#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>
#include <qgsfeature.h>
#include <qgsfield.h>
#include <qgsfields.h>
#include <qgsgeometry.h>
#include <qgsproject.h>
#include <qgsrectangle.h>
#include <qgsspatialindex.h>
#include <qgsunittypes.h>
#include <qgsvectorlayer.h>
#include <qgsvectorfilewriter.h>
#include <qgswkbtypes.h>

#include <cmath>
#include <algorithm>

struct FeatureSpacingCheckDialog::SpacingRule
{
    double bufferMillimeters = -1.0;
    double spacingCentimeters = -1.0;
    QString symbolGroupFieldName;
};

struct FeatureSpacingCheckDialog::RiverRule
{
    double bufferMillimeters = -1.0;
    QString levelField;
    QStringList minorLevels;
    QStringList mainLevels;
    QString nameField;
    double minimumCentimeters = -1.0;
    double maximumCentimeters = -1.0;
};

namespace
{
QgsFields spacingErrorFields()
{
    QgsFields fields;
    fields.append(QgsField(QStringLiteral("check_name"), QVariant::String, QString(), 20));
    fields.append(QgsField(QStringLiteral("file_a"), QVariant::String, QString(), 80));
    fields.append(QgsField(QStringLiteral("fid_a"), QVariant::LongLong));
    fields.append(QgsField(QStringLiteral("file_b"), QVariant::String, QString(), 80));
    fields.append(QgsField(QStringLiteral("fid_b"), QVariant::LongLong));
    fields.append(QgsField(QStringLiteral("dist_m"), QVariant::Double));
    fields.append(QgsField(QStringLiteral("limit_m"), QVariant::Double));
    return fields;
}

QgsFeature makeSpacingErrorFeature(const QgsFields& fields, const QString& checkName,
                                   const QString& fileA, qint64 fidA,
                                   const QString& fileB, qint64 fidB,
                                   const QgsGeometry& centerA, const QgsGeometry& centerB,
                                   double distanceMeters, double limitMeters)
{
    QgsFeature feature(fields);
    QgsPolylineXY line;
    line << centerA.asPoint() << centerB.asPoint();
    feature.setGeometry(QgsGeometry::fromPolylineXY(line));
    QgsAttributes attributes;
    attributes << checkName << fileA << QVariant::fromValue<qlonglong>(fidA)
               << fileB << QVariant::fromValue<qlonglong>(fidB)
               << distanceMeters << limitMeters;
    feature.setAttributes(attributes);
    return feature;
}

QgsFields springSymbolErrorFields()
{
    QgsFields fields;
    fields.append(QgsField(QStringLiteral("check_name"), QVariant::String, QString(), 20));
    fields.append(QgsField(QStringLiteral("file_name"), QVariant::String, QString(), 80));
    fields.append(QgsField(QStringLiteral("fid"), QVariant::LongLong));
    fields.append(QgsField(QStringLiteral("symbol_id"), QVariant::String, QString(), 254));
    fields.append(QgsField(QStringLiteral("other_fil"), QVariant::String, QString(), 80));
    fields.append(QgsField(QStringLiteral("other_fid"), QVariant::LongLong));
    fields.append(QgsField(QStringLiteral("other_sid"), QVariant::String, QString(), 254));
    fields.append(QgsField(QStringLiteral("dist_m"), QVariant::Double));
    fields.append(QgsField(QStringLiteral("limit_m"), QVariant::Double));
    return fields;
}

QgsFeature makeSpringSymbolErrorFeature(const QgsFields& fields,
                                        const QString& checkName,
                                        const QString& fileName, qint64 fid,
                                        const QString& symbolId, const QgsGeometry& symbolGeometry,
                                        const QString& otherFileName, qint64 otherFid,
                                        const QString& otherSymbolId,
                                        double distanceMeters, double limitMeters)
{
    QgsFeature feature(fields);
    QgsGeometry outputGeometry = symbolGeometry;
    outputGeometry.convertToMultiType();
    feature.setGeometry(outputGeometry);
    QgsAttributes attributes;
    attributes << checkName << fileName << QVariant::fromValue<qlonglong>(fid)
               << symbolId << otherFileName << QVariant::fromValue<qlonglong>(otherFid)
               << otherSymbolId << distanceMeters << limitMeters;
    feature.setAttributes(attributes);
    return feature;
}

bool writeSpacingErrorLayer(QWidget* parent, const QString& requestedPath,
                             const QgsCoordinateReferenceSystem& crs,
                             const QgsFields& fields, const QList<QgsFeature>& features,
                             QgsWkbTypes::Type geometryType,
                             const QStringList& protectedPaths, QString& error,
                             bool unattended = false)
{
    QString outputPath = QFileInfo(requestedPath).absoluteFilePath();
    if (!outputPath.endsWith(QStringLiteral(".shp"), Qt::CaseInsensitive))
        outputPath += QStringLiteral(".shp");
    const QString normalizedOutputPath = QDir::cleanPath(outputPath);
    for (const QString& protectedPath : protectedPaths) {
        const QString normalizedInputPath = QDir::cleanPath(QFileInfo(protectedPath).absoluteFilePath());
        if (normalizedOutputPath.compare(normalizedInputPath, Qt::CaseInsensitive) == 0) {
            error = QStringLiteral("错误结果图层不能覆盖输入文件：%1").arg(protectedPath);
            return false;
        }
    }

    const QString basePath = outputPath.left(outputPath.lastIndexOf('.'));
    const QStringList extensions = { QStringLiteral(".shp"), QStringLiteral(".shx"),
                                     QStringLiteral(".dbf"), QStringLiteral(".prj"),
                                     QStringLiteral(".cpg"), QStringLiteral(".qpj") };
    bool hasExistingFile = false;
    for (const QString& extension : extensions)
        hasExistingFile = hasExistingFile || QFile::exists(basePath + extension);
    if (hasExistingFile) {
        if (!unattended) {
            const QMessageBox::StandardButton answer = QMessageBox::question(
                parent, QStringLiteral("覆盖错误图层"),
                QStringLiteral("输出位置已存在同名 Shapefile，是否覆盖？\n%1").arg(outputPath),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) {
                error = QStringLiteral("用户取消覆盖已有错误图层。");
                return false;
            }
        }
        for (const QString& extension : extensions) {
            const QString sidecarPath = basePath + extension;
            if (QFile::exists(sidecarPath) && !QFile::remove(sidecarPath)) {
                error = QStringLiteral("无法覆盖已有文件：%1").arg(sidecarPath);
                return false;
            }
        }
    }

    QgsVectorFileWriter writer(outputPath, QStringLiteral("UTF-8"), fields,
                               geometryType, crs, QStringLiteral("ESRI Shapefile"));
    if (writer.hasError() != QgsVectorFileWriter::NoError) {
        error = writer.errorMessage();
        return false;
    }
    for (QgsFeature feature : features) {
        if (!writer.addFeature(feature)) {
            error = writer.errorMessage();
            return false;
        }
    }
    if (writer.hasError() != QgsVectorFileWriter::NoError) {
        error = writer.errorMessage();
        return false;
    }
    return true;
}
}

FeatureSpacingCheckDialog::FeatureSpacingCheckDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("要素间隔检查"));
    resize(760, 560);

    auto* root = new QVBoxLayout(this);
    root->addWidget(createFileRow(QStringLiteral("源 GDB 对应要素图层 SHP"),
                                  m_sourcePath, m_sourcePaths,
                                  QStringLiteral("选择源 GDB 对应要素 SHP"), true));
    root->addWidget(createFileRow(QStringLiteral("关联要素图层（河流检查上传主区面）"),
                                  m_associatedPath, m_associatedPaths,
                                  QStringLiteral("选择解析后关联要素 SHP"), true));
    root->addWidget(createFileRow(QStringLiteral("待检/解析后要素图层（点/线/面）"),
                                  m_targetPath, m_targetPaths,
                                  QStringLiteral("选择待检或解析后要素 SHP"), true));
    root->addWidget(createOutputPathRow());

    root->addWidget(createFileRow(QStringLiteral("质检 XML"), m_xmlPath, m_xmlPaths,
                                  QStringLiteral("选择质检 XML"), false));
    auto* scaleRow = new QHBoxLayout;
    scaleRow->addWidget(new QLabel(QStringLiteral("比例尺分母（例如 50000 表示 1:50000）"), this));
    m_scale = new QSpinBox(this);
    m_scale->setRange(0, 100000000);
    m_scale->setSingleStep(1000);
    m_scale->setSpecialValueText(QStringLiteral("请填写"));
    m_scale->setToolTip(QStringLiteral("XML 规则使用 mm/cm，需结合比例尺换算为实地距离。"));
    scaleRow->addWidget(m_scale);
    scaleRow->addStretch();
    root->addLayout(scaleRow);

    auto* actions = new QHBoxLayout;
    m_run = new QPushButton(QStringLiteral("开始检查"), this);
    m_run->setEnabled(false);
    auto* clear = new QPushButton(QStringLiteral("清空日志"), this);
    actions->addWidget(m_run);
    actions->addWidget(clear);
    actions->addStretch();
    root->addLayout(actions);

    m_log = new QTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setPlaceholderText(QStringLiteral("检查过程和结果显示在这里。"));
    root->addWidget(m_log, 1);

    connect(m_run, &QPushButton::clicked, this, [this]() {
        switch (m_checkType) {
        case CheckType::Spring: runCheck(); break;
        case CheckType::Well: runWellCheck(); break;
        case CheckType::Village: runVillageCheck(); break;
        case CheckType::River: runRiverCheck(); break;
        case CheckType::RailwayTunnel:
        case CheckType::RoadTunnel:
            runTunnelCheck();
            break;
        default:
            appendLog(QStringLiteral("无法根据 XML 文件名识别检查对象，请确认文件名包含“泉”“井”“行政村”“河流”或“隧道”。"));
            break;
        }
    });
    connect(clear, &QPushButton::clicked, m_log, &QTextEdit::clear);
}

QWidget* FeatureSpacingCheckDialog::createFileRow(const QString& title, QLabel*& pathLabel, QStringList& paths,
                                                   const QString& dialogTitle, bool shapefile)
{
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* titleLabel = new QLabel(title, row);
    titleLabel->setMinimumWidth(160);
    pathLabel = new QLabel(QStringLiteral("未选择"), row);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathLabel->setWordWrap(true);
    auto* button = new QPushButton(shapefile ? QStringLiteral("上传（可多选）…")
                                              : QStringLiteral("上传…"), row);
    layout->addWidget(titleLabel);
    layout->addWidget(pathLabel, 1);
    layout->addWidget(button);

    QStringList* pathsPtr = &paths;
    connect(button, &QPushButton::clicked, this, [this, pathLabel, pathsPtr, dialogTitle, shapefile]() {
        const QString filter = shapefile
            ? QStringLiteral("Shapefile (*.shp);;所有文件 (*)")
            : QStringLiteral("XML 文件 (*.xml);;所有文件 (*)");
        const QStringList selectedPaths = shapefile
            ? QFileDialog::getOpenFileNames(this, dialogTitle, QString(), filter)
            : QStringList{ QFileDialog::getOpenFileName(this, dialogTitle, QString(), filter) };
        if (selectedPaths.isEmpty() || (selectedPaths.size() == 1 && selectedPaths.first().isEmpty())) return;
        *pathsPtr = selectedPaths;
        QStringList displayPaths;
        for (const QString& path : *pathsPtr) displayPaths.append(QDir::toNativeSeparators(path));
        pathLabel->setText(displayPaths.join(QStringLiteral("\n")));
        if (pathLabel == m_xmlPath) updateCheckTypeFromXml(selectedPaths.first());
        updateRunEnabled();
        appendLog(QStringLiteral("已选择 %1 个文件：%2")
                      .arg(pathsPtr->size()).arg(displayPaths.join(QStringLiteral("；"))),
                  m_log);
    });
    return row;
}

QWidget* FeatureSpacingCheckDialog::createOutputPathRow()
{
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* titleLabel = new QLabel(QStringLiteral("间隔错误结果图层（SHP）"), row);
    titleLabel->setMinimumWidth(160);
    m_outputPathLabel = new QLabel(QStringLiteral("未设置"), row);
    m_outputPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_outputPathLabel->setWordWrap(true);
    auto* button = new QPushButton(QStringLiteral("设置输出…"), row);
    layout->addWidget(titleLabel);
    layout->addWidget(m_outputPathLabel, 1);
    layout->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("设置间隔错误结果图层"), QString(),
            QStringLiteral("Shapefile (*.shp)"));
        if (path.isEmpty()) return;
        m_outputFilePath = path;
        if (!m_outputFilePath.endsWith(QStringLiteral(".shp"), Qt::CaseInsensitive))
            m_outputFilePath += QStringLiteral(".shp");
        m_outputPathLabel->setText(QDir::toNativeSeparators(m_outputFilePath));
        updateRunEnabled();
        appendLog(QStringLiteral("已设置错误结果图层输出路径：%1")
                      .arg(QDir::toNativeSeparators(m_outputFilePath)));
    });
    return row;
}

void FeatureSpacingCheckDialog::updateRunEnabled()
{
    const bool hasXml = !m_xmlPaths.isEmpty() && !m_xmlPaths.first().isEmpty();
    const bool hasOutput = !m_outputFilePath.trimmed().isEmpty();
    if (!hasXml || !hasOutput) {
        m_run->setEnabled(false);
    } else if (m_checkType == CheckType::Well) {
        m_run->setEnabled(!m_sourcePaths.isEmpty() && !m_targetPaths.isEmpty() && hasXml);
    } else if (m_checkType == CheckType::Spring) {
        m_run->setEnabled(!m_associatedPaths.isEmpty() && !m_targetPaths.isEmpty()
                          && !m_sourcePaths.isEmpty() && hasXml);
    } else if (m_checkType == CheckType::Village) {
        m_run->setEnabled(!m_associatedPaths.isEmpty() && !m_targetPaths.isEmpty() && hasXml);
    } else if (m_checkType == CheckType::River) {
        m_run->setEnabled(!m_sourcePaths.isEmpty() && !m_associatedPaths.isEmpty()
                          && !m_targetPaths.isEmpty() && hasXml);
    } else if (m_checkType == CheckType::RailwayTunnel) {
        m_run->setEnabled(!m_sourcePaths.isEmpty() && !m_associatedPaths.isEmpty()
                          && !m_targetPaths.isEmpty() && hasXml);
    } else if (m_checkType == CheckType::RoadTunnel) {
        m_run->setEnabled(!m_sourcePaths.isEmpty() && !m_targetPaths.isEmpty() && hasXml);
    } else {
        m_run->setEnabled(false);
    }
}

void FeatureSpacingCheckDialog::updateCheckTypeFromXml(const QString& path)
{
    const QString baseName = QFileInfo(path).completeBaseName();
    const bool hasSpring = baseName.contains(QStringLiteral("泉"));
    const bool hasWell = baseName.contains(QStringLiteral("井"));
    const bool hasRiver = baseName.contains(QStringLiteral("河流"));
    const bool hasTunnel = baseName.contains(QStringLiteral("隧道"));
    const bool hasVillage = baseName.contains(QStringLiteral("行政村"));
    const int matchedTypes = int(hasSpring) + int(hasWell) + int(hasVillage) + int(hasRiver) + int(hasTunnel);

    if (matchedTypes != 1) {
        m_checkType = CheckType::Unknown;
        if (matchedTypes == 0)
            appendLog(QStringLiteral("无法识别 XML 对应的检查对象：文件名“%1”应包含“泉”“井”“行政村”“河流”或“隧道”。").arg(baseName));
        else
            appendLog(QStringLiteral("XML 文件名“%1”包含多个检查对象名称，无法唯一分派检查。").arg(baseName));
    } else if (hasSpring) {
        m_checkType = CheckType::Spring;
        appendLog(QStringLiteral("根据 XML 文件名识别为泉要素间隔检查。"));
    } else if (hasWell) {
        m_checkType = CheckType::Well;
        appendLog(QStringLiteral("根据 XML 文件名识别为井要素间隔检查。"));
    } else if (hasVillage) {
        m_checkType = CheckType::Village;
        appendLog(QStringLiteral("根据 XML 文件名识别为行政村要素间隔检查。"));
    } else if (hasRiver) {
        m_checkType = CheckType::River;
        appendLog(QStringLiteral("根据 XML 文件名识别为河流要素间隔检查。源 GDB 上传河流线，关联图层上传主区面，待检图层上传解析水系线。"));
    } else {
        const bool railway = baseName.contains(QStringLiteral("铁路"));
        const bool road = baseName.contains(QStringLiteral("公路"));
        if (railway == road) {
            m_checkType = CheckType::Unknown;
            appendLog(QStringLiteral("无法区分隧道检查模式：XML 文件名“%1”必须且只能包含“铁路隧道”或“公路隧道”。")
                          .arg(baseName));
        } else {
            m_checkType = railway ? CheckType::RailwayTunnel : CheckType::RoadTunnel;
            appendLog(QStringLiteral("根据 XML 文件名识别为%1隧道间隔检查。")
                          .arg(railway ? QStringLiteral("铁路") : QStringLiteral("公路")));
            appendLog(railway
                          ? QStringLiteral("铁路模式：源 GDB 上传 LRRL，待检图层上传解析铁路隧道线，关联图层上传火车站。")
                          : QStringLiteral("公路模式：源 GDB 上传 LRDL，待检图层上传解析公路隧道线；关联图层不需要上传。"));
        }
    }
}

void FeatureSpacingCheckDialog::appendLog(const QString& message, QTextEdit* target)
{
    if (m_taskManagerMode) {
        m_runLogs << message;
        if (message.startsWith(QStringLiteral("检查失败"))
            || message.contains(QStringLiteral("错误结果图层输出失败"))
            || message.contains(QStringLiteral("错误图层输出失败")))
            m_failed = true;
        if (message.startsWith(QStringLiteral("检查完成"))
            || message.startsWith(QStringLiteral("河流检查完成"))
            || message.startsWith(QStringLiteral("检查结束")))
            m_completed = true;
        return;
    }
    if (!target) target = m_log;
    if (!target) return;
    target->append(QStringLiteral("[%1] %2")
                       .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), message));
}

FeatureSpacingRunResult FeatureSpacingCheckDialog::runConfigured(
    const QStringList& sourcePaths, const QStringList& associatedPaths,
    const QStringList& targetPaths, const QString& xmlPath, int scale,
    const QString& outputShpPath)
{
    m_taskManagerMode = true;
    m_failed = false;
    m_completed = false;
    m_outputWritten = false;
    m_problemCount = 0;
    m_runLogs.clear();
    m_sourcePaths = sourcePaths;
    m_associatedPaths = associatedPaths;
    m_targetPaths = targetPaths;
    m_xmlPaths = QStringList{xmlPath};
    m_outputFilePath = outputShpPath;
    m_scale->setValue(scale);
    updateCheckTypeFromXml(xmlPath);
    switch (m_checkType) {
    case CheckType::Spring: runCheck(); break;
    case CheckType::Well: runWellCheck(); break;
    case CheckType::Village: runVillageCheck(); break;
    case CheckType::River: runRiverCheck(); break;
    case CheckType::RailwayTunnel:
    case CheckType::RoadTunnel: runTunnelCheck(); break;
    default: appendLog(QStringLiteral("检查失败：无法识别 XML 对应的检查对象。")); break;
    }
    FeatureSpacingRunResult result;
    result.succeeded = m_completed && !m_failed;
    result.problemCount = m_problemCount;
    result.logs = m_runLogs;
    if (m_outputWritten && result.succeeded)
        result.outputPaths << m_outputFilePath;
    return result;
}

bool FeatureSpacingCheckDialog::readRules(const QString& path, SpacingRule& rule, QString& error,
                                          bool requireBuffer, bool requireSymbolGroup) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = QStringLiteral("无法打开 XML：%1").arg(path);
        return false;
    }

    QDomDocument document;
    QString parseMessage;
    int line = 0;
    int column = 0;
    if (!document.setContent(&file, &parseMessage, &line, &column)) {
        error = QStringLiteral("XML 解析失败（第 %1 行，第 %2 列）：%3")
                    .arg(line).arg(column).arg(parseMessage);
        return false;
    }

    const QDomNodeList checkItems = document.elementsByTagName(QStringLiteral("check_item"));
    for (int i = 0; i < checkItems.size(); ++i) {
        const QDomElement item = checkItems.at(i).toElement();
        if (item.firstChildElement(QStringLiteral("name")).text().trimmed()
            != QStringLiteral("要素间隔检查")) continue;

        const QDomNodeList rules = item.elementsByTagName(QStringLiteral("rule"));
        for (int j = 0; j < rules.size(); ++j) {
            const QDomElement ruleElement = rules.at(j).toElement();
            const QString name = ruleElement.firstChildElement(QStringLiteral("name")).text().trimmed();
            if (name == QStringLiteral("符号组合")) {
                rule.symbolGroupFieldName =
                    ruleElement.firstChildElement(QStringLiteral("value")).text().trimmed();
                continue;
            }
            bool ok = false;
            const double value = ruleElement.firstChildElement(QStringLiteral("value")).text().trimmed().toDouble(&ok);
            const QString unit = ruleElement.firstChildElement(QStringLiteral("unit")).text().trimmed();
            if (!ok || value <= 0.0) continue;
            if (name == QStringLiteral("缓冲区半径") && unit == QStringLiteral("mm"))
                rule.bufferMillimeters = value;
            else if (name == QStringLiteral("要素间隔") && unit == QStringLiteral("cm"))
                rule.spacingCentimeters = value;
        }
        break;
    }

    if ((requireBuffer && rule.bufferMillimeters <= 0.0)
        || (requireSymbolGroup && rule.symbolGroupFieldName.isEmpty())
        || rule.spacingCentimeters <= 0.0) {
        QStringList missingRules;
        if (requireBuffer && rule.bufferMillimeters <= 0.0)
            missingRules.append(QStringLiteral("缓冲区半径 mm"));
        if (requireSymbolGroup && rule.symbolGroupFieldName.isEmpty())
            missingRules.append(QStringLiteral("符号组合字段"));
        if (rule.spacingCentimeters <= 0.0)
            missingRules.append(QStringLiteral("要素间隔 cm"));
        error = QStringLiteral("XML 中未找到完整的“要素间隔检查”规则（缺少：%1）。")
                    .arg(missingRules.join(QStringLiteral("、")));
        return false;
    }
    return true;
}

bool FeatureSpacingCheckDialog::readTunnelRules(const QString& path, double& spacingMeters,
                                                QStringList& groupingFields, QString& error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = QStringLiteral("无法打开 XML：%1").arg(path);
        return false;
    }
    QDomDocument document;
    QString parseMessage;
    int line = 0;
    int column = 0;
    if (!document.setContent(&file, &parseMessage, &line, &column)) {
        error = QStringLiteral("XML 解析失败（第 %1 行，第 %2 列）：%3")
                    .arg(line).arg(column).arg(parseMessage);
        return false;
    }

    const QDomNodeList checkItems = document.elementsByTagName(QStringLiteral("check_item"));
    for (int i = 0; i < checkItems.size(); ++i) {
        const QDomElement item = checkItems.at(i).toElement();
        if (item.firstChildElement(QStringLiteral("name")).text().trimmed()
            != QStringLiteral("要素间隔检查")) continue;
        const QDomNodeList rules = item.elementsByTagName(QStringLiteral("rule"));
        for (int j = 0; j < rules.size(); ++j) {
            const QDomElement rule = rules.at(j).toElement();
            const QString ruleName = rule.firstChildElement(QStringLiteral("name")).text().trimmed();
            const QString value = rule.firstChildElement(QStringLiteral("value")).text().trimmed();
            const QString unit = rule.firstChildElement(QStringLiteral("unit")).text().trimmed();
            if (ruleName == QStringLiteral("隧道间隔") && unit == QStringLiteral("m")) {
                bool ok = false;
                const double parsedValue = value.toDouble(&ok);
                if (ok && parsedValue > 0.0) spacingMeters = parsedValue;
            } else if (ruleName == QStringLiteral("GDB数据属性")) {
                QString normalizedFields = value;
                normalizedFields.replace(QChar(0xff0c), QChar(','));
                const QStringList names = normalizedFields.split(QChar(','), Qt::SkipEmptyParts);
                for (const QString& name : names) {
                    const QString field = name.trimmed();
                    if (!field.isEmpty() && !groupingFields.contains(field, Qt::CaseInsensitive))
                        groupingFields.append(field);
                }
            }
        }
        break;
    }
    if (spacingMeters <= 0.0) {
        error = QStringLiteral("XML 中缺少有效的“隧道间隔”规则，需填写大于 0 的数值且单位为 m。");
        return false;
    }
    return true;
}

bool FeatureSpacingCheckDialog::readRiverRules(const QString& path, RiverRule& rule,
                                                QString& error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = QStringLiteral("无法打开 XML：%1").arg(path);
        return false;
    }
    QDomDocument document;
    QString parseMessage;
    int line = 0;
    int column = 0;
    if (!document.setContent(&file, &parseMessage, &line, &column)) {
        error = QStringLiteral("XML 解析失败（第 %1 行，第 %2 列）：%3")
                    .arg(line).arg(column).arg(parseMessage);
        return false;
    }
    bool found = false;
    const QDomNodeList items = document.elementsByTagName(QStringLiteral("check_item"));
    for (int i = 0; i < items.size(); ++i) {
        const QDomElement item = items.at(i).toElement();
        if (item.firstChildElement(QStringLiteral("id")).text().trimmed() != QStringLiteral("120")
            || item.firstChildElement(QStringLiteral("name")).text().trimmed()
                   != QStringLiteral("要素间隔检查")) continue;
        found = true;
        const QDomNodeList rules = item.elementsByTagName(QStringLiteral("rule"));
        for (int j = 0; j < rules.size(); ++j) {
            const QDomElement entry = rules.at(j).toElement();
            const QString name = entry.firstChildElement(QStringLiteral("name")).text().trimmed();
            const QString value = entry.firstChildElement(QStringLiteral("value")).text().trimmed();
            const QString unit = entry.firstChildElement(QStringLiteral("unit")).text().trimmed();
            if (name == QStringLiteral("缓冲区半径")) {
                bool ok = false;
                const double number = value.toDouble(&ok);
                if (ok && number > 0.0 && unit == QStringLiteral("mm"))
                    rule.bufferMillimeters = number;
            } else if (name == QStringLiteral("GDB河流等级属性")) {
                rule.levelField = value;
            } else if (name == QStringLiteral("河流等级")) {
                QString normalized = value;
                normalized.replace(QChar(0xff0c), QChar(','));
                for (const QString& part : normalized.split(QChar(','), Qt::SkipEmptyParts)) {
                    const QString level = part.trimmed();
                    if (!level.isEmpty() && !rule.minorLevels.contains(level))
                        rule.minorLevels.append(level);
                }
            } else if (name == QStringLiteral("主干河流等级")) {
                QString normalized = value;
                normalized.replace(QChar(0xff0c), QChar(','));
                for (const QString& part : normalized.split(QChar(','), Qt::SkipEmptyParts)) {
                    const QString level = part.trimmed();
                    if (!level.isEmpty() && !rule.mainLevels.contains(level)) rule.mainLevels.append(level);
                }
            } else if (name == QStringLiteral("GDB河流属性")) {
                rule.nameField = value;
            } else if (name == QStringLiteral("要素间隔")) {
                QString normalized = value;
                normalized.replace(QChar(0xff0c), QChar(','));
                const QStringList limits = normalized.split(QChar(','), Qt::SkipEmptyParts);
                if (unit != QStringLiteral("cm") || limits.size() != 2) continue;
                bool minimumOk = false;
                bool maximumOk = false;
                const double minimum = limits.at(0).trimmed().toDouble(&minimumOk);
                const double maximum = limits.at(1).trimmed().toDouble(&maximumOk);
                if (minimumOk && maximumOk && minimum > 0.0 && maximum >= minimum) {
                    rule.minimumCentimeters = minimum;
                    rule.maximumCentimeters = maximum;
                }
            }
        }
        break;
    }
    if (!found || rule.bufferMillimeters <= 0.0 || rule.levelField.isEmpty()
        || rule.minorLevels.isEmpty() || rule.mainLevels.isEmpty() || rule.nameField.isEmpty()
        || rule.minimumCentimeters <= 0.0 || rule.maximumCentimeters < rule.minimumCentimeters) {
        error = QStringLiteral("河流 XML 的 ID=120 规则不完整：需缓冲区半径（mm）、等级字段、待检等级、主干等级、主干名称字段，以及两个递增的间隔值（cm）；待检等级不能同时是主干等级。");
        return false;
    }
    for (const QString& level : rule.minorLevels) {
        if (rule.mainLevels.contains(level)) {
            error = QStringLiteral("河流 XML 中等级“%1”不能同时属于待检河流和主干河流。")
                        .arg(level);
            return false;
        }
    }
    return true;
}

bool FeatureSpacingCheckDialog::makeSymbolArea(const QgsGeometry& symbolGeometry, QgsGeometry& area) const
{
    if (symbolGeometry.isNull() || symbolGeometry.isEmpty()) return false;
    const QgsWkbTypes::GeometryType geometryType = QgsWkbTypes::geometryType(symbolGeometry.wkbType());
    if (geometryType == QgsWkbTypes::PolygonGeometry) {
        if (!symbolGeometry.isGeosValid()) return false;
        area = symbolGeometry;
        return true;
    }
    if (geometryType != QgsWkbTypes::LineGeometry) return false;

    // A symbol may be split across multiple open paths. Node the complete linework
    // and polygonize it before falling back to individually closed rings.
    QVector<QgsGeometry> lineInputs;
    lineInputs.append(symbolGeometry);
    const QgsGeometry nodedLines = QgsGeometry::unaryUnion(lineInputs);
    if (!nodedLines.isNull() && !nodedLines.isEmpty()) {
        QVector<QgsGeometry> polygonInputs;
        polygonInputs.append(nodedLines);
        QgsGeometry polygonized = QgsGeometry::polygonize(polygonInputs);
        polygonized.convertGeometryCollectionToSubclass(QgsWkbTypes::PolygonGeometry);
        if (!polygonized.isNull() && !polygonized.isEmpty()
            && QgsWkbTypes::geometryType(polygonized.wkbType()) == QgsWkbTypes::PolygonGeometry
            && polygonized.isGeosValid()) {
            area = polygonized;
            return true;
        }
    }

    QgsMultiPolylineXY lines;
    if (symbolGeometry.isMultipart())
        lines = symbolGeometry.asMultiPolyline();
    else
        lines.append(symbolGeometry.asPolyline());

    for (QgsPolylineXY line : lines) {
        if (line.size() < 4) continue;
        const QgsPointXY first = line.first();
        const QgsPointXY last = line.last();
        const double dx = first.x() - last.x();
        const double dy = first.y() - last.y();
        if (std::sqrt(dx * dx + dy * dy) > 1e-6) continue;
        line.last() = first;

        QgsPolygonXY ring;
        ring.append(line);
        const QgsGeometry ringArea = QgsGeometry::fromPolygonXY(ring);
        if (ringArea.isNull() || ringArea.isEmpty() || !ringArea.isGeosValid()) continue;
        area = area.isNull() ? ringArea : area.combine(ringArea);
    }
    return !area.isNull() && !area.isEmpty() && area.isGeosValid();
}

void FeatureSpacingCheckDialog::runCheck()
{
    struct SpringPoint {
        qint64 indexId = 0;
        qint64 sourceId = 0;
        QString sourceName;
        QString symbolId;
        QList<qint64> componentFids;
        QgsGeometry geometry;
        QgsGeometry symbolGeometry;
    };

    m_run->setEnabled(false);
    appendLog(QStringLiteral("开始泉间隔检查。"));

    const int scaleDenominator = m_scale->value();
    if (scaleDenominator <= 0) {
        appendLog(QStringLiteral("检查失败：请填写比例尺分母。XML 中的 mm/cm 规则需按比例尺换算为实地距离。"));
        m_run->setEnabled(true);
        return;
    }

    SpacingRule rule;
    QString error;
    if (!readRules(m_xmlPaths.first(), rule, error)) {
        appendLog(QStringLiteral("检查失败：%1").arg(error));
        m_run->setEnabled(true);
        return;
    }

    QgsVectorLayer firstAssociatedLayer(m_associatedPaths.first(), QStringLiteral("关联要素"), QStringLiteral("ogr"));
    if (!firstAssociatedLayer.isValid()) {
        appendLog(QStringLiteral("检查失败：关联要素 SHP 无法加载：%1").arg(m_associatedPaths.first()));
        m_run->setEnabled(true);
        return;
    }
    const QgsWkbTypes::GeometryType firstAssociatedGeometryType =
        QgsWkbTypes::geometryType(firstAssociatedLayer.wkbType());
    if (firstAssociatedGeometryType != QgsWkbTypes::PointGeometry
        && firstAssociatedGeometryType != QgsWkbTypes::LineGeometry
        && firstAssociatedGeometryType != QgsWkbTypes::PolygonGeometry) {
        appendLog(QStringLiteral("检查失败：解析后关联图层仅支持点、线、面要素：%1").arg(m_associatedPaths.first()));
        m_run->setEnabled(true);
        return;
    }
    const QgsCoordinateReferenceSystem metricCrs = firstAssociatedLayer.crs();
    if (!metricCrs.isValid() || metricCrs.mapUnits() != QgsUnitTypes::DistanceMeters) {
        appendLog(QStringLiteral("检查失败：解析后关联图层必须使用以米为单位的投影坐标系，请先投影转换数据：%1")
                      .arg(m_associatedPaths.first()));
        m_run->setEnabled(true);
        return;
    }

    const double bufferMeters = rule.bufferMillimeters * scaleDenominator / 1000.0;
    const double spacingMeters = rule.spacingCentimeters * scaleDenominator / 100.0;
    appendLog(QStringLiteral("规则：关联要素缓冲半径 %1 mm（实地 %2 m），待检要素中心最小间隔 %3 cm（实地 %4 m）。")
                  .arg(rule.bufferMillimeters).arg(bufferMeters, 0, 'f', 3)
                  .arg(rule.spacingCentimeters).arg(spacingMeters, 0, 'f', 3));
    appendLog(QStringLiteral("泉参与范围：符号面需完全落入关联图层缓冲区；NAME 非空源泉点落入符号面的泉也参与检查。"));

    QgsGeometry waterBuffer;
    int associatedFeatureCount = 0;
    try {
        for (const QString& associatedPath : m_associatedPaths) {
            QgsVectorLayer associatedLayer(associatedPath, QFileInfo(associatedPath).fileName(), QStringLiteral("ogr"));
            if (!associatedLayer.isValid()) {
                appendLog(QStringLiteral("检查失败：关联要素 SHP 无法加载：%1").arg(associatedPath));
                m_run->setEnabled(true);
                return;
            }
            const QgsWkbTypes::GeometryType associatedGeometryType =
                QgsWkbTypes::geometryType(associatedLayer.wkbType());
            if (associatedGeometryType != QgsWkbTypes::PointGeometry
                && associatedGeometryType != QgsWkbTypes::LineGeometry
                && associatedGeometryType != QgsWkbTypes::PolygonGeometry) {
                appendLog(QStringLiteral("检查失败：关联图层仅支持点、线、面要素：%1").arg(associatedPath));
                m_run->setEnabled(true);
                return;
            }
            if (!associatedLayer.crs().isValid()) {
                appendLog(QStringLiteral("检查失败：关联图层缺少有效坐标系：%1").arg(associatedPath));
                m_run->setEnabled(true);
                return;
            }
            QgsCoordinateTransform lineTransform(associatedLayer.crs(), metricCrs,
                                                  QgsProject::instance()->transformContext());
            const bool needsLineTransform = associatedLayer.crs() != metricCrs;
            QgsFeature feature;
            for (QgsFeatureIterator it = associatedLayer.getFeatures(); it.nextFeature(feature);) {
                if (!feature.hasGeometry() || feature.geometry().isEmpty()) continue;
                QgsGeometry associatedGeometry = feature.geometry();
                if (needsLineTransform) associatedGeometry.transform(lineTransform);
                const QgsGeometry featureBuffer = associatedGeometry.buffer(bufferMeters, 8);
                if (featureBuffer.isNull() || featureBuffer.isEmpty()) continue;
                waterBuffer = waterBuffer.isNull() ? featureBuffer : waterBuffer.combine(featureBuffer);
                ++associatedFeatureCount;
            }
        }
        if (associatedFeatureCount == 0 || waterBuffer.isNull() || waterBuffer.isEmpty()) {
            appendLog(QStringLiteral("检查失败：关联图层中没有可用于建立缓冲区的有效点、线或面要素。"));
            m_run->setEnabled(true);
            return;
        }

        QgsSpatialIndex symbolIndex;
        QMap<qint64, SpringPoint> pointByIndexId;
        QList<SpringPoint> candidates;
        QSet<qint64> candidateIndexIds;
        qint64 nextIndexId = 1;
        for (const QString& targetPath : m_targetPaths) {
            QgsVectorLayer targetLayer(targetPath, QFileInfo(targetPath).fileName(), QStringLiteral("ogr"));
            if (!targetLayer.isValid()) {
                appendLog(QStringLiteral("检查失败：待检要素 SHP 无法加载：%1").arg(targetPath));
                m_run->setEnabled(true);
                return;
            }
            const QgsWkbTypes::GeometryType targetGeometryType = QgsWkbTypes::geometryType(targetLayer.wkbType());
            if (targetGeometryType != QgsWkbTypes::LineGeometry
                && targetGeometryType != QgsWkbTypes::PolygonGeometry) {
                appendLog(QStringLiteral("检查失败：解析泉待检图层必须是围成符号面的线要素或面要素：%1").arg(targetPath));
                m_run->setEnabled(true);
                return;
            }
            if (!targetLayer.crs().isValid()) {
                appendLog(QStringLiteral("检查失败：待检要素图层缺少有效坐标系：%1").arg(targetPath));
                m_run->setEnabled(true);
                return;
            }
            const int symbolIdIndex = targetLayer.fields().indexFromName(rule.symbolGroupFieldName);
            if (symbolIdIndex < 0) {
                appendLog(QStringLiteral("检查失败：解析泉图层缺少 XML 指定的符号组合字段“%1”，无法合并同一泉符号的多条路径：%2")
                              .arg(rule.symbolGroupFieldName, targetPath));
                m_run->setEnabled(true);
                return;
            }
            const int elementTypeIndex = targetLayer.fields().indexFromName(QStringLiteral("element_ty"));
            QgsCoordinateTransform targetTransform(targetLayer.crs(), metricCrs,
                                                     QgsProject::instance()->transformContext());
            const bool needsTransform = targetLayer.crs() != metricCrs;
            QMap<QString, SpringPoint> symbolsById;
            QSet<QString> invalidSymbolIds;
            QgsFeature spring;
            for (QgsFeatureIterator it = targetLayer.getFeatures(); it.nextFeature(spring);) {
                if (!spring.hasGeometry() || spring.geometry().isEmpty()) continue;
                if (elementTypeIndex >= 0
                    && spring.attribute(elementTypeIndex).toString().compare(QStringLiteral("path"), Qt::CaseInsensitive) != 0)
                    continue;

                const QString symbolId = spring.attribute(symbolIdIndex).toString().trimmed();
                if (symbolId.isEmpty()) {
                    appendLog(QStringLiteral("跳过 %1 中泉要素 FID %2：符号组合字段 %3 为空，无法确定其符号组成部分。")
                                  .arg(QFileInfo(targetPath).fileName()).arg(spring.id())
                                  .arg(rule.symbolGroupFieldName));
                    continue;
                }

                QgsGeometry componentGeometry = spring.geometry();
                if (needsTransform) componentGeometry.transform(targetTransform);
                QgsGeometry componentArea;
                if (!makeSymbolArea(componentGeometry, componentArea)) {
                    invalidSymbolIds.insert(symbolId);
                    appendLog(QStringLiteral("标记泉符号无效：%1 symbol_id=%2 的组成路径 FID %3 未构成有效闭合面。")
                                  .arg(QFileInfo(targetPath).fileName(), symbolId).arg(spring.id()));
                    continue;
                }

                if (!symbolsById.contains(symbolId)) {
                    SpringPoint symbol;
                    symbol.sourceId = spring.id();
                    symbol.sourceName = QFileInfo(targetPath).fileName();
                    symbol.symbolId = symbolId;
                    symbol.componentFids.append(spring.id());
                    symbol.symbolGeometry = componentArea;
                    symbolsById.insert(symbolId, symbol);
                } else {
                    SpringPoint& symbol = symbolsById[symbolId];
                    const QgsGeometry combinedArea = symbol.symbolGeometry.combine(componentArea);
                    if (combinedArea.isNull() || combinedArea.isEmpty() || !combinedArea.isGeosValid()) {
                        invalidSymbolIds.insert(symbolId);
                        appendLog(QStringLiteral("标记泉符号无效：%1 symbol_id=%2 的多个组成面无法合并。")
                                      .arg(QFileInfo(targetPath).fileName(), symbolId));
                        continue;
                    }
                    symbol.symbolGeometry = combinedArea;
                    symbol.componentFids.append(spring.id());
                }
            }

            for (auto symbolIt = symbolsById.begin(); symbolIt != symbolsById.end(); ++symbolIt) {
                SpringPoint& entry = symbolIt.value();
                if (invalidSymbolIds.contains(entry.symbolId)) {
                    appendLog(QStringLiteral("跳过泉符号：%1 symbol_id=%2 至少有一条组成路径无效。")
                                  .arg(entry.sourceName, entry.symbolId));
                    continue;
                }
                entry.geometry = entry.symbolGeometry.centroid();
                if (entry.geometry.isNull() || entry.geometry.isEmpty()
                    || QgsWkbTypes::geometryType(entry.geometry.wkbType()) != QgsWkbTypes::PointGeometry) {
                    appendLog(QStringLiteral("跳过泉符号：%1 symbol_id=%2 无法从合并符号面计算有效质心。")
                                  .arg(entry.sourceName, entry.symbolId));
                    continue;
                }
                entry.indexId = nextIndexId++;

                QgsFeature indexedSymbol;
                indexedSymbol.setId(entry.indexId);
                indexedSymbol.setGeometry(entry.symbolGeometry);
                symbolIndex.addFeature(indexedSymbol);
                pointByIndexId.insert(entry.indexId, entry);
                // 泉是否进入缓冲区检查范围按整个符号面判断，而不是只看质心是否落入。
                if (waterBuffer.contains(entry.symbolGeometry)) {
                    QStringList componentFidTexts;
                    for (qint64 fid : entry.componentFids) componentFidTexts.append(QString::number(fid));
                    appendLog(QStringLiteral("河源附近泉匹配：%1 symbol_id=%2（组成 FID：%3）的完整符号面落入关联水系缓冲区，纳入间隔检查。")
                                  .arg(entry.sourceName, entry.symbolId, componentFidTexts.join(QStringLiteral(","))));
                    candidates.append(entry);
                    candidateIndexIds.insert(entry.indexId);
                }
            }
        }

        int namedSourceSpringCount = 0;
        int matchedNamedSourceSpringCount = 0;
        constexpr double spatialSearchEpsilonMeters = 1e-6;
        for (const QString& sourcePath : m_sourcePaths) {
            QgsVectorLayer sourceLayer(sourcePath, QFileInfo(sourcePath).fileName(), QStringLiteral("ogr"));
            if (!sourceLayer.isValid()) {
                appendLog(QStringLiteral("检查失败：源 GDB 泉 SHP 无法加载：%1").arg(sourcePath));
                m_run->setEnabled(true);
                return;
            }
            if (QgsWkbTypes::geometryType(sourceLayer.wkbType()) != QgsWkbTypes::PointGeometry) {
                appendLog(QStringLiteral("检查失败：源 GDB 泉图层必须是点要素：%1").arg(sourcePath));
                m_run->setEnabled(true);
                return;
            }
            if (!sourceLayer.crs().isValid()) {
                appendLog(QStringLiteral("检查失败：源 GDB 泉图层缺少有效坐标系：%1").arg(sourcePath));
                m_run->setEnabled(true);
                return;
            }
            const int nameFieldIndex = sourceLayer.fields().indexFromName(QStringLiteral("NAME"));
            if (nameFieldIndex < 0) {
                appendLog(QStringLiteral("检查失败：源 GDB 泉图层缺少 NAME 字段：%1").arg(sourcePath));
                m_run->setEnabled(true);
                return;
            }

            QgsCoordinateTransform sourceTransform(sourceLayer.crs(), metricCrs,
                                                    QgsProject::instance()->transformContext());
            const bool needsTransform = sourceLayer.crs() != metricCrs;
            QgsFeature sourceFeature;
            for (QgsFeatureIterator it = sourceLayer.getFeatures(); it.nextFeature(sourceFeature);) {
                const QVariant nameValue = sourceFeature.attribute(nameFieldIndex);
                if (nameValue.isNull() || nameValue.toString().trimmed().isEmpty()) continue;
                if (!sourceFeature.hasGeometry() || sourceFeature.geometry().isEmpty()) continue;
                if (sourceFeature.geometry().isMultipart()) {
                    appendLog(QStringLiteral("跳过源泉 FID %1：源几何不是单点。")
                                  .arg(sourceFeature.id()));
                    continue;
                }

                ++namedSourceSpringCount;
                QgsGeometry sourcePoint = sourceFeature.geometry();
                if (needsTransform) sourcePoint.transform(sourceTransform);
                const QgsPointXY sourceCenter = sourcePoint.asPoint();
                const QgsRectangle searchRect(sourceCenter.x() - spatialSearchEpsilonMeters,
                                              sourceCenter.y() - spatialSearchEpsilonMeters,
                                              sourceCenter.x() + spatialSearchEpsilonMeters,
                                              sourceCenter.y() + spatialSearchEpsilonMeters);
                QList<qint64> matchedParsedIds;
                const QList<QgsFeatureId> possibleIds = symbolIndex.intersects(searchRect);
                for (QgsFeatureId possibleId : possibleIds) {
                    const SpringPoint parsedSpring = pointByIndexId.value(possibleId);
                    if (sourcePoint.intersects(parsedSpring.symbolGeometry))
                        matchedParsedIds.append(possibleId);
                }

                if (matchedParsedIds.isEmpty()) {
                    appendLog(QStringLiteral("源泉 %1 FID %2（NAME=%3）未落入任何解析泉符号几何范围。")
                                  .arg(QFileInfo(sourcePath).fileName()).arg(sourceFeature.id())
                                  .arg(nameValue.toString()));
                    continue;
                }
                ++matchedNamedSourceSpringCount;
                if (matchedParsedIds.size() > 1) {
                    appendLog(QStringLiteral("空间匹配提示：源泉 %1 FID %2 落入 %3 个解析泉符号几何范围。")
                                  .arg(QFileInfo(sourcePath).fileName()).arg(sourceFeature.id())
                                  .arg(matchedParsedIds.size()));
                }
                for (qint64 parsedId : matchedParsedIds) {
                    const SpringPoint parsedSpring = pointByIndexId.value(parsedId);
                    QStringList componentFidTexts;
                    for (qint64 fid : parsedSpring.componentFids) componentFidTexts.append(QString::number(fid));
                    appendLog(QStringLiteral("空间匹配成功：源 GDB 泉 %1 FID %2（NAME=%3）落在解析泉 %4 symbol_id=%5（组成 FID：%6）的合并符号范围内，该泉参与间隔检查。")
                                  .arg(QFileInfo(sourcePath).fileName()).arg(sourceFeature.id())
                                  .arg(nameValue.toString()).arg(parsedSpring.sourceName)
                                  .arg(parsedSpring.symbolId, componentFidTexts.join(QStringLiteral(","))));
                    if (candidateIndexIds.contains(parsedId)) continue;
                    candidates.append(parsedSpring);
                    candidateIndexIds.insert(parsedId);
                }
            }
        }

        appendLog(QStringLiteral("已处理 %1 个关联图层中的 %2 个要素、%3 个待检图层；河流缓冲区内或包含 NAME 非空源泉点的解析泉共 %4 个。源泉 NAME 非空 %5 个，匹配到解析泉符号 %6 个。")
                      .arg(m_associatedPaths.size()).arg(associatedFeatureCount).arg(m_targetPaths.size())
                      .arg(candidates.size()).arg(namedSourceSpringCount).arg(matchedNamedSourceSpringCount));
        QgsSpatialIndex candidateIndex;
        for (const SpringPoint& candidate : candidates) {
            QgsFeature indexedCandidate;
            indexedCandidate.setId(candidate.indexId);
            indexedCandidate.setGeometry(candidate.geometry);
            candidateIndex.addFeature(indexedCandidate);
        }

        const QgsFields errorFields = springSymbolErrorFields();
        QList<QgsFeature> errorFeatures;
        int violationPairs = 0;
        QSet<QString> reportedPairs;
        for (const SpringPoint& point : candidates) {
            const QgsPointXY center = point.geometry.asPoint();
            const QgsRectangle searchRect(center.x() - spacingMeters, center.y() - spacingMeters,
                                          center.x() + spacingMeters, center.y() + spacingMeters);
            const QList<QgsFeatureId> nearbyIds = candidateIndex.intersects(searchRect);
            for (QgsFeatureId nearbyId : nearbyIds) {
                if (nearbyId == point.indexId) continue;
                const SpringPoint other = pointByIndexId.value(nearbyId);
                const double distance = point.geometry.distance(other.geometry);
                if (distance < spacingMeters) {
                    const qint64 firstId = qMin(point.indexId, other.indexId);
                    const qint64 secondId = qMax(point.indexId, other.indexId);
                    const QString pairKey = QStringLiteral("%1:%2").arg(firstId).arg(secondId);
                    if (reportedPairs.contains(pairKey)) continue;
                    reportedPairs.insert(pairKey);
                    ++violationPairs;
                    errorFeatures.append(makeSpringSymbolErrorFeature(
                        errorFields, QStringLiteral("泉"), point.sourceName, point.sourceId, point.symbolId,
                        point.symbolGeometry, other.sourceName, other.sourceId, other.symbolId,
                        distance, spacingMeters));
                    errorFeatures.append(makeSpringSymbolErrorFeature(
                        errorFields, QStringLiteral("泉"), other.sourceName, other.sourceId, other.symbolId,
                        other.symbolGeometry, point.sourceName, point.sourceId, point.symbolId,
                        distance, spacingMeters));
                    appendLog(QStringLiteral("不符合：%1 中泉符号 symbol_id=%2（代表 FID %3）与 %4 中泉符号 symbol_id=%5（代表 FID %6）质心距 %7 m，小于要求的 %8 m。")
                                  .arg(point.sourceName).arg(point.symbolId).arg(point.sourceId)
                                  .arg(other.sourceName).arg(other.symbolId).arg(other.sourceId)
                                  .arg(distance, 0, 'f', 3).arg(spacingMeters, 0, 'f', 3));
                }
            }
        }

        m_problemCount = violationPairs;
        QString outputError;
        QStringList protectedPaths = m_sourcePaths + m_associatedPaths + m_targetPaths + m_xmlPaths;
        if (errorFeatures.isEmpty()) {
            appendLog(QStringLiteral("本次未发现间隔错误，不生成错误结果图层。若同名旧图层已存在，将保持不变。"));
        } else if (writeSpacingErrorLayer(this, m_outputFilePath, metricCrs, errorFields,
                                          errorFeatures, QgsWkbTypes::MultiPolygon,
                                          protectedPaths, outputError, m_taskManagerMode)) {
            m_outputWritten = true;
            appendLog(QStringLiteral("错误结果图层已输出：%1（%2 对错误，共 %3 个完整泉符号面要素；每个违规泉对各输出两侧符号）。")
                          .arg(QDir::toNativeSeparators(m_outputFilePath))
                          .arg(violationPairs).arg(errorFeatures.size()));
        } else {
            appendLog(QStringLiteral("错误结果图层输出失败：%1").arg(outputError));
        }

        if (candidates.isEmpty())
            appendLog(QStringLiteral("检查结束：没有泉落入河流缓冲区，也没有 NAME 非空源泉匹配到解析符号，未执行间隔判定。"));
        else if (violationPairs == 0)
            appendLog(QStringLiteral("检查完成：参与检查的解析泉中心之间未发现小于规定间隔的点对。"));
        else
            appendLog(QStringLiteral("检查完成：发现 %1 对待检要素间隔不符合规则。相关密度数量规则未在本界面检查。")
                          .arg(violationPairs));
    }
    catch (const QgsCsException& exception) {
        appendLog(QStringLiteral("检查失败：坐标转换异常：%1").arg(exception.what()));
    }
    catch (const std::exception& exception) {
        appendLog(QStringLiteral("检查失败：%1").arg(QString::fromLocal8Bit(exception.what())));
    }
    m_run->setEnabled(true);
}

void FeatureSpacingCheckDialog::runVillageCheck()
{
    struct VillageSymbol {
        qint64 indexId = 0;
        qint64 sourceId = 0;
        QString sourceName;
        QString symbolId;
        QList<qint64> componentFids;
        QgsGeometry componentGeometry;
        QgsGeometry symbolGeometry;
        QgsGeometry center;
    };

    m_run->setEnabled(false);
    appendLog(QStringLiteral("开始行政村间隔检查。"));

    const int scaleDenominator = m_scale->value();
    if (scaleDenominator <= 0) {
        appendLog(QStringLiteral("检查失败：请填写比例尺分母。"));
        m_run->setEnabled(true);
        return;
    }
    if (m_associatedPaths.isEmpty()) {
        appendLog(QStringLiteral("检查失败：请上传“行政区划面普色”关联图层。"));
        m_run->setEnabled(true);
        return;
    }

    SpacingRule rule;
    QString error;
    if (!readRules(m_xmlPaths.first(), rule, error, false, true)) {
        appendLog(QStringLiteral("检查失败：%1").arg(error));
        m_run->setEnabled(true);
        return;
    }

    QString districtPath;
    for (const QString& path : m_associatedPaths) {
        const QString baseName = QFileInfo(path).completeBaseName();
        if (baseName.contains(QStringLiteral("行政区划面普色"))
            || baseName.contains(QStringLiteral("行政区普色"))) {
            if (!districtPath.isEmpty()) {
                appendLog(QStringLiteral("检查失败：上传了多个行政区普色面图层，请只保留一个。"));
                m_run->setEnabled(true);
                return;
            }
            districtPath = path;
        }
    }
    if (districtPath.isEmpty()) {
        appendLog(QStringLiteral("检查失败：请上传文件名包含“行政区划面普色”或“行政区普色”的关联图层。"));
        m_run->setEnabled(true);
        return;
    }

    QgsVectorLayer districtLayer(districtPath, QStringLiteral("行政区划面普色"), QStringLiteral("ogr"));
    if (!districtLayer.isValid()) {
        appendLog(QStringLiteral("检查失败：无法加载行政区划面普色 SHP：%1").arg(districtPath));
        m_run->setEnabled(true);
        return;
    }
    if (QgsWkbTypes::geometryType(districtLayer.wkbType()) != QgsWkbTypes::PolygonGeometry) {
        appendLog(QStringLiteral("检查失败：行政区划面普色必须是面图层。"));
        m_run->setEnabled(true);
        return;
    }
    const QgsCoordinateReferenceSystem metricCrs = districtLayer.crs();
    if (!metricCrs.isValid() || metricCrs.mapUnits() != QgsUnitTypes::DistanceMeters) {
        appendLog(QStringLiteral("检查失败：行政区划面普色必须使用以米为单位的投影坐标系。"));
        m_run->setEnabled(true);
        return;
    }
    const double spacingMeters = rule.spacingCentimeters * scaleDenominator / 100.0;
    appendLog(QStringLiteral("规则：行政村符号中心最小间隔 %1 cm（实地 %2 m）；符号按字段 %3 组合，中心取组合符号面的质心。")
                  .arg(rule.spacingCentimeters).arg(spacingMeters, 0, 'f', 3)
                  .arg(rule.symbolGroupFieldName));

    try {
        auto normalizePolygon = [](QgsGeometry& geometry) {
            if (geometry.isNull() || geometry.isEmpty()) return false;
            if (!geometry.isGeosValid())
                geometry = geometry.makeValid();
            geometry.convertGeometryCollectionToSubclass(QgsWkbTypes::PolygonGeometry);
            return !geometry.isNull() && !geometry.isEmpty()
                && QgsWkbTypes::geometryType(geometry.wkbType()) == QgsWkbTypes::PolygonGeometry
                && geometry.isGeosValid();
        };

        QgsGeometry districtArea;
        int districtFeatureCount = 0;
        QgsCoordinateTransform districtTransform(districtLayer.crs(), metricCrs,
                                                  QgsProject::instance()->transformContext());
        const bool transformDistrict = districtLayer.crs() != metricCrs;
        QgsFeature feature;
        for (QgsFeatureIterator it = districtLayer.getFeatures(); it.nextFeature(feature);) {
            if (!feature.hasGeometry() || feature.geometry().isEmpty()) continue;
            QgsGeometry geometry = feature.geometry();
            if (transformDistrict) geometry.transform(districtTransform);
            const bool neededRepair = !geometry.isGeosValid();
            if (!normalizePolygon(geometry)) {
                appendLog(QStringLiteral("检查失败：行政区划面普色 FID %1 无效，自动修复未能得到有效面。").arg(feature.id()));
                m_run->setEnabled(true);
                return;
            }
            if (neededRepair)
                appendLog(QStringLiteral("已修复行政区划面普色 FID %1 的无效面几何。").arg(feature.id()));
            districtArea = districtArea.isNull() ? geometry : districtArea.combine(geometry);
            if (!normalizePolygon(districtArea)) {
                appendLog(QStringLiteral("检查失败：行政区划面普色要素无法合并。"));
                m_run->setEnabled(true);
                return;
            }
            ++districtFeatureCount;
        }
        if (districtFeatureCount == 0 || districtArea.isNull()) {
            appendLog(QStringLiteral("检查失败：行政区划面普色中没有有效面要素。"));
            m_run->setEnabled(true);
            return;
        }
        appendLog(QStringLiteral("行政区普色范围已合并：%1 个有效面；完整落在行政区普色内部的行政村符号将排除，其余符号作为外区参与间隔检查。")
                      .arg(districtFeatureCount));

        QMap<qint64, VillageSymbol> symbolsByIndexId;
        QList<VillageSymbol> candidates;
        qint64 nextIndexId = 1;
        int targetFeatureCount = 0;
        for (const QString& targetPath : m_targetPaths) {
            QgsVectorLayer targetLayer(targetPath, QFileInfo(targetPath).fileName(), QStringLiteral("ogr"));
            if (!targetLayer.isValid()) {
                appendLog(QStringLiteral("检查失败：待检行政村 SHP 无法加载：%1").arg(targetPath));
                m_run->setEnabled(true);
                return;
            }
            const QgsWkbTypes::GeometryType targetGeometryType = QgsWkbTypes::geometryType(targetLayer.wkbType());
            if (targetGeometryType != QgsWkbTypes::LineGeometry
                && targetGeometryType != QgsWkbTypes::PolygonGeometry) {
                appendLog(QStringLiteral("检查失败：行政村符号图层必须是围成符号面的线要素或面要素：%1").arg(targetPath));
                m_run->setEnabled(true);
                return;
            }
            if (!targetLayer.crs().isValid()) {
                appendLog(QStringLiteral("检查失败：待检行政村图层缺少有效坐标系：%1").arg(targetPath));
                m_run->setEnabled(true);
                return;
            }
            const int symbolIdIndex = targetLayer.fields().indexFromName(rule.symbolGroupFieldName);
            if (symbolIdIndex < 0) {
                appendLog(QStringLiteral("检查失败：行政村图层缺少 XML 指定的符号组合字段“%1”：%2")
                              .arg(rule.symbolGroupFieldName, targetPath));
                m_run->setEnabled(true);
                return;
            }
            const int elementTypeIndex = targetLayer.fields().indexFromName(QStringLiteral("element_ty"));
            QgsCoordinateTransform targetTransform(targetLayer.crs(), metricCrs,
                                                    QgsProject::instance()->transformContext());
            const bool needsTransform = targetLayer.crs() != metricCrs;
            QMap<QString, VillageSymbol> groupedSymbols;
            QSet<QString> invalidSymbolIds;
            QgsFeature targetFeature;
            for (QgsFeatureIterator it = targetLayer.getFeatures(); it.nextFeature(targetFeature);) {
                if (!targetFeature.hasGeometry() || targetFeature.geometry().isEmpty()) continue;
                if (elementTypeIndex >= 0
                    && targetFeature.attribute(elementTypeIndex).toString().compare(QStringLiteral("path"), Qt::CaseInsensitive) != 0)
                    continue;
                ++targetFeatureCount;
                const QString symbolId = targetFeature.attribute(symbolIdIndex).toString().trimmed();
                if (symbolId.isEmpty()) {
                    appendLog(QStringLiteral("跳过 %1 FID %2：符号组合字段 %3 为空。")
                                  .arg(QFileInfo(targetPath).fileName()).arg(targetFeature.id())
                                  .arg(rule.symbolGroupFieldName));
                    continue;
                }
                QgsGeometry component = targetFeature.geometry();
                if (needsTransform) component.transform(targetTransform);
                if (!groupedSymbols.contains(symbolId)) {
                    VillageSymbol symbol;
                    symbol.sourceId = targetFeature.id();
                    symbol.sourceName = QFileInfo(targetPath).fileName();
                    symbol.symbolId = symbolId;
                    symbol.componentFids.append(targetFeature.id());
                    symbol.componentGeometry = component;
                    groupedSymbols.insert(symbolId, symbol);
                } else {
                    VillageSymbol& symbol = groupedSymbols[symbolId];
                    const QgsGeometry combined = symbol.componentGeometry.combine(component);
                    if (combined.isNull() || combined.isEmpty()) {
                        invalidSymbolIds.insert(symbolId);
                        appendLog(QStringLiteral("标记行政村符号无效：%1 symbol_id=%2 的组成面无法合并。")
                                      .arg(QFileInfo(targetPath).fileName(), symbolId));
                        continue;
                    }
                    symbol.componentGeometry = combined;
                    symbol.componentFids.append(targetFeature.id());
                }
            }

            for (auto symbolIt = groupedSymbols.begin(); symbolIt != groupedSymbols.end(); ++symbolIt) {
                VillageSymbol& symbol = symbolIt.value();
                if (invalidSymbolIds.contains(symbol.symbolId)) {
                    appendLog(QStringLiteral("跳过行政村符号：%1 symbol_id=%2 至少有一条组成路径无效。")
                                  .arg(symbol.sourceName, symbol.symbolId));
                    continue;
                }
                if (!makeSymbolArea(symbol.componentGeometry, symbol.symbolGeometry)) {
                    appendLog(QStringLiteral("跳过行政村符号：%1 symbol_id=%2 的全部路径合并后仍未能构成有效符号面。")
                                  .arg(symbol.sourceName, symbol.symbolId));
                    continue;
                }
                symbol.center = symbol.symbolGeometry.centroid();
                if (symbol.center.isNull() || symbol.center.isEmpty()
                    || QgsWkbTypes::geometryType(symbol.center.wkbType()) != QgsWkbTypes::PointGeometry) {
                    appendLog(QStringLiteral("跳过行政村符号：%1 symbol_id=%2 无法计算有效质心。")
                                  .arg(symbol.sourceName, symbol.symbolId));
                    continue;
                }
                symbol.indexId = nextIndexId++;
                symbolsByIndexId.insert(symbol.indexId, symbol);
                // 以完整组合符号面判断是否位于行政区普色内部；内部符号排除，其余作为外区符号。
                if (!districtArea.contains(symbol.symbolGeometry)) {
                    candidates.append(symbol);
                    QStringList componentFidTexts;
                    for (qint64 fid : symbol.componentFids)
                        componentFidTexts.append(QString::number(fid));
                    appendLog(QStringLiteral("外区行政村参与检查：%1 symbol_id=%2（组成 FID：%3）的完整符号面不在行政区普色内部。")
                                  .arg(symbol.sourceName, symbol.symbolId)
                                  .arg(componentFidTexts.join(QStringLiteral(","))));
                } else {
                    appendLog(QStringLiteral("排除行政区内部行政村：%1 symbol_id=%2 的完整符号面位于行政区普色内部。")
                                  .arg(symbol.sourceName, symbol.symbolId));
                }
            }
        }

        appendLog(QStringLiteral("已处理行政区普色 %1 个有效面、%2 个待检图层中的 %3 个路径要素；行政区内部符号已排除，外区参与间隔检查的行政村符号 %4 个。")
                      .arg(districtFeatureCount).arg(m_targetPaths.size())
                      .arg(targetFeatureCount).arg(candidates.size()));

        QgsSpatialIndex centerIndex;
        for (const VillageSymbol& symbol : candidates) {
            QgsFeature indexedCenter;
            indexedCenter.setId(symbol.indexId);
            indexedCenter.setGeometry(symbol.center);
            centerIndex.addFeature(indexedCenter);
        }
        const QgsFields errorFields = springSymbolErrorFields();
        QList<QgsFeature> errorFeatures;
        int violationPairs = 0;
        QSet<QString> reportedPairs;
        for (const VillageSymbol& symbol : candidates) {
            const QgsPointXY center = symbol.center.asPoint();
            const QgsRectangle searchRect(center.x() - spacingMeters, center.y() - spacingMeters,
                                          center.x() + spacingMeters, center.y() + spacingMeters);
            const QList<QgsFeatureId> nearbyIds = centerIndex.intersects(searchRect);
            for (QgsFeatureId nearbyId : nearbyIds) {
                if (nearbyId == symbol.indexId) continue;
                const VillageSymbol other = symbolsByIndexId.value(nearbyId);
                const double distance = symbol.center.distance(other.center);
                if (distance >= spacingMeters) continue;
                const qint64 firstId = qMin(symbol.indexId, other.indexId);
                const qint64 secondId = qMax(symbol.indexId, other.indexId);
                const QString pairKey = QStringLiteral("%1:%2").arg(firstId).arg(secondId);
                if (reportedPairs.contains(pairKey)) continue;
                reportedPairs.insert(pairKey);
                ++violationPairs;
                errorFeatures.append(makeSpringSymbolErrorFeature(
                    errorFields, QStringLiteral("行政村"), symbol.sourceName, symbol.sourceId, symbol.symbolId,
                    symbol.symbolGeometry, other.sourceName, other.sourceId, other.symbolId,
                    distance, spacingMeters));
                errorFeatures.append(makeSpringSymbolErrorFeature(
                    errorFields, QStringLiteral("行政村"), other.sourceName, other.sourceId, other.symbolId,
                    other.symbolGeometry, symbol.sourceName, symbol.sourceId, symbol.symbolId,
                    distance, spacingMeters));
                appendLog(QStringLiteral("间隔不符合：行政村 %1 symbol_id=%2（代表 FID %3）与 %4 symbol_id=%5（代表 FID %6）质心距 %7 m，小于要求的 %8 m。")
                              .arg(symbol.sourceName).arg(symbol.symbolId).arg(symbol.sourceId)
                              .arg(other.sourceName).arg(other.symbolId).arg(other.sourceId)
                              .arg(distance, 0, 'f', 3).arg(spacingMeters, 0, 'f', 3));
            }
        }

        m_problemCount = violationPairs;
        QString outputError;
        const QStringList protectedPaths = m_associatedPaths + m_targetPaths + m_xmlPaths;
        if (errorFeatures.isEmpty()) {
            appendLog(QStringLiteral("本次未发现行政村间隔错误，不生成错误结果图层。若同名旧图层已存在，将保持不变。"));
        } else if (writeSpacingErrorLayer(this, m_outputFilePath, metricCrs, errorFields,
                                          errorFeatures, QgsWkbTypes::MultiPolygon,
                                          protectedPaths, outputError, m_taskManagerMode)) {
            m_outputWritten = true;
            appendLog(QStringLiteral("行政村错误结果图层已输出：%1（%2 对错误，共 %3 个完整行政村符号面要素；每个违规符号对输出两侧符号）。")
                          .arg(QDir::toNativeSeparators(m_outputFilePath)).arg(violationPairs).arg(errorFeatures.size()));
        } else {
            appendLog(QStringLiteral("错误结果图层输出失败：%1").arg(outputError));
        }
        if (candidates.isEmpty())
            appendLog(QStringLiteral("检查结束：没有可参与检查的外区行政村符号，未执行间隔判断。"));
        else if (violationPairs == 0)
            appendLog(QStringLiteral("检查完成：参与检查的行政村符号质心之间未发现小于规定间隔的点对。"));
        else
            appendLog(QStringLiteral("检查完成：发现 %1 对行政村符号间隔不符合规则。密度数量规则未在本界面检查。")
                          .arg(violationPairs));
    }
    catch (const QgsCsException& exception) {
        appendLog(QStringLiteral("检查失败：坐标转换异常：%1").arg(exception.what()));
    }
    catch (const std::exception& exception) {
        appendLog(QStringLiteral("检查失败：%1").arg(QString::fromLocal8Bit(exception.what())));
    }
    m_run->setEnabled(true);
}

void FeatureSpacingCheckDialog::runWellCheck()
{
    struct ParsedWellCircle {
        qint64 indexId = 0;
        qint64 sourceId = 0;
        QString sourceName;
        QgsGeometry geometry;
        QgsGeometry center;
    };

    m_run->setEnabled(false);
    appendLog(QStringLiteral("开始井间隔检查：源井按 NAME 非空筛选，解析井按圆几何匹配。"), m_log);

    const int scaleDenominator = m_scale->value();
    if (scaleDenominator <= 0) {
        appendLog(QStringLiteral("检查失败：请填写比例尺分母。"), m_log);
        m_run->setEnabled(true);
        return;
    }

    SpacingRule rule;
    QString error;
    if (!readRules(m_xmlPaths.first(), rule, error, false, false)) {
        appendLog(QStringLiteral("检查失败：%1").arg(error), m_log);
        m_run->setEnabled(true);
        return;
    }
    QgsVectorLayer firstParsedLayer(m_targetPaths.first(), QStringLiteral("解析后的井"), QStringLiteral("ogr"));
    if (!firstParsedLayer.isValid()) {
        appendLog(QStringLiteral("检查失败：解析后的井 SHP 无法加载：%1").arg(m_targetPaths.first()), m_log);
        m_run->setEnabled(true);
        return;
    }
    const QgsWkbTypes::GeometryType firstParsedGeometryType =
        QgsWkbTypes::geometryType(firstParsedLayer.wkbType());
    if (firstParsedGeometryType != QgsWkbTypes::LineGeometry
        && firstParsedGeometryType != QgsWkbTypes::PolygonGeometry) {
        appendLog(QStringLiteral("检查失败：解析后的井图层应为围成圆圈的线要素或面要素：%1")
                      .arg(m_targetPaths.first()), m_log);
        m_run->setEnabled(true);
        return;
    }
    const QgsCoordinateReferenceSystem metricCrs = firstParsedLayer.crs();
    if (!metricCrs.isValid() || metricCrs.mapUnits() != QgsUnitTypes::DistanceMeters) {
        appendLog(QStringLiteral("检查失败：解析后的井图层需使用以米为单位的投影坐标系：%1")
                      .arg(m_targetPaths.first()), m_log);
        m_run->setEnabled(true);
        return;
    }

    const double spacingMeters = rule.spacingCentimeters * scaleDenominator / 100.0;
    appendLog(QStringLiteral("间隔阈值：%1 cm，按比例尺换算为 %2 m；圆心由解析井闭合符号面的质心计算。")
                  .arg(rule.spacingCentimeters).arg(spacingMeters, 0, 'f', 3), m_log);

    try {
        QgsSpatialIndex circleIndex;
        QMap<qint64, ParsedWellCircle> circlesByIndexId;
        qint64 nextCircleIndexId = 1;
        int circleCount = 0;

        for (const QString& parsedPath : m_targetPaths) {
            QgsVectorLayer parsedLayer(parsedPath, QFileInfo(parsedPath).fileName(), QStringLiteral("ogr"));
            if (!parsedLayer.isValid()) {
                appendLog(QStringLiteral("检查失败：解析后的井 SHP 无法加载：%1").arg(parsedPath), m_log);
                m_run->setEnabled(true);
                return;
            }
            const QgsWkbTypes::GeometryType parsedLayerGeometryType =
                QgsWkbTypes::geometryType(parsedLayer.wkbType());
            if (parsedLayerGeometryType != QgsWkbTypes::LineGeometry
                && parsedLayerGeometryType != QgsWkbTypes::PolygonGeometry) {
                appendLog(QStringLiteral("检查失败：解析后的井图层应为围成圆圈的线要素或面要素：%1")
                              .arg(parsedPath), m_log);
                m_run->setEnabled(true);
                return;
            }
            if (!parsedLayer.crs().isValid()) {
                appendLog(QStringLiteral("检查失败：解析后的井图层缺少有效坐标系：%1").arg(parsedPath), m_log);
                m_run->setEnabled(true);
                return;
            }
            QgsCoordinateTransform parsedTransform(parsedLayer.crs(), metricCrs,
                                                    QgsProject::instance()->transformContext());
            const bool needsTransform = parsedLayer.crs() != metricCrs;
            QgsFeature parsedFeature;
            for (QgsFeatureIterator it = parsedLayer.getFeatures(); it.nextFeature(parsedFeature);) {
                if (!parsedFeature.hasGeometry() || parsedFeature.geometry().isEmpty()) continue;
                const QgsWkbTypes::GeometryType parsedGeometryType =
                    QgsWkbTypes::geometryType(parsedFeature.geometry().wkbType());
                if (parsedGeometryType != QgsWkbTypes::LineGeometry
                    && parsedGeometryType != QgsWkbTypes::PolygonGeometry) {
                    appendLog(QStringLiteral("跳过 %1 中非线/面要素 FID %2。")
                                  .arg(QFileInfo(parsedPath).fileName()).arg(parsedFeature.id()), m_log);
                    continue;
                }

                QgsGeometry parsedBoundary = parsedFeature.geometry();
                if (needsTransform) parsedBoundary.transform(parsedTransform);
                QgsGeometry circleGeometry;
                if (!makeSymbolArea(parsedBoundary, circleGeometry)) {
                    appendLog(QStringLiteral("跳过 %1 中井要素 FID %2：线要素未构成有效闭合面，无法计算中心点。")
                                  .arg(QFileInfo(parsedPath).fileName()).arg(parsedFeature.id()), m_log);
                    continue;
                }
                QgsGeometry centerGeometry = circleGeometry.centroid();
                if (centerGeometry.isNull() || centerGeometry.isEmpty()
                    || QgsWkbTypes::geometryType(centerGeometry.wkbType()) != QgsWkbTypes::PointGeometry) {
                    appendLog(QStringLiteral("跳过 %1 中井要素 FID %2：无法从符号面计算有效质心。")
                                  .arg(QFileInfo(parsedPath).fileName()).arg(parsedFeature.id()), m_log);
                    continue;
                }

                ParsedWellCircle circle;
                circle.indexId = nextCircleIndexId++;
                circle.sourceId = parsedFeature.id();
                circle.sourceName = QFileInfo(parsedPath).fileName();
                circle.geometry = circleGeometry;
                circle.center = centerGeometry;

                QgsFeature indexedCircle;
                indexedCircle.setId(circle.indexId);
                indexedCircle.setGeometry(circle.geometry);
                circleIndex.addFeature(indexedCircle);
                circlesByIndexId.insert(circle.indexId, circle);
                ++circleCount;
            }
        }

        if (circleCount == 0) {
            appendLog(QStringLiteral("检查失败：没有读取到可计算有效质心的解析井符号面。"), m_log);
            m_run->setEnabled(true);
            return;
        }

        QSet<qint64> matchedCircleIds;
        QMap<qint64, int> sourceCountByCircleId;
        QMap<qint64, QStringList> sourceMatchesByCircleId;
        int namedSourceWellCount = 0;
        int matchedSourceWellCount = 0;
        for (const QString& sourcePath : m_sourcePaths) {
            QgsVectorLayer sourceLayer(sourcePath, QFileInfo(sourcePath).fileName(), QStringLiteral("ogr"));
            if (!sourceLayer.isValid()) {
                appendLog(QStringLiteral("检查失败：源 GDB 井 SHP 无法加载：%1").arg(sourcePath), m_log);
                m_run->setEnabled(true);
                return;
            }
            if (QgsWkbTypes::geometryType(sourceLayer.wkbType()) != QgsWkbTypes::PointGeometry) {
                appendLog(QStringLiteral("检查失败：源 GDB 井图层必须是点要素：%1").arg(sourcePath), m_log);
                m_run->setEnabled(true);
                return;
            }
            if (!sourceLayer.crs().isValid()) {
                appendLog(QStringLiteral("检查失败：源 GDB 井图层缺少有效坐标系：%1").arg(sourcePath), m_log);
                m_run->setEnabled(true);
                return;
            }
            const int nameFieldIndex = sourceLayer.fields().indexFromName(QStringLiteral("NAME"));
            if (nameFieldIndex < 0) {
                appendLog(QStringLiteral("检查失败：源 GDB 井图层缺少 NAME 字段：%1").arg(sourcePath), m_log);
                m_run->setEnabled(true);
                return;
            }

            QgsCoordinateTransform sourceTransform(sourceLayer.crs(), metricCrs,
                                                    QgsProject::instance()->transformContext());
            const bool needsTransform = sourceLayer.crs() != metricCrs;
            QgsFeature sourceFeature;
            for (QgsFeatureIterator it = sourceLayer.getFeatures(); it.nextFeature(sourceFeature);) {
                const QVariant nameValue = sourceFeature.attribute(nameFieldIndex);
                if (nameValue.isNull() || nameValue.toString().trimmed().isEmpty()) continue;
                if (!sourceFeature.hasGeometry() || sourceFeature.geometry().isEmpty()) continue;
                if (sourceFeature.geometry().isMultipart()) {
                    appendLog(QStringLiteral("跳过源井 FID %1：井几何不是单点。")
                                  .arg(sourceFeature.id()), m_log);
                    continue;
                }

                ++namedSourceWellCount;
                QgsGeometry sourcePoint = sourceFeature.geometry();
                if (needsTransform) sourcePoint.transform(sourceTransform);
                const QgsPointXY point = sourcePoint.asPoint();
                const QgsRectangle pointRect(point.x(), point.y(), point.x(), point.y());
                QList<qint64> matches;
                const QList<QgsFeatureId> candidateIds = circleIndex.intersects(pointRect);
                for (QgsFeatureId candidateId : candidateIds) {
                    const ParsedWellCircle circle = circlesByIndexId.value(candidateId);
                    if (sourcePoint.intersects(circle.geometry)) matches.append(candidateId);
                }

                if (matches.isEmpty()) {
                    appendLog(QStringLiteral("源井 %1 FID %2（NAME=%3）未落入解析井圆，未计入井间隔检查。")
                                  .arg(QFileInfo(sourcePath).fileName()).arg(sourceFeature.id())
                                  .arg(nameValue.toString()), m_log);
                    continue;
                }

                ++matchedSourceWellCount;
                if (matches.size() > 1) {
                    appendLog(QStringLiteral("空间匹配提示：源井 %1 FID %2 落入 %3 个解析井圆，可能存在圆重叠。")
                                  .arg(QFileInfo(sourcePath).fileName()).arg(sourceFeature.id()).arg(matches.size()), m_log);
                }
                for (qint64 circleId : matches) {
                    matchedCircleIds.insert(circleId);
                    sourceCountByCircleId[circleId] = sourceCountByCircleId.value(circleId) + 1;
                    sourceMatchesByCircleId[circleId].append(
                        QStringLiteral("%1 FID %2（NAME=%3）")
                            .arg(QFileInfo(sourcePath).fileName())
                            .arg(sourceFeature.id())
                            .arg(nameValue.toString()));
                }
            }
        }

        for (auto it = circlesByIndexId.constBegin(); it != circlesByIndexId.constEnd(); ++it) {
            if (!matchedCircleIds.contains(it.key())) {
                appendLog(QStringLiteral("解析井圆 %1 FID %2 未包含任何 NAME 非空的源井点，未计入井间隔检查。")
                              .arg(it.value().sourceName).arg(it.value().sourceId), m_log);
            } else {
                appendLog(QStringLiteral("参与井间隔检查：解析井圆 %1 FID %2；匹配源井：%3。")
                              .arg(it.value().sourceName).arg(it.value().sourceId)
                              .arg(sourceMatchesByCircleId.value(it.key()).join(QStringLiteral("；"))), m_log);
                if (sourceCountByCircleId.value(it.key()) > 1) {
                    appendLog(QStringLiteral("空间匹配提示：解析井圆 %1 FID %2 内包含 %3 个 NAME 非空的源井点。")
                                  .arg(it.value().sourceName).arg(it.value().sourceId)
                                  .arg(sourceCountByCircleId.value(it.key())), m_log);
                }
            }
        }

        QgsSpatialIndex centerIndex;
        QMap<qint64, ParsedWellCircle> matchedCircles;
        for (qint64 circleId : matchedCircleIds) {
            const ParsedWellCircle circle = circlesByIndexId.value(circleId);
            QgsFeature indexedCenter;
            indexedCenter.setId(circleId);
            indexedCenter.setGeometry(circle.center);
            centerIndex.addFeature(indexedCenter);
            matchedCircles.insert(circleId, circle);
        }

        const QgsFields errorFields = spacingErrorFields();
        QList<QgsFeature> errorFeatures;
        int violationPairs = 0;
        QSet<QString> reportedPairs;
        for (auto it = matchedCircles.constBegin(); it != matchedCircles.constEnd(); ++it) {
            const ParsedWellCircle& circle = it.value();
            const QgsPointXY center = circle.center.asPoint();
            const QgsRectangle searchRect(center.x() - spacingMeters, center.y() - spacingMeters,
                                          center.x() + spacingMeters, center.y() + spacingMeters);
            const QList<QgsFeatureId> nearbyIds = centerIndex.intersects(searchRect);
            for (QgsFeatureId nearbyId : nearbyIds) {
                if (nearbyId <= circle.indexId) continue;
                const ParsedWellCircle other = matchedCircles.value(nearbyId);
                const double distance = circle.center.distance(other.center);
                if (distance < spacingMeters) {
                    const QString pairKey = QStringLiteral("%1:%2").arg(circle.indexId).arg(other.indexId);
                    if (reportedPairs.contains(pairKey)) continue;
                    reportedPairs.insert(pairKey);
                    ++violationPairs;
                    errorFeatures.append(makeSpacingErrorFeature(
                        errorFields, QStringLiteral("井"), circle.sourceName, circle.sourceId,
                        other.sourceName, other.sourceId, circle.center, other.center,
                        distance, spacingMeters));
                    appendLog(QStringLiteral("间隔不符合：解析井 %1 FID %2 与 %3 FID %4 的符号面质心距 %5 m，小于要求的 %6 m。")
                                  .arg(circle.sourceName).arg(circle.sourceId)
                                  .arg(other.sourceName).arg(other.sourceId)
                                  .arg(distance, 0, 'f', 3).arg(spacingMeters, 0, 'f', 3), m_log);
                }
            }
        }

        QString outputError;
        m_problemCount = violationPairs;
        const QStringList protectedPaths = m_sourcePaths + m_targetPaths + m_xmlPaths;
        if (errorFeatures.isEmpty()) {
            appendLog(QStringLiteral("本次未发现井间隔错误，不生成错误结果图层。若同名旧图层已存在，将保持不变。"), m_log);
        } else if (writeSpacingErrorLayer(this, m_outputFilePath, metricCrs, errorFields,
                                          errorFeatures, QgsWkbTypes::LineString,
                                          protectedPaths, outputError, m_taskManagerMode)) {
            m_outputWritten = true;
            appendLog(QStringLiteral("错误结果图层已输出：%1（%2 条错误线）。")
                          .arg(QDir::toNativeSeparators(m_outputFilePath)).arg(errorFeatures.size()), m_log);
        } else {
            appendLog(QStringLiteral("错误结果图层输出失败：%1").arg(outputError), m_log);
        }

        appendLog(QStringLiteral("匹配统计：NAME 非空源井 %1 个，其中落入解析圆 %2 个；解析井圆 %3 个，匹配后参与间隔检查 %4 个。")
                      .arg(namedSourceWellCount).arg(matchedSourceWellCount)
                      .arg(circleCount).arg(matchedCircles.size()), m_log);
        if (matchedCircles.isEmpty())
            appendLog(QStringLiteral("检查结束：没有解析井圆匹配到 NAME 非空的源井，无法进行井间隔判断。"), m_log);
        else if (violationPairs == 0)
            appendLog(QStringLiteral("检查完成：已匹配的解析井圆中心之间未发现小于规定间隔的点对。"), m_log);
        else
            appendLog(QStringLiteral("检查完成：发现 %1 对井间隔不符合规则。密度数量规则未在本界面检查。")
                          .arg(violationPairs), m_log);
    }
    catch (const QgsCsException& exception) {
        appendLog(QStringLiteral("检查失败：坐标转换异常：%1").arg(exception.what()), m_log);
    }
    catch (const std::exception& exception) {
        appendLog(QStringLiteral("检查失败：%1").arg(QString::fromLocal8Bit(exception.what())), m_log);
    }
    m_run->setEnabled(true);
}

void FeatureSpacingCheckDialog::runRiverCheck()
{
    struct SourceMinor {
        QString fileName;
        qint64 fid = -1;
        QString level;
        QgsGeometry line;
    };
    struct MainRiver {
        QString name;
        int part = 0;
        QgsGeometry line;
    };
    struct ParsedRiver {
        QString fileName;
        qint64 fid = -1;
        QgsGeometry line;
        QgsGeometry fullFeature;
        QgsPolylineXY vertices;
    };
    struct Junction {
        int mainIndex = -1;
        QString sourceFile;
        qint64 sourceFid = -1;
        QString sourceLevel;
        QString parsedFile;
        qint64 parsedFid = -1;
        QgsGeometry parsedFeature;
        QString parsedFids;
        QgsPointXY point;
        double chainage = -1.0;
    };
    struct SourceMouth {
        int mainIndex = -1;
        double chainage = -1.0;
    };

    m_run->setEnabled(false);
    struct EnableOnExit {
        QPushButton* button;
        ~EnableOnExit() { button->setEnabled(true); }
    } enableOnExit{m_run};
    appendLog(QStringLiteral("开始河流间隔检查。"));
    if (m_scale->value() <= 0 || m_sourcePaths.isEmpty() || m_targetPaths.isEmpty()
        || m_associatedPaths.isEmpty()) {
        appendLog(QStringLiteral("检查失败：请填写比例尺，并上传源 GDB 河流线、解析水系线和关联主区面。"));
        return;
    }
    RiverRule rule;
    QString error;
    if (!readRiverRules(m_xmlPaths.first(), rule, error)) {
        appendLog(QStringLiteral("检查失败：%1").arg(error));
        return;
    }
    QgsVectorLayer firstSource(m_sourcePaths.first(), QFileInfo(m_sourcePaths.first()).fileName(),
                               QStringLiteral("ogr"));
    if (!firstSource.isValid() || !firstSource.crs().isValid()
        || firstSource.crs().mapUnits() != QgsUnitTypes::DistanceMeters) {
        appendLog(QStringLiteral("检查失败：源 GDB 河流图层必须是有效的米制投影 SHP。"));
        return;
    }
    const QgsCoordinateReferenceSystem metricCrs = firstSource.crs();
    const double bufferMeters = rule.bufferMillimeters * m_scale->value() / 1000.0;
    const double minimumMeters = rule.minimumCentimeters * m_scale->value() / 100.0;
    const double maximumMeters = rule.maximumCentimeters * m_scale->value() / 100.0;
    appendLog(QStringLiteral("规则：%1 属于 [%2] 的源河流按 %3 mm 缓冲（实地 %4 m）匹配解析线；%5 属于 [%6] 为主干，按 %7 合并；同一主干相邻交点允许间隔 [%8, %9] cm（实地 [%10, %11] m，含端点）。")
                  .arg(rule.levelField, rule.minorLevels.join(QStringLiteral("、")))
                  .arg(rule.bufferMillimeters, 0, 'f', 3).arg(bufferMeters, 0, 'f', 3)
                  .arg(rule.levelField, rule.mainLevels.join(QStringLiteral("、")), rule.nameField)
                  .arg(rule.minimumCentimeters, 0, 'f', 3)
                  .arg(rule.maximumCentimeters, 0, 'f', 3)
                  .arg(minimumMeters, 0, 'f', 3).arg(maximumMeters, 0, 'f', 3));

    try {
        QgsGeometry mainArea;
        int areaCount = 0;
        for (const QString& path : m_associatedPaths) {
            QgsVectorLayer layer(path, QFileInfo(path).fileName(), QStringLiteral("ogr"));
            if (!layer.isValid() || !layer.crs().isValid()
                || QgsWkbTypes::geometryType(layer.wkbType()) != QgsWkbTypes::PolygonGeometry) {
                appendLog(QStringLiteral("检查失败：关联图层只接收有效的主区面 SHP：%1").arg(path));
                return;
            }
            QgsCoordinateTransform transform(layer.crs(), metricCrs,
                                             QgsProject::instance()->transformContext());
            const bool needsTransform = layer.crs() != metricCrs;
            QgsFeature feature;
            for (QgsFeatureIterator it = layer.getFeatures(); it.nextFeature(feature);) {
                if (!feature.hasGeometry() || feature.geometry().isEmpty()) continue;
                QgsGeometry area = feature.geometry();
                if (needsTransform) area.transform(transform);
                if (!area.isGeosValid()) area = area.makeValid();
                area.convertGeometryCollectionToSubclass(QgsWkbTypes::PolygonGeometry);
                if (area.isNull() || area.isEmpty() || !area.isGeosValid()) {
                    appendLog(QStringLiteral("检查失败：主区面 %1 FID %2 无效且无法修复。")
                                  .arg(QFileInfo(path).fileName()).arg(feature.id()));
                    return;
                }
                mainArea = mainArea.isNull() ? area : mainArea.combine(area);
                ++areaCount;
            }
        }
        if (mainArea.isNull() || mainArea.isEmpty()) {
            appendLog(QStringLiteral("检查失败：关联图层没有可用的主区面。"));
            return;
        }
        if (!mainArea.isGeosValid()) mainArea = mainArea.makeValid();
        if (mainArea.isNull() || mainArea.isEmpty() || !mainArea.isGeosValid()) {
            appendLog(QStringLiteral("检查失败：合并后的主区面无效。"));
            return;
        }

        QMap<QString, QVector<QgsGeometry>> mainInputs;
        QList<SourceMinor> minorRivers;
        int skippedUnnamedMain = 0;
        for (const QString& path : m_sourcePaths) {
            QgsVectorLayer layer(path, QFileInfo(path).fileName(), QStringLiteral("ogr"));
            if (!layer.isValid() || !layer.crs().isValid()
                || QgsWkbTypes::geometryType(layer.wkbType()) != QgsWkbTypes::LineGeometry) {
                appendLog(QStringLiteral("检查失败：源 GDB 河流必须是有效线图层：%1").arg(path));
                return;
            }
            const int levelIndex = layer.fields().indexFromName(rule.levelField);
            const int nameIndex = layer.fields().indexFromName(rule.nameField);
            if (levelIndex < 0 || nameIndex < 0) {
                appendLog(QStringLiteral("检查失败：%1 缺少 XML 指定的 %2 或 %3 字段。")
                              .arg(path, rule.levelField, rule.nameField));
                return;
            }
            QgsCoordinateTransform transform(layer.crs(), metricCrs,
                                             QgsProject::instance()->transformContext());
            const bool needsTransform = layer.crs() != metricCrs;
            QgsFeature feature;
            for (QgsFeatureIterator it = layer.getFeatures(); it.nextFeature(feature);) {
                if (!feature.hasGeometry() || feature.geometry().isEmpty()) continue;
                const QString level = feature.attribute(levelIndex).toString().trimmed();
                if (!rule.minorLevels.contains(level) && !rule.mainLevels.contains(level)) continue;
                QgsGeometry geometry = feature.geometry();
                if (needsTransform) geometry.transform(transform);
                if (rule.minorLevels.contains(level)) {
                    QgsMultiPolylineXY parts;
                    if (geometry.isMultipart()) parts = geometry.asMultiPolyline();
                    else parts.append(geometry.asPolyline());
                    for (const QgsPolylineXY& vertices : parts) {
                        if (vertices.size() < 2) continue;
                        minorRivers.append({QFileInfo(path).fileName(), feature.id(), level,
                                            QgsGeometry::fromPolylineXY(vertices)});
                    }
                } else {
                    const QString name = feature.attribute(nameIndex).toString().trimmed();
                    if (name.isEmpty()) {
                        ++skippedUnnamedMain;
                        continue;
                    }
                    mainInputs[name].append(geometry);
                }
            }
        }
        if (minorRivers.isEmpty() || mainInputs.isEmpty()) {
            appendLog(QStringLiteral("检查失败：源 GDB 中没有 XML 指定等级的待检河流或有名称的主干河流。"));
            return;
        }
        QList<MainRiver> mains;
        for (auto it = mainInputs.cbegin(); it != mainInputs.cend(); ++it) {
            QgsGeometry noded = QgsGeometry::unaryUnion(it.value());
            if (noded.isNull() || noded.isEmpty()) continue;
            noded.convertGeometryCollectionToSubclass(QgsWkbTypes::LineGeometry);
            QgsGeometry merged = noded.isMultipart() ? noded.mergeLines() : noded;
            QgsMultiPolylineXY parts;
            if (merged.isMultipart()) parts = merged.asMultiPolyline();
            else parts.append(merged.asPolyline());
            for (int i = 0; i < parts.size(); ++i) {
                if (parts.at(i).size() < 2) continue;
                QgsGeometry line = QgsGeometry::fromPolylineXY(parts.at(i));
                if (line.length() > 0.0) mains.append({it.key(), i + 1, line});
            }
        }
        if (mains.isEmpty()) {
            appendLog(QStringLiteral("检查失败：主干河流按 NAME 合并后没有有效线段。"));
            return;
        }
        appendLog(QStringLiteral("已读取源河流 %1 段、按 %2 合并的主干连通段 %3 条、主区面 %4 个；跳过 NAME 为空的主干要素 %5 个。")
                      .arg(minorRivers.size()).arg(rule.nameField).arg(mains.size())
                      .arg(areaCount).arg(skippedUnnamedMain));

        QList<ParsedRiver> parsedRivers;
        for (const QString& path : m_targetPaths) {
            QgsVectorLayer layer(path, QFileInfo(path).fileName(), QStringLiteral("ogr"));
            if (!layer.isValid() || !layer.crs().isValid()
                || QgsWkbTypes::geometryType(layer.wkbType()) != QgsWkbTypes::LineGeometry) {
                appendLog(QStringLiteral("检查失败：解析水系图层必须是有效线 SHP：%1").arg(path));
                return;
            }
            QgsCoordinateTransform transform(layer.crs(), metricCrs,
                                             QgsProject::instance()->transformContext());
            const bool needsTransform = layer.crs() != metricCrs;
            QgsFeature feature;
            for (QgsFeatureIterator it = layer.getFeatures(); it.nextFeature(feature);) {
                if (!feature.hasGeometry() || feature.geometry().isEmpty()) continue;
                QgsGeometry geometry = feature.geometry();
                if (needsTransform) geometry.transform(transform);
                QgsMultiPolylineXY parts;
                if (geometry.isMultipart()) parts = geometry.asMultiPolyline();
                else parts.append(geometry.asPolyline());
                for (const QgsPolylineXY& vertices : parts) {
                    if (vertices.size() < 2) continue;
                    QgsGeometry line = QgsGeometry::fromPolylineXY(vertices);
                    if (line.length() > 0.0)
                        parsedRivers.append({QFileInfo(path).fileName(), feature.id(),
                                             line, geometry, vertices});
                }
            }
        }
        if (parsedRivers.isEmpty()) {
            appendLog(QStringLiteral("检查失败：待检图层没有可用的解析水系线。"));
            return;
        }

        QList<Junction> junctions;
        QList<SourceMouth> sourceMouthLocations;
        int sourceMouths = 0;
        int unmatchedParsed = 0;
        int ambiguousIntersections = 0;
        int outsideMainArea = 0;
        const double sourceJoinTolerance = qMax(1.0, qMin(10.0, bufferMeters * 0.05));
        for (const SourceMinor& minor : minorRivers) {
            const QgsPolylineXY sourceVertices = minor.line.asPolyline();
            const QgsPointXY ends[2] = {sourceVertices.first(), sourceVertices.last()};
            for (int mainIndex = 0; mainIndex < mains.size(); ++mainIndex) {
                const MainRiver& main = mains.at(mainIndex);
                if (!minor.line.boundingBox().intersects(main.line.boundingBox())) continue;
                const QgsGeometry intersection = minor.line.intersection(main.line);
                if (intersection.isNull() || intersection.isEmpty()) continue;
                // A line overlap is not a unique tributary mouth. Only real point
                // intersections may be used as spacing-check positions.
                if (QgsWkbTypes::geometryType(intersection.wkbType())
                    != QgsWkbTypes::PointGeometry) {
                    ++ambiguousIntersections;
                    continue;
                }
                QgsMultiPointXY mouths;
                if (intersection.isMultipart()) mouths = intersection.asMultiPoint();
                else mouths.append(intersection.asPoint());
                QgsMultiPointXY distinctMouths;
                for (const QgsPointXY& mouth : mouths) {
                    bool duplicate = false;
                    for (const QgsPointXY& existing : distinctMouths) {
                        if (std::hypot(mouth.x() - existing.x(),
                                       mouth.y() - existing.y()) <= 1.0) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) distinctMouths.append(mouth);
                }
                if (distinctMouths.isEmpty()) continue;
                if (distinctMouths.size() > 1) {
                    int endpointMouth = -1;
                    for (int i = 0; i < distinctMouths.size(); ++i) {
                        const QgsPointXY& mouth = distinctMouths.at(i);
                        const double endDistance = qMin(
                            std::hypot(mouth.x() - ends[0].x(), mouth.y() - ends[0].y()),
                            std::hypot(mouth.x() - ends[1].x(), mouth.y() - ends[1].y()));
                        if (endDistance > sourceJoinTolerance) continue;
                        if (endpointMouth >= 0) { endpointMouth = -1; break; }
                        endpointMouth = i;
                    }
                    if (endpointMouth < 0) {
                        ++ambiguousIntersections;
                        continue;
                    }
                    const QgsPointXY selected = distinctMouths.at(endpointMouth);
                    distinctMouths.clear();
                    distinctMouths.append(selected);
                }
                for (const QgsPointXY& sourceMouth : distinctMouths) {
                    const double sourceChainage = main.line.lineLocatePoint(
                        QgsGeometry::fromPointXY(sourceMouth));
                    if (sourceChainage < 0.0) continue;
                    ++sourceMouths;
                    if (!mainArea.intersects(QgsGeometry::fromPointXY(sourceMouth))) {
                        ++outsideMainArea;
                        continue;
                    }
                    // Keep every in-area source mouth in the ordering, including one
                    // whose parsed stroke cannot be matched. It still separates its
                    // neighboring confluences along this trunk.
                    sourceMouthLocations.append({mainIndex, sourceChainage});
                    const QgsGeometry corridor = minor.line.buffer(bufferMeters, 8);
                    if (corridor.isNull() || corridor.isEmpty()) continue;
                    int bestIndex = -1;
                    double bestCoverage = 0.0;
                    double bestMouthDistance = 1e100;
                    double bestEndDistance = 1e100;
                    for (int i = 0; i < parsedRivers.size(); ++i) {
                        const ParsedRiver& parsed = parsedRivers.at(i);
                        if (!corridor.boundingBox().intersects(parsed.line.boundingBox())) continue;
                        const QgsGeometry startPoint =
                            QgsGeometry::fromPointXY(parsed.vertices.first());
                        const QgsGeometry endPoint =
                            QgsGeometry::fromPointXY(parsed.vertices.last());
                        if (minor.line.distance(startPoint) > bufferMeters
                            || minor.line.distance(endPoint) > bufferMeters) continue;
                        const double startPosition = minor.line.lineLocatePoint(startPoint);
                        const double endPosition = minor.line.lineLocatePoint(endPoint);
                        if (startPosition < 0.0 || endPosition < 0.0
                            || std::abs(endPosition - startPosition)
                                < parsed.line.length() * 0.3) continue;
                        const QgsGeometry mouthPoint = QgsGeometry::fromPointXY(sourceMouth);
                        const double startDistance = startPoint.distance(mouthPoint);
                        const double endDistance = endPoint.distance(mouthPoint);
                        const double endpointDistance = qMin(startDistance, endDistance);
                        const double mouthDistance = parsed.line.distance(mouthPoint);
                        const QgsGeometry overlap = parsed.line.intersection(corridor);
                        const double coverage = overlap.isNull() ? 0.0
                            : overlap.length() / parsed.line.length();
                        if (coverage < 0.5) continue;
                        // One GDB tributary may split into several parsed SHP features.
                        // Use only the complete feature nearest to its GDB confluence.
                        if (mouthDistance < bestMouthDistance - 1e-6
                            || (std::abs(mouthDistance - bestMouthDistance) <= 1e-6
                                && (endpointDistance < bestEndDistance - 1e-6
                                    || (std::abs(endpointDistance - bestEndDistance) <= 1e-6
                                        && coverage > bestCoverage)))) {
                            bestIndex = i;
                            bestMouthDistance = mouthDistance;
                            bestCoverage = coverage;
                            bestEndDistance = endpointDistance;
                        }
                    }
                    if (bestIndex < 0) {
                        ++unmatchedParsed;
                        continue;
                    }
                    const ParsedRiver& parsed = parsedRivers.at(bestIndex);
                    junctions.append({mainIndex, minor.fileName, minor.fid, minor.level,
                                      parsed.fileName, parsed.fid, parsed.fullFeature,
                                      QString::number(parsed.fid),
                                      sourceMouth, sourceChainage});
                    appendLog(QStringLiteral("源 GDB 交汇点匹配：%1 %2 FID %3 → 离交点最近的解析要素 %4 FID %5（距交点 %6 m），主干“%7”，沿河里程 %8 m；不拼接其他解析 FID。")
                                  .arg(minor.level, minor.fileName).arg(minor.fid)
                                  .arg(parsed.fileName).arg(parsed.fid)
                                  .arg(bestMouthDistance, 0, 'f', 2)
                                  .arg(main.name).arg(sourceChainage, 0, 'f', 2));
                }
            }
        }
        appendLog(QStringLiteral("源 GDB 与主干实际交点 %1 个；解析线未匹配 %2 个、重叠或多交点无法唯一确定 %3 处、交点在主区外 %4 个；主区内已匹配解析线的交点 %5 个。")
                      .arg(sourceMouths).arg(unmatchedParsed).arg(ambiguousIntersections)
                      .arg(outsideMainArea).arg(junctions.size()));

        QgsFields fields;
        fields.append(QgsField(QStringLiteral("river"), QVariant::String, QString(), 100));
        fields.append(QgsField(QStringLiteral("srcfile_a"), QVariant::String, QString(), 80));
        fields.append(QgsField(QStringLiteral("src_a"), QVariant::LongLong));
        fields.append(QgsField(QStringLiteral("level_a"), QVariant::String, QString(), 20));
        fields.append(QgsField(QStringLiteral("tgtfile_a"), QVariant::String, QString(), 80));
        fields.append(QgsField(QStringLiteral("tgt_a"), QVariant::LongLong));
        fields.append(QgsField(QStringLiteral("tgt_ids_a"), QVariant::String, QString(), 254));
        fields.append(QgsField(QStringLiteral("srcfile_b"), QVariant::String, QString(), 80));
        fields.append(QgsField(QStringLiteral("src_b"), QVariant::LongLong));
        fields.append(QgsField(QStringLiteral("level_b"), QVariant::String, QString(), 20));
        fields.append(QgsField(QStringLiteral("tgtfile_b"), QVariant::String, QString(), 80));
        fields.append(QgsField(QStringLiteral("tgt_b"), QVariant::LongLong));
        fields.append(QgsField(QStringLiteral("tgt_ids_b"), QVariant::String, QString(), 254));
        fields.append(QgsField(QStringLiteral("dist_m"), QVariant::Double));
        fields.append(QgsField(QStringLiteral("min_m"), QVariant::Double));
        fields.append(QgsField(QStringLiteral("max_m"), QVariant::Double));
        fields.append(QgsField(QStringLiteral("reason"), QVariant::String, QString(), 32));
        QList<QgsFeature> errors;
        int comparedPairs = 0;
        int duplicateJunctions = 0;
        int nonAdjacentPairs = 0;
        for (int mainIndex = 0; mainIndex < mains.size(); ++mainIndex) {
            QList<Junction> onMain;
            for (const Junction& junction : junctions) {
                if (junction.mainIndex == mainIndex) onMain.append(junction);
            }
            std::sort(onMain.begin(), onMain.end(), [](const Junction& a, const Junction& b) {
                return a.chainage < b.chainage;
            });
            QList<Junction> unique;
            for (const Junction& junction : onMain) {
                if (!unique.isEmpty() && junction.chainage - unique.last().chainage <= 1.0) {
                    ++duplicateJunctions;
                    continue;
                }
                unique.append(junction);
            }
            QList<double> allMouthChainages;
            for (const SourceMouth& mouth : sourceMouthLocations)
                if (mouth.mainIndex == mainIndex) allMouthChainages.append(mouth.chainage);
            std::sort(allMouthChainages.begin(), allMouthChainages.end());
            for (int i = 0; i + 1 < unique.size(); ++i) {
                const Junction& first = unique.at(i);
                const Junction& second = unique.at(i + 1);
                bool hasIntermediateMouth = false;
                for (double chainage : allMouthChainages) {
                    if (chainage <= first.chainage + 1.0) continue;
                    if (chainage >= second.chainage - 1.0) break;
                    hasIntermediateMouth = true;
                    break;
                }
                if (hasIntermediateMouth) {
                    ++nonAdjacentPairs;
                    continue;
                }
                const double distance = second.chainage - first.chainage;
                ++comparedPairs;
                if (distance >= minimumMeters - 1e-6
                    && distance <= maximumMeters + 1e-6) continue;
                const QString reason = distance < minimumMeters
                    ? QStringLiteral("间隔过小") : QStringLiteral("间隔过大");
                QgsMultiPolylineXY errorLines;
                auto appendParsedFeature = [&errorLines](const QgsGeometry& geometry) {
                    if (geometry.isMultipart()) {
                        for (const QgsPolylineXY& line : geometry.asMultiPolyline())
                            if (line.size() >= 2) errorLines.append(line);
                    } else {
                        const QgsPolylineXY line = geometry.asPolyline();
                        if (line.size() >= 2) errorLines.append(line);
                    }
                };
                appendParsedFeature(first.parsedFeature);
                appendParsedFeature(second.parsedFeature);
                if (errorLines.size() < 2) continue;
                QgsFeature output(fields);
                output.setGeometry(QgsGeometry::fromMultiPolylineXY(errorLines));
                QgsAttributes attributes;
                attributes << mains.at(mainIndex).name
                           << first.sourceFile << QVariant::fromValue<qlonglong>(first.sourceFid)
                           << first.sourceLevel
                           << first.parsedFile << QVariant::fromValue<qlonglong>(first.parsedFid)
                           << first.parsedFids
                           << second.sourceFile << QVariant::fromValue<qlonglong>(second.sourceFid)
                           << second.sourceLevel
                           << second.parsedFile << QVariant::fromValue<qlonglong>(second.parsedFid)
                           << second.parsedFids
                           << distance << minimumMeters << maximumMeters << reason;
                output.setAttributes(attributes);
                errors.append(output);
                appendLog(QStringLiteral("不符合（%1）：主干“%2”上 %3 解析 FID %4 与 %5 解析 FID %6 对应的相邻源 GDB 交点沿河间隔 %7 m，允许 [%8, %9] m；错误图层保存这两个完整解析 SHP 要素。")
                              .arg(reason, mains.at(mainIndex).name)
                              .arg(first.sourceLevel, first.parsedFids)
                              .arg(second.sourceLevel, second.parsedFids)
                              .arg(distance, 0, 'f', 3).arg(minimumMeters, 0, 'f', 3)
                              .arg(maximumMeters, 0, 'f', 3));
            }
        }
        m_problemCount = errors.size();
        if (!errors.isEmpty()) {
            QString outputError;
            const QStringList protectedPaths = m_sourcePaths + m_associatedPaths
                + m_targetPaths + m_xmlPaths;
            if (writeSpacingErrorLayer(this, m_outputFilePath, metricCrs, fields, errors,
                                       QgsWkbTypes::MultiLineString, protectedPaths, outputError,
                                       m_taskManagerMode)) {
                m_outputWritten = true;
                appendLog(QStringLiteral("河流间隔错误图层已输出：%1（%2 条；每条记录包含一对离各自源 GDB 汇入口最近的完整解析 SHP 要素，不拼接其他解析 FID，也不输出主干几何）。")
                              .arg(QDir::toNativeSeparators(m_outputFilePath)).arg(errors.size()));
            } else
                appendLog(QStringLiteral("错误图层输出失败：%1").arg(outputError));
        } else {
            appendLog(QStringLiteral("未发现河流间隔错误，不生成错误结果图层；已有同名旧文件不变。"));
        }
        appendLog(QStringLiteral("河流检查完成：主区内已匹配交点去重 %1 个，比较同一主干上真正相邻且两侧均匹配解析线的交点 %2 对，错误 %3 对；因中间另有 XML 指定等级的源 GDB 汇入口而跳过 %4 对。")
                      .arg(duplicateJunctions).arg(comparedPairs).arg(errors.size())
                      .arg(nonAdjacentPairs));
    }
    catch (const QgsCsException& exception) {
        appendLog(QStringLiteral("检查失败：坐标转换异常：%1").arg(exception.what()));
    }
    catch (const std::exception& exception) {
        appendLog(QStringLiteral("检查失败：%1").arg(QString::fromLocal8Bit(exception.what())));
    }
}

void FeatureSpacingCheckDialog::runTunnelCheck()
{
    struct TunnelRoute {
        QString name;
        QString layerCode;
        QString layerType;
        QList<QgsGeometry> segments;
    };
    struct TunnelRoutePart {
        QString key;
        QString name;
        QString layerCode;
        QString layerType;
        QgsGeometry geometry;
        bool orderable = true;
    };
    struct ParsedTunnelSegment {
        qint64 fid = 0;
        int partIndex = 0;
        QString fileName;
        QString tunnelName;
        QString layerType;
        QString routePartKey;
        QgsGeometry geometry;
        QgsPointXY startPoint;
        QgsPointXY endPoint;
        double routeStart = -1.0;
        double routeEnd = -1.0;
    };
    m_run->setEnabled(false);
    const bool railway = m_checkType == CheckType::RailwayTunnel;
    const QString layerType = railway ? QStringLiteral("铁路") : QStringLiteral("公路");
    const QString sourceLayerName = railway ? QStringLiteral("LRRL") : QStringLiteral("LRDL");
    appendLog(QStringLiteral("开始%1隧道线间隔检查。").arg(layerType));
    if (m_sourcePaths.isEmpty() || m_targetPaths.isEmpty()
        || (railway && m_associatedPaths.isEmpty())) {
        appendLog(railway
                      ? QStringLiteral("检查失败：请上传 LRRL 源 GDB SHP、待检的解析铁路隧道线和关联火车站图层。")
                      : QStringLiteral("检查失败：请上传 LRDL 源 GDB SHP 和待检的解析公路隧道线。"));
        m_run->setEnabled(true);
        return;
    }

    double spacingMeters = -1.0;
    QStringList groupingFields;
    QString error;
    if (!readTunnelRules(m_xmlPaths.first(), spacingMeters, groupingFields, error)) {
        appendLog(QStringLiteral("检查失败：%1").arg(error));
        m_run->setEnabled(true);
        return;
    }
    if (groupingFields.isEmpty()) {
        appendLog(QStringLiteral("检查失败：%1隧道 XML 缺少“GDB数据属性”规则；请在 <value> 中填写 GDB 分组字段。")
                      .arg(layerType));
        m_run->setEnabled(true);
        return;
    }

    QgsVectorLayer firstSource(m_sourcePaths.first(), QFileInfo(m_sourcePaths.first()).fileName(), QStringLiteral("ogr"));
    if (!firstSource.isValid() || !firstSource.crs().isValid()
        || firstSource.crs().mapUnits() != QgsUnitTypes::DistanceMeters) {
        appendLog(QStringLiteral("检查失败：源 GDB 隧道图层无法加载，或其坐标系不是以米为单位的投影坐标系。"));
        m_run->setEnabled(true);
        return;
    }
    const QgsCoordinateReferenceSystem metricCrs = firstSource.crs();
    appendLog(QStringLiteral("规则：%1源 GDB 图层 %2 按字段组合 [%3] 识别同一路线；相邻解析隧道线相向端点距离不超过 %4 m 应连接，超过则应断开。")
                  .arg(layerType, sourceLayerName, groupingFields.join(QStringLiteral(" + ")))
                  .arg(spacingMeters, 0, 'f', 3));
    appendLog(railway
                  ? QStringLiteral("解析铁路隧道线按所属 GDB 同属性组合路线的投影里程排序，量相邻两段线的相向端点；火车站处无论距离均应断开，端点相距不超过 1 m 视作已连接。")
                  : QStringLiteral("解析公路隧道线按所属 GDB 同属性组合路线的投影里程排序，量相邻两段线的相向端点；端点相距不超过 1 m 视作已连接。"));

    try {
        QMap<QString, TunnelRoute> routes;
        int sourceFeatureCount = 0;
        int railRouteCount = 0;
        int roadRouteCount = 0;
        for (const QString& sourcePath : m_sourcePaths) {
            const QString baseName = QFileInfo(sourcePath).completeBaseName();
            if (!baseName.contains(sourceLayerName, Qt::CaseInsensitive)) {
                appendLog(QStringLiteral("检查失败：%1模式只能上传对应的 %2 源 GDB 图层，当前为 %3。")
                              .arg(layerType, sourceLayerName, QFileInfo(sourcePath).fileName()));
                m_run->setEnabled(true);
                return;
            }
            QgsVectorLayer sourceLayer(sourcePath, baseName, QStringLiteral("ogr"));
            if (!sourceLayer.isValid()) {
                appendLog(QStringLiteral("检查失败：源隧道 GDB SHP 无法加载：%1").arg(sourcePath));
                m_run->setEnabled(true);
                return;
            }
            if (QgsWkbTypes::geometryType(sourceLayer.wkbType()) != QgsWkbTypes::LineGeometry) {
                appendLog(QStringLiteral("检查失败：源隧道图层必须是线要素：%1").arg(sourcePath));
                m_run->setEnabled(true);
                return;
            }
            QVector<int> groupingIndexes;
            for (const QString& fieldName : groupingFields) {
                const int fieldIndex = sourceLayer.fields().indexFromName(fieldName);
                if (fieldIndex < 0) {
                    appendLog(QStringLiteral("检查失败：源隧道图层缺少 XML 指定的分组字段“%1”：%2")
                                  .arg(fieldName, sourcePath));
                    m_run->setEnabled(true);
                    return;
                }
                groupingIndexes.append(fieldIndex);
            }
            if (!sourceLayer.crs().isValid()) {
                appendLog(QStringLiteral("检查失败：源隧道图层缺少有效坐标系：%1").arg(sourcePath));
                m_run->setEnabled(true);
                return;
            }
            QgsCoordinateTransform transform(sourceLayer.crs(), metricCrs,
                                              QgsProject::instance()->transformContext());
            const bool needsTransform = sourceLayer.crs() != metricCrs;
            QgsFeature feature;
            for (QgsFeatureIterator it = sourceLayer.getFeatures(); it.nextFeature(feature);) {
                QStringList values;
                QStringList labels;
                bool hasGroupingValue = false;
                for (int i = 0; i < groupingIndexes.size(); ++i) {
                    const QVariant attribute = feature.attribute(groupingIndexes.at(i));
                    const QString value = attribute.isNull() ? QString() : attribute.toString().trimmed();
                    values.append(value);
                    labels.append(QStringLiteral("%1=%2")
                                      .arg(groupingFields.at(i), value.isEmpty() ? QStringLiteral("<空>") : value));
                    hasGroupingValue = hasGroupingValue || !value.isEmpty();
                }
                if (!hasGroupingValue || !feature.hasGeometry() || feature.geometry().isEmpty()) continue;
                QgsGeometry line = feature.geometry();
                if (needsTransform) line.transform(transform);
                const QString key = sourceLayerName + QChar(0x1f)
                    + values.join(QChar(0x1f));
                if (!routes.contains(key)) {
                    TunnelRoute route;
                    route.name = labels.join(QStringLiteral("，"));
                    route.layerCode = sourceLayerName;
                    route.layerType = layerType;
                    route.segments.append(line);
                    routes.insert(key, route);
                } else {
                    TunnelRoute& route = routes[key];
                    route.segments.append(line);
                }
                ++sourceFeatureCount;
                if (layerType == QStringLiteral("铁路")) ++railRouteCount;
                else ++roadRouteCount;
            }
        }
        if (routes.isEmpty()) {
            appendLog(QStringLiteral("检查失败：%1 图层没有读取到分组字段非空且几何有效的源 GDB 隧道线。")
                          .arg(sourceLayerName));
            m_run->setEnabled(true);
            return;
        }

        // Rebuild each NAME's linework as noded, topologically connected route parts.
        // A disconnected part gets its own chainage system; branches/loops are marked
        // unorderable rather than inventing a false linear order.
        QMap<QString, TunnelRoutePart> routeParts;
        for (auto routeIt = routes.begin(); routeIt != routes.end(); ++routeIt) {
            const TunnelRoute& route = routeIt.value();
            QVector<QgsGeometry> inputs;
            for (const QgsGeometry& segment : route.segments) inputs.append(segment);
            QgsGeometry noded = QgsGeometry::unaryUnion(inputs);
            if (noded.isNull() || noded.isEmpty()) {
                appendLog(QStringLiteral("检查失败：%1隧道属性组合“%2”的线节点化失败。")
                              .arg(route.layerType, route.name));
                m_run->setEnabled(true);
                return;
            }
            noded.convertGeometryCollectionToSubclass(QgsWkbTypes::LineGeometry);
            QgsGeometry merged = noded.isMultipart() ? noded.mergeLines() : noded;
            QList<QgsGeometry> components;
            auto appendLine = [&components](const QgsPolylineXY& line) {
                if (line.size() >= 2)
                    components.append(QgsGeometry::fromPolylineXY(line));
            };
            if (merged.isMultipart()) {
                const QgsMultiPolylineXY multiLine = merged.asMultiPolyline();
                for (const QgsPolylineXY& line : multiLine) appendLine(line);
            } else {
                appendLine(merged.asPolyline());
            }
            if (components.isEmpty()) {
                appendLog(QStringLiteral("检查失败：%1隧道属性组合“%2”没有可用于路线排序的线段。")
                              .arg(route.layerType, route.name));
                m_run->setEnabled(true);
                return;
            }

            bool hasBranchOrCrossing = false;
            for (int i = 0; i < components.size() && !hasBranchOrCrossing; ++i) {
                for (int j = i + 1; j < components.size(); ++j) {
                    if (components.at(i).intersects(components.at(j))) {
                        hasBranchOrCrossing = true;
                        break;
                    }
                }
            }
            if (components.size() > 1) {
                appendLog(QStringLiteral("%1隧道属性组合“%2”的 GDB 线拆成 %3 个线分量（可能是断开或分叉）；只在可排序的连续分量内比较。")
                              .arg(route.layerType).arg(route.name).arg(components.size()));
            }
            if (hasBranchOrCrossing) {
                appendLog(QStringLiteral("路线歧义：%1隧道“%2”的 GDB 线存在分叉或交叉，无法唯一确定解析隧道线的相邻顺序；该路线不参与间隔判定。")
                              .arg(route.layerType, route.name));
            }
            for (int i = 0; i < components.size(); ++i) {
                const QString partKey = routeIt.key() + QChar(0x1e) + QString::number(i);
                TunnelRoutePart part;
                part.key = partKey;
                part.name = route.name;
                part.layerCode = route.layerCode;
                part.layerType = route.layerType;
                part.geometry = components.at(i);
                part.orderable = !hasBranchOrCrossing;
                const QgsPolylineXY coordinates = components.at(i).asPolyline();
                if (coordinates.size() > 2 && coordinates.first() == coordinates.last()) {
                    part.orderable = false;
                    appendLog(QStringLiteral("路线歧义：%1隧道“%2”包含闭合路线，无法确定解析隧道线的首尾相邻关系；该路线段不参与间隔判定。")
                                  .arg(route.layerType, route.name));
                }
                routeParts.insert(partKey, part);
            }
        }

        QList<QgsGeometry> stationGeometries;
        QList<ParsedTunnelSegment> parsedSegments;
        int stationFeatureCount = 0;
        int parsedTunnelLineCount = 0;

        if (railway) {
            for (const QString& stationPath : m_associatedPaths) {
                const QString baseName = QFileInfo(stationPath).completeBaseName();
                if (!baseName.contains(QStringLiteral("车站"))
                    || baseName.contains(QStringLiteral("标注"))) {
                    appendLog(QStringLiteral("跳过非火车站符号关联图层：%1")
                                  .arg(QFileInfo(stationPath).fileName()));
                    continue;
                }
                QgsVectorLayer stationLayer(stationPath, baseName, QStringLiteral("ogr"));
                if (!stationLayer.isValid() || !stationLayer.crs().isValid()) {
                    appendLog(QStringLiteral("检查失败：火车站图层无法加载或缺少有效坐标系：%1").arg(stationPath));
                    m_run->setEnabled(true);
                    return;
                }
                QgsCoordinateTransform transform(stationLayer.crs(), metricCrs,
                                                  QgsProject::instance()->transformContext());
                const bool needsTransform = stationLayer.crs() != metricCrs;
                QgsFeature feature;
                for (QgsFeatureIterator it = stationLayer.getFeatures(); it.nextFeature(feature);) {
                    if (!feature.hasGeometry() || feature.geometry().isEmpty()) continue;
                    QgsGeometry station = feature.geometry();
                    if (needsTransform) station.transform(transform);
                    const QgsGeometry center = station.centroid();
                    if (center.isNull() || center.isEmpty()) continue;
                    stationGeometries.append(station);
                    ++stationFeatureCount;
                }
                appendLog(QStringLiteral("已读取火车站图层 %1：%2 个有效站点符号。")
                              .arg(QFileInfo(stationPath).fileName()).arg(stationFeatureCount));
            }
            if (stationGeometries.isEmpty()) {
                appendLog(QStringLiteral("检查失败：铁路模式必须在关联图层上传有效的火车站符号 SHP。"));
                m_run->setEnabled(true);
                return;
            }
        } else if (!m_associatedPaths.isEmpty()) {
            appendLog(QStringLiteral("公路模式不使用关联图层；已选择的关联文件不会参与检查。"));
        }

        for (const QString& targetPath : m_targetPaths) {
            const QString baseName = QFileInfo(targetPath).completeBaseName();
            if (baseName.contains(QStringLiteral("隧道口"))
                || (railway && baseName.contains(QStringLiteral("公路隧道")))
                || (!railway && baseName.contains(QStringLiteral("铁路隧道")))) {
                appendLog(QStringLiteral("检查失败：待检图层必须是解析后的%1隧道线，不能上传 %2。")
                              .arg(layerType, QFileInfo(targetPath).fileName()));
                m_run->setEnabled(true);
                return;
            }
            QgsVectorLayer targetLayer(targetPath, baseName, QStringLiteral("ogr"));
            if (!targetLayer.isValid() || !targetLayer.crs().isValid()) {
                appendLog(QStringLiteral("检查失败：待检隧道图层无法加载或缺少有效坐标系：%1").arg(targetPath));
                m_run->setEnabled(true);
                return;
            }
            if (QgsWkbTypes::geometryType(targetLayer.wkbType()) != QgsWkbTypes::LineGeometry) {
                appendLog(QStringLiteral("检查失败：解析后的隧道图层必须是线要素：%1").arg(targetPath));
                m_run->setEnabled(true);
                return;
            }
            QgsCoordinateTransform transform(targetLayer.crs(), metricCrs,
                                              QgsProject::instance()->transformContext());
            const bool needsTransform = targetLayer.crs() != metricCrs;
            QgsFeature feature;
            for (QgsFeatureIterator it = targetLayer.getFeatures(); it.nextFeature(feature);) {
                if (!feature.hasGeometry() || feature.geometry().isEmpty()) continue;
                QgsGeometry geometry = feature.geometry();
                if (needsTransform) geometry.transform(transform);
                QgsMultiPolylineXY parts;
                if (geometry.isMultipart()) parts = geometry.asMultiPolyline();
                else parts.append(geometry.asPolyline());
                for (int partIndex = 0; partIndex < parts.size(); ++partIndex) {
                    const QgsPolylineXY& vertices = parts.at(partIndex);
                    if (vertices.size() < 2) continue;
                    const QgsGeometry line = QgsGeometry::fromPolylineXY(vertices);
                    if (line.isEmpty() || line.length() <= 0.0) continue;
                    QString bestKey;
                    double bestOverlap = 0.0;
                    double secondOverlap = 0.0;
                    for (auto routeIt = routeParts.cbegin(); routeIt != routeParts.cend(); ++routeIt) {
                        const TunnelRoutePart& route = routeIt.value();
                        if (!route.orderable) continue;
                        // Cartographic strokes may be offset from the source
                        // centerline. Sustained overlap avoids matching crossings.
                        const QgsGeometry corridor = route.geometry.buffer(50.0, 8);
                        if (corridor.isNull() || corridor.isEmpty()
                            || !corridor.boundingBox().intersects(line.boundingBox())) continue;
                        const double overlap = line.intersection(corridor).length();
                        if (overlap > bestOverlap) {
                            secondOverlap = bestOverlap;
                            bestOverlap = overlap;
                            bestKey = routeIt.key();
                        } else if (overlap > secondOverlap) {
                            secondOverlap = overlap;
                        }
                    }
                    if (bestKey.isEmpty() || bestOverlap < line.length() * 0.5
                        || secondOverlap >= bestOverlap * 0.9) {
                        appendLog(QStringLiteral("跳过解析隧道线 %1 FID %2 第 %3 段：不能唯一匹配到%4 GDB 路线（最佳覆盖 %5/%6 m）。")
                                      .arg(QFileInfo(targetPath).fileName())
                                      .arg(feature.id()).arg(partIndex + 1).arg(layerType)
                                      .arg(bestOverlap, 0, 'f', 2).arg(line.length(), 0, 'f', 2));
                        continue;
                    }
                    const TunnelRoutePart& route = routeParts.value(bestKey);
                    const double firstPosition = route.geometry.lineLocatePoint(
                        QgsGeometry::fromPointXY(vertices.first()));
                    const double lastPosition = route.geometry.lineLocatePoint(
                        QgsGeometry::fromPointXY(vertices.last()));
                    if (firstPosition < 0.0 || lastPosition < 0.0
                        || std::abs(firstPosition - lastPosition) < 1.0) {
                        appendLog(QStringLiteral("跳过解析隧道线 %1 FID %2 第 %3 段：端点无法沿 GDB 路线可靠排序。")
                                      .arg(QFileInfo(targetPath).fileName())
                                      .arg(feature.id()).arg(partIndex + 1));
                        continue;
                    }
                    ParsedTunnelSegment segment;
                    segment.fid = feature.id();
                    segment.partIndex = partIndex + 1;
                    segment.fileName = QFileInfo(targetPath).fileName();
                    segment.tunnelName = route.name;
                    segment.layerType = route.layerType;
                    segment.routePartKey = bestKey;
                    segment.geometry = line;
                    if (firstPosition < lastPosition) {
                        segment.routeStart = firstPosition;
                        segment.routeEnd = lastPosition;
                        segment.startPoint = vertices.first();
                        segment.endPoint = vertices.last();
                    } else {
                        segment.routeStart = lastPosition;
                        segment.routeEnd = firstPosition;
                        segment.startPoint = vertices.last();
                        segment.endPoint = vertices.first();
                    }
                    parsedSegments.append(segment);
                    ++parsedTunnelLineCount;
                    appendLog(QStringLiteral("解析隧道线匹配：%1 FID %2 第 %3 段归属%4 GDB 路线“%5”。")
                                  .arg(segment.fileName).arg(segment.fid).arg(segment.partIndex)
                                  .arg(segment.layerType, segment.tunnelName));
                }
            }
            appendLog(QStringLiteral("已读取待检%1隧道图层：%2。")
                          .arg(layerType, QFileInfo(targetPath).fileName()));
        }
        if (parsedSegments.isEmpty()) {
            appendLog(QStringLiteral("检查失败：待检图层中没有能匹配到 %1 的解析隧道线。")
                          .arg(sourceLayerName));
            m_run->setEnabled(true);
            return;
        }

        QMap<QString, QList<ParsedTunnelSegment>> segmentsByRoute;
        for (const ParsedTunnelSegment& segment : parsedSegments)
            segmentsByRoute[segment.routePartKey].append(segment);
        appendLog(QStringLiteral("已读取 %1 个 GDB 隧道线要素（铁路 %2、公路 %3）；匹配到同属性组合路线的解析隧道线段 %4 个，火车站 %5 个。")
                      .arg(sourceFeatureCount).arg(railRouteCount).arg(roadRouteCount)
                      .arg(parsedTunnelLineCount).arg(stationFeatureCount));

        const bool hasRailSegments = std::any_of(
            parsedSegments.cbegin(), parsedSegments.cend(),
            [](const ParsedTunnelSegment& segment) { return segment.layerType == QStringLiteral("铁路"); });
        if (hasRailSegments && stationGeometries.isEmpty()) {
            appendLog(QStringLiteral("检查失败：存在铁路隧道线，但未在关联图层上传火车站符号图层。"));
            m_run->setEnabled(true);
            return;
        }
        QMap<QString, QList<double>> stationPositionsByRoute;
        for (auto routeIt = routeParts.cbegin(); routeIt != routeParts.cend(); ++routeIt) {
            if (routeIt.value().layerType != QStringLiteral("铁路")
                || !segmentsByRoute.contains(routeIt.key())) continue;
            for (const QgsGeometry& station : stationGeometries) {
                const QgsGeometry center = station.centroid();
                if (center.isNull() || center.isEmpty()) continue;
                const bool intersects = routeIt.value().geometry.intersects(station);
                if (!intersects && routeIt.value().geometry.distance(center) > 1.0) continue;
                QgsGeometry onRoute = intersects
                    ? routeIt.value().geometry.intersection(station) : center;
                if (onRoute.isNull() || onRoute.isEmpty()) onRoute = center;
                const double position = routeIt.value().geometry.lineLocatePoint(onRoute.centroid());
                if (position >= 0.0) stationPositionsByRoute[routeIt.key()].append(position);
            }
        }

        QgsFields errorFields;
        errorFields.append(QgsField(QStringLiteral("tunnel_nam"), QVariant::String, QString(), 254));
        errorFields.append(QgsField(QStringLiteral("layer_type"), QVariant::String, QString(), 20));
        errorFields.append(QgsField(QStringLiteral("file_a"), QVariant::String, QString(), 80));
        errorFields.append(QgsField(QStringLiteral("fid_a"), QVariant::LongLong));
        errorFields.append(QgsField(QStringLiteral("part_a"), QVariant::Int));
        errorFields.append(QgsField(QStringLiteral("file_b"), QVariant::String, QString(), 80));
        errorFields.append(QgsField(QStringLiteral("fid_b"), QVariant::LongLong));
        errorFields.append(QgsField(QStringLiteral("part_b"), QVariant::Int));
        errorFields.append(QgsField(QStringLiteral("dist_m"), QVariant::Double));
        errorFields.append(QgsField(QStringLiteral("limit_m"), QVariant::Double));
        errorFields.append(QgsField(QStringLiteral("reason"), QVariant::String, QString(), 32));
        errorFields.append(QgsField(QStringLiteral("expected"), QVariant::String, QString(), 16));
        errorFields.append(QgsField(QStringLiteral("actual"), QVariant::String, QString(), 16));
        QList<QgsFeature> errorFeatures;
        int comparedPairs = 0;
        int stationBreakPairs = 0;
        int shortGapPairs = 0;
        int stationConnectedPairs = 0;
        int lineCrossesStation = 0;
        int parallelMileageGroups = 0;
        int unmatchedParallelLines = 0;
        const QStringList protectedPaths = m_sourcePaths + m_associatedPaths + m_targetPaths + m_xmlPaths;

        for (auto groupIt = segmentsByRoute.begin(); groupIt != segmentsByRoute.end(); ++groupIt) {
            QList<ParsedTunnelSegment> group = groupIt.value();
            const QList<double> stationPositions = stationPositionsByRoute.value(groupIt.key());
            std::sort(group.begin(), group.end(),
                      [](const ParsedTunnelSegment& a, const ParsedTunnelSegment& b) {
                if (a.routeStart != b.routeStart) return a.routeStart < b.routeStart;
                return a.routeEnd < b.routeEnd;
            });

            // Parallel strokes are separate tunnel lines, not a combined symbol.
            if (!stationPositions.isEmpty()) {
                for (const ParsedTunnelSegment& segment : group) {
                    bool crossesStation = false;
                    for (double position : stationPositions) {
                        if (position > segment.routeStart + 1.0
                            && position < segment.routeEnd - 1.0) {
                            crossesStation = true;
                            break;
                        }
                    }
                    if (!crossesStation) continue;
                    QgsFeature output(errorFields);
                    QgsMultiPolylineXY lines;
                    lines.append(segment.geometry.asPolyline());
                    output.setGeometry(QgsGeometry::fromMultiPolylineXY(lines));
                    QgsAttributes attributes;
                    attributes << segment.tunnelName << segment.layerType
                               << segment.fileName << QVariant::fromValue<qlonglong>(segment.fid)
                               << segment.partIndex
                               << QString() << QVariant() << QVariant()
                               << QVariant() << spacingMeters << QStringLiteral("跨站未断开")
                               << QStringLiteral("断开") << QStringLiteral("连接");
                    output.setAttributes(attributes);
                    errorFeatures.append(output);
                    ++lineCrossesStation;
                    appendLog(QStringLiteral("不符合（跨站未断开）：隧道线 FID %1 跨越同一路线的火车站。")
                                  .arg(segment.fid));
                }
            }

            if (group.size() < 2) {
                appendLog(QStringLiteral("%1 GDB 路线“%2”仅匹配到 1 条解析隧道线，无相邻线可比较。")
                              .arg(group.first().layerType, group.first().tunnelName));
                continue;
            }
            // Overlapping mileages identify parallel strokes at one location. They
            // remain separate lines; only the ordering is grouped so that the next
            // location can be matched left-to-left and right-to-right.
            QList<QList<int>> mileageGroups;
            QList<double> mileageGroupEnds;
            for (int i = 0; i < group.size(); ++i) {
                const ParsedTunnelSegment& segment = group.at(i);
                if (mileageGroups.isEmpty()
                    || segment.routeStart >= mileageGroupEnds.last() - 1.0) {
                    QList<int> nextGroup;
                    nextGroup.append(i);
                    mileageGroups.append(nextGroup);
                    mileageGroupEnds.append(segment.routeEnd);
                } else {
                    mileageGroups.last().append(i);
                    mileageGroupEnds.last() = qMax(mileageGroupEnds.last(), segment.routeEnd);
                }
            }
            for (const QList<int>& mileageGroup : mileageGroups) {
                if (mileageGroup.size() > 1) ++parallelMileageGroups;
            }

            if (mileageGroups.size() < 2) {
                appendLog(QStringLiteral("%1 GDB 路线“%2”的解析线只形成 1 个里程段，没有前后相邻段可比较。")
                              .arg(group.first().layerType, group.first().tunnelName));
                continue;
            }

            const QgsGeometry& routeLine = routeParts.constFind(groupIt.key()).value().geometry;
            const double routeLength = routeLine.length();
            auto lateralOffset = [&routeLine, routeLength](const QgsPointXY& endpoint,
                                                           double chainage) {
                const double position = qBound(0.0, chainage, routeLength);
                const double before = qMax(0.0, position - 10.0);
                const double after = qMin(routeLength, position + 10.0);
                const QgsGeometry center = routeLine.interpolate(position);
                const QgsGeometry start = routeLine.interpolate(before);
                const QgsGeometry finish = routeLine.interpolate(after);
                if (center.isNull() || center.isEmpty() || start.isNull() || start.isEmpty()
                    || finish.isNull() || finish.isEmpty()) return std::nan("");
                const QgsPointXY centerPoint = center.asPoint();
                const QgsPointXY startPoint = start.asPoint();
                const QgsPointXY finishPoint = finish.asPoint();
                const double dx = finishPoint.x() - startPoint.x();
                const double dy = finishPoint.y() - startPoint.y();
                const double tangentLength = std::hypot(dx, dy);
                if (tangentLength < 0.001) return std::nan("");
                return (dx * (endpoint.y() - centerPoint.y())
                        - dy * (endpoint.x() - centerPoint.x())) / tangentLength;
            };
            struct LaneCandidate {
                int segmentIndex = -1;
                double lateral = 0.0;
            };
            auto laneLess = [&group](const LaneCandidate& a, const LaneCandidate& b) {
                if (a.lateral != b.lateral) return a.lateral < b.lateral;
                return group.at(a.segmentIndex).fid < group.at(b.segmentIndex).fid;
            };

            for (int mileageIndex = 0; mileageIndex + 1 < mileageGroups.size(); ++mileageIndex) {
                const QList<int>& earlier = mileageGroups.at(mileageIndex);
                const QList<int>& later = mileageGroups.at(mileageIndex + 1);
                QList<LaneCandidate> earlierLanes;
                QList<LaneCandidate> laterLanes;
                bool invalidLateral = false;
                for (int segmentIndex : earlier) {
                    const ParsedTunnelSegment& segment = group.at(segmentIndex);
                    const double lateral = lateralOffset(segment.endPoint, segment.routeEnd);
                    if (!std::isfinite(lateral)) invalidLateral = true;
                    earlierLanes.append({segmentIndex, lateral});
                }
                for (int segmentIndex : later) {
                    const ParsedTunnelSegment& segment = group.at(segmentIndex);
                    const double lateral = lateralOffset(segment.startPoint, segment.routeStart);
                    if (!std::isfinite(lateral)) invalidLateral = true;
                    laterLanes.append({segmentIndex, lateral});
                }
                if (invalidLateral) {
                    appendLog(QStringLiteral("跳过路线“%1”的一组相邻里程段：GDB 路线局部方向无法计算，不能可靠区分并行线路。")
                                  .arg(group.first().tunnelName));
                    continue;
                }
                std::sort(earlierLanes.begin(), earlierLanes.end(), laneLess);
                std::sort(laterLanes.begin(), laterLanes.end(), laneLess);

                // Match lateral ranks one-to-one. If one side has fewer lines,
                // choose its best ordered subset on the other side; never cross lanes.
                const bool earlierIsSmaller = earlierLanes.size() <= laterLanes.size();
                const QList<LaneCandidate>& smaller = earlierIsSmaller ? earlierLanes : laterLanes;
                const QList<LaneCandidate>& larger = earlierIsSmaller ? laterLanes : earlierLanes;
                const int smallCount = smaller.size();
                const int largeCount = larger.size();
                const double infinity = 1e100;
                QVector<QVector<double>> costs(smallCount + 1,
                                               QVector<double>(largeCount + 1, infinity));
                QVector<QVector<bool>> matched(smallCount + 1,
                                               QVector<bool>(largeCount + 1, false));
                for (int j = 0; j <= largeCount; ++j) costs[0][j] = 0.0;
                for (int i = 1; i <= smallCount; ++i) {
                    for (int j = i; j <= largeCount; ++j) {
                        const double skipped = costs[i][j - 1];
                        const double paired = costs[i - 1][j - 1]
                            + std::abs(smaller.at(i - 1).lateral - larger.at(j - 1).lateral);
                        if (paired <= skipped) {
                            costs[i][j] = paired;
                            matched[i][j] = true;
                        } else {
                            costs[i][j] = skipped;
                        }
                    }
                }
                QList<QPair<int, int>> linePairs;
                int i = smallCount;
                int j = largeCount;
                while (i > 0 && j > 0) {
                    if (matched[i][j]) {
                        const int earlierIndex = earlierIsSmaller
                            ? smaller.at(i - 1).segmentIndex : larger.at(j - 1).segmentIndex;
                        const int laterIndex = earlierIsSmaller
                            ? larger.at(j - 1).segmentIndex : smaller.at(i - 1).segmentIndex;
                        linePairs.prepend(qMakePair(earlierIndex, laterIndex));
                        --i;
                    }
                    --j;
                }
                unmatchedParallelLines += largeCount - smallCount;
                if (largeCount != smallCount) {
                    appendLog(QStringLiteral("路线“%1”的相邻里程段并行线数量不同（%2 / %3）；按左右位置配对 %4 对，未配对 %5 条不参与间隔检查。")
                                  .arg(group.first().tunnelName).arg(earlier.size()).arg(later.size())
                                  .arg(linePairs.size()).arg(largeCount - smallCount));
                }
                for (const QPair<int, int>& pair : linePairs) {
                    const ParsedTunnelSegment& first = group.at(pair.first);
                    const ParsedTunnelSegment& second = group.at(pair.second);
                    if (earlier.size() > 1 || later.size() > 1) {
                        appendLog(QStringLiteral("同侧配对：%1 路线“%2”解析线 FID %3 → FID %4。")
                                      .arg(first.layerType, first.tunnelName,
                                           QString::number(first.fid), QString::number(second.fid)));
                    }
                    const double distance = std::hypot(
                        second.startPoint.x() - first.endPoint.x(),
                        second.startPoint.y() - first.endPoint.y());
                    const bool isConnected = distance <= 1.0;
                    bool stationBetween = false;
                    for (double position : stationPositions) {
                        if (position >= first.routeEnd - 1.0
                            && position <= second.routeStart + 1.0) {
                            stationBetween = true;
                            break;
                        }
                    }
                    ++comparedPairs;
                    if (stationBetween) ++stationBreakPairs;
                    const bool shouldConnect = !stationBetween && distance <= spacingMeters;
                    if (isConnected == shouldConnect) continue;

                    const QString reason = stationBetween
                        ? QStringLiteral("跨站仍连接") : QStringLiteral("短距未连接");
                    QgsMultiPolylineXY pairLines;
                    pairLines.append(first.geometry.asPolyline());
                    pairLines.append(second.geometry.asPolyline());
                    QgsFeature output(errorFields);
                    output.setGeometry(QgsGeometry::fromMultiPolylineXY(pairLines));
                    QgsAttributes attributes;
                    attributes << first.tunnelName << first.layerType
                               << first.fileName
                               << QVariant::fromValue<qlonglong>(first.fid)
                               << first.partIndex
                               << second.fileName
                               << QVariant::fromValue<qlonglong>(second.fid)
                               << second.partIndex
                               << distance << spacingMeters << reason
                               << (shouldConnect ? QStringLiteral("连接") : QStringLiteral("断开"))
                               << (isConnected ? QStringLiteral("连接") : QStringLiteral("断开"));
                    output.setAttributes(attributes);
                    errorFeatures.append(output);
                    if (stationBetween) ++stationConnectedPairs;
                    else ++shortGapPairs;
                    appendLog(QStringLiteral("不符合（%1）：%2 GDB 路线“%3”上前后相邻隧道线 FID %4 与 %5 的相向端点距离 %6 m（阈值 %7 m）；应%8、实际%9。")
                                  .arg(reason, first.layerType, first.tunnelName,
                                       QString::number(first.fid), QString::number(second.fid))
                                  .arg(distance, 0, 'f', 3).arg(spacingMeters, 0, 'f', 3)
                                  .arg(shouldConnect ? QStringLiteral("连接") : QStringLiteral("断开"))
                                  .arg(isConnected ? QStringLiteral("连接") : QStringLiteral("断开")));
                }
            }
        }
        m_problemCount = errorFeatures.size();
        if (!errorFeatures.isEmpty()) {
            QString outputError;
            if (writeSpacingErrorLayer(this, m_outputFilePath, metricCrs, errorFields, errorFeatures,
                                       QgsWkbTypes::MultiLineString, protectedPaths, outputError,
                                       m_taskManagerMode)) {
                m_outputWritten = true;
                appendLog(QStringLiteral("隧道线错误结果图层已输出：%1（%2 条记录；相邻线错误每条包含两条解析隧道线）。")
                              .arg(QDir::toNativeSeparators(m_outputFilePath)).arg(errorFeatures.size()));
            } else
                appendLog(QStringLiteral("错误结果图层输出失败：%1").arg(outputError));
        } else {
            appendLog(QStringLiteral("本次未发现间隔错误，不生成错误结果图层。若同名旧图层已存在，将保持不变。"));
        }
        appendLog(QStringLiteral("检查完成：匹配解析隧道线段 %1 个；识别并行里程段 %2 组，同侧匹配并比较相邻线 %3 对，未配对线 %4 条，跨火车站 %5 对；短距未连接 %6 对、跨站仍连接 %7 对、单线跨站未断开 %8 条，共 %9 条错误（阈值 %10 m）。")
                      .arg(parsedSegments.size()).arg(parallelMileageGroups).arg(comparedPairs)
                      .arg(unmatchedParallelLines).arg(stationBreakPairs).arg(shortGapPairs)
                      .arg(stationConnectedPairs).arg(lineCrossesStation)
                      .arg(errorFeatures.size()).arg(spacingMeters, 0, 'f', 3));
    }
    catch (const QgsCsException& exception) {
        appendLog(QStringLiteral("检查失败：坐标转换异常：%1").arg(exception.what()));
    }
    catch (const std::exception& exception) {
        appendLog(QStringLiteral("检查失败：%1").arg(QString::fromLocal8Bit(exception.what())));
    }
    m_run->setEnabled(true);
}

void showFeatureSpacingCheckDialog(QWidget* parent)
{
    FeatureSpacingCheckDialog dialog(parent);
    dialog.exec();
}

