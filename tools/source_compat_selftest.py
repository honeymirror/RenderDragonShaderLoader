"""Self-test for source compatibility analysis and native material IO validation.

Usage:
  py -3 tools/source_compat_selftest.py
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import source_alias_catalog  # noqa: E402
import source_pack_import  # noqa: E402
import pass_context  # noqa: E402
import source_compat  # noqa: E402
import native_io_map  # noqa: E402
import native_io_validate  # noqa: E402
from lazurite.material import Material  # noqa: E402
from lazurite.material.buffer import Buffer, BufferAccess, BufferType  # noqa: E402
from lazurite.material.shader_pass import Pass  # noqa: E402


class SourceCompatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.catalog = source_compat.load_catalog(
            ROOT / "samples" / "compat" / "source-bedrock-aliases.json"
        )

    def test_direct_attributes_and_comments(self) -> None:
        source = (
            "// vaPosition stays in comments\n"
            "in vec3 vaPosition;\n"
            "in vec2 vaUV0;\n"
            "in vec4 vaColor;\n"
            "void main() { gl_Position = vec4(vaPosition, 1.0); }\n"
        )
        rewritten, report = source_compat.rewrite_text(source, self.catalog)
        self.assertIn("// vaPosition stays in comments", rewritten)
        self.assertIn("in vec3 a_position;", rewritten)
        self.assertIn("in vec2 a_texcoord0;", rewritten)
        self.assertIn("in vec4 a_color0;", rewritten)
        self.assertEqual([], report["errors"])

    def test_unsupported_attribute_rejected(self) -> None:
        _, report = source_compat.rewrite_text(
            "in vec4 mc_Entity;\n", self.catalog
        )
        self.assertTrue(any("unsupported" in error for error in report["errors"]))

    def test_contextual_buffer_requires_context(self) -> None:
        _, report = source_compat.rewrite_text(
            "uniform sampler2D depthtex0;\n", self.catalog
        )
        self.assertTrue(
            any("missing required context" in error for error in report["errors"])
        )

        _, report = source_compat.rewrite_text(
            "uniform sampler2D depthtex0;\n",
            self.catalog,
            {"deferred_lighting"},
        )
        self.assertEqual([], report["errors"])
        self.assertEqual("SceneDepth", report["buffers"]["depthtex0"]["bedrock"])

    def test_ambiguous_buffer_rejected(self) -> None:
        _, report = source_compat.rewrite_text(
            "uniform sampler2D gcolor;\n",
            self.catalog,
            {"deferred_lighting"},
        )
        self.assertTrue(any("ambiguous" in error for error in report["errors"]))

    def test_render_target_directive(self) -> None:
        _, report = source_compat.rewrite_text(
            "/* DRAWBUFFERS:01 */\n",
            self.catalog,
            {"deferred_gbuffer"},
        )
        self.assertEqual(["Color0", "Color1"], report["renderTargets"])
        self.assertEqual("0x061654F8", report["outputBindingSignature"])
        self.assertEqual([], report["errors"])

        _, report = source_compat.rewrite_text(
            "/* DRAWBUFFERS:3 */\n",
            self.catalog,
            {"deferred_gbuffer"},
        )
        self.assertTrue(any("unsupported" in error for error in report["errors"]))


class SourceCatalogTests(unittest.TestCase):
    def test_catalog_against_contracts(self) -> None:
        catalog = source_alias_catalog.load_json(
            ROOT / "samples" / "compat" / "source-bedrock-aliases.json"
        )
        contracts = [
            source_alias_catalog.load_contract(
                ROOT / "samples" / "compat" / "deferred.material-io.json"
            ),
            source_alias_catalog.load_contract(
                ROOT / "samples" / "compat" / "forward.material-io.json"
            ),
        ]
        errors, _ = source_alias_catalog.validate_catalog(catalog, contracts)
        self.assertEqual([], errors)


class PassContextTests(unittest.TestCase):
    def test_context_resolution_and_signature(self) -> None:
        rules = pass_context.load_json(
            ROOT / "samples" / "compat" / "pass-contexts.json"
        )
        contracts = [
            pass_context.load_contract(
                ROOT / "samples" / "compat" / "deferred.material-io.json"
            ),
            pass_context.load_contract(
                ROOT / "samples" / "compat" / "forward.material-io.json"
            ),
        ]
        report = pass_context.resolve_contracts(rules, contracts)
        self.assertEqual(0, report["summary"]["unresolved"])
        self.assertEqual(0, report["summary"]["errors"])
        self.assertEqual(227, report["summary"]["passes"])

        catalog = pass_context.load_json(
            ROOT / "samples" / "compat" / "source-bedrock-aliases.json"
        )
        result = pass_context.generate_signature(
            catalog, {"deferred_gbuffer"}, [0, 1]
        )
        self.assertEqual("0x061654F8", result["outputBindingSignature"])
        self.assertEqual([], result["errors"])

        result = pass_context.generate_signature(
            catalog, {"deferred_gbuffer"}, [3]
        )
        self.assertTrue(any("unsupported" in error for error in result["errors"]))


class PackImportTests(unittest.TestCase):
    def test_smoke_pack_import(self) -> None:
        catalog = source_compat.load_catalog(
            ROOT / "samples" / "compat" / "source-bedrock-aliases.json"
        )
        rules = source_pack_import.load_json(
            ROOT / "samples" / "compat" / "program-contexts.json"
        )
        output = ROOT / "build" / "source-pack-selftest"
        try:
            manifest = source_pack_import.import_pack(
                ROOT / "samples" / "compat" / "source-pack-smoke",
                output,
                catalog,
                rules,
            )
            self.assertEqual(1, manifest["summary"]["programs"])
            self.assertEqual(0, manifest["summary"]["blockers"])
            self.assertEqual(
                "0x061654F8",
                manifest["programs"]["gbuffers_terrain"]["outputBindingSignature"],
            )
        finally:
            shutil.rmtree(output, ignore_errors=True)


class NativeIOValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.engine = native_io_validate.load_contract(
            ROOT / "samples" / "compat" / "RenderDragon-26.51.vanilla.material-io.json"
        )

    @staticmethod
    def candidate() -> dict:
        return native_io_validate.load_contract(
            ROOT / "samples" / "compat" / "deferred.material-io.json"
        )

    @staticmethod
    def codes(report: dict) -> set[str]:
        return {item["code"] for item in report["issues"]}

    def test_known_good_native_packs(self) -> None:
        for filename in ("deferred.material-io.json", "forward.material-io.json"):
            candidate = native_io_validate.load_contract(
                ROOT / "samples" / "compat" / filename
            )
            report = native_io_validate.validate(candidate, self.engine)
            self.assertEqual(0, report["summary"]["errors"], filename)
            self.assertEqual(0, report["summary"]["warnings"], filename)

    def test_contract_load_failure_is_structured(self) -> None:
        report = native_io_validate.load_failure_report(
            "cannot load contract: sampler has no buffer",
            Path("candidate"),
            Path("engine.json"),
            Path("overrides.json"),
        )
        self.assertEqual(1, report["summary"]["errors"])
        self.assertEqual("contract.load-failed", report["issues"][0]["code"])
        self.assertEqual(
            "cannot load contract: sampler has no buffer",
            report["issues"][0]["message"],
        )
        self.assertEqual("overrides.json", Path(report["overridesPath"]).name)

    def test_attribute_semantic_not_provided(self) -> None:
        candidate = self.candidate()
        material = candidate["materials"]["ColorPostProcessing"]
        material["attributes"]["UNKNOWN_SEMANTIC"] = ["vec4"]
        material["passes"]["ColorPostProcess"]["attributes"].append("UNKNOWN_SEMANTIC")
        report = native_io_validate.validate(candidate, self.engine)
        self.assertIn("attribute.semantic-not-provided", self.codes(report))
        self.assertGreater(report["summary"]["errors"], 0)

    def test_attribute_type_mismatch(self) -> None:
        candidate = self.candidate()
        candidate["materials"]["ColorPostProcessing"]["attributes"]["POSITION"] = ["ivec4"]
        report = native_io_validate.validate(candidate, self.engine)
        self.assertIn("attribute.type-mismatch", self.codes(report))

    def test_buffer_slot_mismatches(self) -> None:
        invalid = self.candidate()
        invalid["materials"]["ColorPostProcessing"]["buffers"]["ColorTexture"][
            "bindingSlot"
        ] = "not-a-slot"
        report = native_io_validate.validate(invalid, self.engine)
        self.assertIn("buffer.invalid-slot", self.codes(report))

        missing = self.candidate()
        missing["materials"]["ColorPostProcessing"]["buffers"]["ColorTexture"][
            "bindingSlot"
        ] = 999
        report = native_io_validate.validate(missing, self.engine)
        self.assertIn("buffer.slot-not-provided", self.codes(report))

    def test_buffer_name_slot_mismatch(self) -> None:
        candidate = self.candidate()
        buffers = candidate["materials"]["ColorPostProcessing"]["buffers"]
        buffers["WrongTextureName"] = buffers.pop("ColorTexture")
        report = native_io_validate.validate(candidate, self.engine)
        self.assertIn("buffer.name-slot-mismatch", self.codes(report))

    def test_buffer_type_format_access_mismatches(self) -> None:
        candidate = self.candidate()
        buffer = candidate["materials"]["ColorPostProcessing"]["buffers"]["ColorTexture"]
        buffer["type"] = "texture3D"
        report = native_io_validate.validate(candidate, self.engine)
        self.assertIn("buffer.type-mismatch", self.codes(report))

        candidate = self.candidate()
        buffer = candidate["materials"]["ColorPostProcessing"]["buffers"]["ColorTexture"]
        buffer["format"] = "uint"
        report = native_io_validate.validate(candidate, self.engine)
        self.assertIn("buffer.format-mismatch", self.codes(report))

        candidate = self.candidate()
        buffer = candidate["materials"]["ColorPostProcessing"]["buffers"]["ColorTexture"]
        buffer["unorderedAccess"] = True
        report = native_io_validate.validate(candidate, self.engine)
        self.assertIn("buffer.access-mismatch", self.codes(report))

    def test_signature_mismatch_is_rejected(self) -> None:
        candidate = self.candidate()
        material = candidate["materials"]["ColorPostProcessing"]
        material["passes"]["ColorPostProcess"]["outputBindingSignature"] = "0xDEADBEEF"
        report = native_io_validate.validate(candidate, self.engine)
        self.assertTrue(
            any(
                item["code"] == "pass.output-signature-mismatch"
                for item in report["issues"]
            )
        )

    def test_explicit_override_and_unused_warning(self) -> None:
        candidate = self.candidate()
        candidate["materials"]["ColorPostProcessing"]["passes"]["ColorPostProcess"][
            "outputBindingSignature"
        ] = "0xDEADBEEF"
        override = {
            "id": "intentional-color-signature",
            "code": "pass.output-signature-mismatch",
            "material": "ColorPostProcessing",
            "pass": "ColorPostProcess",
            "reason": "Test fixture",
        }
        report = native_io_validate.apply_overrides(
            native_io_validate.validate(candidate, self.engine),
            {"version": 1, "allow": [override]},
        )
        self.assertEqual(0, report["summary"]["errors"])
        self.assertEqual(1, report["summary"]["overridesApplied"])
        self.assertEqual(1, len(report["overriddenIssues"]))
        overridden = report["overriddenIssues"][0]
        self.assertEqual("pass.output-signature-mismatch", overridden["code"])
        self.assertEqual("intentional-color-signature", overridden["overrideId"])
        self.assertEqual("Test fixture", overridden["reason"])

        unused = dict(override)
        unused["id"] = "unused"
        unused["pass"] = "DifferentPass"
        report = native_io_validate.apply_overrides(
            native_io_validate.validate(
                native_io_validate.load_contract(
                    ROOT / "samples" / "compat" / "deferred.material-io.json"
                ),
                self.engine,
            ),
            {"version": 1, "allow": [unused]},
        )
        self.assertTrue(
            any(item["code"] == "override.unused" for item in report["issues"])
        )
        self.assertEqual(1, report["summary"]["errors"])

    def test_override_validation_rejects_duplicate_id_and_unknown_code(self) -> None:
        duplicate = {
            "version": 1,
            "allow": [
                {
                    "id": "same-id",
                    "code": "pass.output-signature-mismatch",
                    "material": "ColorPostProcessing",
                    "pass": "ColorPostProcess",
                    "reason": "first",
                },
                {
                    "id": "same-id",
                    "code": "buffer.type-mismatch",
                    "material": "ColorPostProcessing",
                    "buffer": "ColorTexture",
                    "reason": "second",
                },
            ],
        }
        unknown = {
            "version": 1,
            "allow": [
                {
                    "id": "unknown-code",
                    "code": "pass.not-a-real-code",
                    "material": "ColorPostProcessing",
                    "pass": "ColorPostProcess",
                    "reason": "negative test",
                }
            ],
        }
        directory = ROOT / "build" / "native-io-override-selftest"
        shutil.rmtree(directory, ignore_errors=True)
        directory.mkdir(parents=True, exist_ok=True)
        try:
            duplicate_path = directory / "duplicate.json"
            unknown_path = directory / "unknown.json"
            duplicate_path.write_text(json.dumps(duplicate), encoding="utf-8")
            unknown_path.write_text(json.dumps(unknown), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "id is duplicated"):
                native_io_validate.load_overrides(duplicate_path)
            with self.assertRaisesRegex(ValueError, "code is not a known IO issue"):
                native_io_validate.load_overrides(unknown_path)
        finally:
            shutil.rmtree(directory, ignore_errors=True)


class NativeIOMappingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.engine = native_io_validate.load_contract(
            ROOT / "samples" / "compat" / "RenderDragon-26.51.vanilla.material-io.json"
        )

    @staticmethod
    def candidate() -> dict:
        return native_io_validate.load_contract(
            ROOT / "samples" / "compat" / "deferred.material-io.json"
        )

    def test_attribute_semantic_mapping(self) -> None:
        candidate = self.candidate()
        material = candidate["materials"]["ColorPostProcessing"]
        material["attributes"]["CUSTOM_POSITION"] = material["attributes"].pop("POSITION")
        attributes = material["passes"]["ColorPostProcess"]["attributes"]
        attributes[attributes.index("POSITION")] = "CUSTOM_POSITION"
        mapping = {
            "version": 1,
            "target": "RenderDragon-26.51",
            "attributes": [
                {
                    "id": "position-alias",
                    "material": "ColorPostProcessing",
                    "pass": "ColorPostProcess",
                    "sourceSemantic": "CUSTOM_POSITION",
                    "targetSemantic": "POSITION",
                    "reason": "Explicit native semantic mapping fixture",
                }
            ],
        }
        report = native_io_map.validate_mapping(mapping, candidate, self.engine)
        self.assertEqual(1, report["summary"]["applied"])
        self.assertEqual(0, report["summary"]["mappingErrors"])
        self.assertEqual(0, report["summary"]["remainingErrors"])

    def test_buffer_name_mapping(self) -> None:
        candidate = self.candidate()
        buffers = candidate["materials"]["ColorPostProcessing"]["buffers"]
        buffers["WrongTextureName"] = buffers.pop("ColorTexture")
        mapping = {
            "version": 1,
            "buffers": [
                {
                    "id": "color-texture-alias",
                    "material": "ColorPostProcessing",
                    "sourceBuffer": "WrongTextureName",
                    "targetBuffer": "ColorTexture",
                    "reason": "Explicit native buffer mapping fixture",
                }
            ],
        }
        report = native_io_map.validate_mapping(mapping, candidate, self.engine)
        self.assertEqual(1, report["summary"]["applied"])
        self.assertEqual(0, report["summary"]["mappingErrors"])
        self.assertEqual(0, report["summary"]["remainingErrors"])

    def test_pass_signature_mapping(self) -> None:
        candidate = self.candidate()
        candidate["materials"]["ColorPostProcessing"]["passes"]["ColorPostProcess"][
            "outputBindingSignature"
        ] = "0xDEADBEEF"
        mapping = {
            "version": 1,
            "passes": [
                {
                    "id": "color-target-alias",
                    "material": "ColorPostProcessing",
                    "pass": "ColorPostProcess",
                    "targetRenderTargets": ["Color0"],
                    "reason": "Verified pass output mapping fixture",
                }
            ],
        }
        report = native_io_map.validate_mapping(mapping, candidate, self.engine)
        self.assertEqual(1, report["summary"]["applied"])
        self.assertEqual(0, report["summary"]["mappingErrors"])
        self.assertEqual(0, report["summary"]["remainingErrors"])

    def test_unused_and_unsafe_mappings_are_rejected(self) -> None:
        unused = {
            "version": 1,
            "buffers": [
                {
                    "id": "unused-buffer",
                    "material": "ColorPostProcessing",
                    "sourceBuffer": "MissingTexture",
                    "targetBuffer": "ColorTexture",
                    "reason": "Unused mapping fixture",
                }
            ],
        }
        report = native_io_map.validate_mapping(unused, self.candidate(), self.engine)
        self.assertEqual("mapping.unused", report["errors"][0]["code"])

        candidate = self.candidate()
        buffers = candidate["materials"]["ColorPostProcessing"]["buffers"]
        buffers["WrongShape"] = buffers.pop("ColorTexture")
        buffers["WrongShape"]["type"] = "texture3D"
        unsafe = {
            "version": 1,
            "buffers": [
                {
                    "id": "unsafe-buffer",
                    "material": "ColorPostProcessing",
                    "sourceBuffer": "WrongShape",
                    "targetBuffer": "ColorTexture",
                    "reason": "Unsafe mapping fixture",
                }
            ],
        }
        report = native_io_map.validate_mapping(unsafe, candidate, self.engine)
        self.assertEqual("mapping.buffer-field-mismatch", report["errors"][0]["code"])

    def test_mapping_document_rejects_duplicates_and_invalid_targets(self) -> None:
        duplicate = {
            "version": 1,
            "attributes": [
                {
                    "id": "same-id",
                    "material": "ColorPostProcessing",
                    "pass": "ColorPostProcess",
                    "sourceSemantic": "POSITION",
                    "targetSemantic": "NORMAL",
                    "reason": "first",
                }
            ],
            "buffers": [
                {
                    "id": "same-id",
                    "material": "ColorPostProcessing",
                    "sourceBuffer": "ColorTexture",
                    "targetBuffer": "RasterColor",
                    "reason": "second",
                }
            ],
        }
        with self.assertRaisesRegex(ValueError, "id is duplicated"):
            native_io_map.validate_mapping_document(duplicate)

        invalid_target = {
            "version": 1,
            "passes": [
                {
                    "id": "invalid-target",
                    "material": "ColorPostProcessing",
                    "pass": "ColorPostProcess",
                    "targetRenderTargets": ["Color4"],
                    "reason": "invalid target fixture",
                }
            ],
        }
        report = native_io_map.validate_mapping(
            invalid_target,
            self.candidate(),
            self.engine,
        )
        self.assertEqual("mapping.invalid-target", report["errors"][0]["code"])

    def test_native_mapping_catalog(self) -> None:
        catalog = native_io_map.build_mapping_catalog(self.engine)
        self.assertEqual(
            {"materials": 157, "attributes": 13, "buffers": 121, "renderTargets": 3},
            catalog["summary"],
        )
        self.assertIn("POSITION", catalog["attributes"])
        self.assertIn("ColorTexture", catalog["buffers"])
        self.assertEqual(
            {"Color0", "Color1", "Color2"},
            set(catalog["renderTargets"]),
        )
        self.assertEqual([], catalog["verifiedAliases"])
        with self.assertRaisesRegex(ValueError, "target attribute NOPE"):
            native_io_map.validate_catalog_targets(
                {
                    "version": 1,
                    "attributes": [
                        {
                            "id": "bad-target",
                            "material": "ColorPostProcessing",
                            "pass": "ColorPostProcess",
                            "sourceSemantic": "POSITION",
                            "targetSemantic": "NOPE",
                            "reason": "catalog negative fixture",
                        }
                    ],
                },
                catalog,
            )

    def test_binary_apply_rewrites_buffer_and_pass_atomically(self) -> None:
        root = ROOT / "build" / "native-io-map-apply-selftest"
        candidate_dir = root / "candidate"
        engine_dir = root / "engine"
        output_dir = root / "mapped"
        shutil.rmtree(root, ignore_errors=True)
        candidate_dir.mkdir(parents=True)
        engine_dir.mkdir(parents=True)
        try:
            def material(buffer_name: str, signature: int) -> Material:
                value = Material()
                value.version = 26
                value.name = "ColorPostProcessing"
                buffer = Buffer()
                buffer.name = buffer_name
                buffer.register_slot = 1
                buffer.binding_slot = 1
                buffer.type = BufferType.texture2D
                buffer.texture_format = "float"
                buffer.access = BufferAccess.readonly
                buffer.unordered_access = False
                value.buffers = [buffer]
                shader_pass = Pass()
                shader_pass.name = "ColorPostProcess"
                shader_pass.output_binding_signature = signature
                value.passes = [shader_pass]
                return value

            from io import BytesIO

            def write(path: Path, value: Material) -> None:
                stream = BytesIO()
                value.write(stream)
                path.write_bytes(stream.getvalue())

            wrong_signature = native_io_map.util.get_output_binding_signature(["Color1"])
            write(
                candidate_dir / "ColorPostProcessing.material.bin",
                material("WrongTextureName", wrong_signature),
            )
            write(engine_dir / "ColorPostProcessing.material.bin", material("ColorTexture", 0x4BA175A2))
            mapping = {
                "version": 1,
                "buffers": [
                    {
                        "id": "apply-buffer",
                        "material": "ColorPostProcessing",
                        "sourceBuffer": "WrongTextureName",
                        "targetBuffer": "ColorTexture",
                        "reason": "binary apply fixture",
                    }
                ],
                "passes": [
                    {
                        "id": "apply-pass",
                        "material": "ColorPostProcessing",
                        "pass": "ColorPostProcess",
                        "targetRenderTargets": ["Color0"],
                        "reason": "binary apply fixture",
                    }
                ],
            }
            report = native_io_map.apply_binary_mapping(
                mapping,
                candidate_dir,
                engine_dir,
                output_dir,
                self.engine,
            )
            self.assertEqual(2, report["summary"]["applied"])
            self.assertEqual(0, report["summary"]["remainingErrors"])
            mapped = native_io_validate.load_contract(output_dir)
            material_contract = mapped["materials"]["ColorPostProcessing"]
            self.assertIn("ColorTexture", material_contract["buffers"])
            self.assertEqual(
                "0x4BA175A2",
                material_contract["passes"]["ColorPostProcess"]["outputBindingSignature"],
            )
        finally:
            shutil.rmtree(root, ignore_errors=True)

    def test_bgfx_source_attribute_mapping(self) -> None:
        root = ROOT / "build" / "native-io-source-bgfx"
        material_dir = root / "materials" / "ColorPostProcessing"
        shutil.rmtree(root, ignore_errors=True)
        material_dir.mkdir(parents=True)
        try:
            varying = material_dir / "varying.def.sc"
            varying.write_text(
                "vec3 a_custom_position : CUSTOM_POSITION;\n",
                encoding="utf-8",
            )
            vertex = material_dir / "vertex.sc"
            vertex.write_text(
                "$input a_custom_position, a_texcoord0\n"
                "void main() { gl_Position = vec4(a_custom_position, 1.0); }\n",
                encoding="utf-8",
            )
            mapping = {
                "version": 1,
                "attributes": [
                    {
                        "id": "source-position",
                        "material": "ColorPostProcessing",
                        "pass": "ColorPostProcess",
                        "sourceSemantic": "CUSTOM_POSITION",
                        "targetSemantic": "POSITION",
                        "sourceName": "a_custom_position",
                        "targetName": "a_position",
                        "reason": "source rewrite fixture",
                    }
                ],
            }
            report = native_io_map.apply_source_attributes(mapping, root, self.engine)
            self.assertEqual(1, report["summary"]["applied"])
            self.assertEqual("attributes", report["applied"][0]["section"])
            self.assertEqual("source rewrite fixture", report["applied"][0]["reason"])
            self.assertIn("a_position : POSITION", varying.read_text(encoding="utf-8"))
            self.assertIn("$input a_position", vertex.read_text(encoding="utf-8"))
        finally:
            shutil.rmtree(root, ignore_errors=True)

    def test_albite_source_mapping_is_local_and_transactional(self) -> None:
        root = ROOT / "build" / "native-io-source-albite"
        shaders = root / "materials" / "ColorPostProcessing" / "shaders"
        shutil.rmtree(root, ignore_errors=True)
        shaders.mkdir(parents=True)
        try:
            vertex = shaders / "shader.vert"
            original = "#version 450\nin vec3 a_custom_position;\nvoid main() {}\n"
            vertex.write_text(original, encoding="utf-8")
            mapping = {
                "version": 1,
                "attributes": [
                    {
                        "id": "albite-position",
                        "material": "ColorPostProcessing",
                        "pass": "ColorPostProcess",
                        "sourceSemantic": "CUSTOM_POSITION",
                        "targetSemantic": "POSITION",
                        "sourceName": "a_custom_position",
                        "targetName": "a_position",
                        "reason": "source rewrite fixture",
                    }
                ],
            }
            report = native_io_map.apply_source_attributes(mapping, root, self.engine)
            rewritten = vertex.read_text(encoding="utf-8")
            self.assertEqual(1, report["summary"]["applied"])
            self.assertEqual("attributes", report["applied"][0]["section"])
            self.assertIn("#define a_custom_position a_position", rewritten)
            self.assertLess(
                rewritten.index("#version 450"),
                rewritten.index("#define a_custom_position a_position"),
            )

            invalid = json.loads(json.dumps(mapping))
            invalid["attributes"][0]["id"] = "unused-source"
            invalid["attributes"][0]["sourceName"] = "missing_input"
            with self.assertRaises(ValueError):
                native_io_map.apply_source_attributes(invalid, root, self.engine)
            self.assertEqual(rewritten, vertex.read_text(encoding="utf-8"))
        finally:
            shutil.rmtree(root, ignore_errors=True)

    def test_source_mapping_breaks_staging_hardlinks(self) -> None:
        root = ROOT / "build" / "native-io-source-hardlink"
        original_dir = root / "original" / "materials" / "ColorPostProcessing"
        staging_dir = root / "staging" / "materials" / "ColorPostProcessing"
        shutil.rmtree(root, ignore_errors=True)
        original_dir.mkdir(parents=True)
        staging_dir.mkdir(parents=True)
        try:
            original_varying = original_dir / "varying.def.sc"
            original_vertex = original_dir / "vertex.sc"
            original_varying.write_text(
                "vec3 a_custom_position : CUSTOM_POSITION;\n",
                encoding="utf-8",
            )
            original_vertex.write_text("$input a_custom_position\n", encoding="utf-8")
            staging_varying = staging_dir / "varying.def.sc"
            staging_vertex = staging_dir / "vertex.sc"
            os.link(original_varying, staging_varying)
            os.link(original_vertex, staging_vertex)
            mapping = {
                "version": 1,
                "attributes": [
                    {
                        "id": "hardlink-position",
                        "material": "ColorPostProcessing",
                        "pass": "ColorPostProcess",
                        "sourceSemantic": "CUSTOM_POSITION",
                        "targetSemantic": "POSITION",
                        "sourceName": "a_custom_position",
                        "targetName": "a_position",
                        "reason": "hardlink isolation fixture",
                    }
                ],
            }
            native_io_map.apply_source_attributes(
                mapping,
                root / "staging",
                self.engine,
            )
            self.assertIn(
                "a_custom_position : CUSTOM_POSITION",
                original_varying.read_text(encoding="utf-8"),
            )
            self.assertIn("$input a_custom_position", original_vertex.read_text(encoding="utf-8"))
            self.assertIn("a_position : POSITION", staging_varying.read_text(encoding="utf-8"))
            self.assertIn("$input a_position", staging_vertex.read_text(encoding="utf-8"))
        finally:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
