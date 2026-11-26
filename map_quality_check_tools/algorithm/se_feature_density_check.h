#pragma once

#ifdef MAKING_MY_PLUGIN_DLL
#define FEATURE_DENSITY_API __declspec(dllexport)
#else
#define FEATURE_DENSITY_API
#endif

// 121要素密度检查。所有字符串为UTF-8；列表用分号分隔。
// 返回0表示运行成功；是否超限读取JSON的err_exist。
// parsedPaths内多个文件必须属于同一个逻辑图层，同symbol_id跨文件合并。
// 没有symbol_id字段或值为空的符号记录（点/线/面）逐条作为完整对象。
extern "C" FEATURE_DENSITY_API int FeatureDensityCheck(
    const char* boundaryPath,
    const char* parsedPaths,
    const char* sourceDataPaths,       // 属性筛选时必需：源SHP列表或GDB
    const char* sourceLayerNames,      // 多图层数据库应明确提供名称；单图层文件可空
    const char* mainAreaPath,          // 主区/外区时必需：行政区划普色面
    const char* annotationAssociationPath, // 注记时必需：200完整匹配图层
    const char* schemaXmlPath,
    int scale,
    const char* severity,
    const char* outputGridPath,
    const char* outputFeaturePath,     // 超限完整对象的原始解析几何（不是中心点）
    const char* outputJsonPath,
    const char* outputLogPath,
    double sourceMatchToleranceMm=5.0); // 集成侧可调整；不是XML规则
