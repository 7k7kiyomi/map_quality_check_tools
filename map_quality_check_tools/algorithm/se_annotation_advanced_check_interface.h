#ifndef SE_ANNOTATION_ADVANCED_CHECK_INTERFACE_H
#define SE_ANNOTATION_ADVANCED_CHECK_INTERFACE_H

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

    /**
     * @brief 全局重叠检查算法
     *
     * 算法描述：检查符号与符号、注记与注记、符号与注记之间是否存在重叠。
     *
     * @param szSymbolLayerPaths            符号图层SHP文件路径，多个路径用分号(;)分隔
     * @param szAnnotationLayerPaths        注记图层SHP文件路径，多个路径用分号(;)分隔
     * @param szSchemaXmlPath               全局重叠检查规则XML文件路径
     * @param scale                         地图比例尺分母（如 100000）
     * @param szOutputCheckResultLayerPath  存在问题的要素对保存为质检问题数据（shp格式）
     * @param szOutputLogFilePath           日志文件路径
     * @param szOutputJsonPath              [out] 输出JSON报告的文件路径
     *
     * @return SE_Error 错误编码
     */
    MY_PLUGIN_API SE_Error AnnotationOverlapCheck(
        const char *szSymbolLayerPaths,
        const char *szAnnotationLayerPaths,
        const char *szSchemaXmlPath,
        int scale,
        const char *szOutputCheckResultLayerPath,
        const char *szOutputLogFilePath,
        const char *szOutputJsonPath);

    /**
     * @brief 符号注记匹配检查算法（ID=210）
     *
     * @param szMatchResultLayerPath        注记-要素匹配结果图层路径（临时标准图层）
        * @param szActualLayerPath             实际要素图层路径；仅自动识别为点符号图层时用于点符号检查，可为空
        * @param szGdbLayerPath                GDB图层路径；用于线/面要素缺失注记检查，可为空
        * @param szAnnotationLayerPath         原始注记Polygon图层路径；用于孤儿注记检测（检测靠近GDB要素但被200算法遗漏的注记），可为空
     * @param szSchemaXmlPath               质检规则XML文件路径
     * @param scale                         地图比例尺分母（如 100000）
     * @param szOutputCheckResultLayerPath  存在问题的要素保存为质检问题数据（shp格式）
     * @param szOutputLogFilePath           日志文件路径
     * @param szOutputJsonPath              [out] 输出JSON报告的文件路径
     *
     * @return SE_Error 错误编码
     */
    MY_PLUGIN_API SE_Error SymbolAnnotationMatchCheck(
        const char *szMatchResultLayerPath,
        const char *szActualLayerPath,
        const char *szGdbLayerPath,
        const char *szAnnotationLayerPath,
        const char *szSchemaXmlPath,
        int scale,
        const char *szOutputCheckResultLayerPath,
        const char *szOutputLogFilePath,
        const char *szOutputJsonPath);

    /**
     * @brief 字距检查算法（ID=204），支持规则启用的散列注记等间距检查
     * @details 检查等间距=true 时，以组内相邻字符中心距的均值为基准；
     * 等间距允许偏差使用小数比例，默认0.10。输入需为米制投影坐标；
     * JSON的info包含无法判定及不适用统计，err_exist=false不表示所有组均已判定。
     *
     * @param szMatchResultLayerPath        注记-要素匹配结果图层路径
     * @param szGdbLayerPath                GDB图层路径
     * @param szSchemaXmlPath               质检规则XML文件路径
     * @param scale                         地图比例尺分母（如 100000）
     * @param szOutputCheckResultLayerPath  存在问题的要素保存为质检问题数据（shp格式）
     * @param szOutputLogFilePath           日志文件路径
     * @param szOutputJsonPath              [out] 输出JSON报告的文件路径
     *
     * @return SE_Error 错误编码
     */
    MY_PLUGIN_API SE_Error CharSpacingCheck(
        const char *szMatchResultLayerPath,
        const char *szGdbLayerPath,
        const char *szSchemaXmlPath,
        int scale,
        const char *szOutputCheckResultLayerPath,
        const char *szOutputLogFilePath,
        const char *szOutputJsonPath);

    /**
     * @brief 注记组距检查算法（ID=207）- 返回JSON字符串
     *
     * @param szMatchResultLayerPath        注记-要素匹配结果图层路径
     * @param szSchemaXmlPath               质检规则XML文件路径
     * @param scale                         地图比例尺分母（如 100000）
     * @param szOutputCheckResultLayerPath  存在问题的要素保存为质检问题数据（shp格式）
     * @param szOutputLogFilePath           日志文件路径
     * @param szOutputJsonPath              [out] 输出JSON报告的文件路径
     *
     * @return SE_Error 错误编码
     */
    MY_PLUGIN_API SE_Error GroupSpacingCheck(
        const char *szMatchResultLayerPath,
        const char *szSchemaXmlPath,
        int scale,
        const char *szOutputCheckResultLayerPath,
        const char *szOutputLogFilePath,
        const char *szOutputJsonPath);

    /**
     * @brief 面要素等间距标注检查算法（ID=208）- 返回JSON字符串
     *
     * @param szMatchResultLayerPath        注记-要素匹配结果图层路径
     * @param szGdbLayerPath                GDB图层路径
     * @param szSchemaXmlPath               质检规则XML文件路径
     * @param scale                         地图比例尺分母（如 100000）
     * @param szOutputCheckResultLayerPath  存在问题的要素保存为质检问题数据（shp格式）
     * @param szOutputLogFilePath           日志文件路径
     * @param szOutputJsonPath              [out] 输出JSON报告的文件路径
     *
     * @return SE_Error 错误编码
     */
    MY_PLUGIN_API SE_Error EqualSpacingCheck(
        const char *szMatchResultLayerPath,
        const char *szGdbLayerPath,
        const char *szSchemaXmlPath,
        int scale,
        const char *szOutputCheckResultLayerPath,
        const char *szOutputLogFilePath,
        const char *szOutputJsonPath);

    /**
     * @brief 符号方向性检查算法（ID=209）- 返回JSON字符串
     *
     * @param szSymbolLayerPaths             待检查的符号图层路径，多个用分号分隔（"路径1;路径2;..."）
     * @param szAssociatedLayerPaths         关联图层路径，多个用分号分隔（"路径1;路径2;..."）
     * @param szSchemaXmlPath                质检规则XML文件路径
     * @param scale                          地图比例尺分母（如 100000）
     * @param szOutputCheckResultLayerPath   存在问题的要素保存为质检问题数据（shp格式）
     * @param szOutputLogFilePath            日志文件路径
     * @param szOutputJsonPath               [out] 输出JSON报告的文件路径
     *
     * @return SE_Error 错误编码
     */
    MY_PLUGIN_API SE_Error SymbolDirectionCheck(
        const char *szSymbolLayerPaths,
        const char *szAssociatedLayerPaths,
        const char *szSchemaXmlPath,
        int scale,
        const char *szOutputCheckResultLayerPath,
        const char *szOutputLogFilePath,
        const char *szOutputJsonPath);

#ifdef __cplusplus
}
#endif

#endif // SE_ANNOTATION_ADVANCED_CHECK_INTERFACE_H