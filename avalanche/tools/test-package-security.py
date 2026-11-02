#!/usr/bin/env python3
"""Exercise production package transactions and signatures with temporary fixtures."""
import os
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="avalanche-package-check-") as temporary:
    folder = pathlib.Path(temporary)
    binary = folder / "package-check"
    subprocess.run([
        os.environ.get("CC", "gcc"), "-no-pie", "-O2", "-flto",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-Iavalanche/include", "avalanche/tools/test-package-recovery.c",
        "avalanche/src/sha256.c", "avalanche/build/bearssl.a",
        "-o", str(binary),
    ], cwd=root, check=True)
    key, certificate = folder / "key.pem", folder / "certificate.pem"
    subprocess.run([
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", str(key), "-out", str(certificate),
        "-subj", "/CN=Temporary-package-check", "-days", "1",
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    der, signature = folder / "certificate.der", folder / "manifest.sig"
    subprocess.run(["openssl", "x509", "-in", str(certificate), "-outform", "DER",
                    "-out", str(der)], check=True)
    subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(key),
                    "-out", str(signature)], input=b"manifest", check=True)
    subprocess.run([str(binary), str(der), str(signature)], check=True)
