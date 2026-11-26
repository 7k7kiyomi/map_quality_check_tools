#include <QString>
#include <QRegExp>
#include <iostream>
#include <QStringList>
class ScaleCheckerAlgorithm { public: static bool isValidScaleText(const QString&, QString*); };
bool ScaleCheckerAlgorithm::isValidScaleText(const QString& text, QString* scaleValue) {
    // 整饰文字可能把“比例尺”和“1:370 000”存成两条要素。
    // 保留带前缀的旧格式；无前缀时仅接受完整的 1:N，避免匹配普通比值或正文。
    QString cleanedText = text;
    cleanedText.remove(QRegExp("\\s+"));
    cleanedText.remove(QChar(0x00a0)).remove(QChar(0x202f)).remove(QChar(0x3000));
    cleanedText.replace("：", ":");
    QRegExp scalePattern("^(比例尺?[0-9]+:|1:)([0-9]+)$");
    if (scalePattern.exactMatch(cleanedText)) {
        bool ok = false;
        const int denominator = scalePattern.cap(2).toInt(&ok);
        if (!ok || denominator <= 0) return false;
        if (scaleValue) {
            cleanedText.remove("比例尺").remove("比例");
            *scaleValue = cleanedText;
        }
        return true;
    }
    return false;
}

int main() {
    const QStringList valid = {"1:370 000", "1：370000", "比例尺 1：370 000", "比例1:370000", "比例尺1:145000", " 1 : 370 000 ", QString::fromUtf8("1：370\xC2\xA0" "000"), QString::fromUtf8("1：370\xE2\x80\xAF" "000"), "1：370　000"};
    for (const auto& text : valid) {
        QString value;
        if (!ScaleCheckerAlgorithm::isValidScaleText(text, &value) || (value != "1:370000" && value != "1:145000")) { std::cerr << "FAIL valid " << text.toStdString(); return 1; }
    }
    const QStringList invalid = {"比例尺", "370000", "2:370000", "1:0", "1:-370000", "1:370000.5", "说明1:370000", "1:370000公里", "1:370000 1:50000", "1:99999999999999999", "比例尺1:0"};
    for (const auto& text : invalid) {
        QString value = "unchanged";
        if (ScaleCheckerAlgorithm::isValidScaleText(text, &value) || value != "unchanged") { std::cerr << "FAIL invalid " << text.toStdString(); return 1; }
    }
    if (!ScaleCheckerAlgorithm::isValidScaleText("1:370 000", nullptr)) return 1;
    std::cout << "PASS 21 scale text cases\n";
}