# Gameplay Systems (Tier 1)

The gameplay layer that turns the engine from a cinematic sandbox into a playable
stealth-combat game. All of it runs from one call site — `gameplay::Update` in the
frame loop (`Engine::Run`, gated by `physics.IsRunning()`), which sequences the
sub-systems in a fixed order: **AI → spawning/encounters → combat → player fire**.
It runs after physics (fresh positions for line-of-sight and hits) and *before*
`nav::UpdateAgents`, so an AI-set navigation target steers the same frame.

Every system follows the engine conventions: plain EnTT components in
`Scene/Components.h`, the 5-site serializer pattern (collector / write / `EntityData`
field / parse / instantiate) with the `runtimeTags` split for runtime-only state,
and — for scripting — the 4-site schematic node lockstep (`NodeType` enum +
`Describe()` + interpreter `SchematicSystem.cpp` + transpiler `SchematicTranspile.cpp`).

## Combat — `Game/CombatSystem.{h,cpp}`

Faction-based, data-driven. The player and every enemy carry the **same** `Health`
component and differ only by `Health.faction` (`Neutral/Player/Enemy/Ally/...`).
Hostility is derived (`Hostile(a,b)`): same faction = friendly (blocked unless the
target opts into `friendlyFire`), different non-neutral = hostile.

- `combat::ApplyDamage(scene, DamageEvent, physics?)` — the single choke point:
  faction/invuln/invincible filtering, i-frames, knockback (if a body), and on the
  alive→dead edge sets `alive=false`.
- `combat::Update` ticks regen, invuln timers, weapon cooldown/reload, and fires the
  **one-shot death dispatch**: sets `onDeathFlag`/`onDeathObjective`, queues the
  `OnDeath` schematic event, stops the `NavigationAgent`, switches to `deathClip`.
- `Weapon` is a capability an actor carries. **Hitscan** does a two-step trace (wall
  occlusion via `PhysicsWorld::Raycast` + gameplay-side ray-vs-sphere over `Health`
  entities, since characters are bodyless capsules a Jolt ray never hits). **Melee**
  sweeps a forward arc. `radius > 0` makes it AoE (LoS-gated radial falloff).
- Schematic nodes: `ApplyDamage`, `Kill`, `Heal`, `SetHealth`, `SetInvulnerable`,
  `GetHealth`, `IsAlive`, and the `OnDeath` event (fires on every listener, filtered
  by `deathTag`).

## AI — `Game/AISystem.cpp`

A compact hand-rolled FSM (`Idle/Patrol/Investigate/Chase/Attack/Search/Flee/Dead`).
Per agent (`AIPerception` + `AIBehavior` + `NavigationAgent`):

1. **Sense** — nearest hostile `Health` entity inside the sight cone with a clear LoS
   (`PhysicsWorld::Raycast`); hearing from the `game::` noise bus.
2. **Awareness** — a 0..1 meter that ramps with proximity while sensed, decays when
   not; crossing `detectThreshold` fires the `OnSpotPlayer` event (on the spotter's
   own graph).
3. **Act** — sets the `NavigationAgent` target (locomotion animates itself from the
   resulting velocity via motion matching), faces + fires in Attack.

Patrol waypoints are inline on `AIBehavior` (loop / ping-pong / once). Noise
(`game::EmitNoise`) is a TTL-aged multi-listener bus (footsteps, gunfire, thrown
props) — the "distraction" hook. Schematic nodes: `SetAIState`, `SetAlert`,
`IsPlayerVisible`, `GetAwareness`, `OnSpotPlayer`.

## Spawning + Encounters — `Game/SpawnSystem.{h,cpp}`

`Spawner` instantiates a `.hbprefab` on its trigger (player-in-volume / story flag /
manual-schematic), scatters the burst on a deterministic disc, tags each root
`Spawned{encounter, spawner}`, throttles to `maxAlive`, and refills (`Once` /
`Continuous`). `Encounter` groups spawners by id, tallies alive members by scanning
`Spawned` tags, and on clear fires an `InteractAction` (flag / objective / dialogue /
cutscene) to gate progression.

**Double-spawn-proof reload** (the key rule): spawned entities are ordinary
serialized entities; the spawner's *progress* (`activated`, `spawnedTotal`) persists
only under `runtimeTags` (snapshots + `.hbsave`, never the authored `.hbscene`), and
tallying is by **string tag, not entt handle** (handles don't survive the save
Replace). Schematic nodes: `SpawnGroup`, `DespawnAll`, `AliveCount`.

## Inventory / Pickup / Crafting — `game::` + `Game/InventorySystem.h`

A per-playthrough item id→count singleton in `game::` (beside the story flags),
riding the same `.hbsave` `SerializeState`/`DeserializeState` and New-Game `Reset`
lifecycle — no new engine save plumbing. `AddItem`/`RemoveItem`/`HasItem`/`ItemCount`
+ `EquipWeapon`/`EquippedWeapon` (the seam a future combat loadout reads).

**Pickups** are a new `InteractAction::GrantItem` (appended after `None` to preserve
serialized indices) on `Interactable`/`TriggerVolume`: grants the item then removes
the pickup **permanently** via a `picked.<pickupId>` flag that survives save/load AND
a full level reload (a per-frame pass destroys already-picked items). **Crafting**
composes from the schematic nodes (`HasItem` + `RemoveItem` + `GrantItem`) — e.g. a
crafting-bench graph. HUD tokens: `{item:<id>}`, `{equipped}`.

## Facial / Blendshapes — `Scene/FacialSystem.{h,cpp}` (CPU layer)

`MorphState` holds blendshape channel weights; `FacialAnimator` drives them each
frame: **lip-sync** (audio-amplitude RMS envelope of the speaking dialogue line,
attack/release-smoothed onto the jaw morph), **eye-blink** (timed quadratic pulse,
deterministic per-entity PRNG), and an **expression preset** (`.hbface` library).
Lip-sync is hooked into dialogue-line playback where the speaker entity is resolved.
Schematic nodes: `SetMorphWeight`, `PlayFacialExpression`.

**GPU vertex deformation** is wired end-to-end: the importer extracts Assimp
`aiAnimMesh` deltas into an RGBA32F delta atlas (one row per target), built at spawn
and read sampler-free in `MeshPBR.hlsl` (`gTextures[idx].Load()`) to accumulate the
top-8 active channels *before* skinning. The object-CB tail (`gMorph*`) is append-only
and sentinel-guarded (`gMorphTexIndex != 0`), mirrored in both backends, so non-morph
draws are byte-identical (verified: shaders compile on both backends, readback shows
no rendering regression). **Known limitations (follow-ups):** position-only deltas (no
normal deltas yet — lighting on morphed regions is approximate); the atlas resolves
only for a fresh-staged single-instance mesh (multi-instance / cached-mesh / modular
head parts need the mesh-cache to carry the atlas); and morph deltas are **not** yet
persisted through the `.uap` pack cook (UAF v8), so they survive a direct model import
but not a cooked pack. The deform itself needs a visual check on a morphing head.
