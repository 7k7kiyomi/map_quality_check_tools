#ifndef XMLCONFIGREADER_H
#define XMLCONFIGREADER_H

#include <QObject>
#include <QMap>
#include <QVariant>
#include <QDomDocument>

class XmlConfigReader : public QObject
{
    Q_OBJECT

public:
    explicit XmlConfigReader(QObject* parent = nullptr);
    ~XmlConfigReader();

    bool loadConfig(const QString& xmlFilePath);
    bool isLoaded() const { return m_isLoaded; }

    QList<int> getAvailableScales() const;
    bool hasScale(int scale) const;
    QList<QString> getCheckItemNames(int scale) const;
    QMap<QString, QVariant> getCheckItemParams(int scale, const QString& itemName) const;
    double getRuleValue(int scale, const QString& itemName, const QString& ruleName, double defaultValue) const;
    QString getRuleStringValue(int scale, const QString& itemName, const QString& ruleName, const QString& defaultValue) const;
    QMap<QString, QVariant> getCheckItemParams(const QString& itemName);
    QList<QString> getCheckItemNames() const;
    double getRuleValue(const QString& itemName, const QString& ruleName, double defaultValue);
    QString getRuleStringValue(const QString& itemName, const QString& ruleName, const QString& defaultValue);
    bool getRuleBoolValue(int scale, const QString& itemName, const QString& ruleName, bool defaultValue = false) const;
    QString getRuleUnit(int scale, const QString& itemName, const QString& ruleName, const QString& defaultValue = QString()) const;
    QStringList getRuleStringList(int scale, const QString& itemName, const QString& ruleName) const;
    bool hasCheckItem(int scale, const QString& itemName) const; 
    bool isCheckItemAvailable(int scale, const QString& itemName) const;
private:
    bool parseCheckList(const QDomElement& checkListElement);
    bool parseScale(const QDomElement& scaleElement);
    bool parseCheckItem(const QDomElement& itemElement, QMap<QString, QVariant>& params);

private:
    QDomDocument m_doc;
    bool m_isLoaded;

    QMap<int, QMap<QString, QMap<QString, QVariant>>> m_scaleConfigs;
};

#endif 