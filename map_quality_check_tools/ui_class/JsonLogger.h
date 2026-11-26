#ifndef JSONLOGGER_H
#define JSONLOGGER_H

#include <QObject>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QString>

class JsonLogger : public QObject
{
    Q_OBJECT

public:
    JsonLogger(QObject* parent = nullptr);

    void addCheckItem(const QString& checkId, const QString& checkItemName,
        const QString& status, bool errExist, const QString& info,
        const QJsonObject& errors = QJsonObject());

    QString createErrorDescription(int featureId, const QString& description);

    static QJsonObject createErrorObject(int featureId, const QString& description);

    bool saveToFile(const QString& filePath);

    static bool saveJsonToFile(const QJsonObject& jsonObject, const QString& filePath);

private:
    QJsonObject m_jsonLog;
};

#endif 