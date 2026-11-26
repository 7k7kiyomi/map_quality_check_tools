#ifndef SE_FONT_MAPPING_H
#define SE_FONT_MAPPING_H

#include <QMap>
#include <QString>
#include "../commontype/se_error_define.h"

class SE_FontMapping
{
public:
    static QMap<QString, FontMapping> getBuiltinFontMappings();
    static QString convertEnglishToChinese(const QString &englishName);
    static QString convertChineseToEnglish(const QString &chineseName);

private:
    // 从 CSV 文件加载映射，返回 true 表示成功且 outMappings 非空
    static bool loadFontMappingsFromCsv(const QString &csvFilePath, QMap<QString, FontMapping> &outMappings);

    // 解析环境变量并返回 mapping CSV 的完整路径（若未设置环境变量或文件不存在返回空串）
    static QString getConfigMappingFilePath();
};

#endif // SE_FONT_MAPPING_H