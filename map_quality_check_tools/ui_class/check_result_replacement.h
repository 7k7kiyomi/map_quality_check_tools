#pragma once

#include <QRegularExpression>
#include <QString>

namespace CheckResultReplacement {
inline bool supports(const QString& base)
{
    return base.startsWith(QStringLiteral("几何走向异常检查_"))
        || base.startsWith(QStringLiteral("直角化角度阈值控制_"))
        || base.startsWith(QStringLiteral("线要素拓扑检查_"));
}

inline bool owns(const QString& base, const QString& fileName)
{
    const QRegularExpression pattern(
        QStringLiteral("^%1(?:_[0-9]+)?\\.(?:shp|shx|dbf|prj|cpg|qix|sbn|sbx|shp\\.xml)$")
            .arg(QRegularExpression::escape(base)),
        QRegularExpression::CaseInsensitiveOption);
    return supports(base) && pattern.match(fileName).hasMatch();
}
}
