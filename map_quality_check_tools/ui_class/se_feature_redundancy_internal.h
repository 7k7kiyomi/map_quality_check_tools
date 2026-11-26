#pragma once
#include "map_check_algorithms.h"
#include <QMap>
#include <QJsonArray>
#include <QSet>
#include <ogrsf_frmts.h>
#include <memory>
#include <vector>

namespace redundancy {
using Geo = std::shared_ptr<OGRGeometry>;
Geo geometry(OGRGeometry* raw);
Geo copy(const OGRGeometry* raw);
double length(const OGRGeometry* raw);
Geo polygon(const OGRGeometry* raw, bool repair = false);
Geo unite(const std::vector<Geo>& parts);
QStringList split(const QString& text);

struct Feature {
    QString key, path, problem;
    QJsonArray fids;
    QMap<QString, QString> attrs;
    QSet<QString> attributeConflicts;
    // A merged river skeleton may represent several source ELEMIDs (one per segment).
    QSet<QString> memberElemIds;
    Geo geom;
    // Immutable raw path/FID members survive grouping, centering and clipping.
    QJsonArray originals;
    // Per-reference cache avoids buffering a long skeleton for every rendered fragment.
    mutable Geo matchBuffer;
    mutable double matchRadius = -1;
    QString text(const QString& name) const;
};
struct Config {
    QString xml, severity;
    QMap<QString, QString> values, units;
    QString text(const QString& key, const QString& fallback = QString()) const;
    double number(const QString& key, double fallback) const;
    bool flag(const QString& key, bool fallback) const;
    QStringList list(const QString& key, const QString& fallback = QString()) const;
};
struct Context {
    AlgorithmFilePaths paths;
    Config cfg;
    int scale = 0;
    std::shared_ptr<OGRSpatialReference> srs;
    std::vector<Feature> targets;
    std::vector<Feature> uncertainAreas;
    QJsonObject info, errors;
    QJsonArray reviews, notices;
    QSet<QString> reviewed;
    std::vector<std::pair<Geo, QJsonObject>> locations;
    QMap<QString, Geo> originalGeometry;
    QSet<QString> emitted;
    double metresPerUnit = 1;
    double metres(double units) const;
    double units(double metres) const;
    double mapMm(double units) const;
    double fromMm(double mm) const;
    std::vector<Feature> actual(const QString& name, bool required = true);
    std::vector<Feature> gdb(const QString& name, const QString& filter = QString(), bool required = true);
    Geo area(const QString& name, bool required = true, bool allowPartial = false);
    void error(const QString& rule, const QString& description, const Feature& feature,
               QJsonObject detail = QJsonObject(), Geo location = Geo());
    void review(const QString& rule, const QString& description, const Feature* feature = nullptr,
                QJsonObject detail = QJsonObject());
    void notice(const QString& rule, const QString& description, const Feature* feature = nullptr);
    void absent(const QString& kind, const QString& rule, const QString& description,
                const Feature& reference, QJsonObject detail = QJsonObject(), Geo expected = Geo());
};
std::vector<Feature> symbols(std::vector<Feature> parts, const QString& field = QStringLiteral("symbol_id"));
// Used only for result river/tunnel grouping; reference and other symbol grouping stay separate.
std::vector<Feature> resultSymbols(Context& ctx, std::vector<Feature> parts, const QString& field,
                                   const QStringList& attributes, const QString& diagnosticRule);
bool reliableResultId(const QString& value);
bool resultAttributePresent(const QString& value);
QJsonObject identity(const Feature& f);
bool comparison(double actual, const QString& compare, double limit);
QJsonObject numericDetail(double actual, const QString& compare, double limit,
                         const QString& unit, const QString& xmlRule, const QString& decision);
QString numericMessage(const QString& subject, const QJsonObject& detail);
QJsonObject forbiddenDetail(const QString& xmlRule);
// Returns a unique, geometry-constrained source match; -1 means no match, -2 ambiguous.
int match(const Feature& rendered, const std::vector<Feature>& sources, double radius,
          bool lineCoverage = false);
void checkTransport(Context& ctx);
void checkRivers(Context& ctx);
void checkBridges(Context& ctx);
void checkVillages(Context& ctx);
}
