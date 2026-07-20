import re
import sys
from pathlib import Path


PATTERN = re.compile(
    r'DOLA_CODEGEN_OPERATION\((\w+),\s*(\w+),\s*(\d+),\s*"([^"]+)",'
)


def main() -> None:
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    operations = [match.groups() for match in PATTERN.finditer(source)]
    if not operations:
        raise RuntimeError("codegen operation registry is empty")
    values = [int(operation[2]) for operation in operations]
    if values != list(range(len(values))):
        raise RuntimeError("codegen operation values must be contiguous and ordered")

    lines = [
        "#[repr(u32)]",
        "#[derive(Clone, Copy, Debug, Eq, PartialEq)]",
        "enum RuntimeOperation {",
    ]
    lines.extend(f"    {name} = {value}," for name, _, value, _ in operations)
    lines.extend(
        [
            "}",
            "",
            "impl TryFrom<u32> for RuntimeOperation {",
            "    type Error = ();",
            "",
            "    fn try_from(value: u32) -> Result<Self, Self::Error> {",
            "        match value {",
        ]
    )
    lines.extend(
        f"            {value} => Ok(Self::{name}),"
        for name, _, value, _ in operations
    )
    lines.extend(
        [
            "            _ => Err(()),",
            "        }",
            "    }",
            "}",
            "",
        ]
    )
    Path(sys.argv[2]).write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
