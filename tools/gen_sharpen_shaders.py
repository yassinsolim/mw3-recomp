#!/usr/bin/env python3
"""Writes the SPIR-V of the fixed shaders in runtime/gpu/vulkan/shaders as
arrays of words: sharpen_shaders.inc, the two that sharpen the frame on its
way to the window (runtime/gpu/vulkan/sharpen.cpp), and selftest_shaders.inc,
the three of the driver self-test (runtime/android/driver_selftest.cpp).

The runtime writes its other SPIR-V itself (spirv.h), because those shaders
are made while the title runs. These are fixed, and the fragment shader is a
published filter kept as its authors wrote it, so they are GLSL compiled once
and the result is committed: no build needs a GLSL compiler. Run it after
changing a shader there:

    python3 tools/gen_sharpen_shaders.py

It needs glslc: the one named by GLSLC, or on the PATH, or the Android NDK's
(shader-tools/<host>/glslc under ANDROID_NDK_HOME or ANDROID_NDK_ROOT).
`--check` writes nothing and fails when the committed file is not what the
sources compile to.
"""
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = os.path.join(ROOT, "runtime", "gpu", "vulkan", "shaders")
# Each file written: the arrays' names and the files they are compiled from.
OUTPUTS = [
    ("sharpen_shaders.inc", [
        ("kSharpenVertex", "sharpen.vert"),
        ("kSharpenFragment", "sharpen.frag"),
    ]),
    ("selftest_shaders.inc", [
        ("kSelfTestVertex", "selftest.vert"),
        ("kSelfTestFragment", "selftest.frag"),
        ("kSelfTestCompute", "selftest.comp"),
    ]),
]


def find_glslc():
    named = os.environ.get("GLSLC")
    if named:
        return named
    found = shutil.which("glslc")
    if found:
        return found
    for variable in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"):
        ndk = os.environ.get(variable)
        if ndk:
            for candidate in glob.glob(os.path.join(ndk, "shader-tools", "*", "glslc*")):
                return candidate
    sys.exit("glslc not found: set GLSLC, or ANDROID_NDK_HOME to an NDK")


def compile_shader(glslc, name):
    with tempfile.TemporaryDirectory() as folder:
        out = os.path.join(folder, "out.spv")
        # Vulkan 1.1 is the least the renderer runs on (docs/android.md).
        subprocess.run([glslc, "--target-env=vulkan1.1", "-O", "-o", out, os.path.join(SOURCES, name)],
                       check=True)
        with open(out, "rb") as spv:
            data = spv.read()
    return struct.unpack("<%dI" % (len(data) // 4), data)


def main():
    glslc = find_glslc()
    for file, shaders in OUTPUTS:
        output = os.path.join(SOURCES, file)
        lines = ["// Written by tools/gen_sharpen_shaders.py from the shaders beside it; not edited by hand.", ""]
        for array, name in shaders:
            words = compile_shader(glslc, name)
            lines.append("// %s" % name)
            lines.append("constexpr uint32_t %s[] = {" % array)
            for i in range(0, len(words), 8):
                lines.append("    " + " ".join("0x%08x," % word for word in words[i:i + 8]))
            lines.append("};")
            lines.append("")
        text = "\n".join(lines)
        if "--check" in sys.argv[1:]:
            with open(output, newline="") as committed:
                if committed.read() != text:
                    sys.exit("%s is not what the shaders compile to: run tools/gen_sharpen_shaders.py" % output)
            continue
        with open(output, "w", newline="\n") as out:
            out.write(text)
        print("wrote %s" % os.path.relpath(output, ROOT))


if __name__ == "__main__":
    main()
