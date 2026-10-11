#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "KardsCardBuilderCommandlet.generated.h"

/**
 * KARDS card blueprint builder.
 *
 *  -Mode=Dump    reflect real signatures out of the loaded game modules.
 *  -Mode=Build   assemble a real K2 graph, compile it, save only when clean.
 */
UCLASS()
class KARDSCARDBUILDER_API UKardsCardBuilderCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    UKardsCardBuilderCommandlet();
    virtual int32 Main(const FString& Params) override;
};
