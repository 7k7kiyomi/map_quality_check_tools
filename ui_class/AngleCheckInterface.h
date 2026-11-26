#ifndef ANGLE_CHECK_INTERFACE_H
#define ANGLE_CHECK_INTERFACE_H

#include "se_error_define.h"

#ifndef MAX_PATH_LENGTH
#define MAX_PATH_LENGTH 260
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 执行尖角检查
     * @param szShpFilePath 输入SHP文件路径
     * @param szXmlConfigPath XML配置文件路径
     * @param szOutputLogPath 输出运行日志完整路径
     * @param szOutputErrorShpPath 输出错误要素SHP完整路径
     * @param szOutputJsonPath 输出JSON报告完整路径
     * @param scale 比例尺
     * @return 错误代码
     */
    SE_Error SharpAngleCheck(
        const char *szShpFilePath,
        const char *szXmlConfigPath,
        const char *szOutputLogPath,
        const char *szOutputErrorShpPath,
        const char *szOutputJsonPath,
        int scale);

#ifdef __cplusplus
}
#endif

#endif // ANGLE_CHECK_INTERFACE_H