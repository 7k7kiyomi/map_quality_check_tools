#include "QualityJsonExporter.h"
#include <QFileDialog>
#include <QTreeWidgetItem>
#include <QMessageBox>
#include <QFile>
#include <QDateTime>
#include <QIcon> 
#include <QJsonDocument>
#include <QJsonArray>

static QString checkItemIdFromName(const QString& checkItemName)
{
    // 任务树导出时没有保存算法返回的 CheckItemResult；至少保留已知检查项的固定 ID。
    if (checkItemName.trimmed() == QString::fromUtf8(u8"削尖角度阈值控制"))
        return QStringLiteral("110");
    return QString();
}

QualityJsonExporter::QualityJsonExporter(const QString& userName, QObject* parent)
    : QObject(parent), m_userName(userName)
{
    m_globalStartTime = QDateTime::currentDateTime();
    m_globalEndTime = QDateTime::currentDateTime();
}

bool QualityJsonExporter::exportFromTreeWidget(QTreeWidget* treeWidget, const QString& outputLogPath)
{
    if (!treeWidget) {
        QMessageBox::critical(nullptr, tr("错误"), tr("任务列表控件不存在！"));
        return false;
    }

    // 1. 从treeWidget提取任务数据
    auto tasks = extractTasksFromTree(treeWidget);
    if (tasks.empty()) {
        QMessageBox::warning(nullptr, tr("警告"), tr("未从任务列表中提取到任何任务！"));
        return false;
    }

    // 2. 构建JSON根对象
    QJsonObject rootObj = buildRootObject(tasks);

    // 3. 处理保存路径
    QString savePath = outputLogPath;
    if (savePath.isEmpty()) {
        QString defaultFileName = QString("quality_check_result_%1.json")
            .arg(QDateTime::currentDateTime().toString("yyyyMMddHHmmss"));

        savePath = QFileDialog::getSaveFileName(
            nullptr,
            tr("保存质检JSON日志"),
            defaultFileName,
            tr("JSON文件 (*.json);;所有文件 (*)")
        );
    }

    if (savePath.isEmpty()) {
        QMessageBox::information(nullptr, tr("取消保存"), tr("已取消JSON日志保存"));
        return false;
    }

    // 4. 写入文件
    QFile file(savePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(nullptr, tr("保存失败"),
            tr("无法打开文件：%1").arg(savePath));
        return false;
    }

    QJsonDocument doc(rootObj);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    QMessageBox::information(nullptr, tr("保存成功"),
        tr("JSON日志已保存至：%1").arg(savePath));
    return true;
}

std::vector<QualityTask> QualityJsonExporter::extractTasksFromTree(QTreeWidget* treeWidget)
{
    std::vector<QualityTask> tasks;

    // 遍历所有顶层节点（子任务列表）
    for (int i = 0; i < treeWidget->topLevelItemCount(); ++i) {
        QTreeWidgetItem* taskItem = treeWidget->topLevelItem(i);
        if (!taskItem) continue;

        QualityTask task;
        task.layer_name = taskItem->text(0);
        task.status = "已完成";

        // 设置任务时间
        task.start_time = m_globalStartTime.addSecs(-(treeWidget->topLevelItemCount() - i) * 60);
        task.end_time = m_globalEndTime;

        // 遍历子节点（质检项）
        for (int j = 0; j < taskItem->childCount(); ++j) {
            QTreeWidgetItem* checkItem = taskItem->child(j);
            if (!checkItem) continue;

            QualityCheckItem checkItemData;
            checkItemData.name = checkItem->text(1);
            checkItemData.id = checkItemIdFromName(checkItemData.name);

            // 通过图标判断质检项状态
            checkItemData.status = getCheckItemStatusByIcon(checkItem);

            // 预留统计字段
            checkItemData.feature_num = 0;
            checkItemData.err_num = 0;

            task.check_items.push_back(checkItemData);
        }

        if (!task.check_items.empty()) {
            tasks.push_back(task);
        }
    }

    return tasks;
}

QJsonObject QualityJsonExporter::buildRootObject(const std::vector<QualityTask>& tasks)
{
    QJsonObject rootObj;

    rootObj["user"] = m_userName;

    QJsonObject timingObj;
    // --- 已修复：直接使用 QString，移除不必要的 .toStdString() ---
    timingObj["start_time"] = m_globalStartTime.toString("yyyy-MM-ddTHH:mm:ss.zzzZ");
    timingObj["end_time"] = m_globalEndTime.toString("yyyy-MM-ddTHH:mm:ss.zzzZ");
    rootObj["timing"] = timingObj;

    // 构建 info 字段
    int totalTasks = tasks.size();
    int completedTasks = 0;
    int totalErrNum = 0;
    for (const auto& task : tasks) {
        if (task.status == "已完成") {
            completedTasks++;
        }
        for (const auto& checkItem : task.check_items) {
            totalErrNum += checkItem.errors.size();
        }
    }
    QJsonObject infoObj;
    infoObj["tasks_num"] = totalTasks;
    infoObj["completed_num"] = completedTasks;
    infoObj["uncompleted_num"] = totalTasks - completedTasks;
    infoObj["err_num"] = totalErrNum;
    rootObj["info"] = infoObj;

    QJsonArray tasksArray;
    for (const auto& task : tasks) {
        tasksArray.append(buildTaskObject(task));
    }
    rootObj["quality_tasks"] = tasksArray;

    return rootObj;
}

QJsonObject QualityJsonExporter::buildTaskObject(const QualityTask& task)
{
    QJsonObject taskObj;

    taskObj["layer_name"] = task.layer_name;
    taskObj["status"] = task.status;

    QJsonObject timingObj;
    timingObj["start_time"] = task.start_time.toString("yyyy-MM-ddTHH:mm:ss.zzzZ");
    timingObj["end_time"] = task.end_time.toString("yyyy-MM-ddTHH:mm:ss.zzzZ");
    taskObj["timing"] = timingObj;

    QJsonArray checkItemsArray;
    for (const auto& checkItem : task.check_items) {
        checkItemsArray.append(buildCheckItemObject(checkItem));
    }
    taskObj["check_items"] = checkItemsArray;

    return taskObj;
}

QJsonObject QualityJsonExporter::buildCheckItemObject(const QualityCheckItem& checkItem)
{
    QJsonObject checkItemObj;

    checkItemObj["id"] = checkItem.id;
    checkItemObj["name"] = checkItem.name;
    checkItemObj["status"] = checkItem.status;

    QJsonObject checkItemInfoObj;
    checkItemInfoObj["feature_num"] = checkItem.feature_num;
    checkItemInfoObj["err_num"] = checkItem.err_num;
    checkItemObj["info"] = checkItemInfoObj;

    QJsonArray errorsArray;
    for (const auto& error : checkItem.errors) {
        errorsArray.append(buildErrorObject(error));
    }
    checkItemObj["errors"] = errorsArray;

    return checkItemObj;
}

QJsonObject QualityJsonExporter::buildErrorObject(const QualityError& error)
{
    QJsonObject errorObj;
    errorObj["feature_id"] = error.feature_id;
    errorObj["description"] = error.description;
    return errorObj;
}

QString QualityJsonExporter::getCheckItemStatusByIcon(const QTreeWidgetItem* item)
{
    QIcon statusIcon = item->icon(2);
    QString iconPath = statusIcon.availableSizes().isEmpty() ? "" : statusIcon.name();

    if (iconPath.contains("红叉", Qt::CaseInsensitive)) {
        return "未通过";
    }
    else if (iconPath.contains("叹号", Qt::CaseInsensitive)) {
        return "未通过";
    }
    else if (iconPath.contains("勾选", Qt::CaseInsensitive)) {
        return "通过";
    }

    return "未检查";
}
