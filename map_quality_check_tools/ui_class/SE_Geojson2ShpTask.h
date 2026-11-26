//#ifndef SE_ANNOTATION_CHECK_INTERFACE_H
//#define SE_ANNOTATION_CHECK_INTERFACE_H
//
////#include "../commontype/se_error_define.h"
//
//#ifdef __cplusplus
//extern "C"
//{
//#endif
//
//    /*@brief 注记检查算法
//     *
//     * 算法描述：检查地图注记的字体、字号、颜色、字间距等属性是否统一规范，
//     *          检查注记属性不一致、注记缺漏、注记位置超限、注记压盖、
//     *          要素符号不一致、要素符号压盖等问题
//     *
//     * @param szShpFilePath:					注记shp数据全路径
//     * @param szSchemaXmlPath:				注记质检方案xml文件全路径（算法解析质检项及规则）
//     * @param szOutputCheckReportPath：		注记检查结果文件.txt（参考质检报告的模板写法输出）
//     * @param szOutputCheckResultLayerPath:	存在问题的要素保存为质检问题数据（shp格式），如果无问题，不输出shp数据
//     * @param szOutputLogFilePath:			日志文件路径，使用spdlog实现日志输出
//     *
//     * @return SE_Error错误编码
//     */
//    SE_Error AnnotationCheck(
//        const char *szShpFilePath,
//        const char *szSchemaXmlPath,
//        const char *szOutputCheckReportPath,
//        const char *szOutputCheckResultLayerPath,
//        const char *szOutputLogFilePath);
//
//    /*@brief 注记字体检查算法
//     *
//     * 算法描述：检查注记字体是否符合规范要求
//     *
//     * @param szShpFilePath:					注记shp数据全路径
//     * @param szSchemaXmlPath:				注记质检方案xml文件全路径
//     * @param szOutputCheckReportPath：		检查结果文件.txt
//     * @param szOutputCheckResultLayerPath:	存在问题的要素保存为质检问题数据（shp格式）
//     * @param szOutputLogFilePath:			日志文件路径
//     *
//     * @return SE_Error错误编码
//     */
//    SE_Error AnnotationFontCheck(
//        const char *szShpFilePath,
//        const char *szSchemaXmlPath,
//        const char *szOutputCheckReportPath,
//        const char *szOutputCheckResultLayerPath,
//        const char *szOutputLogFilePath);
//
//    /*@brief 注记字号检查算法
//     *
//     * 算法描述：检查注记字号是否符合规范要求
//     *
//     * @param szShpFilePath:					注记shp数据全路径
//     * @param szSchemaXmlPath:				注记质检方案xml文件全路径
//     * @param szOutputCheckReportPath：		检查结果文件.txt
//     * @param szOutputCheckResultLayerPath:	存在问题的要素保存为质检问题数据（shp格式）
//     * @param szOutputLogFilePath:			日志文件路径
//     *
//     * @return SE_Error错误编码
//     */
//    SE_Error AnnotationFontSizeCheck(
//        const char *szShpFilePath,
//        const char *szSchemaXmlPath,
//        const char *szOutputCheckReportPath,
//        const char *szOutputCheckResultLayerPath,
//        const char *szOutputLogFilePath);
//
//    /*@brief 注记颜色检查算法
//     *
//     * 算法描述：检查注记颜色是否符合规范要求
//     *
//     * @param szShpFilePath:					注记shp数据全路径
//     * @param szSchemaXmlPath:				注记质检方案xml文件全路径
//     * @param szOutputCheckReportPath：		检查结果文件.txt
//     * @param szOutputCheckResultLayerPath:	存在问题的要素保存为质检问题数据（shp格式）
//     * @param szOutputLogFilePath:			日志文件路径
//     *
//     * @return SE_Error错误编码
//     */
//    SE_Error AnnotationColorCheck(
//        const char *szShpFilePath,
//        const char *szSchemaXmlPath,
//        const char *szOutputCheckReportPath,
//        const char *szOutputCheckResultLayerPath,
//        const char *szOutputLogFilePath);
//
//    /*@brief 注记斜体检查算法
//     *
//     * 算法描述：检查注记斜体角度是否符合规范要求
//     *
//     * @param szShpFilePath:					注记shp数据全路径
//     * @param szSchemaXmlPath:				注记质检方案xml文件全路径
//     * @param szOutputCheckReportPath：		检查结果文件.txt
//     * @param szOutputCheckResultLayerPath:	存在问题的要素保存为质检问题数据（shp格式）
//     * @param szOutputLogFilePath:			日志文件路径
//     *
//     * @return SE_Error错误编码
//     */
//    SE_Error AnnotationItalicCheck(
//        const char *szShpFilePath,
//        const char *szSchemaXmlPath,
//        const char *szOutputCheckReportPath,
//        const char *szOutputCheckResultLayerPath,
//        const char *szOutputLogFilePath);
//
//#ifdef __cplusplus
//}
//#endif
//
//#endif // SE_ANNOTATION_CHECK_INTERFACE_H