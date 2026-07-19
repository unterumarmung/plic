load("@aspect_rules_lint//lint:clang_tidy.bzl", "lint_clang_tidy_aspect")
load("@aspect_rules_lint//lint:lint_test.bzl", "lint_test")

clang_tidy = lint_clang_tidy_aspect(
    binary = Label("//tools/quality:clang_tidy"),
    configs = [
        Label("//:.clang-tidy"),
        Label("//compiler/syntax:.clang-tidy"),
    ],
    header_filter = "(^|/)dola/(compiler|runtime|tools|test)/.*\\.(h|hpp)$",
    lint_target_headers = False,
    rule_kinds = ["cc_binary", "cc_library", "cc_test"],
)

clang_tidy_test = lint_test(aspect = clang_tidy)
