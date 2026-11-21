#!/usr/bin/env python3
"""Opt-in native worker protocol tests. No model files or inference are required."""
import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib

WORKER = str(Path(sys.argv.pop(1)).resolve())


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="vulkana-diffusion-")
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([WORKER, "--work-dir", self.directory.name],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.log, text=True, bufsize=1)
        self.messages = queue.Queue()
        self.counter = 0
        def read():
            for line in self.process.stdout:
                try:
                    self.messages.put(json.loads(line))
                except ValueError:
                    self.messages.put({"invalid_stdout": line})
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()
        self.hello = self.messages.get(timeout=30)
        self.assertEqual(self.hello.get("event"), "hello")
        self.assertEqual(self.hello["protocol"], 1)

    def tearDown(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                self.fail("Worker failed to shut down after EOF")
        self.reader.join(timeout=2)
        self.process.stdin.close()
        self.process.stdout.close()
        self.log.close()
        self.directory.cleanup()

    def call(self, method, params=None):
        self.counter += 1
        ident = f"test-{self.counter}"
        self.process.stdin.write(json.dumps({"protocol": 1, "id": ident,
                                            "method": method, "params": params or {}}) + "\n")
        self.process.stdin.flush()
        while True:
            result = self.messages.get(timeout=30)
            self.assertNotIn("invalid_stdout", result)
            if result.get("id") == ident:
                return result

    def test_discovery_and_lifecycle(self):
        self.assertTrue(self.hello["generationStages"])
        self.assertTrue(self.hello["devices"])
        self.assertIn("euler", self.hello["samplers"])
        self.assertTrue(any(p["id"] == "qwen-image-2.1-turbo" for p in self.hello["profiles"]))
        krea = next(p for p in self.hello["profiles"] if p["id"] == "krea-2-turbo")
        self.assertEqual(krea["modes"], ["generate"])
        self.assertFalse(krea["referenceImages"])
        self.assertEqual(krea["spatialGuidance"], "none")
        self.assertEqual(krea["steps"], 8)
        self.assertEqual(krea["guidance"], 1)
        self.assertEqual(krea["scheduler"], "simple")
        self.assertEqual(krea["alignment"], 8)
        h3 = next(p for p in self.hello["profiles"] if p["id"] == "minimax-h3-image")
        self.assertEqual(h3["modes"], ["generate", "edit"])
        self.assertEqual(h3["engineModel"], "MiniMax-H3-Image")
        self.assertEqual(h3["maxReferenceImages"], 16)
        self.assertFalse(h3["imageToImage"])
        self.assertTrue(h3["visionRequiredForReferences"])
        self.assertTrue(self.hello["layerStackResults"])
        layered = next(p for p in self.hello["profiles"] if p["id"] == "qwen-image-layered")
        self.assertEqual(layered["modes"], ["decompose"])
        self.assertEqual(layered["engineModel"], "Qwen Image Layered")
        self.assertEqual(layered["maxReferenceImages"], 1)
        self.assertTrue(layered["rgba"])
        self.assertFalse(layered["imageToImage"])
        self.assertFalse(layered["visionRequiredForReferences"])
        self.assertEqual((layered["sampler"], layered["scheduler"], layered["steps"], layered["guidance"]),
                         ("euler", "discrete", 20, 4))
        flux = next(p for p in self.hello["profiles"] if p["id"] == "flux3-image-klein-distilled")
        self.assertEqual(flux["alignment"], 16)
        qwen = next(p for p in self.hello["profiles"] if p["id"] == "qwen-image-2.1-turbo")
        self.assertEqual(qwen["alignment"], 32)
        sd = next(p for p in self.hello["profiles"] if p["id"] == "stable-diffusion")
        self.assertTrue(sd["checkpoint"])
        self.assertEqual(sd["modes"], ["generate"])
        self.assertFalse(sd["referenceImages"])
        self.assertIn("SD 1.x", sd["engineModels"])
        self.assertIn("SDXL", sd["engineModels"])
        self.assertIn(sd["sampler"], self.hello["samplers"])
        self.assertIn(sd["scheduler"], self.hello["schedulers"])
        variants = [p for p in self.hello["profiles"] if p.get("familyGroup") == "flux-klein"]
        self.assertEqual(len(variants), 4)
        for variant in variants:
            self.assertIn(variant["sampler"], self.hello["samplers"])
            self.assertIn(variant["scheduler"], self.hello["schedulers"])
        status = self.call("status")["result"]
        self.assertFalse(status["loaded"])
        self.assertEqual(status["loadedModels"], [])
        self.assertFalse(self.call("cancel")["result"]["requested"])
        self.assertTrue(self.call("unload")["ok"])
        self.assertEqual(self.call("status")["result"]["loadedModels"], [])
        self.assertTrue(self.call("shutdown")["ok"])
        self.assertEqual(self.process.wait(timeout=10), 0)

    def test_validation_and_recovery(self):
        errors = [self.call("unknown"), self.call("status", {"unknown": True}),
                  self.call("load", {"profile": "unknown"}),
                  self.call("generate", {"prompt": "A test image"}),
                  self.call("load", {"profile": "qwen-image-2.1-turbo", "files": {}})]
        for error in errors:
            self.assertFalse(error["ok"])
            self.assertTrue(error["error"]["code"])
            self.assertTrue(error["error"]["message"])
        self.assertTrue(self.call("ping")["ok"])
        self.assertFalse(self.call("status")["result"]["loaded"])
        self.assertEqual(self.call("status")["result"]["loadedModels"], [])

    def test_pointer_rejection(self):
        pointer = Path(self.directory.name) / "model.gguf"
        pointer.write_text("version https://git-lfs.github.com/spec/v1\noid sha256:abc\nsize 1000\n")
        result = self.call("load", {"profile": "qwen-image-2.1-turbo", "files": {
            "diffusion": str(pointer), "textEncoder": str(pointer), "vae": str(pointer)}})
        self.assertFalse(result["ok"])
        self.assertIn("Git LFS pointer", result["error"]["message"])
        self.assertTrue(pointer.exists())
        self.assertTrue(self.call("ping")["ok"])

    @unittest.skipUnless(os.getenv("VULKANA_DIFFUSION_TEST_LAYERED_LOAD"),
                         "Set a local Qwen-Image-Layered load configuration to test request rejection.")
    def test_layered_request_validation(self):
        load = json.loads(Path(os.environ["VULKANA_DIFFUSION_TEST_LAYERED_LOAD"]).read_text())
        self.assertEqual(load["profile"], "qwen-image-layered")
        loaded = self.call("load", load)
        self.assertTrue(loaded["ok"], loaded)
        # A valid 1 × 1 RGBA image lets us exercise the final size check without running inference.
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        image = Path(self.directory.name, "pixel.png")
        image.write_bytes(b"\x89PNG\r\n\x1a\n" +
                          chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) +
                          chunk(b"IDAT", zlib.compress(b"\0\xff\0\0\xff")) + chunk(b"IEND", b""))
        params = {"prompt": "A red square.", "width": 640, "height": 640,
                  "layerCount": 4, "referenceImages": [str(image)]}
        for override, expected in [
            ({"layerCount": 1}, "layerCount"), ({"layerCount": 11}, "layerCount"),
            ({"layerCount": 2.5}, "integer"), ({"batch": 2}, "one image stack"),
            ({"referenceImages": []}, "one source image"),
            ({"referenceImages": [str(image)] * 2}, "one source image"),
            ({"width": 4096, "height": 4096}, "64 megapixels"),
            ({"referenceMaxSize": 128}, "omit referenceMaxSize"),
            ({"initialImage": str(image)}, "denoising"), ({"mask": str(image)}, "Generative Fill"),
            ({"prompt": ""}, "processing dimensions"), ({"prompt": "  \n"}, "processing dimensions"),
            ({}, "processing dimensions"),
        ]:
            with self.subTest(override=override):
                reply = self.call("generate", {**params, **override})
                self.assertEqual(reply["error"]["code"], "invalid_arguments", reply)
                self.assertIn(expected, reply["error"]["message"])
        self.assertTrue(self.call("status")["result"]["loaded"])
        self.assertFalse(any(Path(self.directory.name).glob("job-*")))

    def test_checkpoint_optional_components_validation(self):
        checkpoint = Path(self.directory.name) / "checkpoint.safetensors"
        checkpoint.write_bytes(b"test fixture, not loaded")
        load = {"profile": "stable-diffusion", "files": {"diffusion": str(checkpoint)},
                "settings": {"threads": -1}}
        error = self.call("load", load)["error"]
        self.assertEqual(error["code"], "invalid_arguments")
        self.assertIn("threads", error["message"])  # No missing encoder/VAE requirement.
        load["files"]["vae"] = str(Path(self.directory.name) / "missing-vae.safetensors")
        self.assertIn("vae must be", self.call("load", load)["error"]["message"])
        self.assertTrue(self.call("ping")["ok"])

    def test_malformed_input_stays_on_protocol(self):
        self.process.stdin.write("invalid JSON\n")
        self.process.stdin.flush()
        result = self.messages.get(timeout=10)
        self.assertEqual(result["error"]["code"], "invalid_json")
        self.assertTrue(self.call("ping")["ok"])

    def test_unrecognized_architecture_names_diffusion_file(self):
        path = Path(self.directory.name) / "unknown-layout.safetensors"
        header = json.dumps({"unknown.weight": {"dtype": "F32", "shape": [1],
                                                "data_offsets": [0, 4]}}).encode()
        path.write_bytes(struct.pack("<Q", len(header)) + header + struct.pack("<f", 1))
        for profile, files in (("stable-diffusion", {"diffusion": str(path)}),
                               ("flux3-image-klein-distilled", {key: str(path) for key in
                                   ("diffusion", "textEncoder", "vae")})):
            with self.subTest(profile=profile):
                result = self.call("load", {"profile": profile, "files": files})
                self.assertFalse(result["ok"])
                self.assertEqual(result["error"]["code"], "unsupported_model_format")
                self.assertIn("Unrecognized model architecture", result["error"]["message"])
                self.assertIn(path.name, result["error"]["message"])
                self.assertNotIn("---", result["error"]["message"])
                self.assertNotIn("''", result["error"]["message"])
                self.assertTrue(self.call("ping")["ok"])
                self.assertFalse(self.call("status")["result"]["loaded"])

    def test_unsupported_convrot_is_recoverable(self):
        # Tiny, structurally valid weights; reject the quantization during header
        # loading, before trying to identify or allocate a complete model.
        for group, width in ((16384, 16384), (0, 256), (1024, 256)):
            with self.subTest(group=group, width=width):
                metadata = json.dumps({"format": "int8_tensorwise", "convrot": True,
                                       "convrot_groupsize": group}).encode()
                tensors = {"layer.comfy_quant": ("U8", [len(metadata)], metadata),
                           "layer.weight": ("I8", [4, width], bytes(4 * width)),
                           "layer.weight_scale": ("F32", [4], struct.pack("<4f", 1, 1, 1, 1))}
                header, payload = {}, b""
                for name, (dtype, shape, data) in tensors.items():
                    header[name] = {"dtype": dtype, "shape": shape,
                                    "data_offsets": [len(payload), len(payload) + len(data)]}
                    payload += data
                encoded = json.dumps(header).encode()
                path = Path(self.directory.name) / "unsupported-encoder.safetensors"
                path.write_bytes(struct.pack("<Q", len(encoded)) + encoded + payload)
                result = self.call("load", {"profile": "stable-diffusion",
                    "files": {"diffusion": str(path)}})
                self.assertFalse(result["ok"])
                self.assertEqual(result["error"]["code"], "unsupported_model_format")
                self.assertIn(str(group), result["error"]["message"])
                self.assertIn(path.name, result["error"]["message"])
                self.assertIn("layer.weight", result["error"]["message"])
                self.assertIn("GGUF", result["error"]["message"])
                self.assertTrue(self.call("ping")["ok"])
                self.assertFalse(self.call("status")["result"]["loaded"])
        # A new request must not inherit an earlier compatibility error.
        path.write_bytes(b"broken model")
        result = self.call("load", {"profile": "stable-diffusion", "files": {"diffusion": str(path)}})
        self.assertNotIn("ConvRot", result["error"]["message"])
        self.assertTrue(self.call("ping")["ok"])


if __name__ == "__main__":
    unittest.main()
