#include "IMapFrameAlignmentCheck.h"
#include "MapFrameAlignmentCheck.h"

extern "C" IMapFrameAlignmentCheck * createMapFrameAlignmentChecker()
{
    return new MapFrameAlignmentCheck();
}
