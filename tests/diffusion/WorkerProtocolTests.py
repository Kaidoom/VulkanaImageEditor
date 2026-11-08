#!/usr/bin/env python3
"""Opt-in native worker protocol tests. No model files or inference are required."""
import json
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import unittest

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


if __name__ == "__main__":
    unittest.main()
