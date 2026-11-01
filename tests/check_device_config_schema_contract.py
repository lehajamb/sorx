#!/usr/bin/env python3
"""Fail when DeviceConfigV2's C++ object graph and protobuf schema drift."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Field:
    name: str
    type_name: str
    repeated: bool = False
    number: int = 0


def without_comments(document: str) -> str:
    document = re.sub(r"/\*.*?\*/", "", document, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", document)


def named_blocks(document: str, opening: re.Pattern[str]) -> dict[str, str]:
    blocks: dict[str, str] = {}
    cursor = 0
    while match := opening.search(document, cursor):
        name = match.group(1)
        depth = 1
        index = match.end()
        while index < len(document) and depth != 0:
            if document[index] == "{":
                depth += 1
            elif document[index] == "}":
                depth -= 1
            index += 1
        if depth != 0:
            raise ValueError(f"unterminated block {name}")
        if name in blocks:
            raise ValueError(f"duplicate block {name}")
        blocks[name] = document[match.end() : index - 1]
        cursor = index
    return blocks


def top_level_cpp_statements(body: str) -> list[str]:
    statements: list[str] = []
    current: list[str] = []
    brace_depth = 0
    quote = ""
    escaped = False
    for character in body:
        current.append(character)
        if quote:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = ""
            continue
        if character == '"':
            quote = character
        elif character == "{":
            brace_depth += 1
        elif character == "}":
            brace_depth -= 1
        elif character == ";" and brace_depth == 0:
            statements.append("".join(current[:-1]))
            current.clear()
    if "".join(current).strip():
        statements.append("".join(current))
    return statements


def parse_cpp_structs(
    document: str,
) -> tuple[dict[str, list[Field]], dict[str, list[str]]]:
    opening = re.compile(r"\bstruct\s+([A-Za-z_]\w*)\s+final\s*\{")
    declaration = re.compile(
        r"^(?P<type>[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*(?:<.*>)?)\s+"
        r"(?P<name>[A-Za-z_]\w*)\s*(?:\{.*\}|=.*)?$"
    )
    output: dict[str, list[Field]] = {}
    unsupported: dict[str, list[str]] = {}
    for name, body in named_blocks(document, opening).items():
        fields: list[Field] = []
        unsupported[name] = []
        for raw_statement in top_level_cpp_statements(body):
            statement = " ".join(raw_statement.split())
            if (
                not statement
                or "operator" in statement
                or "(" in statement
                or statement.endswith(":")
                or statement.startswith(("using ", "static "))
            ):
                continue
            match = declaration.fullmatch(statement)
            if match is None:
                unsupported[name].append(statement)
                continue
            cpp_type = re.sub(r"\s+", "", match.group("type"))
            repeated = cpp_type.startswith("std::vector<")
            if repeated:
                cpp_type = cpp_type[len("std::vector<") : -1]
            fields.append(Field(match.group("name"), cpp_type, repeated))
        output[name] = fields
    return output, unsupported


def parse_cpp_enums(document: str) -> dict[str, list[tuple[str, int]]]:
    opening = re.compile(
        r"\benum\s+class\s+([A-Za-z_]\w*)\s*:\s*[^\{]+\{")
    output: dict[str, list[tuple[str, int]]] = {}
    for name, body in named_blocks(document, opening).items():
        values: list[tuple[str, int]] = []
        next_value = 0
        for raw_item in body.split(","):
            item = " ".join(raw_item.split())
            if not item:
                continue
            match = re.fullmatch(
                r"([A-Za-z_]\w*)(?:\s*=\s*([0-9]+)(?:U|UL|ULL)?)?", item)
            if match is None:
                raise ValueError(f"unsupported C++ enum value {name}::{item}")
            if match.group(2) is not None:
                next_value = int(match.group(2))
            values.append((match.group(1), next_value))
            next_value += 1
        output[name] = values
    return output


def parse_proto_messages(document: str) -> dict[str, list[Field]]:
    opening = re.compile(r"\bmessage\s+([A-Za-z_]\w*)\s*\{")
    declaration = re.compile(
        r"^(?:(repeated)\s+)?([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*=\s*([0-9]+)$"
    )
    output: dict[str, list[Field]] = {}
    for name, body in named_blocks(document, opening).items():
        fields: list[Field] = []
        seen_names: set[str] = set()
        seen_numbers: set[int] = set()
        for raw_statement in body.split(";"):
            statement = " ".join(raw_statement.split())
            if not statement:
                continue
            match = declaration.fullmatch(statement)
            if match is None:
                raise ValueError(f"unsupported protobuf statement in {name}: {statement}")
            field = Field(
                name=match.group(3),
                type_name=match.group(2),
                repeated=match.group(1) is not None,
                number=int(match.group(4)),
            )
            if field.name in seen_names or field.number in seen_numbers:
                raise ValueError(f"duplicate protobuf field in {name}: {field}")
            seen_names.add(field.name)
            seen_numbers.add(field.number)
            fields.append(field)
        output[name] = fields
    return output


def parse_proto_enums(document: str) -> dict[str, list[tuple[str, int]]]:
    opening = re.compile(r"\benum\s+([A-Za-z_]\w*)\s*\{")
    output: dict[str, list[tuple[str, int]]] = {}
    for name, body in named_blocks(document, opening).items():
        values: list[tuple[str, int]] = []
        for raw_statement in body.split(";"):
            statement = " ".join(raw_statement.split())
            if not statement:
                continue
            match = re.fullmatch(r"([A-Z][A-Z0-9_]*)\s*=\s*([0-9]+)", statement)
            if match is None:
                raise ValueError(f"unsupported protobuf enum value {name}::{statement}")
            values.append((match.group(1), int(match.group(2))))
        output[name] = values
    return output


def upper_snake(identifier: str) -> str:
    identifier = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", identifier)
    identifier = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", identifier)
    return identifier.upper()


def protobuf_type(cpp_type: str) -> str:
    scalars = {
        "bool": "bool",
        "std::string": "string",
        "std::uint8_t": "uint32",
        "std::uint16_t": "uint32",
        "std::uint32_t": "uint32",
        "std::uint64_t": "uint64",
    }
    return scalars.get(cpp_type, cpp_type)


def scalar_cpp_type(cpp_type: str) -> bool:
    return cpp_type in {
        "bool",
        "std::string",
        "std::uint8_t",
        "std::uint16_t",
        "std::uint32_t",
        "std::uint64_t",
    }


def reachable_contract(
    structs: dict[str, list[Field]],
    enums: dict[str, list[tuple[str, int]]],
    unsupported: dict[str, list[str]],
) -> tuple[dict[str, list[Field]], dict[str, list[tuple[str, int]]]]:
    messages: dict[str, list[Field]] = {}
    used_enums: dict[str, list[tuple[str, int]]] = {}

    def visit(name: str) -> None:
        if name in messages:
            return
        if name not in structs:
            raise ValueError(f"runtime type {name} is not a parsed struct")
        if unsupported.get(name):
            raise ValueError(
                f"unsupported C++ declaration in normative type {name}: "
                f"{unsupported[name][0]}"
            )
        messages[name] = structs[name]
        for field in structs[name]:
            if field.type_name in enums:
                used_enums[field.type_name] = enums[field.type_name]
            elif not scalar_cpp_type(field.type_name):
                visit(field.type_name)

    visit("DeviceConfigV2")
    return messages, used_enums


def verify_documents(
    cpp_document: str, proto_document: str
) -> tuple[int, int, int]:
    cpp = without_comments(cpp_document)
    proto = without_comments(proto_document)
    if not re.search(r'^\s*syntax\s*=\s*"proto3"\s*;', proto, re.MULTILINE):
        raise ValueError("schema must declare proto3 syntax")
    if not re.search(
        r"^\s*package\s+sorx\.appliance\.v2\s*;", proto, re.MULTILINE
    ):
        raise ValueError("schema package must be sorx.appliance.v2")

    cpp_structs, unsupported = parse_cpp_structs(cpp)
    cpp_messages, cpp_enums = reachable_contract(
        cpp_structs, parse_cpp_enums(cpp), unsupported
    )
    proto_messages = parse_proto_messages(proto)
    proto_enums = parse_proto_enums(proto)

    if set(proto_messages) != set(cpp_messages):
        missing = sorted(set(cpp_messages) - set(proto_messages))
        extra = sorted(set(proto_messages) - set(cpp_messages))
        raise ValueError(f"message set mismatch; missing={missing}, extra={extra}")
    if set(proto_enums) != set(cpp_enums):
        missing = sorted(set(cpp_enums) - set(proto_enums))
        extra = sorted(set(proto_enums) - set(cpp_enums))
        raise ValueError(f"enum set mismatch; missing={missing}, extra={extra}")

    field_count = 0
    for message_name, cpp_fields in cpp_messages.items():
        expected = [
            Field(field.name, protobuf_type(field.type_name), field.repeated, index)
            for index, field in enumerate(cpp_fields, start=1)
        ]
        actual = proto_messages[message_name]
        if actual != expected:
            raise ValueError(
                f"field contract mismatch for {message_name}; "
                f"expected={expected}, actual={actual}"
            )
        field_count += len(expected)

    for enum_name, cpp_values in cpp_enums.items():
        prefix = upper_snake(enum_name)
        expected = [
            (f"{prefix}_{upper_snake(value_name)}", number)
            for value_name, number in cpp_values
        ]
        if proto_enums[enum_name] != expected:
            raise ValueError(
                f"enum contract mismatch for {enum_name}; "
                f"expected={expected}, actual={proto_enums[enum_name]}"
            )

    return len(cpp_messages), len(cpp_enums), field_count


def verify_contract(header: Path, schema: Path) -> tuple[int, int, int]:
    return verify_documents(
        header.read_text(encoding="utf-8"),
        schema.read_text(encoding="utf-8"),
    )


def negative_self_tests(header: Path, schema: Path) -> None:
    cpp = header.read_text(encoding="utf-8")
    proto = schema.read_text(encoding="utf-8")

    def must_detect(label: str, changed_cpp: str, changed_proto: str) -> None:
        try:
            verify_documents(changed_cpp, changed_proto)
        except ValueError:
            return
        raise ValueError(f"negative self-test did not detect {label}")

    must_detect(
        "an added runtime field",
        cpp.replace(
            "    std::string hostname;",
            "    std::string hostname;\n    std::string injected_schema_drift;",
            1,
        ),
        proto,
    )
    must_detect(
        "a renamed protobuf field",
        cpp,
        proto.replace("  string hostname = 3;", "  string host_name = 3;", 1),
    )
    must_detect(
        "an enum numeric mismatch",
        cpp,
        proto.replace(
            "  DEPLOYMENT_MODE_PRODUCTION = 2;",
            "  DEPLOYMENT_MODE_PRODUCTION = 7;",
            1,
        ),
    )


def main() -> int:
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--header",
        type=Path,
        default=repository / "include" / "sorx" / "appliance" / "device_config.hpp",
    )
    parser.add_argument(
        "--schema",
        type=Path,
        default=repository / "schemas" / "device_config_v2.proto",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="also prove that representative runtime, schema, and enum drift fail",
    )
    arguments = parser.parse_args()
    try:
        messages, enums, fields = verify_contract(arguments.header, arguments.schema)
        if arguments.self_test:
            negative_self_tests(arguments.header, arguments.schema)
    except (OSError, ValueError) as error:
        print(f"DeviceConfigV2 schema contract FAILED: {error}", file=sys.stderr)
        return 1
    print(
        "DeviceConfigV2 schema contract verified: "
        f"{messages} messages, {enums} enums, {fields} fields"
        + ("; negative drift probes passed" if arguments.self_test else "")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
