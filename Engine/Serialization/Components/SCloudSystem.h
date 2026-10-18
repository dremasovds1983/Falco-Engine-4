#pragma once
#include "SComponent.h"
#include "../Assets/SMaterial.h"

namespace GX
{
    class SCloudSystem : public SComponent
    {
    public:
        int getVersion() override { return 1; }
        void serialize(Serializer* s) override
        {
            SComponent::serialize(s);
            data(coverage);
            data(density);
            data(wind);
            data(hasMaterial);
            if (hasMaterial) data(material);
            if (version >= 1) { data(baseHeight); data(thickness); data(spatialScale); data(animationSpeed); }
        }
        float coverage = 0.5f;
        float density = 1.0f;
        SVector3 wind = SVector3(1, 0, 0);
        bool hasMaterial = false;
        SMaterial material;
        float baseHeight = 600.0f, thickness = 350.0f, spatialScale = 0.003f, animationSpeed = 6.0f;
    };
}
