def F(name, sig, desc, params=(), returns=None, **kw):
    d = dict(name=name, sig=sig, desc=desc, params=list(params), returns=returns)
    d.update(kw)
    return d


LEVELS = [(0, "TRACE"), (1, "DEBUG"), (2, "INFO"), (3, "WARNING"), (4, "ERROR"), (5, "ASSERT"), (6, "FATAL")]
CW = F("CreateWrapper", "object", "Returns its argument unchanged. Every Boost.Python module of the engine registers one; the engine uses it to hand its own objects to Python.",
       [("object", "object", "Any object.")], "The same object.", errors=[("TypeError", "Not exactly one argument.")])
NOINIT = "The class cannot be created from Python: its `__init__` raises RuntimeError \"This class cannot be instantiated from Python\"."

TRMAIN = dict(name="extern_trmain_trmain", desc="""The application module: start-up, shutdown, the game client object and the log. `trmain.py` (trpython.zip) imports it as `trmain` and drives the client's lifetime through it: InitApp, InitGameClient, then GetGameClient().Run().

Its functions are plain module functions (the module has no class).""", functions=[
    F("GetDataPaths", "", "Gets the paths of the loadable data modules (Python zips) named in `tabula_rasa.cfg`.", [],
      "A list of str: the value of every key of the `[Data]` section, in the order the engine's config table iterates them (its hash order - alphabetical, ignoring case, while the section has four keys or fewer). Stock: `['data/game.zip']`.",
      remarks="`clientbootstrap.py` puts these on `sys.path` before importing the game data, so a zip named ahead of `GameData` can override modules of `data/game.zip`.",
      engine="FUN_00405480 (loads tabula_rasa.cfg; iterates the section with FUN_00478ac0)."),
    F("GetGameClient", "", "Gets the current instance of GameClient.", [], "The [[extern_gameclient_gameclient.gameclient|GameClient]] object (the engine's single game client).",
      engine="0x00405620 (DAT_00d4fdf4)."),
    F("InitApp", "", "Initialises the game application.", [], "None.",
      remarks="Sets up the application before the game client: the log (`[Logging]` in tabula_rasa.cfg: `Enabled`, unless \"false\"; `File`, default `tabula_rasa.log`), the window and the renderer.",
      openrasa="The application is set up before the scripts run; the call does nothing.", engine="0x00404f20 -> FUN_00404ac0."),
    F("InitGameClient", "", "Initialises the game client.", [],
      "True when the game client started; otherwise False, after an error dialog showing the log's contents.",
      remarks="Builds the engine side of the client (the UI manager and its root window), then sets the UI script package `client.ui`, builds the `uievent` and `uianchor` modules, imports `client.main`, initialises the input state router and runs `client.main.Initialize()`, then loads the UI layout (`Tabula_Rasa_UI_EXPORT.xml`), whose windows import their scripts as they load. A script failure at any step makes it fail.",
      engine="FUN_00404d80 -> FUN_004cd8c0 / FUN_004d4c10."),
    F("OutputDebugString", "string", "Sends a string to the debugger for display.", [("string", "str", "The text.")], "None.",
      openrasa="Written to the standard error stream.", engine="0x00404f70 (Win32 OutputDebugString)."),
    F("PostQuitRequest", "", "Requests that the game loop end.", [], "None.",
      remarks="The loop in GameClient.Run returns at the end of the current frame.", engine="0x00404f50."),
    F("ShutdownApp", "", "Terminates the game application.", [], "None.",
      remarks="Shuts down and deletes the game client, then the application, and closes the log.",
      openrasa="Nothing: OpenRasa shuts down when the scripts return.", engine="0x00404f40 -> 0x00404e90."),
    F("_ShouldLog", "severity", "Queries whether a log of type severity should be logged.",
      [("severity", "int", "The severity level.", LEVELS)],
      "True if a call to [[extern_trmain_trmain._writelog|_WriteLog]] with the same severity would be written, otherwise False.",
      remarks="True when the log file exists (logging is enabled in tabula_rasa.cfg) and the level is at or above the threshold, which the release client leaves at 0 - every level is logged.",
      engine="FUN_00405070."),
    F("_WriteLog", "severity, format, args", "Writes an entry to the log file.",
      [("severity", "int", "The severity level.", LEVELS),
       ("format", "str or unicode", "The text, or a `%` format string when args is not None."),
       ("args", "object", "None, or the value(s) for `format % args`.")], "None.",
      remarks="""Nothing is written unless [[extern_trmain_trmain._shouldlog|_ShouldLog]](severity) is True. The text is `format` when args is None, else `format % args`; when that fails, `Format Error: <repr(format)> % <repr(args)>`. Unicode text is written as ASCII, or as its repr when it is not ASCII; a value that is not a string is written as `[Non-string object could not be translated to ASCII for logging]`.

The line is `LEVEL: text` with the level's name (`none` past 6). Levels 0 - 2 go to the log; 3 and above go through the engine's error handler, which writes the same line (and, for ASSERT / FATAL, may show its dialog).""",
      openrasa="Written to the standard error stream, not the log file; the error handler's dialog is not shown.",
      engine="FUN_004050b0 (log FUN_00705e70, error handler FUN_007066d0)."),
])

CONFIG = dict(name="extern_util_config", desc="""The engine's configuration files: the `Config` class reads and writes the `.cfg` format of `tabula_rasa.cfg`, `login.cfg` and `TRBookmarks.bmf`.

Unlike the other modules' classes, `Config` can be created from Python (`extern_util_config.Config()`); the engine itself makes two at start-up, returned by [[extern_gameclient_gameclient.gameclient.getappconfig|GetAppConfig]] and [[extern_gameclient_gameclient.gameclient.getloginconfig|GetLoginConfig]].""",
    functions=[CW], classes=[dict(name="Config", desc="""A configuration file in memory: sections of `key = value` items.

Section and key names are matched ignoring case and keep the spelling they were first given. Sections and their keys are kept in the engine's hash tables, so [[extern_util_config.config.getallsections|GetAllSections]] and [[extern_util_config.config.getallitems|GetAllItems]] list them in hash order; [[extern_util_config.config.exportfile|ExportFile]] writes them in a separate order (see there).""",
    remarks="""The file format ([[extern_util_config.config.importfile|ImportFile]]):

`[section]` starts a section; `[name = source]` starts one and copies the items of section `source` into it. `key = value` sets an item of the current section; the value may be a bare word, a quoted string (`"..."` or `` `...` ``, with backslash escapes, may span lines), a variable `$name` or `$"name"`, a reference `%section%key`, or nothing (an empty value). `#` and `;` start a comment to the end of the line.

Statements: `.include file` and `.includeoptional file` (at most 20 per Config), `.define name value` (a variable), `.section target = source` (copy a section's items), `.importenv NAME` (the variable NAME from the environment).

The first bad statement is logged (`In "file:line" problem with ...`) and ends the import as failed; what was read before it stays.""",
    methods=[
        F("ImportFile", "filename", "Reads a configuration file into this Config.", [("filename", "str", "The file's path.")], "None.",
          errors=[("RuntimeError", "\"Unable to import file: <filename>\" - the file cannot be opened or a statement is bad.")],
          remarks="Items already present are overwritten; the current section carries over from the previous import.", engine="FUN_00484560 -> FUN_00476160."),
        F("ExportFile", "filename", "Writes this Config to a file.", [("filename", "str", "The file's path.")], "None.",
          errors=[("RuntimeError", "\"Unable to export file: <filename>\".")],
          remarks="""Written in text mode (CR LF line ends): per section `[name]`, its `key = value` lines and a blank line. Sections are written ordered by name ignoring case, their keys in the order they were added. A name or value is written bare when every character is a letter, a digit or one of `.,_-@:/` and it does not start with `.`; otherwise it is quoted, with `\\` and `"` escaped. Items copied from another section (`.section`, `[a = b]`) and empty sections are not written.""",
          engine="FUN_004845d0 -> FUN_00476780."),
        F("GetItemAsString", "section, key, default", "Gets an item's value as a string.",
          [("section", "str", "The section."), ("key", "str", "The key."), ("default", "str", "Returned when the item does not exist.")],
          "The value, else default.", engine="0x00484640 -> FUN_004780e0."),
        F("GetItemAsInt", "section, key, default", "Gets an item's value as an integer.",
          [("section", "str", "The section."), ("key", "str", "The key."), ("default", "int", "Returned when the item does not exist.")],
          "The value read by `strtol` with base 0 (so `0x` hexadecimal and leading-zero octal are read), clamped to a 32-bit long; 0 for text that is not a number; default when the item does not exist.",
          engine="FUN_00484670."),
        F("GetItemAsBool", "section, key, default", "Gets an item's value as a boolean.",
          [("section", "str", "The section."), ("key", "str", "The key."), ("default", "bool", "Returned when the item does not exist.")],
          "True when the value, lower-cased, is `true`, `yes`, `on` or `1`; otherwise False; default when the item does not exist.",
          engine="FUN_00478160."),
        F("GetItemAsFloat", "section, key, default", "Gets an item's value as a float.",
          [("section", "str", "The section."), ("key", "str", "The key."), ("default", "float", "Returned when the item does not exist.")],
          "The value read by `strtod` as a single-precision float; default when the item does not exist.", engine="FUN_004846f0."),
        F("HasItem", "section, key", "Queries whether an item exists.", [("section", "str", "The section."), ("key", "str", "The key.")],
          "True when the section has the key."),
        F("SetItem", "section, key, value", "Sets an item's value, adding the section and the item when needed.",
          [("section", "str", "The section."), ("key", "str", "The key."), ("value", "str", "The value.")], "None.",
          remarks="A new item also joins the export order ([[extern_util_config.config.exportfile|ExportFile]]).", engine="FUN_00478430."),
        F("SetItemFromInt", "section, key, value", "Sets an item to an integer, written as `%d`.",
          [("section", "str", "The section."), ("key", "str", "The key."), ("value", "int", "The value.")], "None."),
        F("SetItemFromBool", "section, key, value", "Sets an item to `true` or `false`.",
          [("section", "str", "The section."), ("key", "str", "The key."), ("value", "bool", "The value.")], "None."),
        F("SetItemFromFloat", "section, key, value", "Sets an item to a float, written as `%g` of the single-precision value.",
          [("section", "str", "The section."), ("key", "str", "The key."), ("value", "float", "The value.")], "None.",
          remarks="The client's C runtime writes exponents with three digits (`1e+006`).", engine="FUN_00484830."),
        F("GetAllSections", "", "Gets the names of all sections.", [], "A list of str, in the config's hash order.", engine="FUN_00484880."),
        F("GetAllItems", "section", "Gets the keys of a section.", [("section", "str", "The section.")],
          "A list of str in hash order; an empty list for a section that does not exist.", engine="FUN_00484940."),
    ])])

STATEMACHINE = dict(name="extern_game_characterstatemachine", desc="A character's posture state, held by its movement controller ([[extern_gameclient_clientmovingentitycontroller.clientmovingentitycontroller|ClientMovingEntityController]]`.characterstatemachine`).",
    functions=[CW], classes=[dict(name="CharacterStateMachine", desc="The posture state of a moving entity: standing, sitting, lying down, swimming, crouched, flying or flailing. " + NOINIT,
    remarks="Each Enter method stores its state number (0 - 6) and nothing else; nothing in the engine was found to read it (UNVERIFIED).",
    methods=[F(n, "", f"Puts the character into the {w} state.", [], "None.", engine=f"0x{0x00473f80 + 0x10 * i:08x} (stores {i}).")
             for i, (n, w) in enumerate([("EnterStanding", "standing"), ("EnterSitting", "sitting"), ("EnterLyingDown", "lying down"),
                                         ("EnterSwimming", "swimming"), ("EnterCrouched", "crouched"), ("EnterFlying", "flying"),
                                         ("EnterFlailing", "flailing")])])])

MOVING = dict(name="extern_gameclient_clientmovingentitycontroller", desc="The movement controller of an entity body, made by [[extern_gameclient_entitybody.entitybody.registerasmovingentity|EntityBody.RegisterAsMovingEntity]] and returned by the body's `movement` property.",
    functions=[CW], classes=[
    dict(name="MovingEntityController", desc="The base movement controller. " + NOINIT, properties=[("characterstatemachine", "The controller's [[extern_game_characterstatemachine.characterstatemachine|CharacterStateMachine]], or None.")],
         methods=[
        F("Jump", "", "Makes the controlled character jump.", [], "None.",
          remarks="Only for the player's own body (the local control state) with no movement block requested ([[extern_gameclient_usercontroller.usercontroller.requestcharactermovementblock|RequestCharacterMovementBlock]]); otherwise nothing.",
          engine="0x0051d080 -> FUN_00518e20."),
        F("SyncYawToFacing", "", "Turns the body to its facing.", [], "None.",
          remarks="The current control state turns the body towards the facing's yaw at its turn rate (the player's and server-driven bodies queue a turn); without a state the yaw is set at once.",
          engine="FUN_0051d040."),
    ]),
    dict(name="ClientMovingEntityController", desc="The client's movement controller: it moves the player's body from input and server-driven bodies from the server's movement messages, with their facing and turning. " + NOINIT,
         bases="[[extern_gameclient_clientmovingentitycontroller.movingentitycontroller|MovingEntityController]]",
         properties=[("characterstatemachine", "Inherited: the controller's CharacterStateMachine, or None.")],
         methods=[F("InMotion", "", "Queries whether the controlled body is moving.", [],
                    "True when its speed is above 0, or its control state is in motion: the player's when in the air or a jump is asked for; a server-driven body's while falling or jumping; rushing and sliding always; a knockback until it ends.",
                    engine="0x0051d0a0.")])])

OPTION = dict(name="extern_gameclient_option_wrap", desc="The nodes of the options tree ([[extern_gameclient_optionsmanager_wrap.optionsmanager|OptionsManager]]).",
    functions=[CW], classes=[dict(name="Option", desc="""One node of the options tree: a name, a value, a default, a persistence flag and child options. The manager's options are addressed by path (`Client.Graphics.ViewDistance`); each part is a node. """ + NOINIT,
    remarks="Values are text; the manager's typed getters and setters convert. Get / Set methods here change the node directly and do not notify the option observers (unlike [[extern_gameclient_optionsmanager_wrap.optionsmanager.setoptionstring|OptionsManager.SetOption...]]).",
    methods=[
        F("GetName", "", "Gets the option's name (its last path part).", [], "The name as str.", engine="0x004efeb0."),
        F("SetName", "name", "Sets the option's name.", [("name", "str", "The new name.")], "None."),
        F("GetValue", "", "Gets the option's value.", [], "The value as str.", engine="0x004eff50."),
        F("SetValue", "value", "Sets the option's value.", [("value", "str", "The value.")], "None.",
          remarks="The option observers are not told; use OptionsManager.SetOption... for that."),
        F("GetDefault", "", "Gets the option's default value.", [], "The default as str.", engine="0x004efff0."),
        F("SetDefault", "value", "Sets the option's default value.", [("value", "str", "The default.")], "None."),
        F("IsPersistant", "", "Queries whether the option is saved (to client.cfg or the server).", [], "True or False.", engine="0x004f0090."),
        F("SetPersistant", "flag", "Sets whether the option is saved.", [("flag", "bool", "True to save it.")], "None.", engine="0x004f0080."),
        F("IsName", "name", "Compares the option's name, ignoring case.", [("name", "str", "The name to compare.")], "True when equal.", engine="0x004f0120."),
        F("IsPath", "path", "Compares the option's full path, ignoring case.", [("path", "str", "A dotted path.")], "True when equal.", engine="0x004f0720."),
        F("GetPath", "", "Gets the option's full path.", [], "The names of its ancestors below the root and its own, joined with `.`.", engine="0x004f0670."),
        F("GetParent", "", "Gets the option's parent.", [], "The parent Option, or None for the root.", engine="0x004f0590."),
        F("GetChildren", "", "Gets the option's children.", [], "A list of Option, in the order they were added.", engine="0x004f0490."),
        F("GetChild", "name", "Gets a child by name (ignoring case).", [("name", "str", "The child's name.")], "The child Option, or None.", engine="0x004f03b0."),
        F("AddChild", "name, value, persistent", "Adds a child option, or returns the existing one of that name.",
          [("name", "str", "The child's name."), ("value", "str", "Its value and default."), ("persistent", "bool", "Whether it is saved.")],
          "The child Option (an existing child is returned unchanged).", engine="0x004f01d0 -> FUN_004ef900."),
        F("RemoveChild", "name", "Removes and deletes the first child of that name.", [("name", "str", "The child's name.")],
          "True when a child was removed, else False.", engine="0x004f0290."),
        F("RemoveChildren", "", "Removes and deletes every child.", [], "True.", engine="0x004f0320."),
    ])])

def opt_get(t, ptype, conv, addr):
    return F(f"GetOption{t}", "name, default", f"Gets an option's value as {ptype}.",
             [("name", "str", "The option's dotted path."), ("default", ptype, "Returned when the option does not exist.")],
             conv, engine=addr)

OM_DESC_SET = "The option is created when missing (with its missing parents, which are made persistent); when its value changes, the option observers are told (the engine applies graphics, audio and control settings this way)."
OPTIONSMANAGER = dict(name="extern_gameclient_optionsmanager_wrap", desc="The options tree: client, user and character settings.",
    functions=[CW], classes=[dict(name="OptionsManager", desc="""The game client's options: a tree of [[extern_gameclient_option_wrap.option|Option]] nodes addressed by dotted paths (`Client.Graphics.ViewDistance`, `User.Camera.DesiredDistance`, `Character....`). Returned by [[extern_gameclient_gameclient.gameclient.getoptionsmanager|GameClient.GetOptionsManager]]. """ + NOINIT,
    remarks="""Paths are split at `.`, `/` and `\\`; empty parts are skipped and parts are matched ignoring case. Values are stored as text: booleans as `True` / `False`, numbers written with the "usa" stream locale (grouped digits, `%g`, three-digit exponents).

Persistent `Client.*` options are saved to client.cfg, `User.*` and `Character.*` ones to the server ([[extern_gameclient_optionsmanager_wrap.optionsmanager.endediting|EndEditing]]).""",
    methods=[
        *[F(f"AddOption{t}", "name, value, persistent", f"Adds an option with a {d} value, or returns the existing one.",
            [("name", "str", "The option's dotted path."), ("value", pt, "Its value and default."), ("persistent", "bool", "Whether it is saved.")],
            "The [[extern_gameclient_option_wrap.option|Option]]. An option that exists is returned unchanged; a new one gets the value as both value and default (missing parents are created, persistent) and the observers are told it was added.",
            engine=a)
          for t, d, pt, a in [("String", "string", "str", "FUN_00494970"), ("Unicode", "unicode", "unicode", "FUN_00494a60"),
                              ("Int", "integer", "int", "FUN_00494b80"), ("Float", "float", "float", "FUN_00494ca0"), ("Bool", "boolean", "bool", "FUN_00494dc0")]],
        *[F(f"SetOption{t}", "name, value", f"Sets an option to a {d} value.", [("name", "str", "The option's dotted path."), ("value", pt, "The value.")],
            "None.", remarks=OM_DESC_SET + " " + note, engine="FUN_00494270")
          for t, d, pt, note in [("String", "string", "str", ""), ("Unicode", "unicode", "unicode", "Characters above 255 are stored as `@` (the engine narrows by its C locale)."),
                                 ("Int", "integer", "int", "Written by the \"usa\" locale's stream (grouped thousands)."),
                                 ("Float", "float", "float", "Written by the \"usa\" locale's stream as `%g`."), ("Bool", "boolean", "bool", "Written as `True` or `False`.")]],
        opt_get("String", "str", "The value as it is stored, else default.", "FUN_00495200"),
        opt_get("Unicode", "unicode", "The value widened to unicode (Latin-1), else default.", "FUN_004952e0"),
        opt_get("Int", "int", "The value read as a number by the \"usa\" stream; default when the option does not exist or its text is not a number.", "FUN_00495430"),
        opt_get("Float", "float", "The value read as a number by the \"usa\" stream; default when the option does not exist or its text is not a number.", "FUN_004954e0"),
        opt_get("Bool", "bool", "True when the value is `True`, `Yes`, `On` or `1` (ignoring case), else False; default when the option does not exist.", "FUN_00495590"),
        F("RemoveOption", "name", "Removes an option and its children.", [("name", "str", "The option's dotted path.")],
          "True when it was removed; False when there is no such option (logged as `RemoveOption failed: Option \"name\" not registered.`).",
          remarks="The observers are told before it goes.", engine="0x00495650 -> FUN_00494540."),
        F("RestoreDefault", "name", "Sets an option back to its default.", [("name", "str", "The option's dotted path.")],
          "True; False when there is no such option (logged).", remarks="The observers are told when the value changes.", engine="0x00495640 -> FUN_00494460."),
        F("ForceOptionChangeEvent", "text", "Tells the observers about every option whose path contains the text, as if it had changed.",
          [("text", "str", "Compared with each option's path, both lower-cased; an empty text matches every option.")], "None.",
          engine="0x00495660 -> FUN_004946d0."),
        F("BeginEditing", "", "Brings the options up to date with the engine's current settings before an options window shows them.", [], "None.", engine="Observer slot 4."),
        F("UpdateFromEngine", "", "The same as [[extern_gameclient_optionsmanager_wrap.optionsmanager.beginediting|BeginEditing]].", [], "None.", engine="Observer slot 4."),
        F("EndEditing", "", "Saves the persistent options.", [], "None.",
          remarks="`User.*` options are sent to the server with `clientmethod.SaveUserOptions(names)`, `Character.*` ones with `SaveCharacterOptions(names)`, and `Client.*` ones are written to client.cfg (`[ConfigInfo] Version 2`, `[Options]`).",
          engine="Observer slot 6, FUN_005360c0."),
        F("Export", "", "The same as [[extern_gameclient_optionsmanager_wrap.optionsmanager.endediting|EndEditing]].", [], "None.", engine="Observer slot 6."),
    ])])

MODULES = [TRMAIN, CONFIG, STATEMACHINE, MOVING, OPTION, OPTIONSMANAGER]
