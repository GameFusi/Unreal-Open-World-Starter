#pragma once

#include "CoreMinimal.h"

namespace OWSContact
{
// Normal momentum exchange, in Unreal's kg/cm/s units. Separating contacts do
// not receive another impulse. This is not a launch velocity or a mass override.
inline double NormalImpulse(double ClosingSpeed, double MassA, double MassB)
{
    if (!FMath::IsFinite(ClosingSpeed) || !FMath::IsFinite(MassA) ||
        !FMath::IsFinite(MassB) || ClosingSpeed <= 0. || MassA <= 0. || MassB <= 0.) return 0.;
    return ClosingSpeed / (1. / MassA + 1. / MassB);
}

// Effective contact mass includes rotation about the center of mass. Using
// chassis mass alone for an off-center impact would inject rotational energy.
inline double EffectiveContactMass(double Mass, const FVector& LeverCrossNormal,
    const FVector& LocalInertia)
{
    if (!FMath::IsFinite(Mass) || Mass <= 0. || LeverCrossNormal.ContainsNaN() ||
        LocalInertia.ContainsNaN()) return 0.;
    double InverseMass = 1. / Mass;
    for (int32 Axis = 0; Axis < 3; ++Axis)
        if (LocalInertia[Axis] > UE_SMALL_NUMBER)
            InverseMass += FMath::Square(LeverCrossNormal[Axis]) / LocalInertia[Axis];
    return 1. / InverseMass;
}

inline bool CanRecover(bool HasSupport, bool HasClearance, bool IsSettled)
{
    return HasSupport && HasClearance && IsSettled;
}

inline bool CanEvade(bool PlayerControlled, bool Upright, bool MovementEnabled)
{
    return !PlayerControlled && Upright && MovementEnabled;
}

// Closest approach of two finite footprints under their measured relative
// motion. Time is seconds; all distances/velocities are cm and cm/s.
inline bool ThreatTime(const FVector& RelativePosition, const FVector& RelativeVelocity,
    double CombinedRadius, double Horizon, double& OutTime)
{
    OutTime = 0.;
    if (RelativePosition.ContainsNaN() || RelativeVelocity.ContainsNaN() ||
        !FMath::IsFinite(CombinedRadius) || !FMath::IsFinite(Horizon) ||
        CombinedRadius <= 0. || Horizon <= 0.) return false;
    const FVector P(RelativePosition.X, RelativePosition.Y, 0.);
    const FVector V(RelativeVelocity.X, RelativeVelocity.Y, 0.);
    const double C = P.SizeSquared() - FMath::Square(CombinedRadius);
    if (C <= 0.) return V.SizeSquared() > UE_SMALL_NUMBER && FVector::DotProduct(P, V) <= 0.;
    const double A = V.SizeSquared();
    const double B = FVector::DotProduct(P, V);
    if (A <= UE_SMALL_NUMBER || B >= 0.) return false;
    const double Discriminant = B * B - A * C;
    if (Discriminant < 0.) return false;
    OutTime = (-B - FMath::Sqrt(Discriminant)) / A;
    return OutTime >= 0. && OutTime <= Horizon;
}
}
