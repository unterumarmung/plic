"""Hermetic LLVM lit integration modeled after OpenXLA's xla/lit.bzl."""

load("@rules_shell//shell:sh_test.bzl", "sh_test")

def _tools_on_path_impl(ctx):
    symlinks = {}
    runfiles = ctx.runfiles()
    for tool in ctx.attr.tools:
        executable = tool[DefaultInfo].files_to_run.executable
        if not executable:
            fail("lit tool {} does not provide an executable".format(tool.label))
        path = ctx.attr.bin_dir + "/" + executable.basename
        if path in symlinks:
            fail("lit tools must have unique basenames: {}".format(executable.basename))
        symlinks[path] = executable
        runfiles = runfiles.merge(tool[DefaultInfo].default_runfiles)
    return [DefaultInfo(runfiles = ctx.runfiles(symlinks = symlinks).merge(runfiles))]

_tools_on_path = rule(
    implementation = _tools_on_path_impl,
    attrs = {
        "bin_dir": attr.string(mandatory = True),
        "tools": attr.label_list(cfg = "exec", allow_files = True, mandatory = True),
    },
)

def lit_test(name, test_file, cfg, tools = None, args = None, data = None, env = None, **kwargs):
    """Runs one source file with lit and tools staged on lit's search path."""
    tools = tools or []
    args = args or []
    data = data or []
    env = env or {}
    internal = "_{}_tools_on_path".format(name.replace("/", "_").replace(".", "_"))
    bin_dir = native.package_name() + "/" + internal + "/lit_bin"
    _tools_on_path(
        name = internal,
        tools = tools,
        bin_dir = bin_dir,
        testonly = True,
    )
    test_env = {"FILECHECK_OPTS": "--enable-var-scope"}
    test_env.update(env)
    sh_test(
        name = name,
        srcs = ["//bazel:lit_runner.sh"],
        args = [
            "$(location @llvm-project//llvm:lit)",
            "-a",
            "--path",
            bin_dir,
            "$(location {})".format(test_file),
        ] + args,
        data = [
            cfg,
            test_file,
            internal,
            "@llvm-project//llvm:lit",
        ] + data,
        env = test_env,
        **kwargs
    )

def lit_test_suite(name, srcs, cfg, tools = None, args = None, data = None, **kwargs):
    """Creates one lit test per source and a Bazel test_suite containing them."""
    tools = tools or []
    args = args or []
    data = data or []
    tests = []
    for source in srcs:
        test_name = source.replace("/", "_") + ".test"
        tests.append(test_name)
        lit_test(
            name = test_name,
            test_file = source,
            cfg = cfg,
            tools = tools,
            args = args,
            data = data,
            **kwargs
        )
    native.test_suite(name = name, tests = tests)
