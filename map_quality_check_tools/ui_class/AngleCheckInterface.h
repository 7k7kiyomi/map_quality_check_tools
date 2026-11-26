#ifndef ANGLE_CHECK_INTERFACE_H
#define ANGLE_CHECK_INTERFACE_H

#include "se_error_define.h"

#ifndef MAX_PATH_LENGTH
#define MAX_PATH_LENGTH 260
#endif

#ifdef __cplusplus
#define SHARP_ANGLE_DEFAULT(value) = value
extern "C"
{
#else
#define SHARP_ANGLE_DEFAULT(value)
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

    // 扩展入口：旧接口保持兼容；false/省略筛选开关时源数据可为空。
    // parsed为单个解析文件；sourceDataPaths为对应源图层，关闭筛选时可省略。
    // 尖角点固定输出至错误要素同目录，文件名自动生成，不需要单独传路径。
    // 内部按图上2毫米源线缓冲区、解析线长覆盖率至少80%定位，不由调用方传参。
    SE_Error SharpAngleCheckWithSource(
        const char* szShpFilePath,const char* szXmlConfigPath,
        const char* szOutputLogPath,const char* szOutputErrorShpPath,
        const char* szOutputJsonPath,int scale,
        const char* sourceDataPaths SHARP_ANGLE_DEFAULT(nullptr));

#ifdef __cplusplus
}
#endif

#undef SHARP_ANGLE_DEFAULT

#endif // ANGLE_CHECK_INTERFACE_H
