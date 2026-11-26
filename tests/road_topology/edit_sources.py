"""Apply exact edits without normalizing the existing doubled-CR line endings."""
from pathlib import Path

root=Path(__file__).resolve().parents[2]
def edit(name,replacements):
    p=root/name
    text=p.read_bytes().decode('utf-8')
    for old,new in replacements:
        old=old.replace('\r\r\n','\r\n')
        new=new.replace('\r\r\n','\r\n')
        assert text.count(old)==1,(name,old[:100],text.count(old))
        text=text.replace(old,new,1)
    p.write_bytes(text.encode('utf-8'))

edit('ui_class/ReadShp.cpp',[
    ('#include "line_topology_issue_aggregation.h"','#include "line_topology_issue_aggregation.h"\r\r\n#include "road_topology_network.h"'),
    ('std::vector<QualityCheckResult> ReadShpDialog::checkRoadLineTopology(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile)\r\r\n{',
     'std::vector<QualityCheckResult> ReadShpDialog::checkRoadLineTopology(QgsVectorLayer* parsedLayer, QgsVectorLayer* referenceLayer, const QString& sourceFile)\r\r\n{\r\r\n    if (RoadTopology::family(sourceFile) == RoadTopology::Road)\r\r\n        return RoadTopology::check(*this, parsedLayer, sourceFile);\r\r\n'),
    ('m_minLineLength = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("min_line_length"));',
     'm_minLineLength = getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("min_segment_length"));')])
p=root/'ui_class/ReadShp.cpp';s=p.read_bytes().decode('utf-8')
a=s.index('bool ReadShpDialog::isRoadFile(const QString& fileName)');b=s.index('//道路库体文件识别函数',a)
s=s[:a]+'bool ReadShpDialog::isRoadFile(const QString& fileName)\r\n{\r\n    return RoadTopology::family(fileName) != RoadTopology::None;\r\n}\r\n'+s[b:]
p.write_bytes(s.encode('utf-8'))
edit('ui_class/QualityCheckConfig.cpp',[
    ('else if (ruleName.contains("悬挂线容差")) {',
     'else if (ruleName == QStringLiteral("道路连接缓冲距离")) {\r\n            paramName = "road_connection_buffer";\r\n        }\r\n        else if (ruleName == QStringLiteral("居民点缓冲距离")) {\r\n            paramName = "road_settlement_buffer";\r\n        }\r\n        else if (ruleName.contains("悬挂线容差")) {')])
print('Applied targeted ReadShp and parameter edits; original line endings preserved.')
