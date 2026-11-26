#ifndef QUALITY_JSON_EXPORTER_H
#define QUALITY_JSON_EXPORTER_H

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QTreeWidgetItem>
#include <vector>
#include <string>
#include <QJsonObject>  // <--- 替换
#include <QJsonArray>   // <--- 添加


// 错误信息结构
struct QualityError {
    QString feature_id;
    QString description;
};

// 质检项结构
struct QualityCheckItem {
    QString id;
    QString name;
    QString status;
    int feature_num = 0;
    int err_num = 0;
    std::vector<QualityError> errors;
};

// 任务结构
struct QualityTask {
    QString layer_name;
    QString status;
    QDateTime start_time;
    QDateTime end_time;
    std::vector<QualityCheckItem> check_items;
};

class QualityJsonExporter : public QObject
{
    Q_OBJECT
public:
    explicit QualityJsonExporter(const QString& userName, QObject* parent = nullptr);

    // 从treeWidget提取任务数据并生成JSON
    bool exportFromTreeWidget(QTreeWidget* treeWidget, const QString& outputLogPath);

private:
    // 从treeWidget提取任务数据
    std::vector<QualityTask> extractTasksFromTree(QTreeWidget* treeWidget);

    // --- 修改函数签名 ---
    // 构建JSON根对象
    QJsonObject buildRootObject(const std::vector<QualityTask>& tasks);
    // 构建单个任务的JSON对象
    QJsonObject buildTaskObject(const QualityTask& task);
    // 构建单个质检项的JSON对象
    QJsonObject buildCheckItemObject(const QualityCheckItem& checkItem);
    // 构建单个错误的JSON对象
    QJsonObject buildErrorObject(const QualityError& error);

    // 通过图标获取质检项状态
    QString getCheckItemStatusByIcon(const QTreeWidgetItem* item);

private:
    QString m_userName;
    QDateTime m_globalStartTime;
    QDateTime m_globalEndTime;
};

#endif // QUALITY_JSON_EXPORTER_H
