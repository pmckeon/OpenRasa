from spec_a import F, CW, NOINIT

B = lambda name, desc, sig="", params=(), ret="None.", **kw: F(name, sig, desc, params, ret, **kw)
P3 = ("tuple", "(x, y, z).")

WORLD = [
    B("AddTerrain", "Loads the map's terrain.", "templateId, terrainId, mapName", [("templateId", "int", ""), ("terrainId", "int", ""), ("mapName", "str", "")],
      errors=[("RuntimeError", "\"Unable to initialize terrain!\"")], remarks="The old terrain is removed first; the terrain's visualizer and clutter are made.", engine="FUN_00546360."),
    B("SetTerrainVisSettings", "Sets the terrain's draw distances.", "detail, algorithm, maxDist, fadeDist",
      [("detail", "int", ""), ("algorithm", "int", ""), ("maxDist", "float", "Either distance negative = the defaults 350 / 300."), ("fadeDist", "float", "")],
      remarks="The terrain distance also limits the view's far plane (100 - 500 m) and the fog.", engine="FUN_00546530."),
    B("ClearWalkableSurfaces", "Removes every walkable surface and the terrain.", engine="0x00544570."),
    B("LoadVisFile", "Loads the map's visibility (.vis PVS) file.", "filename", [("filename", "str", "")],
      remarks="gamemap.py builds the name from the cfg's Maps path including its `|.map` part, so no shipped .vis is ever found; the engine then builds an empty PVS in which everything is visible.", engine="FUN_00546600."),
    B("SetWalkableSurfaceFlags", "Records flags for an entity's walkable surface.", "entityId, wsName, flags", [("entityId", "long", ""), ("wsName", "str", ""), ("flags", "int", "")],
      remarks="Stored but never read by the engine.", engine="FUN_005444f0."),
    B("AddCollisionBody", "Puts a body (back) in the collision manager.", "body", [("body", "EntityBody", "")], remarks="Only for a body in the world that does not ignore ABVs (door.py on closing).", engine="0x00543d80."),
    B("RemoveCollisionBody", "Takes a body out of the collision manager.", "body", [("body", "EntityBody", "")], engine="0x00543dc0."),
    B("HasCollisionBody", "Queries whether a body is in the collision manager.", "body", [("body", "EntityBody", "")], "True or False.", engine="0x00543db0."),
    B("InitWaterBody", "Marks a body as water.", "body", [("body", "EntityBody", "")], remarks="Sets a flag nothing was found to read.", engine="0x00546680."),
    B("GetPickLocation", "Casts a ray against the terrain and pickable bodies.", "origin, direction, length",
      [("origin",) + P3, ("direction", "tuple", "(x, y, z), normalised by the call."), ("length", "float", "")], "(x, y, z): the first hit, else the ray's end.",
      engine="FUN_005447d0."),
    B("GetIntersection", "Casts a segment against the collision manager's bodies (ABVs, doors).", "start, end", [("start",) + P3, ("end",) + P3],
      "(x, y, z): the hit, else end.", remarks="Walkable surfaces and the terrain are not tested.", engine="FUN_00544d80."),
    B("InLineOfSight", "Queries whether two bodies can see each other.", "body0, body1", [("body0", "EntityBody", ""), ("body1", "EntityBody", "")],
      "True when at least one of five rays between them (mid, top and low points of each) is clear of collision bodies, walkable surfaces and terrain.", engine="0x00544590."),
    B("GetEntityIdInLOS", "Gets what blocks the line of sight between two bodies.", "body0, body1", [("body0", "EntityBody", ""), ("body1", "EntityBody", "")],
      "0 when in sight; else what blocked the fifth ray: a body's or walkable surface's entity id, or 1 for the terrain.", engine="0x005445c0."),
    B("AddCameraScript", "Adds a camera script (gamemap.py, from the .map).", "scriptId, returnMs, keyframes",
      [("scriptId", "int", ""), ("returnMs", "int", "Milliseconds to return to the starting pose after the last key."),
       ("keyframes", "list", "Tuples (x, y, z, rw, rx, ry, rz, focalLength, arriveMs, holdMs).")],
      errors=[("RuntimeError", "\"Argument 3 must be a list\" / \"Each item in the list must be a tuple of size 10. Received size=n\".")],
      remarks="An id already used is not replaced. Played by [[extern_gameclient_usercontroller.usercontroller.runcamerascript|UserController.RunCameraScript]].", engine="FUN_005458f0."),
    B("ClearCameraScripts", "Deletes every camera script.", engine="0x00545820."),
    B("CombineObjects", "Merges static bodies into one (Client.Developer.CombineObjects only).", "glomEntityId, items",
      [("glomEntityId", "long", ""), ("items", "list", "Tuples of size 2, (entity id, ...).")],
      errors=[("RuntimeError", "\"Each item in the list must be a tuple of size 2. Received size=n\".")], engine="FUN_00545310.", openrasa="Checks the list only."),
    B("SetTrackingTarget", "Does nothing (an empty function in the client).", "a, b", [("a", "object", ""), ("b", "object", "")], engine="0x005452e0."),
    B("SetDimension", "Sets which dimension the world shows.", "dimension", [("dimension", "int", "")], engine="0x005452f0."),
    B("ClientCollisionIsVisible", "Queries the client collision debug view.", ret="False (release build).", engine="0x00544610."),
    B("ServerCollisionIsVisible", "Queries the server collision debug view.", ret="False (release build).", engine="0x00544620."),
    B("WalkableSurfacesAreVisible", "Queries the walkable surface debug view.", ret="False (release build).", engine="0x00544640."),
    B("SetWalkableSurfacesVisible", "Does nothing (release build).", "flag", [("flag", "bool", "")], engine="0x00544630."),
    B("SetCollisionVisible", "Does nothing (release build).", "client, server", [("client", "bool", ""), ("server", "bool", "")], engine="0x00544600."),
]

WORLDMOD = dict(name="extern_gameclient_world", desc="The game world: terrain, collision, line of sight, picking and camera scripts.",
    functions=[B("GetSingleton", "Gets the world.", ret="The [[extern_gameclient_world.world|World]].", engine="0x004db9a0."), CW],
    enums=[dict(name="PickResult", desc="What a pick hit.", values=[("PickNothing", 0, ""), ("PickTerrain", 1, ""), ("PickBody", 2, "")], remarks="No method takes or returns it.")],
    classes=[dict(name="World", desc="The game world. There is one ([[extern_gameclient_world.getsingleton|GetSingleton]]). " + NOINIT, methods=sorted(WORLD, key=lambda m: m["name"].lower()))])
MODULES = [WORLDMOD]

NOMAP = "Used only by .map formats 2.12 - 2.37 (gamemap.py _LoadSky3 / _LoadSky4 / _LoadDirLights1); no shipped map uses them."
WEATHER = dict(name="WeatherManager", desc="The sky clock and the scripted sky, returned by [[extern_gameclient_gameclient.gameclient.getweathermanager|GameClient.GetWeatherManager]] (always the same object).",
    remarks="A plain C object (PyCPPObject), every method taking positional arguments. Current maps take their skies from the regions' .sky files instead of the scripted-sky methods. " + NOMAP, methods=[
    B("SetLengthOfDay", "Sets the length of a day.", "ms", [("ms", "int", "Milliseconds.")]),
    B("SetCurrentDate", "Sets the sky clock.", "day, time", [("day", "int", ""), ("time", "float", "The time of day, 0 - 1.")]),
    B("CreateSkyLayer", "Creates a sky layer.", "layer, mesh, manualAnim, byRotation, rotsPerDay, x, y, z, w, qx, qy, qz",
      [("layer", "int", "The layer id."), ("mesh", "str", "The model ('' for none)."), ("manualAnim", "bool", ""), ("byRotation", "bool", ""), ("rotsPerDay", "float", "Turns a day."),
       ("x, y, z", "float", "The direction."), ("w, qx, qy, qz", "float", "The rotation.")],
      errors=[("RuntimeError", "\"failed to create sky layer: mesh\" - the id exists or the model cannot be made.")], engine="0x00530e80 -> FUN_00530900."),
    B("SetSkyLayer", "Sets a sky layer's model.", "layer, index, mesh", [("layer", "int", ""), ("index", "int", ""), ("mesh", "str", "")],
      errors=[("RuntimeError", "\"failed to create sky layer body: mesh\".")], remarks="In the client its third argument is never stored (undefined behaviour); nothing calls it.", engine="0x00530aa0."),
    B("CreateSkyLight", "Creates a directional light on a sky layer.", "layer, lensFlare, x, y, z", [("layer", "int", ""), ("lensFlare", "bool", ""), ("x, y, z", "float", "The direction.")],
      "The light's id: (layer + 1) x 0x10000 + the first free number.", errors=[("RuntimeError", "\"failed to create light for layer n\".")], engine="FUN_00823620."),
    B("CreateGlobalLight", "Creates a global light.", "lensFlare, x, y, z, hemi", [("lensFlare", "bool", ""), ("x, y, z", "float", ""), ("hemi", "bool", "A hemispheric light (else directional).")],
      "The light's id (layer -1).", errors=[("RuntimeError", "\"failed to create global light\".")]),
    B("AddLightColorKey", "Adds a colour key to a light.", "layer, light, time, aI, dI, sI, ambient, diffuse, specular, flag",
      [("layer", "int", ""), ("light", "int", "The light's id."), ("time", "float", "0 - 1."), ("aI", "float", "Not used."), ("dI", "float", "The intensity."), ("sI", "float", "Not used."),
       ("ambient", "int", "Not used."), ("diffuse", "int", "0xRRGGBB."), ("specular", "int", "Not used."), ("flag", "int", "For a hemispheric light 128 or more = the bottom colour.")],
      remarks="The key's colour is dI x diffuse / 255."),
    B("AddLayerAlphaKey", "Adds an alpha key to a sky layer.", "layer, time, alpha", [("layer", "int", ""), ("time", "float", ""), ("alpha", "float", "")]),
    B("AddLayerOffsetKey", "Adds an offset key to a sky layer.", "layer, time, x, y, z", [("layer", "int", ""), ("time", "float", ""), ("x, y, z", "float", "")]),
    B("AddFogKey", "Adds a fog key.", "time, distance, r, g, b", [("time", "float", ""), ("distance", "float", ""), ("r, g, b", "int", "0 - 255.")]),
    B("EnableFogOverride", "Overrides the fog with one ending at a distance.", "distance", [("distance", "float", "")], engine="FUN_00803720."),
    B("DisableFogOverride", "Ends the fog override."),
    B("Reset", "Removes every sky layer and light.", engine="0x00530ca0 -> FUN_00822790."),
])
REGION = dict(name="RegionManager", desc="The map regions' features (sky, environment map, sounds, music, name), returned by [[extern_gameclient_gameclient.gameclient.getregionmanager|GameClient.GetRegionManager]] (a new object each call).",
    methods=[
    B("Transition", "Applies the regions the player is in.", "initial, regions",
      [("initial", "bool", ""), ("regions", "list", "regionmanager.py's region tuples (priority, enabledFlags, ambient, music, environment map, sky, display name id), sorted by priority.")],
      remarks="Per enabledFlags bit the first region with it supplies that feature: bit 0 sky, 1 environment map, 2 ambient sound, 3 music, 4 the shown name (which calls client.gameui.DisplayRegionName). An empty list changes nothing.",
      engine="0x0052c060 -> FUN_0077d150."),
    B("SetTransSky", "Sets whether region skies use the transition sky.", "flag", [("flag", "int", "gamemap.py passes 0.")], engine="0x0052c230."),
    B("Reset", "Resets the region state.", engine="0x0052bff0."),
])
LIGHT = dict(name="LightManager", desc="The scripted light manager, returned by [[extern_gameclient_gameclient.gameclient.getlightmanager|GameClient.GetLightManager]].",
    remarks="Every method returns None and does nothing in the release client - decoration.py's apartment lights fail on it in the game too.",
    methods=[B(n, "Does nothing (returns None).", "*args", [("args", "object", "Any arguments.")]) for n in
             ["CreatePointLight", "CreateDirLight", "AddLight", "RemoveLight", "RemoveAllLights", "AttachLightToBody", "SnapLightToBody"]])
OBJECTS = [WEATHER, REGION, LIGHT]
