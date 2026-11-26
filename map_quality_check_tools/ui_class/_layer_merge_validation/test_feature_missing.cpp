#include "se_feature_missing_check_algorithm.h"
#include <spdlog/sinks/null_sink.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDomDocument>
#include <iostream>
#include <cstdlib>
#define REQUIRE(x) do { if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " << #x << std::endl; std::exit(1); } } while (0)
static FeatureMissingCheckParams parse(const QString& path, std::shared_ptr<spdlog::logger> logger)
{
    return ParseXmlForFeatureMissingParams(path.toUtf8().toStdString(), 145000, logger);
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::null_sink_mt>());
    const QDir rules(QStringLiteral("D:/Desktop/挂图XML文件汇总_11.4/成果质检启用的xml（测试图层已覆盖）"));
    for (const QString& name : {QStringLiteral("晕带"), QStringLiteral("湖泊水库面"),
            QStringLiteral("街区面"), QStringLiteral("自然保护区界线"), QStringLiteral("高速出入口")}) {
        const auto params = parse(rules.filePath(name + QStringLiteral(".xml")), logger);
        REQUIRE(params.bAttributeConfigValid); REQUIRE(!params.bHasAttributeFilter);
        REQUIRE(params.attributeConditions.empty()); REQUIRE(params.dBufferRadius > 0);
    }
    const auto valid = parse(rules.filePath(QStringLiteral("乡镇级行政区界线.xml")), logger);
    REQUIRE(valid.bAttributeConfigValid); REQUIRE(valid.bHasAttributeFilter);
    REQUIRE(valid.attributeConditions.size() == 1);
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    QFile original(rules.filePath(QStringLiteral("晕带.xml")));
    REQUIRE(original.open(QIODevice::ReadOnly));
    QDomDocument document; REQUIRE(document.setContent(&original));
    const auto items = document.elementsByTagName(QStringLiteral("check_item"));
    bool changed = false;
    for (int i = 0; i < items.size(); ++i) {
        auto item = items.at(i).toElement();
        if (item.firstChildElement(QStringLiteral("id")).text().trimmed() != QStringLiteral("102")) continue;
        const auto rules = item.elementsByTagName(QStringLiteral("rule"));
        for (int j = 0; j < rules.size(); ++j) {
            auto rule = rules.at(j).toElement();
            if (rule.firstChildElement(QStringLiteral("name")).text().trimmed() != QStringLiteral("属性项")) continue;
            auto value = rule.firstChildElement(QStringLiteral("value"));
            value.appendChild(document.createTextNode(QStringLiteral("CLASID")));
            changed = true; break;
        }
    }
    REQUIRE(changed);
    const QString malformed = QDir(temporary.path()).filePath(QStringLiteral("晕带.xml"));
    QFile output(malformed); REQUIRE(output.open(QIODevice::WriteOnly));
    REQUIRE(output.write(document.toByteArray()) > 0); output.close();
    REQUIRE(!parse(malformed, logger).bAttributeConfigValid);
    std::cout << "PASS: five real empty attribute pairs disable filtering; populated pair still filters; incomplete pair fails" << std::endl;
}
