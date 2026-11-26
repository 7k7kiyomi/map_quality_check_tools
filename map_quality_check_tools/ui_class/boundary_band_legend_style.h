#ifndef BOUNDARY_BAND_LEGEND_STYLE_H
#define BOUNDARY_BAND_LEGEND_STYLE_H
#include "se_boundary_band_render_check_algorithm.h"
class OGRLayer;

SE_Error InspectBoundaryBandLegendStyles(
    OGRLayer* layer, const SE_BoundaryBandRenderRuleConfig& config,
    const QString& outputShp, const QString& outputGeoJson, QJsonObject& result);
#endif
