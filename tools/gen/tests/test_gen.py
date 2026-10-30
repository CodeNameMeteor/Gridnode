"""Tests for gridnode-gen. Run: python -m unittest discover -s tools/gen/tests -t tools/gen"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
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

    def test_bad_event_hash(self):
        self.assertError("""
            events:
              - { name: StartReplay, hash: 0x12345678 }
            """, "does not match djb2-xor")

    def test_duplicate_event(self):
        self.assertError("""
            events:
              - { name: StartReplay }
              - { name: StartReplay }
            """, "already in")

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
        clean = emit_rust._strip(header)
        api = clean[clean.index("typedef struct gn_api {"):clean.index("} gn_api;")]
        members = re.findall(r"\(\*\s*(\w+)\)\s*\(", api)
        self.assertGreater(len(members), 40)
        for m in members:
            self.assertIn(f"pub {emit_rust._safe(m)}: Option<unsafe extern \"C\" fn(", rs, m)
        self.assertIn("#[repr(C, align(16))]\n#[derive(Copy, Clone)]\npub struct gn_transform", rs)


class Docs(unittest.TestCase):
    """The docs pages from the fixture (no mkdocs needed)."""

    @classmethod
    def setUpClass(cls):
        cls.db, p = annotations.load(os.path.join(FIXTURE, "annotations"))
        assert not p.errors, p.errors
        cls.r = reflected.Reflected.load(os.path.join(FIXTURE, "reflected.json"))

    def test_annotated_pages(self):
        from gridnode_gen import emit_docs
        pages = emit_docs.annotated_pages(self.db)
        self.assertIn("annotated/index.md", pages)
        self.assertIn("annotated/fixture.md", pages)
        self.assertIn("## MyEntity", pages["annotated/fixture.md"])
        self.assertNotIn("reference/", pages["annotated/fixture.md"])  # no dump: no links into it

    def test_reference_pages(self):
        from gridnode_gen import emit_docs
        pages = emit_docs.reference_pages(self.r, self.db)
        self.assertIn("reference/index.md", pages)
        text = "\n".join(pages.values())
        self.assertIn("{ #mydata }", text)
        self.assertIn("Annotated: [fixture](../annotated/fixture.md#myentity)", text)

    def test_site(self):
        from gridnode_gen import emit_docs
        with tempfile.TemporaryDirectory() as d:
            pages = emit_docs.write_site(ROOT, d, self.db, None)
            self.assertTrue(os.path.isfile(os.path.join(d, "docs", "annotated", "fixture.md")))
            with open(os.path.join(d, "mkdocs.yml")) as f:
                conf = f.read()
            self.assertIn("annotated/fixture.md", conf)
            self.assertNotIn("reference/", conf)
            self.assertTrue(all(p.startswith("annotated/") for p in pages))


class GhidraSync(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db, p = annotations.load(os.path.join(FIXTURE, "annotations"))
        assert not p.errors, p.errors
        cls.r = reflected.Reflected.load(os.path.join(FIXTURE, "reflected.json"))

    def test_to_ghidra(self):
        from gridnode_gen import ghidra_sync
        doc = ghidra_sync.to_ghidra(self.db, self.r)
        self.assertEqual(doc["format"], "gridnode-ghidra")
        names = {f["name"] for f in doc["functions"]}
        self.assertTrue(any("::" in n for n in names) or names)  # virtuals as Class::name when annotated
        self.assertTrue(all("GridNode [" in f["comment"] for f in doc["functions"]))

    def test_class_structs_and_vtables(self):
        """Class entries carry their vtable and depth, and the annotated members of their base classes;
        extra classes come from the dump."""
        from gridnode_gen import ghidra_sync
        with tempfile.TemporaryDirectory() as d:
            for fn in os.listdir(os.path.join(FIXTURE, "annotations")):
                shutil.copy(os.path.join(FIXTURE, "annotations", fn), d)
            with open(os.path.join(d, "zz-base.yaml"), "w") as f:
                f.write("classes:\n  - name: Entity\n    members: [ { offset: 0x8, name: owner, type: \"void*\" } ]\n")
            db, p = annotations.load(d)
            self.assertEqual(p.errors, [])
        doc = ghidra_sync.to_ghidra(db, self.r, classes=["MyData"])
        by = {s["name"]: s for s in doc["structs"]}
        me = by["MyEntity"]
        self.assertEqual(me["vtable"], 0x140020400)
        self.assertEqual(me["depth"], 2)
        owner = [m for m in me["members"] if m["name"] == "owner"]
        self.assertEqual(len(owner), 1)
        self.assertIn("from Entity", owner[0]["comment"])
        self.assertIn("Bases: Entity", me["comment"])
        md = by["MyData"]  # not annotated: from the dump
        self.assertEqual((md["vtable"], md["depth"], md["size"]), (0x140020200, 3, 128))

    def test_shared_functions_merge(self):
        """A function at one address gets one entry: the annotated name first, else the base class's
        virtual; every comment is kept; sibling classes with no known common base get no prototype."""
        from gridnode_gen import ghidra_sync
        v = lambda cls, va, c: {"name": f"{cls}::f", "va": va, "signature": "", "comment": c,
                                "proto": {"returns": "void", "params": [], "this": cls}}
        plain = {"name": "Spawner_f", "va": 0x10, "signature": "", "comment": "plain",
                 "proto": {"returns": "void", "params": [{"type": "Spawner*", "name": "s"}]}}
        out = {f["va"]: f for f in ghidra_sync._merge_shared(
            [v("PamSpawner", 0x10, "pam"), plain,
             v("PamSpawner", 0x20, "pam"), v("Spawner", 0x20, "base"),
             v("ServerThing", 0x30, "s"), v("ClientThing", 0x30, "c")], None)}
        self.assertEqual(len(out), 3)
        self.assertEqual(out[0x10]["name"], "Spawner_f")
        self.assertIn("pam", out[0x10]["comment"])
        self.assertIn("Also: PamSpawner::f", out[0x10]["comment"])
        self.assertEqual((out[0x20]["name"], out[0x20]["proto"]["this"]), ("Spawner::f", "Spawner"))
        self.assertNotIn("proto", out[0x30])

    def test_round_trip(self):
        """What Ghidra exports comes back as annotations that load next to the existing ones,
        and what GridNode already knows is not repeated."""
        from gridnode_gen import ghidra_sync
        known = next(iter(self.db.functions.values()))
        export = {
            "format": "gridnode-ghidra-export", "version": 1,
            "functions": [
                {"name": known.name, "va": known.va, "prototype": "void x(void)", "signatureUserDefined": False},
                {"name": "Thing::update", "va": 0x140123450, "prototype": "undefined8 update(longlong * this, float dt)",
                 "signatureUserDefined": True},
                {"name": "odd", "va": 0x140123460, "prototype": "?? broken", "signatureUserDefined": True},
            ],
            "labels": [{"name": "g_thing", "va": 0x142000010, "type": "pointer"},
                       {"name": "Thing::update", "va": 0x140123450, "type": ""}],  # label at a function entry
            "structs": [],
        }
        text, counts = ghidra_sync.from_ghidra(export, self.db)
        self.assertEqual(counts["functions"], 2)
        self.assertEqual(counts["globals"], 1)
        self.assertIn('signature: "u64(i64* this, f32 dt)"', text)
        self.assertIn("name: \"Thing_update\"", text)
        with tempfile.TemporaryDirectory() as d:
            for fn in os.listdir(os.path.join(FIXTURE, "annotations")):
                shutil.copy(os.path.join(FIXTURE, "annotations", fn), d)
            with open(os.path.join(d, "zz.yaml"), "w") as f:
                f.write(text)
            _, p = annotations.load(d)
            self.assertEqual(p.errors, [])

    def test_import_skips_gridnodes_own_members(self):
        """`vtbl` at +0 and base members GridNodeSync copied into a subclass are not findings."""
        from gridnode_gen import ghidra_sync
        export = {"format": "gridnode-ghidra-export", "version": 1, "functions": [], "labels": [],
                  "structs": [{"name": "MyEntity", "members": [
                      {"offset": 0, "name": "vtbl", "type": "MyEntity_vtbl *"},
                      {"offset": 0x8, "name": "owner", "type": "MyEntity *"},     # another class's annotated member
                      {"offset": 0x60, "name": "speed", "type": "float"}]}]}     # a real finding
        text, counts = ghidra_sync.from_ghidra(export, self.db)
        self.assertEqual(counts["members"], 1)
        self.assertIn("speed", text)
        self.assertNotIn("vtbl", text)

    def test_friend_import_and_relay(self):
        """A friend's export: their name in sources and notes, their comment carried; relayed, it
        becomes an import file for another Ghidra."""
        from gridnode_gen import ghidra_sync
        export = {"format": "gridnode-ghidra-export", "version": 1, "program": "MEC",
                  "functions": [{"name": "Rope::swing", "va": 0x140223450, "prototype": "", "signatureUserDefined": False,
                                 "comment": "launches Faith"}],
                  "labels": [{"name": "g_rope", "va": 0x142000020, "type": "", "comment": ""}], "structs": []}
        text, _ = ghidra_sync.from_ghidra(export, self.db, who="Alex")
        self.assertIn("sources: [ghidra, alex]", text)
        self.assertIn("Named in Alex's Ghidra as Rope::swing. Signature not known yet. launches Faith", text)
        with tempfile.TemporaryDirectory() as d:
            for fn in os.listdir(os.path.join(FIXTURE, "annotations")):
                shutil.copy(os.path.join(FIXTURE, "annotations", fn), d)
            with open(os.path.join(d, "zz.yaml"), "w") as f:
                f.write(text)
            _, p = annotations.load(d)
            self.assertEqual(p.errors, [])
        doc = ghidra_sync.relay(export, who="Alex")
        self.assertEqual(doc["format"], "gridnode-ghidra")
        self.assertEqual(doc["functions"][0]["name"], "Rope::swing")
        self.assertTrue(doc["functions"][0]["comment"].startswith("GridNode [relay] (Alex's Ghidra)"))
        self.assertIn("launches Faith", doc["functions"][0]["comment"])


class AnnotationsDiff(unittest.TestCase):
    def test_diff(self):
        from gridnode_gen import annotations_diff
        base = {"a.yaml": "defaults: { confidence: confirmed }\n"
                          "functions:\n  - { name: f1, va: 0x140001000, signature: \"void()\" }\n"
                          "  - { name: f2, va: 0x140002000 }\n  - { name: gone, va: 0x140003000 }\n"
                          "classes:\n  - name: C\n    members:\n      - { offset: 0x10, name: m, type: u32 }\n"}
        other = {"a.yaml": "defaults: { confidence: inferred }\n"
                           "functions:\n  - { name: f1, va: 0x140001000, signature: \"i32()\" }\n"
                           "  - { name: f2_better, va: 0x140002000 }\n  - { name: new, va: 0x140004000 }\n"
                           "classes:\n  - name: C\n    members:\n      - { offset: 0x10, name: m, type: u64 }\n"
                           "      - { offset: 0x18, name: n, type: f32 }\n"}
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            for d, files in ((a, base), (b, other)):
                for n, t in files.items():
                    with open(os.path.join(d, n), "w") as f:
                        f.write(t)
            d = annotations_diff.diff(a, b)
            self.assertEqual([c.name for c in d.added], ["new"])
            self.assertEqual([c.name for c in d.removed], ["gone"])
            self.assertEqual([(c.base.name, c.name) for c in d.renamed], [("f2", "f2_better")])
            changed = {c.name: c.what for c in d.changed}
            self.assertIn("signature: 'void()' -> 'i32()'", changed["f1"])
            self.assertIn("confidence: 'confirmed' -> 'inferred'", changed["f1"])
            self.assertTrue(any("member 0x10 m: type 'u32' -> 'u64'" == w for w in changed["C"]))
            self.assertTrue(any(w.startswith("member 0x18 n: only in other") for w in changed["C"]))
            text = annotations_diff.inbox(d)
            self.assertIn("va: 0x140004000", text)
            self.assertIn("confidence: inferred", text)
            annotations_diff.report(d)


class OptionalSignature(unittest.TestCase):
    def test_function_without_signature(self):
        with tempfile.TemporaryDirectory() as d:
            write_annotations(d, """
                functions:
                  - { name: Mystery, va: 0x140001000, note: "named in Ghidra" }
            """)
            db, p = annotations.load(d)
            self.assertEqual(p.errors, [])
            self.assertIsNone(db.functions["Mystery"].signature)
            self.assertIn("Mystery = 0x140001000", emit_engine.emit(db))


def _fake_pe(path, code: bytes, rdata: bytes, base=0x140000000, pdata=None):
    """A minimal 64-bit PE: .text at base+0x1000 (code), .rdata at base+0x2000 (rdata).
    pdata: (rva, size) of an exception directory (the unwind table) inside .rdata."""
    import struct
    d = bytearray(0x3000)
    d[0:2] = b"MZ"
    struct.pack_into("<I", d, 0x3C, 0x40)
    d[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHI", d, 0x44, 0x8664, 2, 0x12345678)
    struct.pack_into("<H", d, 0x54, 0xF0)
    opt = 0x58
    struct.pack_into("<H", d, opt, 0x20B)
    struct.pack_into("<Q", d, opt + 24, base)
    struct.pack_into("<II", d, opt + 32, 0x1000, 0x1000)
    struct.pack_into("<I", d, opt + 56, 0x3000)
    if pdata:
        struct.pack_into("<I", d, opt + 108, 16)
        struct.pack_into("<II", d, opt + 112 + 3 * 8, *pdata)
    sh = opt + 0xF0
    for i, (name, rva, ch) in enumerate(((b".text", 0x1000, 0x60000020), (b".rdata", 0x2000, 0x40000040))):
        h = sh + i * 40
        d[h:h + 8] = name.ljust(8, b"\0")
        struct.pack_into("<IIII", d, h + 8, 0x1000, rva, 0x1000, rva)
        struct.pack_into("<I", d, h + 36, ch)
    d[0x1000:0x1000 + len(code)] = code
    d[0x2000:0x2000 + len(rdata)] = rdata
    with open(path, "wb") as f:
        f.write(d)


def _unwind_pe(path):
    """Unwind table: one function at 0x1000..0x1040, a second part of it at 0x1100..0x1120 (chained back
    to the first), nothing at 0x1080."""
    import struct
    rdata = bytearray(0x400)
    struct.pack_into("<III", rdata, 0x100, 0x1000, 0x1040, 0x2200)
    struct.pack_into("<III", rdata, 0x10C, 0x1100, 0x1120, 0x2210)
    rdata[0x200:0x204] = bytes([1, 0, 0, 0])                  # version 1, no flags
    rdata[0x210:0x214] = bytes([1 | (0x4 << 3), 0, 0, 0])     # version 1, UNW_FLAG_CHAININFO, no codes
    struct.pack_into("<III", rdata, 0x214, 0x1000, 0x1040, 0x2200)
    _fake_pe(path, b"\x90" * 0x200, bytes(rdata), pdata=(0x2100, 24))


class UnwindTable(unittest.TestCase):
    def test_function_start(self):
        from gridnode_gen import pe_image
        with tempfile.TemporaryDirectory() as d:
            exe = os.path.join(d, "game.exe")
            _unwind_pe(exe)
            img = pe_image.PeImage(exe)
            self.assertEqual(img.function_start(0x140001010), 0x140001000)
            self.assertEqual(img.function_start(0x140001110), 0x140001000)   # through the chain
            self.assertIsNone(img.function_start(0x140001080))
            self.assertIsNone(img.function_start(0x140009000))

    def test_recording_to_ghidra(self):
        import json
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            exe = os.path.join(d, "game.exe")
            _unwind_pe(exe)
            rec = os.path.join(d, "rec.jsonl")
            evs = [{"kind": "header", "level": "Levels/Test", "events": 3},
                   {"kind": "message", "t": 1.0, "type": "FooMessage", "stack": ["0x140001010", "0x140001110", "0x140001080"]},
                   {"kind": "message", "t": 1.5, "type": "FooMessage", "stack": ["0x140001010"]},
                   {"kind": "call", "t": 2.0, "class": "Thing", "slot": "0x20", "fn": "0x140001000", "skipped_before": 4,
                    "stack": ["0x140001110"]}]
            with open(rec, "w") as f:
                f.write("\n".join(json.dumps(e) for e in evs))
            out = os.path.join(d, "g.json")
            script = os.path.join(ROOT, "tools", "recording", "summarize.py")
            r = subprocess.run([sys.executable, script, rec, "--ghidra", out, "--exe", exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            doc = json.load(open(out))
            self.assertEqual(doc["format"], "gridnode-ghidra")
            self.assertEqual([c["va"] for c in doc["createFunctions"]], [0x140001000])
            self.assertEqual(sorted(b["va"] for b in doc["bookmarks"]), [0x140001010, 0x140001080, 0x140001110])
            self.assertTrue(all(b["afterCall"] for b in doc["bookmarks"]))
            (c,) = doc["comments"]
            self.assertEqual(c["va"], 0x140001000)
            first, second = c["comment"].split("\n")[:2]
            self.assertEqual(first, "GridNode [recording] (rec.jsonl)")
            self.assertIn("In 3 recorded stacks", second)   # recursion through the chained part counts once
            self.assertIn("2x  msg FooMessage", c["comment"])
            self.assertIn("5x  call Thing::vf+0x20", c["comment"])   # traced, with skipped calls


class RecordingObjects(unittest.TestCase):
    """Recordings with destroyed entities, list watches and the object table (summarize.py)."""

    def test_summarize_objects_and_lists(self):
        import json
        evs = [{"kind": "header", "level": "Levels/Test", "events": 6, "objects": 3},
               {"kind": "created", "t": 0.5, "type": "PamServerCharacterEntity", "ptr": "0x1000", "blueprint": "Chars/Ghost", "obj": 2},
               {"kind": "list", "t": 0.6, "field": "spawned (server)", "target": "ServerCharacterSpawnEntity", "ptr": "0x2000",
                "obj": 1, "count": 1, "first": True, "items": [{"index": 0, "ptr": "0x1000", "type": "PamServerCharacterEntity", "obj": 2}]},
               {"kind": "marker", "t": 1.0, "name": "despawn"},
               {"kind": "event", "t": 1.1, "event": "StopReplay", "event_id": "d6a2182e", "target": "PamServerReplayEntity", "ptr": "0x3000", "obj": 3},
               {"kind": "list", "t": 1.2, "field": "spawned (server)", "target": "ServerCharacterSpawnEntity", "ptr": "0x2000",
                "obj": 1, "count": 0, "old_count": 1, "items": [],
                "removed": [{"ptr": "0x1000", "type": "PamServerCharacterEntity", "obj": 2}]},
               {"kind": "destroyed", "t": 1.25, "type": "PamServerCharacterEntity", "ptr": "0x1000", "obj": 2},
               {"kind": "object", "id": 1, "ptr": "0x2000", "type": "ServerCharacterSpawnEntity", "created": None, "destroyed": None, "first_seen": 0.6},
               {"kind": "object", "id": 2, "ptr": "0x1000", "type": "PamServerCharacterEntity", "blueprint": "Chars/Ghost",
                "created": 0.5, "destroyed": 1.25, "first_seen": 0.5},
               {"kind": "object", "id": 3, "ptr": "0x3000", "type": "PamServerReplayEntity", "created": None, "destroyed": None, "first_seen": 1.1}]
        script = os.path.join(ROOT, "tools", "recording", "summarize.py")
        with tempfile.TemporaryDirectory() as d:
            rec = os.path.join(d, "rec.jsonl")
            with open(rec, "w") as f:
                f.write("\n".join(json.dumps(e) for e in evs))
            r = subprocess.run([sys.executable, script, rec, "--all"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("gone PamServerCharacterEntity [Chars/Ghost]", r.stdout)   # the blueprint from the object table
            self.assertIn("list spawned (server) -> ServerCharacterSpawnEntity: -PamServerCharacterEntity", r.stdout)
            self.assertIn("#2", r.stdout)
            r = subprocess.run([sys.executable, script, rec, "--object", "0x1000"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("#2 PamServerCharacterEntity at 0x1000 [Chars/Ghost]", r.stdout)
            self.assertIn("created 0.500 s, destroyed 1.250 s", r.stdout)
            self.assertEqual(r.stdout.count("(points at it)"), 2)   # in the list, then removed from it
            r = subprocess.run([sys.executable, script, rec, "--ghidra", os.path.join(d, "g.json")], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)


class RecordingClasses(unittest.TestCase):
    def test_traced_classes_get_structs(self):
        """--ghidra adds class structs and vtables for the classes a recording traced or watched."""
        import json
        evs = [{"kind": "header", "events": 3},
               {"kind": "call", "t": 1.0, "class": "ServerCharacterSpawnEntity", "slot": "0xd0", "fn": "0x143390830"},
               {"kind": "list", "t": 1.1, "field": "spawned (client)", "target": "ClientSpawnEntity", "count": 0, "first": True, "items": []},
               {"kind": "created", "t": 1.2, "type": "PamClientCharacterEntity", "ptr": "0x1000"}]
        script = os.path.join(ROOT, "tools", "recording", "summarize.py")
        with tempfile.TemporaryDirectory() as d:
            rec = os.path.join(d, "rec.jsonl")
            with open(rec, "w") as f:
                f.write("\n".join(json.dumps(e) for e in evs))
            for mode, want in (("traced", {"ServerCharacterSpawnEntity", "ClientSpawnEntity"}),
                               ("all", {"ServerCharacterSpawnEntity", "ClientSpawnEntity", "PamClientCharacterEntity"})):
                out = os.path.join(d, f"g-{mode}.json")
                r = subprocess.run([sys.executable, script, rec, "--ghidra", out, "--classes", mode,
                                    "--reflected", os.path.join(d, "none.json")], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)
                doc = json.load(open(out))
                self.assertEqual({s["name"] for s in doc["structs"]}, want)
                by = {s["name"]: s for s in doc["structs"]}
                self.assertEqual(by["ServerCharacterSpawnEntity"]["vtable"], 0x141b4e0c8)   # from the annotations


class RecordingDiff(unittest.TestCase):
    """summarize.py --diff: what one recording has that another does not."""

    def test_compare_rates(self):
        sys.path.insert(0, os.path.join(ROOT, "tools", "recording"))
        import summarize
        a = {"spawn": 3, "tick": 100, "boost": 40, "rare": 1}
        b = {"tick": 50, "boost": 5, "rare": 1, "idle": 4}
        only_a, more_a, only_b, more_b = summarize.compare(a, 10.0, b, 5.0)
        self.assertEqual(only_a, [(3, "spawn")])
        self.assertEqual([x[3] for x in more_a], ["boost"])   # 4/s vs 1/s; tick is 10/s in both
        self.assertEqual(only_b, [(4, "idle")])
        self.assertEqual(more_b, [])                           # rare: once each, not a pattern
        self.assertEqual(summarize.compare({"x": 4}, 1.0, {"x": 2}, 1.0)[1], [])   # 4 against 2: not a pattern
        marks = [{"kind": "marker", "t": t, "name": "m"} for t in (1.0, 1.5, 5.0)]
        self.assertAlmostEqual(summarize._scope(marks, "*", 1.0)[1], 4.5)   # 0-2.5 and 4-6: overlap counted once

    def test_diff_around_markers(self):
        """Values do not split labels; each side is cut to its own marker's window."""
        import json
        def rec(path, evs):
            with open(path, "w") as f:
                f.write("\n".join(json.dumps(e) for e in [{"kind": "header"}] + evs))
        a = [{"kind": "anim", "t": 0.5, "property": "Speed", "value": 1.0, "target": "Faith"},
             {"kind": "marker", "t": 10.0, "name": "go"},
             {"kind": "event", "t": 10.2, "event": "StartReplay", "target": "PamServerReplayEntity",
              "stack": ["0x14396d7a0"]},
             {"kind": "anim", "t": 10.3, "property": "Speed", "value": 7.5, "target": "Faith"},
             {"kind": "anim", "t": 30.0, "property": "Far", "value": 1, "target": "Faith"}]
        b = [{"kind": "marker", "t": 5.0, "name": "nothing"},
             {"kind": "anim", "t": 5.1, "property": "Speed", "value": 2.0, "target": "Faith"}]
        script = os.path.join(ROOT, "tools", "recording", "summarize.py")
        with tempfile.TemporaryDirectory() as d:
            ra, rb = os.path.join(d, "a.jsonl"), os.path.join(d, "b.jsonl")
            rec(ra, a)
            rec(rb, b)
            r = subprocess.run([sys.executable, script, ra, "--diff", rb, "--marker", "go", "--diff-marker", "nothing",
                                "--window", "1"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            only_a = r.stdout.split("Only in A")[1].split("More in A")[0]
            self.assertIn("event StartReplay -> PamServerReplayEntity", only_a)
            self.assertNotIn("Speed", only_a)        # in both: the value is left out
            self.assertNotIn("Far", r.stdout)        # outside the window
            self.assertIn("+0.200  event StartReplay", r.stdout)
            self.assertIn("0x14396d7a0", r.stdout)   # the stack behind it
            # Exported without Clear: the second recording starts with the first one.
            first = [{"kind": "anim", "t": i * 0.01, "property": "P", "value": i, "target": "Faith"} for i in range(60)]
            rec(ra, first + [{"kind": "marker", "t": 1.0, "name": "marker 1"}])
            rec(rb, first + [{"kind": "marker", "t": 1.0, "name": "marker 1"},
                             {"kind": "marker", "t": 9.0, "name": "marker 2"}])
            r = subprocess.run([sys.executable, script, rb, "--diff", ra, "--marker", "marker 1"],
                               capture_output=True, text=True)
            self.assertIn("A's own markers: marker 2", r.stdout)
            self.assertIn("B is all in the other one", r.stdout)
            r = subprocess.run([sys.executable, script, ra, "--diff", rb, "--marker", "missing"],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertIn("no marker named 'missing'", r.stdout)


class FrameNames(unittest.TestCase):
    """summarize.py names each stack frame by its function, keeping the address first."""

    def test_frames_line_and_annotated_events(self):
        import json
        evs = [{"kind": "header", "events": 2},
               {"kind": "event", "t": 1.0, "event": "2734a476", "event_id": "2734a476", "target": "PamServerReplayEntity",
                "stack": ["0x14314e759", "0x143999010"]},
               {"kind": "marker", "t": 1.1, "name": "m"},
               {"kind": "frames", "starts": {"0x14314e759": "0x14314e4a0", "0x143999010": "0x143999000"}}]
        script = os.path.join(ROOT, "tools", "recording", "summarize.py")
        with tempfile.TemporaryDirectory() as d:
            rec = os.path.join(d, "rec.jsonl")
            with open(rec, "w") as f:
                f.write("\n".join(json.dumps(e) for e in evs))
            r = subprocess.run([sys.executable, script, rec, "--all"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("event StartReplay -> PamServerReplayEntity", r.stdout)   # named from db/annotations/events.yaml
            self.assertIn("0x14314e759 (EntityBus_fireEvent+0x2b9)", r.stdout)     # annotated function + offset
            self.assertIn("0x143999010 (FUN_143999000+0x10)", r.stdout)            # unnamed: Ghidra's FUN_ name


class Fingerprints(unittest.TestCase):
    def test_gridnode_and_mecmp(self):
        import struct
        from gridnode_gen import fingerprint_gen, pe_image
        code = bytes.fromhex("4C 89 CB 41 B9 0F A2 C3 90 C3") + b"\x90" * 6 + bytes.fromhex("48 89 E0 57 48 83 EC 70")
        rdata = struct.pack("<Q", 0x140001000)  # a vtable slot holding the first function
        with tempfile.TemporaryDirectory() as d:
            exe = os.path.join(d, "game.exe")
            _fake_pe(exe, code, rdata)
            img = pe_image.PeImage(exe)
            ann = os.path.join(d, "ann")
            os.mkdir(ann)
            with open(os.path.join(ann, "core.yaml"), "w") as f:
                f.write("functions:\n  - { name: FnA, va: 0x140001000 }\n  - { name: FnB, va: 0x140001010 }\n")
            fpy = os.path.join(ann, "fingerprint.yaml")
            with open(fpy, "w") as f:
                f.write('fingerprint:\n  code:\n    - { at: FnA, bytes: "49 8B D9 41 B9", note: "mov rbx,r9" }\n')
            db, p = annotations.load(ann)
            self.assertEqual(p.errors, [])
            res = fingerprint_gen.check_gridnode(db, img)
            self.assertEqual([r.ok for r in res], [False])
            self.assertEqual(fingerprint_gen.write_gridnode(fpy, res), 1)
            self.assertIn('bytes: "4C 89 CB 41 B9"', open(fpy).read())
            fingerprint_gen.add_gridnode(fpy, annotations.load(ann)[0], img, "FnB", length=8)
            db, p = annotations.load(ann)
            self.assertEqual(p.errors, [])
            self.assertTrue(all(r.ok for r in fingerprint_gen.check_gridnode(db, img)))
            # MECMP
            m = os.path.join(d, "mecmp", "src", "game")
            os.makedirs(m)
            with open(os.path.join(m, "engine.h"), "w") as f:
                f.write("constexpr uintptr_t Fn_A = 0x140001000;\nconstexpr uintptr_t Fn_B = 0x140001010;\n"
                        "constexpr uintptr_t Vtbl_X = 0x140002000;\nconstexpr size_t Off_A = 0x0, Off_B = 0x8;\n")
            with open(os.path.join(m, "fingerprint.cpp"), "w") as f:
                f.write('const Slot kSlots[] = {\n    {"X::a", Vtbl_X, Off_A, Fn_A},\n};\n'
                        'const Code kCode[] = {\n    {"A", Fn_A, 0, {0x49, 0x8B, 0xD9}, 3},\n'
                        '    {"B", Fn_B, 0,\n     {0x48, 0x8B, 0xC4, 0x57}, 4},\n};\n')
            mres = fingerprint_gen.check_mecmp(os.path.join(d, "mecmp"), img)
            self.assertEqual({r.what: r.ok for r in mres}, {"X::a": True, "A": False, "B": False})
            self.assertEqual(fingerprint_gen.write_mecmp(os.path.join(d, "mecmp"), mres), 2)
            text = open(os.path.join(m, "fingerprint.cpp")).read()
            self.assertIn("{0x4C, 0x89, 0xCB}, 3}", text)
            self.assertIn("{0x48, 0x89, 0xE0, 0x57}, 4}", text)
            self.assertTrue(all(r.ok for r in fingerprint_gen.check_mecmp(os.path.join(d, "mecmp"), img)))
