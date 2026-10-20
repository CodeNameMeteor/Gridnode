# MECMP: third person, camera and player collision

[MECMP](https://github.com/ReGlitched/mecmp) (MIT) added third person and player collision on
2026-10-16 (commit 8010e62, documented in its `docs/PLAYER_OPTIONS.md`). The addresses below are in
GridNode's annotations with `sources: [mecmp]`; most are hypotheses until GridNode uses them.

## Camera

- The active camera (ClientCameraViewManager +0x208, an `FPSCamera` in normal play, vtable
  0x141b18e58) recomputes itself every frame in its vtable +0x68 update. That calls +0x70
  (`FPSCamera_computeTransforms`, 0x14327fb80), which fills two LinearTransforms: the camera and the
  camera mesh. The update stores them at camera +0x10 and +0x50.
- `ClientCameraViewManager_frameUpdate` (0x14327ba00) runs the camera's update, then copies camera
  +0x10..+0x9f into the manager's +0x90..+0x11f, which feeds the render view. Writes made from the
  post-simulation listeners (+0x1b90) come before this and are lost.
- MECMP's third person replaces the +0x70 slot in the FPSCamera vtable and moves both transforms
  back along +forward (Frostbite looks down -forward). GridNode's free camera instead gives the one
  camera object a copy of its vtable whose +0x68 entry runs the game's update and then writes both
  transforms.

## Body mesh

- The client character has an embedded interface at +0x1d8. Its getViewMode (slot 0x141c3cc68,
  0x14390ed90, `int(iface)`) chooses the body: 1 is the third-person body with a head. MECMP returns
  1 for the local player while third person is active. GridNode does the same, through a vtable copy
  on that interface, while the free camera is on.

## Collision bodies (for spawned barriers later)

- MECMP builds native Havok boxes through the body factory 0x1435169e0 (shape type 1, 16-byte
  aligned half-extents at +0x20, body kind 2 at +0x14, StaticLayer filter), with the physics
  creation info 0x142d4d1a0 (owner entity bus at +0x28).
- Body transform 0x1434f8930 / source 0x1434f6d90; enable 0x1434e90f0; spatial registration
  0x1434e9270; spatial removal 0x1434f6610; disable 0x1434f65f0; deferred destruction 0x1434f5550.
- Faith's capsule: ClassInfo 0x1427bd350, body id +0x40, active +0x44. Move tracker ClassInfo
  0x142853420, state +0x150.
