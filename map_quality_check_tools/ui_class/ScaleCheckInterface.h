#ifndef SCALE_CHECK_INTERFACE_H
#define SCALE_CHECK_INTERFACE_H

#include <QString>
#include <QMap>

class QgsVectorLayer;
class QgsMapCanvas;

/**
 * @brief 比例尺与投影参数检查接口（静态类）
 *
 * 本接口用于外部模块调用比例尺与投影参数检查算法。
 * 可独立于 UI 使用，适合插件、批处理或命令行集成。
 */
class ScaleCheckInterface
{
public:
    /**
     * @brief 检查地图画布的比例尺是否符合预期
     * @param canvas QGIS 地图画布对象
     * @param expectScale 期望比例尺（如 50000 表示 1:50000）
     * @param errMsg 输出：错误提示信息
     * @return true 比例尺符合要求；false 不符合
     */
    static bool checkScale(QgsMapCanvas* canvas, double expectScale, QString& errMsg);

    /**
     * @brief 从 XML 文件加载比例尺 / 投影检查规则
     * @param xmlPath XML 文件路径
     * @param errMsg 输出：错误提示信息
     * @return true 加载成功；false 加载失败
     */
    static bool loadRules(const QString& xmlPath, QString& errMsg);

    /**
     * @brief 检查图层的投影参数是否符合 XML 中定义的标准
     * @param layer 待检查的矢量图层
     * @param errMsg 输出：错误提示信息
     * @return true 投影参数符合；false 不符合
     */
    static bool checkProjection(QgsVectorLayer* layer, QString& errMsg);

    /**
     * @brief 获取当前加载的检查规则（用于日志或调试）
     * @return QMap<QString, QString> 格式的规则键值对
     */
    static QMap<QString, QString> currentRules();

private:
    // 内部缓存规则数据
    static QString m_xmlFilePath;
    static QMap<QString, QString> m_rules;
};

#endif // SCALE_CHECK_INTERFACE_H
