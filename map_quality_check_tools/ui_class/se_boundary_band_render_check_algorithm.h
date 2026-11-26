#ifndef SE_BOUNDARY_BAND_RENDER_CHECK_ALGORITHM_H
#define SE_BOUNDARY_BAND_RENDER_CHECK_ALGORITHM_H

#include "../commontype/se_error_define.h"
#include <QJsonObject>

struct SE_BoundaryBandRenderRuleConfig
{
    char szBoundaryLevelValue[64];
    char szBoundaryLevelField[64];
    char szColorField[64];
    char szOpacityField[64];
    char szOpacityUnit[24];
    char szStandardInnerColor[64];
    char szStandardOuterColor[64];
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
    double fUniformityCvThreshold; // Retired; ignored when reading legacy XML.
    double fSampleIntervalMm;
    double fSampleDensityPerMm;
    double fSideOverflowToleranceRatio;
    double fSideProfileLengthFactor;
    double fMinFragmentAreaMm2;
    double fComplexGeometryToleranceMm;
    double fSharpCornerMinAngleDeg;
    double fSharpCornerLocalOverlapToleranceMm;
    double fDistortionWidthRatioThreshold; // Retired; not a check criterion.
    double fColorTolerance; // Retired RGB tolerance; production comparison is exact.
    double fCmykColorTolerance;
    double fStandardOpacity;
    double fOpacityTolerance;
    int nScale;
    int nWidthSampleLimit;
    bool bCheckGeometryValidity;
    bool bCheckColor;
    bool bCheckGradient;
    bool bCheckOpacity;
    bool bRequireOpacity;
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

// Check every original band feature before extracting the inner/outer polygons.
SE_Error BoundaryBandLegendStyleCheck(
    const char* bandPolygonPath,
    const char* schemaXmlPath,
    const char* outputJsonPath,
    const char* outputShpPath,
    const char* outputGeoJsonPath,
    bool& passed,
    QJsonObject& result);

#endif // SE_BOUNDARY_BAND_RENDER_CHECK_ALGORITHM_H
