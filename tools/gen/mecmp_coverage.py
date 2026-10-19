#!/usr/bin/env python3
"""Checks that every address, offset and size in MECMP's src/game/engine.h is in the GridNode
database (annotations, or the reflected dump), with the same value. Used for the M2 exit test and
for moving MECMP onto GridNode (M5).

    python tools/gen/mecmp_coverage.py path/to/MECMP/src/game/engine.h --reflected gridnode-reflected.json
"""

from __future__ import annotations

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gridnode_gen import annotations, reflected  # noqa: E402
from gridnode_gen.cli import ANNOTATIONS  # noqa: E402


def build_lookup(db: annotations.Database, r: reflected.Reflected | None):
    """name in engine.h -> (value from the database, where it comes from)."""

    def cls(c, kind, name):
        a = db.classes[c]
        if kind == "off":
            return next(m.offset for m in a.members if m.name == name), f"annotation {c}.{name}"
        if kind == "vf":
            return next(v.offset for v in a.virtuals if v.name == name), f"annotation {c}::{name}"
        if kind == "va":
            return next(v.va for v in a.virtuals if v.name == name), f"annotation {c}::{name} va"
        return a.vtable, f"annotation {c} vtable"

    def st(s, name=None):
        a = db.structs[s]
        if name is None:
            return a.size, f"annotation struct {s} size"
        return next(m.offset for m in a.members if m.name == name), f"annotation {s}.{name}"

    def hook(h, what="slot"):
        x = db.hooks[h]
        return getattr(x, what), f"annotation hook {h}.{what}"

    def ref(t, what, field=None):
        if r is None:
            return None, "needs --reflected"
        x = r.by_name[t]
        if what == "ti":
            return x["_ti"], f"reflected {t} ti"
        if what == "size":
            return x["size"], f"reflected {t} size"
        if what == "vtable":
            return int(x["vtables"][0], 16), f"reflected {t} vtable"
        if what == "field":
            return next(f["offset"] for f in x["fields"] if f["name"] == field), f"reflected {t}.{field}"

    fn = lambda n: (db.functions[n].va, f"annotation function {n}")  # noqa: E731
    gl = lambda n: (db.globals[n].va, f"annotation global {n}")  # noqa: E731
    code = lambda n: (db.code[n].va, f"annotation code site {n}")  # noqa: E731
    const = lambda n: (db.constants[n].value, f"annotation constant {n}")  # noqa: E731

    return {
        "Slot_PamUISystemUpdate": lambda: hook("PamUISystem_update"),
        "Fn_PamUISystemUpdate": lambda: hook("PamUISystem_update", "original"),
        "Slot_PresenceInfoManagerPoolUpdate": lambda: hook("PresenceInfoManagerPool_update"),
        "Fn_PresenceInfoManagerPoolUpdate": lambda: hook("PresenceInfoManagerPool_update", "original"),
        "Vtbl_PamClientPresenceInfoManagerEntity": lambda: cls("PamClientPresenceInfoManagerEntity", "vtable", None),
        "Fn_PresenceInfoManager_assignSlot": lambda: fn("PresenceInfoManager_assignSlot"),
        "Slot_ServerNamedChallengeCreator": lambda: hook("PamServerNamedChallengeEntity_create"),
        "Fn_ServerNamedChallengeCreator": lambda: hook("PamServerNamedChallengeEntity_create", "original"),
        "Slot_ServerChallengeMarkerCreator": lambda: hook("PamServerChallengeMarkerEntity_create"),
        "Fn_ServerChallengeMarkerCreator": lambda: hook("PamServerChallengeMarkerEntity_create", "original"),
        "Slot_PresenceInfoManagerCreator": lambda: hook("PamClientPresenceInfoManagerEntity_create"),
        "Fn_PresenceInfoManagerCreator": lambda: hook("PamClientPresenceInfoManagerEntity_create", "original"),
        "UISystem_PauseController": lambda: cls("PamUISystem", "off", "pauseController"),
        "kTypeInfoListHead": lambda: gl("TypeInfoListHead"),
        "kServerGameWorld": lambda: gl("ServerGameWorld"),
        "World_Level": lambda: st("ServerGameWorld", "level"),
        "Level_Data": lambda: st("ServerLevel", "data"),
        "kClientCameraViewManagers": lambda: gl("ClientCameraViewManagers"),
        "Code_LoadCameraViewManagers": lambda: code("CameraViewManagers_load"),
        "CamMgrs_List": lambda: st("ClientCameraViewManagers", "managersBegin"),
        "CamMgr_Transform": lambda: st("ClientCameraViewManager", "transform"),
        "CamMgr_FovYDeg": lambda: st("ClientCameraViewManager", "fovYDegrees"),
        "CamMgr_ActiveCamera": lambda: st("ClientCameraViewManager", "activeCamera"),
        "Camera_BaseFovYDeg": lambda: st("CameraObject", "baseFovYDegrees"),
        "kXInputGetStatePtr": lambda: gl("XInputGetStatePtr"),
        "Code_StoreXInputGetState": lambda: code("XInputGetState_store"),
        "TI_ServerCharacterEntity": lambda: ref("ServerCharacterEntity", "ti"),
        "TI_ObjectBlueprint": lambda: ref("ObjectBlueprint", "ti"),
        "TI_PamReplayPlayerComponentData": lambda: ref("PamReplayPlayerComponentData", "ti"),
        "TI_PamServerReplayPlayerComponent": lambda: ref("PamServerReplayPlayerComponent", "ti"),
        "TI_ServerCharacterCustomizationComponent": lambda: ref("ServerCharacterCustomizationComponent", "ti"),
        "TI_PamClientReplayRecordingComponent": lambda: ref("PamClientReplayRecordingComponent", "ti"),
        "Slot_ServerCharacterSpawnFactory": lambda: hook("ServerCharacterSpawnEntity_factory"),
        "Fn_ServerCharacterSpawnFactory": lambda: hook("ServerCharacterSpawnEntity_factory", "original"),
        "Slot_PamServerCharacterSpawnFactory": lambda: hook("PamServerCharacterSpawnEntity_factory"),
        "Fn_PamServerCharacterSpawnFactory": lambda: hook("PamServerCharacterSpawnEntity_factory", "original"),
        "Slot_ClientReplayPlayerFactory": lambda: hook("PamClientReplayPlayerComponent_factory"),
        "Fn_ClientReplayPlayerFactory": lambda: hook("PamClientReplayPlayerComponent_factory", "original"),
        "Vtbl_ClientReplayUpdater": lambda: hook("ClientReplayUpdater_updateAll", "vtable"),
        "Vtbl_ClientReplayUpdateAll": lambda: hook("ClientReplayUpdater_updateAll", "offset"),
        "Fn_ClientReplayUpdateAll": lambda: hook("ClientReplayUpdater_updateAll", "original"),
        "Vtbl_PamCustomizeCharacterData": lambda: ref("PamCustomizeCharacterData", "vtable"),
        "PamCustomizeCharacterData_Size": lambda: ref("PamCustomizeCharacterData", "size"),
        "ClientChar_GhostShader": lambda: cls("PamClientCharacterEntity", "off", "ghostShader"),
        "VisualUnlockAsset_Size": lambda: ref("VisualUnlockAsset", "size"),
        "DataContainer_Header": lambda: ref("Asset", "size"),
        "VisualUnlock_RenderIn1p": lambda: ref("VisualUnlockAsset", "field", "RenderIn1p"),
        "Slot_ClientCharacterFactory": lambda: hook("PamClientCharacterEntity_factory"),
        "Slot_CustomizeProjectionFactory": lambda: hook("PamServerCustomizeProjectionCharacterEntity_create"),
        "Fn_CustomizeProjectionCreate": lambda: hook("PamServerCustomizeProjectionCharacterEntity_create", "original"),
        "Fn_ClientCharacterFactory": lambda: hook("PamClientCharacterEntity_factory", "original"),
        "Slot_ServerWorldPostUpdate": lambda: hook("ServerGameWorld_postUpdate"),
        "Fn_ServerWorldPostUpdate": lambda: hook("ServerGameWorld_postUpdate", "original"),
        "Vtbl_PamServerCharacterSpawnEntity": lambda: cls("PamServerCharacterSpawnEntity", "vtable", None),
        "Vtbl_ServerCharacterSpawnEntity": lambda: cls("ServerCharacterSpawnEntity", "vtable", None),
        "Vtbl_PamClientReplayPlayerComponent": lambda: cls("PamClientReplayPlayerComponent", "vtable", None),
        "Vtbl_PamClientCharacterEntity": lambda: cls("PamClientCharacterEntity", "vtable", None),
        "Vtbl_PamServerCharacterEntity": lambda: cls("PamServerCharacterEntity", "vtable", None),
        "Vtbl_PamServerCustomizeProjectionCharacterEntity": lambda: ref("PamServerCustomizeProjectionCharacterEntity", "vtable"),
        "Fn_SpatialEntity_setTransform": lambda: fn("SpatialEntity_setTransform"),
        "Fn_ServerCharacterEntity_teleport": lambda: fn("ServerCharacterEntity_teleport"),
        "Fn_RaycastQuery": lambda: fn("RaycastQuery_execute"),
        "Fn_NetState_setDirty": lambda: fn("NetState_setDirty"),
        "Fn_Arena_allocFromMember": lambda: fn("Arena_allocFromMember"),
        "Fn_LocalClientCharacter": lambda: fn("LocalClientCharacter"),
        "Fn_RbTreeIncrement": lambda: fn("RbTree_increment"),
        "Vtbl_SpawnerSpawn": lambda: cls("ServerCharacterSpawnEntity", "vf", "spawn"),
        "Vtbl_GetComponent": lambda: cls("PamClientCharacterEntity", "vf", "getComponent"),
        "Vtbl_ApplyCustomize": lambda: cls("PamServerCharacterEntity", "vf", "applyCustomization"),
        "Vtbl_ClientSetVisible": lambda: cls("PamClientCharacterEntity", "vf", "setVisible"),
        "Asset_Name": lambda: ref("Asset", "field", "Name"),
        "ReferenceObjectData_Blueprint": lambda: ref("ReferenceObjectData", "field", "Blueprint"),
        "ObjectBlueprint_Object": lambda: ref("ObjectBlueprint", "field", "Object"),
        "Component_Collection": lambda: cls("Component", "off", "collection"),
        "CI_Bus": lambda: st("EntityCreationInfo", "bus"),
        "CI_Data": lambda: st("EntityCreationInfo", "data"),
        "Spawner_Data": lambda: cls("ServerSpawnEntity", "off", "data"),
        "Spawner_Spawned": lambda: cls("ServerSpawnEntity", "off", "spawnedBegin"),
        "Spawner_Enabled": lambda: cls("ServerSpawnEntity", "off", "enabled"),
        "SpawnerData_Template": lambda: ref("CharacterSpawnReferenceObjectData", "field", "Template"),
        "Template_Customization": lambda: ref("CharacterSpawnTemplateData", "field", "CustomizeCharacterData"),
        "Character_Data": lambda: cls("ComponentEntity", "off", "data"),
        "Character_Player": lambda: cls("ServerCharacterEntity", "off", "player"),
        "CharData_GhostShader": lambda: ref("PamCharacterEntityData", "field", "PamUseRunnersGhostShader"),
        "Entity_TransformSpace": lambda: cls("ComponentEntity", "off", "transformSpace"),
        "World_PhysicsManager": lambda: const("World_PhysicsManager"),
        "PhysManager_Holder": lambda: const("PhysicsManager_Holder"),
        "PhysHolder_Physics": lambda: const("PhysicsHolder_Physics"),
        "Phys_HavokWorld": lambda: const("Physics_HavokWorld"),
        "HavokWorld_FilterOwner": lambda: const("HavokWorld_FilterOwner"),
        "FilterOwner_Filter": lambda: const("FilterOwner_Filter"),
        "SRP_PlaybackState": lambda: cls("PamServerReplayPlayerComponent", "off", "playbackState"),
        "SRP_Setup": lambda: cls("PamServerReplayPlayerComponent", "off", "setup"),
        "SRP_Pose": lambda: cls("PamServerReplayPlayerComponent", "off", "pose"),
        "SRP_PoseEnd": lambda: cls("PamServerReplayPlayerComponent", "off", "poseEnd"),
        "SRP_PoseCap": lambda: cls("PamServerReplayPlayerComponent", "off", "poseCapacity"),
        "SRP_PoseArena": lambda: cls("PamServerReplayPlayerComponent", "off", "poseArena"),
        "SRP_NetState": lambda: cls("PamServerReplayPlayerComponent", "off", "netState"),
        "CRP_PlaybackState": lambda: cls("PamClientReplayPlayerComponent", "off", "playbackState"),
        "CRP_PlaybackForced": lambda: cls("PamClientReplayPlayerComponent", "off", "playbackForced"),
        "CRP_Setup": lambda: cls("PamClientReplayPlayerComponent", "off", "setup"),
        "CRP_ReceivedPose": lambda: cls("PamClientReplayPlayerComponent", "off", "receivedPose"),
        "CRP_ExpectedBones": lambda: cls("PamClientReplayPlayerComponent", "off", "expectedBones"),
        "CRP_SkipRoot": lambda: cls("PamClientReplayPlayerComponent", "off", "skipRoot"),
        "Rec_Stride": lambda: st("ReplayRecorder", "stride"),
        "Rec_Capacity": lambda: st("ReplayRecorder", "capacity"),
        "Rec_Base": lambda: st("ReplayRecorder", "base"),
        "Rec_Head": lambda: st("ReplayRecorder", "head"),
        "kGhostBlueprint": lambda: const("GhostBlueprint"),
        "kPlayerBlueprint": lambda: const("PlayerBlueprint"),
        "kMaxBones": lambda: const("MaxReplayBones"),
    }


DECL = re.compile(r"constexpr\s+(?:uintptr_t|size_t|uint32_t|const char\*)\s+([^;]+);")


def parse_engine_h(path: str) -> dict[str, object]:
    out = {}
    text = open(path, encoding="utf-8").read()
    text = re.sub(r"//[^\n]*", "", text)
    for m in DECL.finditer(text):
        for part in m.group(1).split(","):
            name, _, value = part.partition("=")
            name, value = name.strip(), value.strip()
            if not name or not value:
                continue
            out[name] = value.strip('"') if value.startswith('"') else int(value, 0)
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("engine_h")
    ap.add_argument("--reflected")
    a = ap.parse_args(argv)
    db, p = annotations.load(ANNOTATIONS)
    r = reflected.Reflected.load(a.reflected) if a.reflected else None
    values = parse_engine_h(a.engine_h)
    lookup = build_lookup(db, r)
    ok = missing = wrong = 0
    for name, value in values.items():
        if name not in lookup:
            print(f"NOT MAPPED  {name} = {value}")
            missing += 1
            continue
        got, where = lookup[name]()
        if got == value:
            ok += 1
        else:
            print(f"DIFFERS     {name} = {value!r} in engine.h, {got!r} from {where}")
            wrong += 1
    print(f"{len(values)} values in engine.h: {ok} match the database, {wrong} differ, {missing} not mapped")
    return 0 if not wrong and not missing else 1


if __name__ == "__main__":
    sys.exit(main())
