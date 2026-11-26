#ifndef SE_ANNOTATION_CHECK_INTERFACE_H
#define SE_ANNOTATION_CHECK_INTERFACE_H

#include "../commontype/se_error_define.h"

// 定义一个宏来控制导出和导入
#ifdef MAKING_MY_PLUGIN_DLL
#define MY_PLUGIN_API __declspec(dllexport)
#else
#define MY_PLUGIN_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    /* @brief 注记检查算法，检查地图注记的字体、字号、颜色、字间距等属性是否统一规范
     *
     * @param szShpFilePath:					注记shp数据全路径
     * @param szSchemaXmlPath:				注记质检方案xml文件全路径
     * @param szScale:						地图比例尺（如100000）
     * @param szOutputCheckResultLayerPath:	存在问题的要素保存为质检问题数据（shp格式）
     * @param szOutputLogFilePath:			日志文件路径
     * @param szOutputJsonPath              [out] 输出JSON报告的文件路径
     *
     * @return SE_Error错误编码
     */
    MY_PLUGIN_API SE_Error AnnotationCheck(
        const char *szShpFilePath,
        const char *szSchemaXmlPath,
        int scale, // 修改为 int
        const char *szOutputCheckResultLayerPath,
        const char *szOutputLogFilePath,
        const char *szOutputJsonPath);

    /*
     * @brief 注记字体检查算法
     * @param szShpFilePath 注记shp数据全路径
     * @param szSchemaXmlPath 注记质检方案xml文件全路径
     * @param szScale 地图比例尺（如100000）
     * @param szOutputCheckResultLayerPath 存在问题的要素保存为质检问题数据（shp格式）
     * @param szOutputLogFilePath 日志文件路径
     * @param szOutputJsonPath [out] 输出JSON报告的文件路径
     */
    MY_PLUGIN_API SE_Error AnnotationFontCheck(
        const char *szShpFilePath,
        const char *szSchemaXmlPath,
        int scale,
        const char *szOutputCheckResultLayerPath,
        const char *szOutputLogFilePath,
        const char *szOutputJsonPath);

    /*
     * @brief 注记字号检查算法
     * @param szShpFilePath 注记shp数据全路径
     * @param szSchemaXmlPath 注记质检方案xml文件全路径
     * @param szScale 地图比例尺（如100000）
     * @param szOutputCheckResultLayerPath 存在问题的要素保存为质检问题数据（shp格式）
     * @param szOutputLogFilePath 日志文件路径
     * @param szOutputJsonPath [out] 输出JSON报告的文件路径
     */
    MY_PLUGIN_API SE_Error AnnotationFontSizeCheck(
        const char *szShpFilePath,
        const char *szSchemaXmlPath,
        int scale,
        const char *szOutputCheckResultLayerPath,
        const char *szOutputLogFilePath,
        const char *szOutputJsonPath);

    /*
     * @brief 注记颜色检查算法
     * @param szShpFilePath 注记shp数据全路径
     * @param szSchemaXmlPath 注记质检方案xml文件全路径
     * @param szScale 地图比例尺（如100000）
     * @param szOutputCheckResultLayerPath 存在问题的要素保存为质检问题数据（shp格式）
     * @param szOutputLogFilePath 日志文件路径
     * @param szOutputJsonPath [out] 输出JSON报告的文件路径
     */
    MY_PLUGIN_API SE_Error AnnotationColorCheck(
        const char *szShpFilePath,
        const char *szSchemaXmlPath,
        int scale,
        const char *szOutputCheckResultLayerPath,
        const char *szOutputLogFilePath,
        const char *szOutputJsonPath);

    /*
     * @brief 注记斜体检查算法，针对专门设置了斜角的注记
     * @param szShpFilePath 注记shp数据全路径
     * @param szSchemaXmlPath 注记质检方案xml文件全路径
     * @param szScale 地图比例尺（如100000）
     * @param szOutputCheckResultLayerPath 存在问题的要素保存为质检问题数据（shp格式）
     * @param szOutputLogFilePath 日志文件路径
     * @param szOutputJsonPath [out] 输出JSON报告的文件路径
     */
    MY_PLUGIN_API SE_Error AnnotationItalicCheck(
        const char *szShpFilePath,
        const char *szSchemaXmlPath,
        int scale,
        const char *szOutputCheckResultLayerPath,
        const char *szOutputLogFilePath,
        const char *szOutputJsonPath);

    /**
     * @brief 执行单行规则的注记要素匹配（新版）
     * @param standardLayer 标准图层名称
     * @param gdbLayerPath GDB图层文件路径
     * @param szRuleXmlPath 质检规则XML文件路径（从 id=200 的 check_item 中读取三个 rule：
     *                      name 为 "属性项" -> levelAttr；
     *                      name 为 "属性值" -> levelValuesCsv（逗号/分号分隔）；
     *                      name 为 "标注字段" -> gdbLabelField）
     * @param actualLayerPath 实际要素图层路径
     * @param actualAnnoLayerPath 实际注记图层路径
     * @param mapScale 地图比例尺
     * @param outputTempLayerPath 输出临时标准图层路径
     * @param outputUnmatchedAnnotations 是否输出未匹配注记，0 表示不输出，非 0 表示输出；可不传，默认不输出
     * @return SE_Error错误编码
     */
    MY_PLUGIN_API SE_Error AnnotationMatchFeatures(
        const char *standardLayer,
        const char *gdbLayerPath,
        const char *szRuleXmlPath,
        const char *actualLayerPath,
        const char *actualAnnoLayerPath,
        int scale, // 修改为 int
        const char *outputTempLayerPath,
        int outputUnmatchedAnnotations
#ifdef __cplusplus
            = 0
#endif
    );

#ifdef __cplusplus
}
#endif

#endif // SE_ANNOTATION_CHECK_INTERFACE_H