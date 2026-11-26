#ifndef BOUNDARY_BAND_COLOR_H
#define BOUNDARY_BAND_COLOR_H

#include <QString>
#include <QStringList>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <algorithm>

// CMYK tuples and legend shorthand use percentages. RGB uses 0..255.
// Eight-digit hex is RRGGBBAA; bare r,g,b,a uses a byte alpha (QGIS format).
namespace BoundaryBandColor {
struct Value {
    double r = 0, g = 0, b = 0;
    double c = 0, m = 0, y = 0, k = 0;
    double opacity = 1;
    bool cmyk = false;
    bool hasOpacity = false;
};

inline bool number(const QString& text, double& value, double upper)
{
    bool ok = false;
    value = text.trimmed().toDouble(&ok);
    return ok && std::isfinite(value) && value >= 0 && value <= upper;
}

inline bool parseOpacity(QString text, const QString& unit, double& opacity)
{
    text = text.trimmed();
    const bool percent = text.endsWith('%');
    if (percent) text.chop(1);
    double value = 0;
    if (!number(text, value, 255)) return false;
    const QString mode = unit.trimmed().toLower();
    if (mode == "alpha" || mode == "byte") {
        if (percent) return false;
        opacity = value / 255.0;
    } else if (mode == "fraction" || mode == "0-1") {
        if (percent || value > 1) return false;
        opacity = value;
    } else if (percent || mode == "percent" || mode == "%" || mode == "0-100" || value > 1) {
        if (value > 100) return false;
        opacity = value / 100.0;
    } else {
        opacity = value;
    }
    return std::isfinite(opacity) && opacity >= 0 && opacity <= 1;
}

inline void convertCmyk(Value& color)
{
    color.cmyk = true;
    color.r = 255 * (1 - color.c / 100) * (1 - color.k / 100);
    color.g = 255 * (1 - color.m / 100) * (1 - color.k / 100);
    color.b = 255 * (1 - color.y / 100) * (1 - color.k / 100);
}

inline bool parse(QString text, Value& color)
{
    color = Value();
    text = text.trimmed().toLower();
    text.replace(QRegularExpression("\\s+"), "");
    text.replace(QChar(0xff0c), ',');
    text.replace(';', ',');
    if (text.startsWith('#')) {
        QString hex = text.mid(1);
        if (hex.size() == 3) {
            QString expanded;
            for (const QChar ch : hex) { expanded += ch; expanded += ch; }
            hex = expanded;
        }
        if ((hex.size() != 6 && hex.size() != 8) ||
            !QRegularExpression("^[0-9a-f]+$").match(hex).hasMatch()) return false;
        color.r = hex.mid(0, 2).toUInt(nullptr, 16);
        color.g = hex.mid(2, 2).toUInt(nullptr, 16);
        color.b = hex.mid(4, 2).toUInt(nullptr, 16);
        if (hex.size() == 8) {
            color.opacity = hex.mid(6, 2).toUInt(nullptr, 16) / 255.0;
            color.hasOpacity = true;
        }
        return true;
    }
    // C20Y15 means C=20, M=0, Y=15, K=0, all in percent.
    if (QRegularExpression("^[cmyk][0-9]").match(text).hasMatch()) {
        QRegularExpression re("([cmyk])([0-9]+(?:\\.[0-9]+)?)(%?)");
        auto matches = re.globalMatch(text);
        QSet<QString> channels;
        int end = 0;
        while (matches.hasNext()) {
            const auto match = matches.next();
            const QString channel = match.captured(1);
            double value = 0;
            if (match.capturedStart() != end || channels.contains(channel) ||
                !number(match.captured(2), value, 100)) return false;
            channels.insert(channel);
            if (channel == "c") color.c = value;
            if (channel == "m") color.m = value;
            if (channel == "y") color.y = value;
            if (channel == "k") color.k = value;
            end = match.capturedEnd();
        }
        if (end != text.size() || channels.isEmpty()) return false;
        convertCmyk(color);
        return true;
    }
    const auto function = QRegularExpression("^(cmyk|rgb|rgba)\\((.*)\\)$").match(text);
    QString mode;
    if (function.hasMatch()) {
        mode = function.captured(1);
        text = function.captured(2);
    } else if (text.startsWith('(') && text.endsWith(')')) {
        text = text.mid(1, text.size() - 2);
    }
    const QStringList parts = text.split(',', QString::KeepEmptyParts);
    if (mode == "cmyk") {
        if (parts.size() != 4) return false;
        double channels[4];
        for (int i = 0; i < 4; ++i) {
            QString part = parts[i];
            if (part.endsWith('%')) part.chop(1);
            if (!number(part, channels[i], 100)) return false;
        }
        color.c = channels[0]; color.m = channels[1];
        color.y = channels[2]; color.k = channels[3];
        convertCmyk(color);
        return true;
    }
    if (parts.size() != 3 && parts.size() != 4) return false;
    if ((mode == "rgb" && parts.size() != 3) || (mode == "rgba" && parts.size() != 4)) return false;
    double rgb[3];
    for (int i = 0; i < 3; ++i) {
        QString part = parts[i];
        const bool percent = part.endsWith('%');
        if (percent) part.chop(1);
        if (!number(part, rgb[i], percent ? 100 : 255)) return false;
        if (percent) rgb[i] *= 2.55;
    }
    color.r = rgb[0]; color.g = rgb[1]; color.b = rgb[2];
    if (parts.size() == 4) {
        // rgba() accepts fraction, percent or byte alpha; bare tuples are byte alpha.
        const QString alphaUnit = mode == "rgba" ? "auto" : "alpha";
        if (mode == "rgba" && !parts[3].endsWith('%')) {
            double alpha = 0;
            if (!number(parts[3], alpha, 255)) return false;
            color.opacity = alpha <= 1 ? alpha : alpha / 255.0;
        } else if (!parseOpacity(parts[3], alphaUnit, color.opacity)) return false;
        color.hasOpacity = true;
    }
    return true;
}

inline double distance(const Value& a, const Value& b)
{
    if (a.cmyk && b.cmyk)
        return std::max(std::max(std::fabs(a.c - b.c), std::fabs(a.m - b.m)),
                        std::max(std::fabs(a.y - b.y), std::fabs(a.k - b.k)));
    return std::sqrt((a.r - b.r) * (a.r - b.r) + (a.g - b.g) * (a.g - b.g) + (a.b - b.b) * (a.b - b.b));
}

inline bool matches(const QString& expected, const QString& actual, double rgbTolerance, double cmykTolerance = 1.0)
{
    if (expected.trimmed().isEmpty()) return true;
    Value a, b;
    if (!parse(expected, a) || !parse(actual, b)) return false;
    if (!(a.cmyk && b.cmyk) && rgbTolerance == 0.0) {
        // Compare the displayed 8-bit channels, including CMYK conversion rounding.
        return std::round(a.r) == std::round(b.r) && std::round(a.g) == std::round(b.g) && std::round(a.b) == std::round(b.b);
    }
    const double tolerance = a.cmyk && b.cmyk ? cmykTolerance : rgbTolerance;
    return std::isfinite(tolerance) && tolerance >= 0 && distance(a, b) <= tolerance;
}
}
#endif
