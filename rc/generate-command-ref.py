"""
Generate x64dbg-command-ref.txt from the x64dbg source documentation.

Usage:
    python generate-command-ref.py [x64dbg_source_dir]

Defaults to C:\code\x64dbg if no argument given.
Output: rc/x64dbg-command-ref.txt (same directory as this script).

This concatenates the introduction docs (Values, Expressions, Expression-functions,
Formatting, Variables) followed by all command docs organized by category.
The output is embedded as a Win32 resource (IDR_COMMAND_REF) in the plugin
and used by the get_command_help tool.
"""

import os
import sys
import pathlib

def main():
    x64dbg_dir = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(r"C:\code\x64dbg")
    docs_dir = x64dbg_dir / "docs"
    commands_dir = docs_dir / "commands"
    intro_dir = docs_dir / "introduction"

    if not commands_dir.exists():
        print(f"ERROR: {commands_dir} not found", file=sys.stderr)
        sys.exit(1)

    output = []

    # Introduction/reference docs first
    for name in ["Values.md", "Expressions.md", "Expression-functions.md", "Formatting.md", "Variables.md"]:
        p = intro_dir / name
        if p.exists():
            output.append(f"# REFERENCE: {name.replace('.md', '')}")
            output.append(p.read_text(encoding="utf-8").strip())
            output.append("")

    # All command docs by category
    for category in sorted(os.listdir(commands_dir)):
        cat_path = commands_dir / category
        if not cat_path.is_dir():
            continue
        for md in sorted(cat_path.glob("*.md")):
            content = md.read_text(encoding="utf-8").strip()
            output.append(content)
            output.append("")

    result = "\n".join(output)
    out_path = pathlib.Path(__file__).parent / "x64dbg-command-ref.txt"
    out_path.write_text(result, encoding="utf-8")
    print(f"Written {len(result)} bytes to {out_path}")

if __name__ == "__main__":
    main()
