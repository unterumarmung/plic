"""Private declared-input compiler test used by Bazel integration coverage."""

load("//bazel:dola_rules.bzl", "DolaInfo")

def _shell_quote(value):
    return "'" + value.replace("'", "'\\''") + "'"

def _dola_compile_failure_test_impl(ctx):
    transitive = [dep[DolaInfo].transitive_sources for dep in ctx.attr.deps]
    sources = depset(ctx.files.srcs, transitive = transitive).to_list()
    compiler = ctx.executable.compiler
    output = ctx.actions.declare_file(ctx.label.name + ".sh")
    command = " ".join(
        [_shell_quote(compiler.short_path), "--check"] +
        [_shell_quote(source.short_path) for source in sources]
    )
    if ctx.attr.should_fail:
        body = """#!/bin/sh
set -eu
output="$TEST_TMPDIR/compiler.txt"
if {command} >"$output" 2>&1; then
  echo "expected Dola compilation to fail" >&2
  exit 1
fi
grep -F {expected} "$output" >/dev/null
""".format(command = command, expected = _shell_quote(ctx.attr.expected))
    else:
        body = "#!/bin/sh\nset -eu\n%s\n" % command
    ctx.actions.write(output, body, is_executable = True)
    runfiles = ctx.runfiles(files = sources + [compiler])
    runfiles = runfiles.merge(ctx.attr.compiler[DefaultInfo].default_runfiles)
    return [DefaultInfo(executable = output, runfiles = runfiles)]

dola_compile_failure_test = rule(
    implementation = _dola_compile_failure_test_impl,
    test = True,
    attrs = {
        "srcs": attr.label_list(allow_files = [".dola"]),
        "deps": attr.label_list(providers = [DolaInfo]),
        "expected": attr.string(),
        "should_fail": attr.bool(default = True),
        "compiler": attr.label(
            default = "//tools/dola",
            executable = True,
            cfg = "exec",
        ),
    },
)
