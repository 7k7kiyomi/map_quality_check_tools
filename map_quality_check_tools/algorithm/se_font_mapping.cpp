#include "se_font_mapping.h"
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDebug>

QMap<QString, FontMapping> SE_FontMapping::getBuiltinFontMappings()
{
    static QMap<QString, FontMapping> mappings;

    if (mappings.isEmpty())
    {
        // 内置字体映射数据（原有写死的映射）
        FontMapping mapping;

        mapping.chineseName = QString::fromUtf8("方正报宋简体");
        mapping.englishName = "FZBSJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正粗圆简体");
        mapping.englishName = "FZY4JW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正大标宋简体");
        mapping.englishName = "FZDBSJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正大黑简体");
        mapping.englishName = "FZDHTJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正仿宋简体");
        mapping.englishName = "FZFSJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正黑体简体");
        mapping.englishName = "FZHTJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正琥珀简体");
        mapping.englishName = "FZHPJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正楷体简体");
        mapping.englishName = "FZKTJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正隶变简体");
        mapping.englishName = "FZLBJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正隶书简体");
        mapping.englishName = "FZLSJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正美黑简体");
        mapping.englishName = "FZMHJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正书宋简体");
        mapping.englishName = "FZSSJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正舒体简体");
        mapping.englishName = "FZSTJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("左斜方正宋体");
        mapping.englishName = "FZSTZX";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正水柱简体");
        mapping.englishName = "FZSZJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正宋黑简体");
        mapping.englishName = "FZSHJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正宋三简体");
        mapping.englishName = "FZS3JW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正魏碑简体");
        mapping.englishName = "FZWBJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正细等线简体");
        mapping.englishName = "FZXDXJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正细黑一简体");
        mapping.englishName = "FZXH1JW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正细圆简体");
        mapping.englishName = "FZY1JW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正小标宋简体");
        mapping.englishName = "FZXBSJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正行楷简体");
        mapping.englishName = "FZXKJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正姚体简体");
        mapping.englishName = "FZYTJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正中等线简体");
        mapping.englishName = "FZZDXJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正准圆简体");
        mapping.englishName = "FZY3JW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正综艺简体");
        mapping.englishName = "FZZYJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正彩云简体");
        mapping.englishName = "FZCYJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正隶二简体");
        mapping.englishName = "FZL2JW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正康体简体");
        mapping.englishName = "FZKANGJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正超粗黑简体");
        mapping.englishName = "FZCCHJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正新报宋简体");
        mapping.englishName = "FZNBSJW-GB109";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正新舒体简体");
        mapping.englishName = "FZNSTJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正黄草简体");
        mapping.englishName = "FZHCJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正少儿简体");
        mapping.englishName = "FZSEJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正稚艺简体");
        mapping.englishName = "FZZHYJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正细珊瑚简体");
        mapping.englishName = "FZXSHJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正粗宋简体");
        mapping.englishName = "FZCSJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正平和简体");
        mapping.englishName = "FZPHTJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正华隶简体");
        mapping.englishName = "FZHLJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正瘦金书简体");
        mapping.englishName = "FZSJSJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正细倩简体");
        mapping.englishName = "FZXQJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正中倩简体");
        mapping.englishName = "FZZQJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正粗倩简体");
        mapping.englishName = "FZCQJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正胖娃简体");
        mapping.englishName = "FZPWJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正宋一简体");
        mapping.englishName = "FZSYJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正报宋繁体");
        mapping.englishName = "FZXLFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正彩云繁体");
        mapping.englishName = "FZCYFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正超粗黑繁体");
        mapping.englishName = "FZCCHFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正粗黑繁体");
        mapping.englishName = "FZH4FW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正粗圆繁体");
        mapping.englishName = "FZY4FW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正大标宋繁体");
        mapping.englishName = "FZDBSFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正仿宋繁体");
        mapping.englishName = "FZFSFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正黑体繁体");
        mapping.englishName = "FZHTFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正琥珀繁体");
        mapping.englishName = "FZHPFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正楷体繁体");
        mapping.englishName = "FZKTFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正隶变繁体");
        mapping.englishName = "FZLBFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正平黑繁体");
        mapping.englishName = "FZPHFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正书宋繁体");
        mapping.englishName = "FZSSFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正舒体繁体");
        mapping.englishName = "FZSTFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正魏碑繁体");
        mapping.englishName = "FZWBFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正细黑一繁体");
        mapping.englishName = "FZXH1FW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正细圆繁体");
        mapping.englishName = "FZY1FW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正小标宋繁体");
        mapping.englishName = "FZXBSFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正新书宋繁体");
        mapping.englishName = "FZXSSFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正新秀丽繁体");
        mapping.englishName = "FZXXLFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正行楷繁体");
        mapping.englishName = "FZXKFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正幼线繁体");
        mapping.englishName = "FZYXFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正中楷繁体");
        mapping.englishName = "FZZKFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正准圆繁体");
        mapping.englishName = "FZY3FW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正综艺繁体");
        mapping.englishName = "FZZYFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正隶二繁体");
        mapping.englishName = "FZL2FW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正新舒体繁体");
        mapping.englishName = "FZNSTFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正康体繁体");
        mapping.englishName = "FZKANGFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正水柱繁体");
        mapping.englishName = "FZSZFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正姚体繁体");
        mapping.englishName = "FZYTFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正瘦金书繁体");
        mapping.englishName = "FZSJSFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正少儿繁体");
        mapping.englishName = "FZSEFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正稚艺繁体");
        mapping.englishName = "FZZHYFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正细珊瑚繁体");
        mapping.englishName = "FZXSHFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正粗宋繁体");
        mapping.englishName = "FZCSFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正平和繁体");
        mapping.englishName = "FZPHTFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正华隶繁体");
        mapping.englishName = "FZHLFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正中等线繁体");
        mapping.englishName = "FZZDXFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正细倩繁体");
        mapping.englishName = "FZXQFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正中倩繁体");
        mapping.englishName = "FZZQFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正粗倩繁体");
        mapping.englishName = "FZCQFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正胖娃繁体");
        mapping.englishName = "FZPWFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正宋一繁体");
        mapping.englishName = "FZSYFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正卡通简体");
        mapping.englishName = "FZKATJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正卡通繁体");
        mapping.englishName = "FZKATFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正艺黑简体");
        mapping.englishName = "FZYHJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正艺黑繁体");
        mapping.englishName = "FZYHFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正水黑简体");
        mapping.englishName = "FZSHHJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正水黑繁体");
        mapping.englishName = "FZSHHFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正古隶简体");
        mapping.englishName = "FZGLJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正古隶繁体");
        mapping.englishName = "FZGLFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正小篆体");
        mapping.englishName = "FZXZTFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正幼线简体");
        mapping.englishName = "FZYXJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正启体简体");
        mapping.englishName = "FZQTJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正启体繁体");
        mapping.englishName = "FZQTFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正硬笔楷书简体");
        mapping.englishName = "FZYBKSJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正硬笔楷书繁体");
        mapping.englishName = "FZYBKSFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正毡笔黑简体");
        mapping.englishName = "FZZBHJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正毡笔黑繁体");
        mapping.englishName = "FZZBHFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正硬笔行书简体");
        mapping.englishName = "FZYBXSJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正硬笔行书繁体");
        mapping.englishName = "FZYBXSFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正剪纸简体");
        mapping.englishName = "FZJZJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正剪纸繁体");
        mapping.englishName = "FZJZFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正胖头鱼简体");
        mapping.englishName = "FZPTYJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正铁筋隶书简体");
        mapping.englishName = "FZTJLSJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正铁筋隶书繁体");
        mapping.englishName = "FZTJLSFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正北魏楷书简体");
        mapping.englishName = "FZBWKSJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正北魏楷书繁体");
        mapping.englishName = "FZBWKSFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正祥隶简体");
        mapping.englishName = "FZXIANGLJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正祥隶繁体");
        mapping.englishName = "FZXIANGLFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正粗活意简体");
        mapping.englishName = "FZCHYJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正粗活意繁体");
        mapping.englishName = "FZCHYFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正流行体简体");
        mapping.englishName = "FZLXTJW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正流行体繁体");
        mapping.englishName = "FZLXTFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正宋黑繁体");
        mapping.englishName = "FZSHFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正大黑繁体");
        mapping.englishName = "FZDHTFW";
        mappings.insert(mapping.englishName, mapping);

        mapping.chineseName = QString::fromUtf8("方正隶书繁体");
        mapping.englishName = "FZLSFW";
        mappings.insert(mapping.englishName, mapping);
    }

    // 尝试从配置文件加载并合并（CSV 中的项覆盖内置项）
    QString csvPath = getConfigMappingFilePath();
    if (!csvPath.isEmpty())
    {
        QMap<QString, FontMapping> configMappings;
        if (loadFontMappingsFromCsv(csvPath, configMappings) && !configMappings.isEmpty())
        {
            // 覆盖或插入
            for (auto it = configMappings.constBegin(); it != configMappings.constEnd(); ++it)
            {
                mappings.insert(it.key(), it.value());
            }
        }
    }

    return mappings;
}

QString SE_FontMapping::convertEnglishToChinese(const QString &englishName)
{
    QMap<QString, FontMapping> mappings = getBuiltinFontMappings();

    if (mappings.contains(englishName))
    {
        return mappings[englishName].chineseName;
    }

    return englishName; // 如果找不到映射，返回原名称
}

QString SE_FontMapping::convertChineseToEnglish(const QString &chineseName)
{
    QMap<QString, FontMapping> mappings = getBuiltinFontMappings();

    for (auto it = mappings.begin(); it != mappings.end(); ++it)
    {
        if (it.value().chineseName == chineseName)
        {
            return it.value().englishName;
        }
    }

    return chineseName; // 如果找不到映射，返回原名称
}

// 解析配置 CSV 文件
bool SE_FontMapping::loadFontMappingsFromCsv(const QString &csvFilePath, QMap<QString, FontMapping> &outMappings)
{
    QFile file(csvFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        // 无法打开文件，返回 false
        return false;
    }

    QTextStream in(&file);
    outMappings.clear();

    bool firstLine = true;
    while (!in.atEnd())
    {
        QString line = in.readLine().trimmed();
        if (line.isEmpty())
            continue;

        // 支持以 '#' 开头的注释行
        if (line.startsWith("#"))
            continue;

        // 如果行包含 BOM（可能在第一行），去掉
        if (firstLine && line.startsWith("\xEF\xBB\xBF"))
        {
            line = line.mid(3);
        }
        firstLine = false;

        // 支持生成 CSV 格式：englishName,chineseName（首行为可选表头）
        QStringList parts = line.split(',', Qt::SkipEmptyParts);
        if (parts.size() < 2)
            continue;

        QString english = parts.at(0).trimmed();
        QString chinese = parts.at(1).trimmed();

        // 如果当前行是表头（英文为 englishName 或 ENGLISH 等），跳过
        if (english.compare("englishName", Qt::CaseInsensitive) == 0 ||
            english.compare("english", Qt::CaseInsensitive) == 0 ||
            english.compare("english_name", Qt::CaseInsensitive) == 0)
        {
            continue;
        }

        if (!english.isEmpty() && !chinese.isEmpty())
        {
            FontMapping m;
            m.englishName = english;
            m.chineseName = chinese;
            outMappings.insert(english, m);
        }
    }

    file.close();
    return !outMappings.isEmpty();
}

// 依据示例函数思路组装 CSV 的完整路径
QString SE_FontMapping::getConfigMappingFilePath()
{
    QByteArray prefix = qgetenv("MPQIS_INSTALL_DIRECTORY_PREFIX_V_0_00_01");
    QString baseDir = QString::fromUtf8(prefix);

    if (!baseDir.isEmpty())
    {
        QString mpqisDir = QDir(baseDir).filePath("MPQIS");
        QString csvPath = QDir(mpqisDir).filePath(QString::fromUtf8("字体名中英文映射表.csv"));
        QFile f(csvPath);
        if (f.exists())
            return csvPath;
    }

    // 未设置环境或文件不存在，返回空串
    return QString();
}