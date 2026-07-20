load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

DolaInfo = provider(
    doc = "Transitive Dola sources supplied to a whole-program compiler invocation.",
    fields = {"transitive_sources": "depset of declared .dola files"},
)

def _dola_library_impl(ctx):
    transitive = [dep[DolaInfo].transitive_sources for dep in ctx.attr.deps]
    sources = depset(ctx.files.srcs, transitive = transitive)
    return [DefaultInfo(files = sources), DolaInfo(transitive_sources = sources)]

_dola_library = rule(
    implementation = _dola_library_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = [".dola"]),
        "deps": attr.label_list(providers = [DolaInfo]),
    },
)

def dola_library(name, srcs, deps = [], visibility = None, **kwargs):
    _dola_library(name = name, srcs = srcs, deps = deps, visibility = visibility, **kwargs)

def _compile_object(name, srcs, deps, testonly = False):
    native.genrule(
        name = name + "__object",
        srcs = srcs + deps,
        outs = [name + ".dola.o"],
        cmd = "$(location //tools/dola) --emit-object -o $@ $(SRCS)",
        tools = ["//tools/dola"],
        testonly = testonly,
    )
    return ":" + name + "__object"

def dola_binary(name, srcs, deps = [], visibility = None, **kwargs):
    cc_binary(
        name = name,
        srcs = [_compile_object(name, srcs, deps)],
        deps = ["//runtime:runtime"],
        visibility = visibility,
        **kwargs
    )

def dola_test(name, srcs, deps = [], **kwargs):
    cc_test(
        name = name,
        srcs = [_compile_object(name, srcs, deps, testonly = True)],
        deps = ["//runtime:runtime"],
        **kwargs
    )
