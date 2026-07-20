import os
import tempfile

import lit.formats

config.name = "Dola"
config.suffixes = [".dola", ".mlir"]
config.test_format = lit.formats.ShTest()
config.test_exec_root = (
    os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR")
    or os.environ.get("TEST_TMPDIR")
    or os.path.join(tempfile.gettempdir(), "dola-lit")
)
