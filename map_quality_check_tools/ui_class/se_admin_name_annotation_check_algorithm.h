#ifndef SE_ADMIN_NAME_ANNOTATION_CHECK_ALGORITHM_H
#define SE_ADMIN_NAME_ANNOTATION_CHECK_ALGORITHM_H

#include "../commontype/se_error_define.h"

struct SE_AdminNameAnnotationRuleConfig
{
    char szGdbLabelField[64];
    char szAnnoTextField[64];
    double fSimilarityThreshold;
    double fAllowOutsideDistanceMm;
    double fPrefixSuffixBaseScore;
    double fPrefixSuffixLengthWeight;
    double fCharsetOverlapRatio;
    double fCharsetCoverageRatio;
    double fJaroSequenceMin;
    double fSingleCharEquivalentScore;
    double fSingleCharGroupScore;
    double fSingleCharGroupPriorityBoost;
    double fFallbackJaroCap;
    double fSingleCharFontTolerance;
    double fSingleCharSpacingRatio;
    double fAliasMatchScore;
    double fAbbreviationMatchScore;
    double fNonstandardAbbreviationScore;
    double fReasonableShiftCentroidRatio;
    double fReasonableShiftPenalty;
    double fReasonableShiftNeighborRatio;
};

SE_Error ParseXmlForAdminNameAnnotation(
    const char* szSchemaXmlPath,
    SE_AdminNameAnnotationRuleConfig& config);

SE_Error AdminNameAnnotationCheck(
    const char* szActualAdminShpPath,
    const char* szActualAnnoShpPath,
    const char* szGdbAdminShpPath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    const char* szLabelField,
    const char* szOutputShpPath,
    const char* szOutputGeoJsonPath,
    const char* szTempAdminMatchShpPath,
    const char* szTempAnnotationPointShpPath,
    const char* szTempMergedSingleCharShpPath,
    const char* szTempStandardMatchShpPath,
    int nScale,
    bool& bPassed);

#endif // SE_ADMIN_NAME_ANNOTATION_CHECK_ALGORITHM_H
