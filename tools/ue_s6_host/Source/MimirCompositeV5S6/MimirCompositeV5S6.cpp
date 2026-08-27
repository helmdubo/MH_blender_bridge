#include "Composite/MHCompositePlanReport.h"
#include "Composite/MHRuntimeCompositeActor.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"

using namespace UE::MimirComposite;

/** Test-host runner only. The shipping plugin does not launch tests or exit. */
class FMimirCompositeV5S6Host final : public FDefaultGameModuleImpl
{
public:
    virtual void StartupModule() override
    {
        if (FParse::Param(FCommandLine::Get(), TEXT("MHS6PackagedSmoke")))
        {
            StartedAt = FPlatformTime::Seconds();
            TickHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateRaw(this, &FMimirCompositeV5S6Host::Tick), 0.1f);
        }
    }

    virtual void ShutdownModule() override
    {
        if (TickHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        TickHandle.Reset();
    }

private:
    bool Finish(const bool bSuccess, const FString& Message)
    {
        if (bSuccess)
        {
            UE_LOG(LogTemp, Display, TEXT("MHS6 PACKAGED PASS: %s"), *Message);
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("MHS6 PACKAGED FAIL: %s"), *Message);
        }
        FPlatformMisc::RequestExitWithStatus(false, bSuccess ? 0 : 1);
        return false;
    }

    bool Tick(float DeltaSeconds)
    {
        if (FPlatformTime::Seconds() - StartedAt > 120.0) return Finish(false, TEXT("game world BeginPlay timeout"));
        if (GEngine == nullptr) return true;
        UWorld* World = nullptr;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.WorldType == EWorldType::Game && Context.World() != nullptr && Context.World()->HasBegunPlay())
            {
                World = Context.World();
                break;
            }
        }
        if (World == nullptr) return true;
        const bool bRuntimeOnly = !FModuleManager::Get().IsModuleLoaded(TEXT("MimirCompositeEditor")) &&
            !FModuleManager::Get().IsModuleLoaded(TEXT("MimirCompositeTests")) &&
            !FModuleManager::Get().IsModuleLoaded(TEXT("SQLiteCore")) &&
            !FModuleManager::Get().IsModuleLoaded(TEXT("UnrealEd"));
        if (!bRuntimeOnly) return Finish(false, TEXT("editor-only module is loaded in packaged path"));
        TMap<int32, AMHRuntimeCompositeActor*> Actors;
        for (TActorIterator<AMHRuntimeCompositeActor> It(World); It; ++It)
        {
            if (!It->HasActorBegunPlay()) return Finish(false, TEXT("runtime actor has not begun play"));
            if (Actors.Contains(It->GetSeed())) return Finish(false, TEXT("duplicate runtime placement seed"));
            if (It->GetRuntimeInput().GraphBytes.IsEmpty()) return Finish(false, TEXT("cooked graph input is absent"));
            Actors.Add(It->GetSeed(), *It);
        }
        if (Actors.Num() != 7) return Finish(false, FString::Printf(TEXT("expected seven cooked runtime placements, found %d"), Actors.Num()));
        TArray<TSharedPtr<FJsonValue>> Reports;
        const int32 Seeds[] = {0, 1, 2, 42, 123, 1024, 2147483647};
        for (const int32 Seed : Seeds)
        {
            AMHRuntimeCompositeActor* const* Actor = Actors.Find(Seed);
            if (Actor == nullptr) return Finish(false, FString::Printf(TEXT("missing cooked seed %d"), Seed));
            const FMHResolvedCompositePlan* Plan = (*Actor)->GetResolvedPlan();
            if (Plan == nullptr) return Finish(false, (*Actor)->GetLastRuntimeError());
            TSharedPtr<FJsonObject> Report;
            FString Error;
            if (!MHBuildCompositePlanReport(*Plan, (*Actor)->GetMaterializedComponents(), Report, Error)) return Finish(false, Error);
            Reports.Add(MakeShared<FJsonValueObject>(Report));
        }
        FString Error;
        if (!MHWriteCompositeParityReport(TEXT("packaged"), TEXT("Game"), bRuntimeOnly, Reports, Error)) return Finish(false, Error);
        return Finish(true, MHCompositeParityReportPath(TEXT("packaged")));
    }

    FTSTicker::FDelegateHandle TickHandle;
    double StartedAt = 0.0;
};

IMPLEMENT_PRIMARY_GAME_MODULE(FMimirCompositeV5S6Host, MimirCompositeV5S6, "MimirCompositeV5S6");
