# Creates the Enhanced Input assets this mod uses. Run headless from the SML project:
#   UnrealEditor-Cmd.exe C:\SF\SML\FactoryGame.uproject -run=pythonscript -script="<this file>"
# Re-running updates the existing assets in place.
import unreal

ROOT = "/TerrainBelts/Input"
# Bound while the build gun is building, so the key only works while placing something.
PARENT_CONTEXT = "/Game/FactoryGame/Inputs/Equipment/Buildgun/Build/MC_BuildGunBuild"
CHORD_ALT = "/Game/FactoryGame/Inputs/IA_ChordAlt"

tools = unreal.AssetToolsHelpers.get_asset_tools()
lib = unreal.EditorAssetLibrary
# A commandlet hasn't scanned plugin content yet, so the registry wouldn't know the assets exist
# and create_asset would refuse to overwrite them.
unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([ROOT], True)


def get_or_create(name, cls):
    path = f"{ROOT}/{name}"
    asset = lib.load_asset(path) if lib.does_asset_exist(path) else unreal.load_object(None, f"{path}.{name}")
    if asset is None:
        factory = unreal.DataAssetFactory()
        factory.set_editor_property("data_asset_class", cls)
        asset = tools.create_asset(name, ROOT, cls, factory)
    if asset is None:
        raise RuntimeError(f"Could not create {path}")
    return asset


def make_key(name):
    key = unreal.Key()
    key.set_editor_property("key_name", name)
    return key


def make_mapping(context, action, key_name, triggers, mappable_name, display_name):
    mapping = unreal.EnhancedActionKeyMapping()
    mapping.set_editor_property("action", action)
    mapping.set_editor_property("key", make_key(key_name))
    mapping.set_editor_property("triggers", list(triggers))
    settings = unreal.new_object(unreal.PlayerMappableKeySettings, outer=context)
    settings.set_editor_property("name", mappable_name)
    settings.set_editor_property("display_name", display_name)
    mapping.set_editor_property("setting_behavior", unreal.PlayerMappableKeySettingBehaviors.OVERRIDE_SETTINGS)
    mapping.set_editor_property("player_mappable_key_settings", settings)
    return mapping


toggle = get_or_create("IA_TB_ToggleFollowTerrain", unreal.InputAction)
toggle.set_editor_property("value_type", unreal.InputActionValueType.BOOLEAN)
toggle.set_editor_property("action_description", "Toggle Follow Terrain")

# Rebindable key, shown under Options -> Controls.
keybinds = get_or_create("MC_TB_Keybinds", unreal.FGChildInputMappingContext)
keybinds.set_editor_property("m_display_name", "Follow Terrain")
keybinds.set_editor_property("m_parent_context", unreal.load_asset(PARENT_CONTEXT))
pressed = unreal.new_object(unreal.InputTriggerPressed, outer=keybinds)
# Alt + T. The chord uses the game's own Alt action, which its key rebinding recognises and
# replaces, so a player rebind doesn't keep a stray Alt requirement. Chorded mappings also win over
# plain T (Hide HUD) while Alt is held.
# ChordAction is EditInstanceOnly and Python treats anything inside the asset as a template, so set
# it on a transient trigger and then move that into the asset.
alt = unreal.new_object(unreal.InputTriggerChordAction)  # no outer: the transient package
alt.set_editor_property("chord_action", unreal.load_asset(CHORD_ALT))
if alt.get_editor_property("chord_action") is None:
    raise RuntimeError(f"Could not load {CHORD_ALT}")
if not alt.rename(outer=keybinds):
    raise RuntimeError("Could not move the Alt chord trigger into the keybind context")
keybinds.set_editor_property("mappings", [
    make_mapping(keybinds, toggle, "T", [pressed, alt], "TB.ToggleFollowTerrain", "Toggle Follow Terrain"),
])

for asset in (toggle, keybinds):
    if not lib.save_loaded_asset(asset, only_if_is_dirty=False):
        raise RuntimeError(f"Failed to save {asset.get_path_name()}")
    unreal.log(f"TB: saved {asset.get_path_name()}")
