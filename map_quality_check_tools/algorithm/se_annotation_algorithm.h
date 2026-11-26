#ifndef SE_ANNOTATION_ALGORITHM_H
#define SE_ANNOTATION_ALGORITHM_H

#include <QString>
#include <QList>
#include <QMap>
#include <QColor>
#include <QSizeF>
#include <QPointF>
#include "../commontype/se_error_define.h"

QT_BEGIN_NAMESPACE
class QgsVectorLayer;
class QgsFeature;
QT_END_NAMESPACE

// 扩展规则项结构
struct CheckRule
{
    QString name;        // 规则名称
    QString description; // 规则描述
    QString value;       // 规则值
    QString unit;        // 单位（可选）
};

// 检查项结构
struct CheckItem
{
    int id = -1;            // 新增：检查项ID（默认-1表示无ID）
    QString name;           // 检查项名称
    QList<CheckRule> rules; // 规则列表
};

// 比例尺配置结构
struct ScaleConfig
{
    QString value;          // 比例尺值（可为空）
    QList<CheckItem> items; // 检查项列表
};

class SE_AnnotationAlgorithm
{
public:
    SE_AnnotationAlgorithm();
    ~SE_AnnotationAlgorithm();

    // 设置质检规则文件路径（默认比例尺为1:100000）
    bool loadCheckRules(const QString &xmlFilePath, const QString &scale = "100000");

    // 执行注记检查
    QList<AnnotationCheckResult> checkAnnotationLayer(QgsVectorLayer *layer);

    // 单项检查
    QList<AnnotationCheckResult> checkFont(QgsVectorLayer *layer);
    QList<AnnotationCheckResult> checkFontSize(QgsVectorLayer *layer);
    QList<AnnotationCheckResult> checkColor(QgsVectorLayer *layer);
    QList<AnnotationCheckResult> checkItalic(QgsVectorLayer *layer);

    // 主区路径由调用方从标准图层映射到实际图层；省内路径指向 GDB 面图层。
    void setRegionLayerPaths(const QString &mainRegionPath, const QString &provincePath);
    QString lastError() const { return m_lastError; }

    // 设置检查选项
    void setCheckOptions(bool fontCheck, bool fontSizeCheck, bool colorCheck, bool italicCheck);

    // 检查图层是否需要执行特定检查项
    bool isCheckRequired(const QString &checkType) const;

    // 新增：按检查项ID判断是否存在
    bool hasCheckItemById(int itemId) const;

    // 新增：按检查项ID获取规则值（可指定规则名），未找到返回空串
    QString getRuleValueById(int itemId, const QString &ruleName = "") const;

private:
    bool prepareRegionCheck(int itemId, QgsVectorLayer *layer, const QList<QStringList> &values);
    QString regionName(const QgsFeature &feature) const;
    int regionIndex(const QgsFeature &feature) const;

    // 解析XML质检规则
    bool parseXmlRules(const QString &xmlContent, const QString &scale);

    // 选择适用的比例尺配置
    ScaleConfig selectScaleConfig(const QList<ScaleConfig> &configs, const QString &scale);

    // 解析颜色值
    bool compareCMYKColor(const QString &actualCMYK, const QString &expectedCMYK, int tolerance);
    QColor parseStandardCMYKColor(const QString &cmykString);
    QColor parseCMYKColor(const QString &cmykString);
    QString hexToCMYKString(const QString &hexColor);

    // 字体名称转换
    QString convertFontName(const QString &englishName);

    // 解析字号
    QSizeF parseFontSize(const QString &sizeString);

    // 比较字号
    bool compareFontSize(const QSizeF &actual, const QSizeF &expected, double tolerance = 0.1);

    // 比较颜色
    bool compareColor(const QColor &actual, const QColor &expected, int tolerance = 5);

    // 解析斜体角度
    double parseShearAngle(const QString &angleString);

    // 比较斜体角度
    bool compareItalicAngle(double actualAngle, const QString &expectedDirection,
                            double minAngle, double maxAngle);

    // 获取字段完整名称（截取前10个字符）
    QString getFieldName(const QString &fullName) const;

    // 获取规则值（支持多规则查找）
    QString getRuleValue(const QString &itemName, const QString &ruleName = "") const;

private:
    QString m_mainRegionPath;
    QString m_provincePath;
    QString m_lastError;
    bool m_regionCheckEnabled = false;
    QMap<qint64, int> m_featureRegions;
    QMap<QString, FontMapping> m_fontMappings; // 字体映射表
    QString m_currentScale;                    // 当前比例尺
    ScaleConfig m_activeConfig;                // 当前活动的比例尺配置

    // 检查选项
    bool m_fontCheck;
    bool m_fontSizeCheck;
    bool m_colorCheck;
    bool m_italicCheck;
};

#endif // SE_ANNOTATION_ALGORITHM_H