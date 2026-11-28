// Dispatcher for ObjectTemplate.hlsl (split into cohesive Object* includes).
#include "includes/ObjectCommon.hlsl"

#ifdef VS
    #include "includes/ObjectVertex.hlsl"
#elif defined(PS) && (!defined(LIGHTS) || LIGHTS < 4)
    #include "includes/ObjectPixelForward.hlsl"
#elif defined(PS) && LIGHTS >= 4
    #include "includes/ObjectPixelManyLights.hlsl"
#endif
