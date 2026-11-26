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

    /*
     * 配置、集成与验证说明见 docs/表面注记分区域样式检查说明.md。
     * 201/202/203 区域规则（各检查项独立配置）：
     *   关联主区：标准图层名，由调用方按映射传入对应的实际面图层路径；
     *   关联省内：GDB 图层名，由调用方传入对应的 GDB 面图层路径；
     *   省内筛选字段/省内筛选值：例如 NAME / 甘肃省，按属性值精确筛选。
     * 无区域规则时保留原有多值候选语义，两个路径可省略且不会读取。
     * 启用区域规则时四条规则必须完整，取值只能为一个共用值或三个区域值；
     * 三个值依次为主区、外区省内、外区省外，不允许空项。
     * 字号范围的上下界分别支持一个或三个值，允许单侧边界，仍保持范围优先。
     * 多个主区面合并，单面直接使用；省内面按条件筛选后合并。
     * 关联图层转换到注记坐标系，无效几何仅在内存中修复。
     * group_id 非空且非负时按整组字符面覆盖面积选档；无有效组号时按单要素选档。
     * 区域重叠部分按主区、省内、省外顺序归属，跨区时取覆盖面积最大的区域。
     * 外区省外为注记面扣除主区和省内后的部分；覆盖面积并列时无法可靠归类并报错。
     * 区域规则、图层或空间分类失败时返回 SE_ERROR_CORRUPT_DATA，不生成成功报告。
     * C++ 调用可省略末尾两个参数；函数指针/C 调用须显式传入空指针。
     * 导出函数签名发生变化，调用方须使用新头文件重新编译。
     */

    /* @brief 注记检查算法，检查地图注记的字体、字号、颜色、字间距等属性是否统一规范
     *
     * @param szShpFilePath:					注记shp数据全路径
     * @param szSchemaXmlPath:				注记质检方案xml文件全路径
     * @param szScale:						地图比例尺（如100000）
     * @param szOutputCheckResultLayerPath:	存在问题的要素保存为质检问题数据（shp格式）
     * @param szOutputLogFilePath:			日志文件路径
     * @param szOutputJsonPath              [out] 输出JSON报告的文件路径
     * @param szMainRegionLayerPath 可选，关联主区标准图层对应的实际面图层完整路径
     * @param szProvinceLayerPath 可选，关联省内 GDB 面图层完整路径
     *
     * @return SE_Error错误编码
     */
    MY_PLUGIN_API SE_Error AnnotationCheck(
        const char *szShpFilePath,
        const char *szSchemaXmlPath,
        int scale, // 修改为 int
        const char *szOutputCheckResultLayerPath,
        const char *szOutputLogFilePath,
        const char *szOutputJsonPath,
        const char *szMainRegionLayerPath
#ifdef __cplusplus
            = nullptr
#endif
        ,
        const char *szProvinceLayerPath
#ifdef __cplusplus
            = nullptr
#endif
    );

    /*
     * @brief 注记字体检查算法
     * @note 201“字体名”的value支持分号分隔的多个允许值（兼容中文分号），普通模式命中任一值即合格，区域模式按上方约定选档。
     *       普通模式例如：<value>方正宋体;方正仿宋；方正黑体</value>，忽略各值首尾空白和空项。
     * @param szShpFilePath 注记shp数据全路径
     * @param szSchemaXmlPath 注记质检方案xml文件全路径
     * @param szScale 地图比例尺（如100000）
     * @param szOutputCheckResultLayerPath 存在问题的要素保存为质检问题数据（shp格式）
     * @param szOutputLogFilePath 日志文件路径
     * @param szOutputJsonPath [out] 输出JSON报告的文件路径
     * @param szMainRegionLayerPath 可选，关联主区标准图层对应的实际面图层完整路径
     * @param szProvinceLayerPath 可选，关联省内 GDB 面图层完整路径
     */
    MY_PLUGIN_API SE_Error AnnotationFontCheck(
        const char *szShpFilePath,
        const char *szSchemaXmlPath,
        int scale,
        const char *szOutputCheckResultLayerPath,
        const char *szOutputLogFilePath,
        const char *szOutputJsonPath,
        const char *szMainRegionLayerPath
#ifdef __cplusplus
            = nullptr
#endif
        ,
        const char *szProvinceLayerPath
#ifdef __cplusplus
            = nullptr
#endif
    );

    /*
     * @brief 注记字号检查算法
     * @note 202“字体大小”的value支持分号分隔的多个允许值（兼容中文分号），如2.5;3；4*4。
     *       “最小字大”/“最大字大”也支持多值，按位置配对，如最小2;4、最大3;5表示2～3或4～5；
     *       单侧只有一个值时共用该边界；
     *       普通模式两侧均为多值时数量必须一致，否则记录警告并跳过字号检查。缺少一侧时为单边范围。
     *       普通模式命中任一候选值/范围即合格；保留范围优先及原有容差，各值忽略首尾空白和空项。
     * @param szShpFilePath 注记shp数据全路径
     * @param szSchemaXmlPath 注记质检方案xml文件全路径
     * @param szScale 地图比例尺（如100000）
     * @param szOutputCheckResultLayerPath 存在问题的要素保存为质检问题数据（shp格式）
     * @param szOutputLogFilePath 日志文件路径
     * @param szOutputJsonPath [out] 输出JSON报告的文件路径
     * @param szMainRegionLayerPath 可选，关联主区标准图层对应的实际面图层完整路径
     * @param szProvinceLayerPath 可选，关联省内 GDB 面图层完整路径
     */
    MY_PLUGIN_API SE_Error AnnotationFontSizeCheck(
        const char *szShpFilePath,
        const char *szSchemaXmlPath,
        int scale,
        const char *szOutputCheckResultLayerPath,
        const char *szOutputLogFilePath,
        const char *szOutputJsonPath,
        const char *szMainRegionLayerPath
#ifdef __cplusplus
            = nullptr
#endif
        ,
        const char *szProvinceLayerPath
#ifdef __cplusplus
            = nullptr
#endif
    );

    /*
     * @brief 注记颜色检查算法
     * @note 203“颜色代码”的value支持分号分隔的多个允许值（兼容中文分号），如K100;K70；C100。
     *       普通模式命中任一颜色即合格，保留CMYK分量容差，各值忽略首尾空白和空项。
     *       201、202、203的候选值独立匹配，不按位置绑定为字体、字号、颜色组合。
     * @param szShpFilePath 注记shp数据全路径
     * @param szSchemaXmlPath 注记质检方案xml文件全路径
     * @param szScale 地图比例尺（如100000）
     * @param szOutputCheckResultLayerPath 存在问题的要素保存为质检问题数据（shp格式）
     * @param szOutputLogFilePath 日志文件路径
     * @param szOutputJsonPath [out] 输出JSON报告的文件路径
     * @param szMainRegionLayerPath 可选，关联主区标准图层对应的实际面图层完整路径
     * @param szProvinceLayerPath 可选，关联省内 GDB 面图层完整路径
     */
    MY_PLUGIN_API SE_Error AnnotationColorCheck(
        const char *szShpFilePath,
        const char *szSchemaXmlPath,
        int scale,
        const char *szOutputCheckResultLayerPath,
        const char *szOutputLogFilePath,
        const char *szOutputJsonPath,
        const char *szMainRegionLayerPath
#ifdef __cplusplus
            = nullptr
#endif
        ,
        const char *szProvinceLayerPath
#ifdef __cplusplus
            = nullptr
#endif
    );

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

#include <QString>
#include <QStringList>
#include <QVector>
// 本次地图中已执行的200任务；仅这些任务参与交叉验证，不扫描历史输出。
struct AnnotationMatchBatchTask
{
    QString standardLayer, sourcePath, xmlPath, actualPath, annotationPath, outputPath;
};
// 在所有200完成、任一依赖200的检查开始前调用。false时不得消费未校正结果。
MY_PLUGIN_API bool ReconcileAnnotationMatchBatch(
    QVector<AnnotationMatchBatchTask> &tasks, int scale,
    QString &summary, QStringList &ambiguities);

#endif // SE_ANNOTATION_CHECK_INTERFACE_H
