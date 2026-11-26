#pragma once

// 全局空白检查（ID=122）。路径均为 UTF-8；返回 0 表示执行成功，
// 是否存在待复核空白区域请读取 JSON 的 err_exist，不等同于函数返回值。
// 仅依赖 QtCore、QtXml、GDAL/OGR，不依赖 QGIS 窗口或 Python。
#ifdef MAKING_MY_PLUGIN_DLL
#define GLOBAL_BLANK_API __declspec(dllexport)
#else
#define GLOBAL_BLANK_API
#endif

extern "C" GLOBAL_BLANK_API int GlobalBlankCheck(
    const char* boundaryPath,       // 必填：集成程序传入内图廓面/闭合线
    const char* occupiedPaths,      // 必填：解析图层文件夹，或分号分隔的图层路径
    const char* excludedLayers,     // 排除图层：完整文件路径/图层名/通配符，分号分隔；可为空
    const char* sourceDataPaths,    // 提取源要素时必填：.gdb/源图层文件夹/文件列表；仅查空白可为空
    const char* sourceLayerNames,   // 数据库内部提取层名，分号分隔；为空时处理传入源文件的全部图层
    const char* schemaXmlPath,
    int scale,                     // 比例尺分母，必须 > 0
    const char* severity,           // 由集成程序读取的严重程度，如“高”；可为空
    const char* outputRegionPath,   // .shp/.geojson/.gpkg；保留孔洞
    const char* outputJsonPath,
    const char* outputLogPath);
