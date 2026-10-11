#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "OWSContactPolicy.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOWSContactMomentumTest, "OWS.Contact.Momentum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOWSContactMomentumTest::RunTest(const FString&)
{
    for (double VehicleMass : {500., 1500., 15000.})
        for (double CharacterMass : {50., 100., 150.})
            for (double Speed : {1., 50., 312.928, 3000.})
            {
                const double J = OWSContact::NormalImpulse(Speed, VehicleMass, CharacterMass);
                const double CarAfter = Speed - J / VehicleMass;
                const double PersonAfter = J / CharacterMass;
                TestTrue(TEXT("Finite masses conserve linear momentum"), FMath::IsNearlyEqual(
                    VehicleMass * Speed, VehicleMass * CarAfter + CharacterMass * PersonAfter, 1.e-7));
                TestTrue(TEXT("No kinetic energy creation or extra launch"),
                    VehicleMass * CarAfter * CarAfter + CharacterMass * PersonAfter * PersonAfter <= VehicleMass * Speed * Speed + 1.e-7);
                TestTrue(TEXT("Inelastic normal contact removes closing motion"), FMath::IsNearlyEqual(CarAfter, PersonAfter, 1.e-7));
                TestTrue(TEXT("A character does not stop a much heavier vehicle"), CarAfter > 0.);
            }
    TestEqual(TEXT("Separating contacts do not receive an impulse"), OWSContact::NormalImpulse(-1., 1500., 100.), 0.);
    TestEqual(TEXT("Stationary touching bodies do not receive an impulse"), OWSContact::NormalImpulse(0., 1500., 100.), 0.);
    TestEqual(TEXT("Invalid mass is rejected"), OWSContact::NormalImpulse(1., 0., 100.), 0.);
    TestEqual(TEXT("Nonfinite input is rejected"), OWSContact::NormalImpulse(std::numeric_limits<double>::infinity(), 1500., 100.), 0.);
    const FVector Inertia(1000000., 2000000., 3000000.);
    TestEqual(TEXT("Center contact uses measured body mass"), OWSContact::EffectiveContactMass(1500., FVector::ZeroVector, Inertia), 1500.);
    const double Effective = OWSContact::EffectiveContactMass(1500., FVector(0., 0., 100.), Inertia);
    TestTrue(TEXT("Off-center contact accounts for rotational compliance"), Effective > 0. && Effective < 1500.);
    const double J = OWSContact::NormalImpulse(100., Effective, 100.);
    const double LinearAfter = 100. - J / 1500.;
    const double CharacterAfter = J / 100.;
    const double AngularAfter = -100. * J / Inertia.Z;
    TestTrue(TEXT("Off-center translation plus rotation does not create energy"),
        1500. * FMath::Square(LinearAfter) + 100. * FMath::Square(CharacterAfter) +
        Inertia.Z * FMath::Square(AngularAfter) <= 1500. * FMath::Square(100.) + 1.e-7);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOWSContactRecoveryPolicyTest, "OWS.Contact.RecoveryAndPlayerControl",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOWSContactRecoveryPolicyTest::RunTest(const FString&)
{
    for (bool Support : {false, true})
        for (bool Clearance : {false, true})
            for (bool Settled : {false, true})
                TestEqual(TEXT("Recovery requires ground, free standing space and settled body"),
                    OWSContact::CanRecover(Support, Clearance, Settled), Support && Clearance && Settled);
    for (bool Upright : {false, true})
        for (bool Moving : {false, true})
        {
            TestFalse(TEXT("A player never receives automatic evasive movement"), OWSContact::CanEvade(true, Upright, Moving));
            TestEqual(TEXT("NPC evasion requires available locomotion"), OWSContact::CanEvade(false, Upright, Moving), Upright && Moving);
        }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOWSContactThreatPolicyTest, "OWS.Contact.ThreatPrediction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOWSContactThreatPolicyTest::RunTest(const FString&)
{
    double Time = 0.;
    TestTrue(TEXT("Closing threat forecast"), OWSContact::ThreatTime(FVector(1000., 0., 0.), FVector(-500., 0., 0.), 100., 3., Time));
    TestTrue(TEXT("First footprint contact time, not center collision time"), FMath::IsNearlyEqual(Time, 1.8, 1.e-7));
    TestFalse(TEXT("Receding vehicle is not an approaching threat"), OWSContact::ThreatTime(FVector(1000., 0., 0.), FVector(500., 0., 0.), 100., 3., Time));
    TestFalse(TEXT("Passing course misses the footprint"), OWSContact::ThreatTime(FVector(1000., 500., 0.), FVector(-500., 0., 0.), 100., 3., Time));
    TestFalse(TEXT("Stationary rear car does not magically threaten"), OWSContact::ThreatTime(FVector(-50., 0., 0.), FVector::ZeroVector, 100., 3., Time));
    TestTrue(TEXT("Already touching and moving is immediate"), OWSContact::ThreatTime(FVector::ZeroVector, FVector(500., 0., 0.), 100., 3., Time));
    TestFalse(TEXT("Threat outside the forecast horizon is ignored"), OWSContact::ThreatTime(FVector(1000., 0., 0.), FVector(-500., 0., 0.), 100., 1., Time));
    return true;
}
#endif
