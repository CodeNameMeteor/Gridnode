"""Tests for gridnode-gen. Run: python -m unittest discover -s tools/gen/tests -t tools/gen"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest

from gridnode_gen import annotations, emit_engine, emit_types, reflected, validate
from gridnode_gen.ctypes_ import djb2_xor, parse_signature, parse_type

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
FIXTURE = os.path.join(HERE, "fixture")
CXX = next((c for c in ("g++", "clang++", "c++") if shutil.which(c)), None)


def compile_cpp(source: str, include_dirs: list[str]) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "t.cpp")
        with open(src, "w") as f:
            f.write(source)
        cmd = [CXX, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsyntax-only", src]
        for inc in include_dirs:
            cmd += ["-I", inc]
        return subprocess.run(cmd, capture_output=True, text=True)


def write_annotations(d: str, text: str):
    with open(os.path.join(d, "a.yaml"), "w") as f:
        f.write(textwrap.dedent(text))


class Cli(unittest.TestCase):
    def test_missing_dump_is_a_message_not_a_traceback(self):
        from gridnode_gen import cli
        self.assertEqual(cli.main(["validate", "--reflected", "no/such/file.json"]), 1)

    def test_paths_relative_to_repo_root(self):
        from gridnode_gen import cli
        old = os.getcwd()
        try:
            os.chdir(HERE)  # not the repository root
            rel = os.path.relpath(os.path.join(FIXTURE, "reflected.json"), ROOT)
            self.assertEqual(cli._find(rel, "dump"), os.path.join(FIXTURE, "reflected.json"))
        finally:
            os.chdir(old)


class TypeLanguage(unittest.TestCase):
    def test_types(self):
        t = parse_type("const LinearTransform*")
        self.assertEqual((t.base, t.pointers, t.const), ("LinearTransform", 1, True))
        self.assertEqual(parse_type("u8[0x10]").count, 16)
        self.assertEqual(parse_type("void**").pointers, 2)
        with self.assertRaises(ValueError):
            parse_type("foo bar")

    def test_signature(self):
        s = parse_signature("void*(void** arenaMember, u64 size, u64)")
        self.assertEqual(str(s.returns), "void*")
        self.assertEqual([p.name for p in s.params], ["arenaMember", "size", None])
        self.assertEqual(len(parse_signature("PamClientCharacterEntity*()").params), 0)

    def test_hash(self):
        # Values confirmed in the game (research.md).
        for name, h in {"Visible": 0x35BC69AB, "Enable": 0x8BA423E4, "Value": 0x0D6EF36E, "Spawn": 0x0DC320DE,
                        "Out": 0x0B87DF4B, "IsTarget": 0x1D8FAC4E, "Enabled": 0x0028A000}.items():
            self.assertEqual(djb2_xor(name), h, name)


class RealAnnotations(unittest.TestCase):
    """The repository's own db/annotations must load cleanly and produce a compiling engine.hpp."""

    def test_load(self):
        db, p = annotations.load(os.path.join(ROOT, "db", "annotations"))
        self.assertEqual(p.errors, [])
        self.assertGreaterEqual(len(db.fp_slots), 6)
        self.assertIn("ServerGameWorld_postUpdate", db.hooks)

    @unittest.skipUnless(CXX, "no C++ compiler")
    def test_engine_compiles(self):
        db, _ = annotations.load(os.path.join(ROOT, "db", "annotations"))
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "gridnode"))
            with open(os.path.join(d, "gridnode", "engine.hpp"), "w") as f:
                f.write(emit_engine.emit(db))
            r = compile_cpp('#include "gridnode/engine.hpp"\nint main() { return 0; }\n', [d])
            self.assertEqual(r.returncode, 0, r.stderr)


class Fixture(unittest.TestCase):
    """A synthetic dump with the awkward layout cases (not game data)."""

    @classmethod
    def setUpClass(cls):
        cls.db, p = annotations.load(os.path.join(FIXTURE, "annotations"))
        assert not p.errors, p.errors
        cls.r = reflected.Reflected.load(os.path.join(FIXTURE, "reflected.json"))

    def test_validate_clean(self):
        p = validate.check(self.db, self.r)
        self.assertEqual(p.errors, [])

    def test_idents(self):
        idents = sorted(t["_ident"] for t in self.r.types if t["displayName"] == "BoneNameId")
        self.assertEqual(idents, ["BoneNameId", "BoneNameId_Morph"])

    def test_engine(self):
        text = emit_engine.emit(self.db)
        self.assertIn("inline constexpr uint32_t tick = 0x10;", text)
        self.assertIn('{"MyEntity::tick", 0x140020410, 0x140001000}', text)
        self.assertIn("inline constexpr uint32_t Visible = 0x35bc69ab;", text)

    def test_types_text(self):
        text = emit_types.emit(self.db, self.r)
        self.assertIn("struct alignas(16) MyData : Asset {", text)
        self.assertIn("Asset* Asset_; // +0x18", text)            # field named like a type
        self.assertIn("uint8_t Odd[0x4]; // +0x21", text)         # misaligned u32 kept as bytes
        self.assertIn("Array<Asset*> Items; // +0x28", text)     # array of classes holds pointers
        self.assertIn("void* __vftable; // +0x0", text)
        self.assertIn("MyState state; // +0x28", text)            # annotation struct by value
        self.assertIn("static_assert(offsetof(MyEntity, transform) == 0x40", text)
        self.assertIn("using DoThing = ::gn::fb::MyEntity* (*)(const ::gn::fb::LinearTransform*, int32_t);", text)

    @unittest.skipUnless(CXX, "no C++ compiler")
    def test_types_compile(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "gridnode", "generated"))
            with open(os.path.join(d, "gridnode", "generated", "types.hpp"), "w") as f:
                f.write(emit_types.emit(self.db, self.r))
            r = compile_cpp('#include "gridnode/generated/types.hpp"\nint main() { return 0; }\n', [d])
            self.assertEqual(r.returncode, 0, r.stderr)


class Errors(unittest.TestCase):
    def load(self, text: str, with_dump=True):
        with tempfile.TemporaryDirectory() as d:
            write_annotations(d, text)
            db, p = annotations.load(d)
        if with_dump:
            p.extend(validate.check(db, reflected.Reflected.load(os.path.join(FIXTURE, "reflected.json"))))
        return p

    def assertError(self, text: str, fragment: str):
        p = self.load(text)
        self.assertTrue(any(fragment in e for e in p.errors), f"no error containing {fragment!r} in {p.errors}")

    def test_bad_hash(self):
        self.assertError("""
            classes:
              - name: MyEntity
                properties: [ { name: Visible, hash: 0x12345678 } ]
            """, "does not match djb2-xor")

    def test_wrong_vtable(self):
        self.assertError("""
            classes:
              - { name: MyEntity, vtable: 0x140099999 }
            """, "not among the reflected vtables")

    def test_member_past_end(self):
        self.assertError("""
            classes:
              - name: MyEntity
                members: [ { offset: 0x7c, name: x, type: u64 } ]
            """, "runs past the class size")

    def test_reflected_field(self):
        self.assertError("""
            classes:
              - name: MyData
                members: [ { offset: 0x18, name: other, type: "void*" } ]
            """, "is the reflected field")

    def test_realm(self):
        self.assertError("""
            classes:
              - { name: MyEntity, realm: server }
            """, "only created in ['client']")

    def test_unknown_type(self):
        self.assertError("""
            structs:
              - name: S
                size: 8
                members: [ { offset: 0, name: p, type: "Nope*" } ]
            """, "unknown type 'Nope'")

    def test_unknown_key(self):
        self.assertError("""
            globals:
              - { name: G, va: 0x140000000, type: "void*", colour: red }
            """, "unknown key 'colour'")

    def test_struct_name_clash(self):
        self.assertError("""
            structs:
              - { name: Vec3, size: 8 }
            """, "same name as a reflected type")

    def test_misaligned_hook(self):
        self.assertError("""
            hooks:
              - { name: H, kind: pointer, slot: 0x140000004, original: 0x140001000 }
            """, "not 8-byte aligned")

    def test_fingerprint_ref(self):
        self.assertError("""
            fingerprint:
              slots: [ Nothing.here ]
            """, "not a hook name or Class.virtual")


if __name__ == "__main__":
    unittest.main()


class NameHashTest(unittest.TestCase):
    def test_known_hashes(self):
        from gridnode_gen.hashes import name_hash, parse_hash
        self.assertEqual(name_hash("Visible"), 0x35BC69AB)            # PamClientCharacterEntity property
        self.assertEqual(name_hash("ControllableTransform"), 0x7C46EA68)  # research.md's "Transform" id
        self.assertEqual(parse_hash("-2024647575"), 0x87525469)       # Frosty shows ids signed
        self.assertEqual(parse_hash("0x35bc69ab"), 0x35BC69AB)


class RustBindingsTest(unittest.TestCase):
    def test_every_api_member_is_bound(self):
        import os
        import re
        from gridnode_gen import emit_rust
        root = os.path.join(os.path.dirname(__file__), "..", "..", "..")
        header = open(os.path.join(root, "sdk", "include", "gridnode", "api.h"), encoding="utf-8").read()
        rs = emit_rust.emit(header)
        api = header[header.index("typedef struct gn_api {"):header.index("} gn_api;")]
        members = re.findall(r"\(\*(\w+)\)\(", api)
        self.assertGreater(len(members), 50)
        for m in members:
            self.assertIn(f"pub {emit_rust._safe(m)}: Option<unsafe extern \"C\" fn(", rs, m)
        self.assertIn("#[repr(C, align(16))]\n#[derive(Copy, Clone)]\npub struct gn_transform", rs)
