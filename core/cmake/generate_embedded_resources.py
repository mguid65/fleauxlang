from __future__ import annotations

import argparse
from pathlib import Path


def parse_resource_mapping(raw: str) -> tuple[str, Path]:
    if "=" not in raw:
        raise argparse.ArgumentTypeError(f"invalid resource mapping '{raw}'; expected <virtual_name>=<source_path>")
    virtual_name, source_path = raw.split("=", 1)
    virtual_name = virtual_name.strip()
    source = Path(source_path.strip())
    if not virtual_name:
        raise argparse.ArgumentTypeError(f"invalid resource mapping '{raw}'; virtual name must not be empty")
    if source_path.strip() == "":
        raise argparse.ArgumentTypeError(f"invalid resource mapping '{raw}'; source path must not be empty")
    return virtual_name, source


def format_byte_array(data: bytes) -> str:
    if not data:
        return ""

    chunks: list[str] = []
    line: list[str] = []
    for index, byte in enumerate(data, start=1):
        line.append(f"std::byte{{0x{byte:02x}}}")
        if index % 12 == 0:
            chunks.append(", ".join(line))
            line = []
    if line:
        chunks.append(", ".join(line))
    return ",\n        ".join(chunks)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate embedded resource C++ source")
    parser.add_argument("--output", required=True, help="Output .cpp path")
    parser.add_argument(
        "--resource",
        dest="resources",
        action="append",
        required=True,
        help="Resource mapping in the form <virtual_name>=<source_path>",
    )
    args = parser.parse_args()

    parsed_resources = [parse_resource_mapping(raw) for raw in args.resources]

    seen_virtual_names: set[str] = set()
    resource_entries: list[tuple[str, Path, bytes]] = []
    for virtual_name, source_path in parsed_resources:
        if virtual_name in seen_virtual_names:
            raise SystemExit(f"duplicate embedded resource virtual name: {virtual_name}")
        seen_virtual_names.add(virtual_name)
        if not source_path.exists():
            raise SystemExit(f"embedded resource source file does not exist: {source_path}")
        resource_entries.append((virtual_name, source_path, source_path.read_bytes()))

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    lines: list[str] = [
        '#include <array>',
        '#include <cstddef>',
        '#include <span>',
        '',
        '#include "fleaux/common/embedded_resource.hpp"',
        '',
        'namespace {',
        '',
    ]

    for index, (virtual_name, source_path, data) in enumerate(resource_entries):
        lines.append(f'// Embedded from {source_path.as_posix()} as {virtual_name}')
        lines.append(f'static constexpr std::array<std::byte, {len(data)}> kEmbeddedResourceData{index}{{')
        if data:
            lines.append(f'        {format_byte_array(data)}')
        lines.append('};')
        lines.append('')

    lines.append(f'static const std::array<fleaux::common::EmbeddedResourceView, {len(resource_entries)}> kEmbeddedResources{{{{')
    for index, (virtual_name, _source_path, _data) in enumerate(resource_entries):
        lines.append(
            f'    {{.name = "{virtual_name}", .bytes = std::span<const std::byte>(kEmbeddedResourceData{index}.data(), '
            f'kEmbeddedResourceData{index}.size())}},')
    lines.append('}};')
    lines.append('')
    lines.append('struct EmbeddedResourceRegistryInitializer {')
    lines.append('  EmbeddedResourceRegistryInitializer() {')
    lines.append('    fleaux::common::set_embedded_resource_registry(std::span{kEmbeddedResources});')
    lines.append('  }')
    lines.append('};')
    lines.append('')
    lines.append('[[maybe_unused]] static const EmbeddedResourceRegistryInitializer kEmbeddedResourceRegistryInitializer;')
    lines.append('')
    lines.append('}  // namespace')
    lines.append('')

    output_path.write_text("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

