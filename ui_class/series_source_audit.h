#pragma once

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <cmath>

namespace SeriesSourceAudit {
inline QString pathKey(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

inline QJsonObject layerEvidence(const QJsonArray& audits,const QString& shpPath)
{
    for(const auto& value:audits) {
        const QJsonObject audit=value.toObject();
        if(pathKey(audit.value(QStringLiteral("shp_dir")).toString())!=pathKey(QFileInfo(shpPath).absolutePath())) continue;
        QJsonObject layer=audit.value(QStringLiteral("layers")).toObject().value(QFileInfo(shpPath).fileName()).toObject();
        layer[QStringLiteral("pdf_path")]=audit.value(QStringLiteral("pdf_path"));
        layer[QStringLiteral("pdf_sha256")]=audit.value(QStringLiteral("pdf_sha256"));
        return layer;
    }
    return QJsonObject();
}

inline QString colorStatus(const QJsonObject& evidence,const QString& field,const QString& text)
{
    if(evidence.value(QStringLiteral("pdf_sha256")).toString().isEmpty()) return QStringLiteral("unverified");
    const auto colors=evidence.value(QStringLiteral("colors")).toObject().value(field).toObject();
    for(const auto& value:colors.value(QStringLiteral("verified_values")).toArray())
        if(value.toString().trimmed().compare(text.trimmed(),Qt::CaseInsensitive)==0) return QStringLiteral("verified");
    for(const auto& value:colors.value(QStringLiteral("mismatched_values")).toArray())
        if(value.toString().trimmed().compare(text.trimmed(),Qt::CaseInsensitive)==0) return QStringLiteral("mismatch");
    return QStringLiteral("unverified");
}

inline bool widthVerifiedInPoints(const QJsonObject& evidence,double actual)
{
    if(evidence.value(QStringLiteral("pdf_sha256")).toString().isEmpty() || !std::isfinite(actual)) return false;
    for(const auto& value:evidence.value(QStringLiteral("widths_pt")).toObject().value(QStringLiteral("verified_values")).toArray()) {
        const double width=value.toDouble();
        if(std::fabs(actual-width)<=qMax(.001,std::fabs(width)*.003)) return true;
    }
    return false;
}

inline QJsonObject run(QString python,const QString& script,const QString& pdf,const QString& shpDir,const QString& cacheDir)
{
    QJsonObject unavailable{{QStringLiteral("status"),QStringLiteral("not_evaluable")},
        {QStringLiteral("pdf_path"),pdf},{QStringLiteral("shp_dir"),shpDir}};
    if(!QFileInfo::exists(script) || !QFileInfo::exists(pdf) || !QDir(shpDir).exists()) {
        unavailable[QStringLiteral("reason")]=QStringLiteral("源核验脚本、PDF 或 SHP 目录不可用");return unavailable;
    }
    QDir root=QFileInfo(script).dir();root.cdUp();root.cdUp();
    const QString runtime=root.filePath(QStringLiteral("OSGEO4W_32815"));
    const QString runtimePython=QDir(runtime).filePath(QStringLiteral("apps/Python39/python.exe"));
    if(QFileInfo::exists(runtimePython)) python=runtimePython;
    if(!QFileInfo::exists(python) || !QDir().mkpath(cacheDir)) {
        unavailable[QStringLiteral("reason")]=QStringLiteral("源核验 Python 运行环境或缓存目录不可用");return unavailable;
    }
    const QString key=QString::fromLatin1(QCryptographicHash::hash((pathKey(pdf)+QStringLiteral("|")+pathKey(shpDir)).toUtf8(),QCryptographicHash::Sha256).toHex());
    const QString output=QDir(cacheDir).filePath(key+QStringLiteral(".json"));
    QProcess process;
    QProcessEnvironment environment=QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONHOME"),QFileInfo(python).absolutePath());
    environment.remove(QStringLiteral("PYTHONPATH"));environment.remove(QStringLiteral("GDAL_DRIVER_PATH"));
    environment.insert(QStringLiteral("PYTHONNOUSERSITE"),QStringLiteral("1"));
    if(QDir(runtime).exists()) {
        environment.insert(QStringLiteral("GDAL_DATA"),QDir(runtime).filePath(QStringLiteral("share/gdal")));
        environment.insert(QStringLiteral("PROJ_LIB"),QDir(runtime).filePath(QStringLiteral("share/proj")));
        environment.insert(QStringLiteral("PROJ_DATA"),QDir(runtime).filePath(QStringLiteral("share/proj")));
        environment.insert(QStringLiteral("PATH"),QDir(runtime).filePath(QStringLiteral("bin"))+QStringLiteral(";")+environment.value(QStringLiteral("PATH")));
    }
    process.setProcessEnvironment(environment);
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args){args->flags|=0x08000000;});
#endif
    process.start(python,{script,QStringLiteral("--pdf"),pdf,QStringLiteral("--shp-dir"),shpDir,QStringLiteral("--output"),output});
    if(!process.waitForStarted(10000)) {unavailable[QStringLiteral("reason")]=process.errorString();return unavailable;}
    QElapsedTimer timer;timer.start();
    while(process.state()!=QProcess::NotRunning) {
        process.waitForFinished(100);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents,25);
        if(timer.elapsed()>300000) {process.kill();process.waitForFinished(5000);unavailable[QStringLiteral("reason")]=QStringLiteral("源核验超时");return unavailable;}
    }
    if(process.exitStatus()!=QProcess::NormalExit || process.exitCode()!=0) {
        unavailable[QStringLiteral("reason")]=QString::fromUtf8(process.readAllStandardError()).right(1600);return unavailable;
    }
    QFile file(output);
    if(!file.open(QIODevice::ReadOnly)) {unavailable[QStringLiteral("reason")]=QStringLiteral("源核验结果不可读");return unavailable;}
    const auto report=QJsonDocument::fromJson(file.readAll()).object();
    if(report.value(QStringLiteral("pdf_sha256")).toString().isEmpty()
        || pathKey(report.value(QStringLiteral("shp_dir")).toString())!=pathKey(shpDir)
        || pathKey(report.value(QStringLiteral("pdf_path")).toString())!=pathKey(pdf)) {
        unavailable[QStringLiteral("reason")]=QStringLiteral("源核验证据与当前输入不一致");return unavailable;
    }
    return report;
}
} // namespace SeriesSourceAudit
