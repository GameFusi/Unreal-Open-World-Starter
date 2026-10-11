# Character and vehicle contact repair — OWS upstream port

Status: Aurora reported the #22/#144 fixes working in Entourage on 2026-10-10 and authorized this OWS port, followed by an AuroraBot commit, push and pull request. This port has not been compiled or gameplay-tested in OWS by this task. Aurora is handling gameplay testing; automation execution was not requested. Git history and the pull request record publication status. Merging is not part of this authorization.

## Approved behavior

- Very slow car contact pushes an upright character rather than treating them as an immovable wall. A stationary touch adds no launch impulse.
- Harder contact uses **7 mph relative normal closing speed** as the initial adjustable knockdown threshold (312.928 cm/s). This is a gameplay starting point, not a medically validated universal threshold. Speed matching and glancing contact therefore differ from a head-on strike at the same dashboard speed.
- Sustained contact without clearing the car also causes loss of balance. The initial accumulated contact-travel threshold is one capsule radius, exposed as `SustainedPushRadiusMultiplier`.
- Fallen characters use the existing finite-mass skeletal physics asset and authored joint constraints. Gravity, actual chassis contact and surface contact are solved by Chaos. No added upward kick, injected spin, huge character force or maximum-recovery timeout.
- Players never receive automatic evasive input. NPCs may attempt navigable escape after seeing a threat through enabled, appropriately filtered selector cones, hearing an actual playing vehicle audio source within the separate hearing sphere, or being physically touched. Occlusion and reaction delay apply. Silent vehicles are not magically heard.
- No injury/death handling. Ground support, settled bodies and clear standing space are all required to recover. Cars or other characters can keep someone pinned. Clearance/support are rechecked during the actual floor get-up montage.
- Existing bailout/controlled-roll calculations, clips and handoff behavior are not changed. A read-only bailout ownership query prevents this contact system from taking over that motion.

## Collision implementation

`UOWSContactWorldSubsystem` attaches the contact components to OWS-derived characters and stock-compatible vehicles in game/PIE worlds. The component can also be added to a Blueprint to expose its tuning. No character reparenting or movement-stick/locomotion configuration change is required.

Each supported simulated chassis gets an identical hidden, query-only primitive shape. The original physical hull ignores upright `Pawn` capsules; the non-simulated, non-welded query hull still blocks their locomotion and preserves the authored Traversable response. This removes the infinite-mass capsule physics contact and prevents CharacterMovement's touch/push/repulsion routines from applying large impulses to the chassis. Unsupported primitive classes leave that vehicle's original responses unchanged and report a diagnostic.

Upright contact uses measured point velocity, finite contact masses and an equal-and-opposite inelastic normal impulse. Off-center chassis contact includes rotational compliance from its principal inertia and center of mass. At knockdown, vehicle/body impact is handed to Chaos rather than adding a second synthetic impact impulse. Local limb motors, angular damping, CCD and bounded initial-overlap correction constrain the fallen response without pulling the pelvis toward an upright world-space target.

The `OWSCharacterBody` object channel separates fallen bodies from upright capsules. Both still collide with the real world; fallen bodies still collide with the actual chassis. Suspension queries ignore both character categories so a character cannot become an artificial wheel support ramp. Other suspension responses remain unchanged. Former stock selector category lists include the new body category so downed characters retain targeting; developer-customized filters are not overwritten.

The suspension source had GBK-encoded Chinese comments. A mechanically verified GBK-to-UTF-8 conversion preserves their decoded text; its behavior change is confined to the character support filters in `Initialize`.

## Verification handoff

The existing Entourage gameplay verification is Aurora's confirmation, not an automated-test result. No game or editor was stopped or restarted for this OWS port. Any OWS compilation must be coordinated with the team.

The eight `OWS.Contact.*` regression tests available for a coordinated editor run are: `Momentum`, `RecoveryAndPlayerControl`, `ThreatPrediction`, `CollisionHullIsolation`, `SlowPushConservation`, `WheelsExcludeCharacters`, `ReblockedGetUpOwnership`, and `PostMovementActorTickOrder`. The collision fixtures use disposable worlds and call production code; policy tests check conservation and the gating combinations. The get-up interruption fixture uses the real mannequin rig and floor get-up montage, verifies repeated re-pinning preserves the original capsule collision setting, and checks that the body returns to physics while pinned. It deliberately does not simulate a full settling/get-up cycle. These tests do **not** substitute for exercising the actual character physics rigs, montage slots, camera, navmesh or suspension in the game.

Also available are the three `OWS.Vehicle.Bailout.*` tests in `FOWSMissingRollFixture`: `MissingRollRestoration`, `MissingRollReentry`, and `ValidRollOwnership`. They exercise production bailout cleanup, actual seat entry and moving exit in a disposable world, using the existing mannequin mesh and animation assets. No test changes a live map or editor play session.

### Issue #144: unavailable roll asset

The public OWS source at `c31d64f` warns after a failed roll load but leaves controlled-bailout/input ownership active. Entourage's existing `96231694` checkpoint already calls `FinishControlledBailout` on this failure. That donor fix is not newly authored by this repair; the failure-cleanup call is included in this local upstream port.

This repair removes cleanup's unrelated `DefaultSlot` dynamic-montage stop when no roll animation was ever owned. A missing asset must release only bailout-owned state: ordinary locomotion/input and reentry are restored without stopping another animation or clearing someone else's movement-input ignore. The tests cover null and stale references, both roll sides, stacked pre-existing input ignores, repeated fallback cleanup, repeated moving exit/reentry, and the unchanged valid-roll ownership/handoff path. Aurora has reported the fixes working in Entourage. The regression tests have been ported but not executed by this task; this is not a claim of independently verified automation results.

Manual checks:

| Scenario | Required observation |
| --- | --- |
| Stationary car touching character, then gentle acceleration | No initial kick; car pushes rather than dead-stopping/bouncing |
| Continued slow pushing without clearing contact | Character eventually falls; car can continue over them |
| Higher-speed and glancing strikes; speed-matched contact | Relative normal motion controls the response; no synthetic launch or spin |
| Player faces an approaching vehicle | No automatic dodge/input replacement |
| NPC sees, hears or touches an approaching vehicle | Attempts a validated route when there is time; cannot teleport through obstacles; ambient activity input must not override the reflex |
| Vehicle stops over a fallen character; another character stands over one | Remains pinned; gets up only after obstruction clears and support/settling conditions hold |
| New obstruction arrives during get-up | Returns to fallen-body physics rather than standing through the obstruction |
| Jump/climb/press against wheels or chassis | No character-driven chassis launch/flip; normal traversal support remains available |
| Vehicle entry after recovery; activation while still down | Re-entry works after recovery; entry is rejected while bodily contact recovery owns the character |
| Existing stationary exit and controlled moving bailout | Their previously verified behavior remains unchanged |
| Temporarily absent roll asset, moving exit, then reentry | Control immediately returns; no recovery/input/reentry lock; unrelated animation/input owners are preserved |
| Quinn/Manny/Jane appearance layers | Physics follows the independent main locomotion rig and retargeted visuals; compatible floor get-up montages play, not a jump/land/stumble substitute |

Authority executes the contact response and NPC reflex. This implementation does not replicate the component's contact state or skeletal physical poses. Multiplayer integration/testing remains separate work; do not describe this source handoff as multiplayer-ready.

Diagnostics: `[OWSContact]` logs state changes; `[OWSContactSample]` and `[OWSContactBody]` sample contact/settling data at the configurable 0.1-second default interval. Logs include masses, point/body velocities, closing speed, pressure travel, penetration, gravity and recovery support/clearance. `bLogTransitions=false` disables this component's transition/sample logging.

Physics references used: [OpenStax: impulse and collisions](https://openstax.org/books/university-physics-volume-1/pages/9-2-impulse-and-collisions), [OpenStax: elastic and inelastic collisions](https://openstax.org/books/physics/pages/8-3-elastic-and-inelastic-collisions), and [Epic: physics-driven animation](https://dev.epicgames.com/documentation/unreal-engine/physics-driven-animation-in-unreal-engine). Engine 5.8 source was also inspected for CharacterMovement force paths, skeletal-body mass/inertia, physics-animation drives, collision queries and audio attenuation. Conservation equations inform contact response; human balance thresholds and visual motor tuning still require gameplay validation.

## Upstream scope and compatibility

Only the character/vehicle contact components, installation subsystem, contact policy, suspension character filters, fallen-body collision profile/selector category, bailout ownership guards and missing-roll cleanup are ported. The existing controlled-roll energy model, animations, steering and handoff calculations remain unchanged.

OWS's current public vehicle API predates Entourage's separate seating and NPC-boarding work. The contact guard recognizes the same `OWS.Pose.Seated` tag directly without depending on the downstream seated-pose library. The missing-roll fixture initializes public default seats through their normal BeginPlay lifecycle and reserves the passenger slot so both doors exercise the existing driver-only path. These are upstream compatibility adaptations, not new seating or boarding features.

No private product plugins, extra recreation systems, maps, character assets, renderer settings or unrelated source changes were copied. Pre-existing dirty maps and JanePrototype assets in OWS are preserved. Multiplayer replication remains separate as described above.
