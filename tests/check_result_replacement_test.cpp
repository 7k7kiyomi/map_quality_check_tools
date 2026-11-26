#include "ui_class/check_result_replacement.h"
#include <QCoreApplication>
#include <stdexcept>
#include <iostream>

static void require(bool value) {
    if (!value) throw std::runtime_error("result replacement matching failed");
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString base = QStringLiteral("线要素拓扑检查_村道_errors_lines");
    require(CheckResultReplacement::supports(base));
    require(CheckResultReplacement::owns(base, base + QStringLiteral(".shp")));
    require(CheckResultReplacement::owns(base, base + QStringLiteral("_12.dbf")));
    require(CheckResultReplacement::owns(base, base + QStringLiteral("_2.shp.xml")));
    require(!CheckResultReplacement::owns(base, base + QStringLiteral("_points.shp")));
    require(!CheckResultReplacement::owns(base, base + QStringLiteral("_2.notes")));
    require(!CheckResultReplacement::supports(QStringLiteral("点要素孤立_水库_errors")));
    std::cout << "PASS result replacement matching" << std::endl;
}
