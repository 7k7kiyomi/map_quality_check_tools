#ifndef SE_BOUNDARY_BAND_RENDER_CHECK_ALGORITHM_H
#define SE_BOUNDARY_BAND_RENDER_CHECK_ALGORITHM_H

#include "../commontype/se_error_define.h"

struct SE_BoundaryBandRenderRuleConfig
{
    char szBoundaryLevelValue[64];
    char szBoundaryLevelField[64];
    char szColorField[64];
    char szGradientStartColorField[64];
    char szGradientEndColorField[64];
    char szGradientDirectionField[64];
    char szGradientFunctionField[64];
    char szExpectedSide[32];
    char szStandardFillColor[64];
    char szStandardGradientStartColor[64];
    char szStandardGradientEndColor[64];
    char szStandardGradientDirection[64];
    char szStandardGradientFunction[64];
    double fStandardWidthMm;
    double fWidthToleranceMm;
    double fUniformityCvThreshold;
    double fSampleIntervalMm;
    double fSampleDensityPerMm;
    double fSideOverflowToleranceRatio;
    double fSideProfileLengthFactor;
    double fMinFragmentAreaMm2;
    double fComplexGeometryToleranceMm;
    double fSharpCornerMinAngleDeg;
    double fSharpCornerLocalOverlapToleranceMm;
    double fDistortionWidthRatioThreshold;
    double fColorTolerance;
    int nScale;
    bool bCheckGeometryValidity;
    bool bCheckColor;
    bool bCheckGradient;
};

SE_Error ParseXmlForBoundaryBandRender(
    const char* szSchemaXmlPath,
    SE_BoundaryBandRenderRuleConfig& config);

SE_Error BoundaryBandRenderCheck(
    const char* szBoundaryLineShpPath,
    const char* szAdminPolygonShpPath,
    const char* szBandPolygonShpPath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    const char* szOutputShpPath,
    const char* szOutputGeoJsonPath,
    const char* szTempSampleLineShpPath,
    int nScale,
    bool& bPassed,
    bool bOnlyCheckSide = false);

#endif // SE_BOUNDARY_BAND_RENDER_CHECK_ALGORITHM_H
