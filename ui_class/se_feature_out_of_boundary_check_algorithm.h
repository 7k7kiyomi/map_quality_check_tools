#ifndef SE_FEATURE_OUT_OF_BOUNDARY_CHECK_ALGORITHM_H
#define SE_FEATURE_OUT_OF_BOUNDARY_CHECK_ALGORITHM_H

#include "se_error_define.h"

#ifdef MAKING_MY_PLUGIN_DLL
#define MY_PLUGIN_API __declspec(dllexport)
#else
#define MY_PLUGIN_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

MY_PLUGIN_API SE_Error FeatureOutOfBoundaryCheck(
    const char* szBoundaryShpFilePath,
    const char* szFeatureShpPaths,
    const char* szOutputErrorLayerPath,
    const char* szOutputJsonPath,
    const char* szOutputLogFilePath);

#ifdef __cplusplus
}
#endif

#endif
