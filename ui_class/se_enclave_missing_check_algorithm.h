#pragma once

#include <string>
#include <memory>
#include "../commontype/se_error_define.h"
#include "spdlog/spdlog.h"

using namespace std;

#ifdef MAKING_MY_PLUGIN_DLL
#define MY_PLUGIN_API __declspec(dllexport)
#else
#define MY_PLUGIN_API __declspec(dllimport)
#endif

/**
 * @brief 飞地漏绘检查参数
 */
struct EnclaveMissingCheckParams
{
    std::string strGdbLayerName;     // GDB图层名称（如"BOUA6"）
    std::string strNameField;        // 行政区名称字段（如"NAME"）
    std::string strResultLayerName;  // 成果图层名称（用于报告）
    std::string strAdminCodeField;   // 行政编码字段（如"XQ"或"PAC"，用于过滤辖区）
    std::string strAdminCodeValue;   // 目标行政编码值（如"620121"永登县）
    double dSearchRadius;            // 搜索半径（图上毫米，容差）
    double dMinEnclaveArea;          // 最小飞地面积（图层单位），过小的忽略
    bool bHasMinArea;                // 是否启用最小面积过滤
    bool bHasAdminCodeFilter;        // 是否启用行政编码过滤
};

/**
 * @brief 解析XML配置文件，获取飞地漏绘检查参数
 * @param xmlFilePath XML文件路径
 * @param scale 比例尺
 * @param logger 日志记录器
 * @return 解析后的参数结构体
 */
EnclaveMissingCheckParams ParseXmlForEnclaveMissingParams(
    const std::string& xmlFilePath,
    int scale,
    std::shared_ptr<spdlog::logger> logger);

#ifdef __cplusplus
extern "C" {
#endif

    /**
     * @brief 飞地漏绘检查
     *
     * GDB数据模型：主区与飞地各自独立成行，共享同一个OBJECTID，
     * NAME字段区分——主区为"永登县"，飞地为"属永登县"。
     *
     * 成果SHP为纯图形面（无业务属性），且因制图综合/抽稀/移位，
     * 图面几何已变形。因此算法不依赖精确Contains，而是以Buffer
     * 容差圈吸收位移误差，通过Intersects判定。
     *
     * 算法步骤：
     * 1. 解析XML获取检查参数
     * 2. 打开GDB → 遍历要素 → 按NAME以"属"开头筛选飞地行
     * 3. 打开成果SHP → 收集所有面几何
     * 4. 每个飞地中心点 → Buffer容差圈 → 与成果面Intersects
     * 5. 无任何成果面与容差圈相交 → 判定为飞地漏绘
     * 6. 输出缺失飞地SHP + JSON报告
     *
     * @param szGdbPath           GDB文件路径（真值/地理要素清单库）
     * @param szResultShpPath     成果SHP文件路径（被检查的成果图面）
     * @param szSchemaXmlPath     质检方案XML文件路径
     * @param scale               比例尺
     * @param szOutputShpPath     缺失飞地输出SHP路径
     * @param szOutputJsonPath    JSON报告输出路径
     * @param szOutputLogPath     日志文件路径
     * @param szGdbLayerName      GDB图层名称（来自CSV gdb图层列，必传）
     * @return SE_Error 错误码
     */
    MY_PLUGIN_API SE_Error EnclaveMissingCheck(
        const char* szGdbPath,
        const char* szResultShpPath,
        const char* szSchemaXmlPath,
        int scale,
        const char* szOutputShpPath,
        const char* szOutputJsonPath,
        const char* szOutputLogPath,
        const char* szGdbLayerName = nullptr);

#ifdef __cplusplus
}
#endif
