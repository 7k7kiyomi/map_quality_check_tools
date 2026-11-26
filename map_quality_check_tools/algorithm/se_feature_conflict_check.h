#pragma once
#ifdef MAKING_MY_PLUGIN_DLL
#define FEATURE_CONFLICT_API __declspec(dllexport)
#else
#define FEATURE_CONFLICT_API
#endif
// 123要素表示冲突检查，UTF-8字符串，图层文件列表用英文分号分隔。
// associationInputsJson例如：{"1":{"parsed_paths":"a.shp;b.shp","source_paths":"source.shp","source_layers":""}}
// 每组按XML编号提供，不在XML填写路径。source_layers指定多图层数据库的源图层名。
extern "C" FEATURE_CONFLICT_API int FeatureConflictCheck(
    const char* xmlPath, const char* targetParsedPaths, const char* targetSourcePaths,
    const char* targetSourceLayers, const char* associationInputsJson, int scale,
    const char* outputErrorPath, const char* outputJsonPath, const char* outputLogPath,
    double sourceMatchToleranceMm=5.0);
