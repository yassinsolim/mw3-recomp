#!/usr/bin/env python3
"""What the renderer would do on a device, from the device's Vulkan report.

vulkan.gpuinfo.org holds the features, limits, extensions and formats real
phones' drivers reported. This reads one of its reports and walks the same
checks the runtime makes as it starts (runtime/gpu/vulkan/pipeline.cpp
CreateDevice, renderer.cpp Initialise, texture_cache.cpp, presenter.cpp), in
the same order, and says for each what the runtime would do:

    PASS      the device has it
    FALLBACK  it does not, and the runtime has another way, which is named
    FAIL      it does not, and the runtime refuses the device, with its reason
    WARN      it does not, the runtime goes on, and something may draw wrong
    UNKNOWN   the report does not say

    check_device_report.py [--cache <dir>] [--summary] <report.json | report id>[=<GPU name>]...

A report is the "profile" JSON the site exports for it
(https://vulkan.gpuinfo.org/api/v3/getprofile.php?id=<id>); a number is
downloaded into --cache (the folder device-reports beside the working
directory by default) the first time. The export names the phone and not its
GPU, so the table's first column is what follows "=", when given. With --summary only the table across
the reports is printed. The exit status is 1 when a report FAILs.

The numbers below are the renderer's own (bindings.h, renderer_state.h): when
one changes there it changes here.
"""
import argparse
import json
import os
import re
import sys
import urllib.request

PROFILE_URL = "https://vulkan.gpuinfo.org/api/v3/getprofile.php?id={}"

# bindings.h
IMAGE_COUNT = 65536
SAMPLER_COUNT = 4096
RECORD_LOCATION = 16
PIXEL_CONSTANT_BYTES = 256 * 16
# renderer_state.h, guest_memory.h
ARENA_WANTED = 512 << 20
ARENA_LEAST = 32 << 20
FRAME_SLOTS = 3
SHADOW_BYTES = 0xC0000000 - 0xA0000000

VENDOR_ARM = 0x13B5
VENDOR_QUALCOMM = 0x5143

# texture_formats.cpp: what a texture of the title's is uploaded as. The block
# compressed ones go to the driver only where it samples them; elsewhere, and
# on any Adreno, bc_decoder.cpp decodes them into the formats under "decoded".
TEXTURE_FORMATS = [
    "R8_UNORM", "R5G6B5_UNORM_PACK16", "R8G8B8A8_UNORM", "R8G8B8A8_SRGB", "A2B10G10R10_UNORM_PACK32",
    "R8G8_UNORM", "R4G4B4A4_UNORM_PACK16", "R16_UNORM", "R16G16_UNORM", "R16G16B16A16_UNORM",
    "R16_SFLOAT", "R16G16_SFLOAT", "R16G16B16A16_SFLOAT", "R32_SFLOAT", "R32G32_SFLOAT",
    "R32G32B32A32_SFLOAT",
]
BC_FORMATS = [
    "BC1_RGBA_UNORM_BLOCK", "BC1_RGBA_SRGB_BLOCK", "BC2_UNORM_BLOCK", "BC2_SRGB_BLOCK",
    "BC3_UNORM_BLOCK", "BC3_SRGB_BLOCK", "BC4_UNORM_BLOCK", "BC5_UNORM_BLOCK",
]
# renderer_targets.cpp: the console's colour surfaces. Both games draw into
# 8_8_8_8 and 2_10_10_10 alone; the others are there for a title that asks.
TARGET_FORMATS_USED = ["R8G8B8A8_UNORM", "A2B10G10R10_UNORM_PACK32"]
TARGET_FORMATS_OTHER = ["R16G16_SNORM", "R16G16B16A16_SNORM", "R16G16_SFLOAT", "R16G16B16A16_SFLOAT",
                        "R32_SFLOAT", "R32G32_SFLOAT"]


class Report:
    def __init__(self, document):
        profile = next(iter(document["profiles"].values()))
        self.label = profile.get("label", "?")
        self.api = tuple(int(part) for part in profile["api-version"].split("."))
        device = next(iter(document["capabilities"].values()))
        self.features = device.get("features", {})
        self.properties = device.get("properties", {})
        self.extensions = device.get("extensions", {})
        self.formats = device.get("formats", {})
        core = self.properties.get("VkPhysicalDeviceProperties", {})
        self.limits = core.get("limits", {})
        self.vendor = core.get("vendorID")
        self.name = core.get("deviceName") or self.label

    def feature(self, name):
        """True, False, or None when no structure in the report has the member."""
        for members in self.features.values():
            if name in members:
                return bool(members[name])
        return None

    def property(self, name):
        if name in self.limits:
            return self.limits[name]
        for members in self.properties.values():
            if isinstance(members, dict) and name in members:
                return members[name]
        return None

    def extension(self, name):
        return name in self.extensions

    def format(self, name, bit):
        entry = self.formats.get("VK_FORMAT_" + name)
        return bool(entry) and "VK_FORMAT_FEATURE_" + bit in entry["VkFormatProperties"]["optimalTilingFeatures"]

    def driver(self):
        return self.property("driverInfo") or self.property("driverName") or "?"

    def release(self):
        """The driver's release, short: Arm's "v1.r54p1-12eac0.<hash>" is r54p1."""
        info = self.driver()
        if info.startswith("v1.r"):
            return info[3:].split("-")[0]
        return info[:24]


class Walk:
    """The checks of one report, in the runtime's order."""

    def __init__(self, report):
        self.report = report
        self.lines = []
        self.refused = None     # the first FAIL's reason: where the runtime stops
        self.fallbacks = []
        self.warnings = []
        self.paths = []

    def add(self, status, item, detail):
        self.lines.append((status, item, detail))
        if status == "FAIL" and not self.refused:
            self.refused = f"{item}: {detail}"
        elif status == "FALLBACK":
            self.fallbacks.append(item)
        elif status == "WARN":
            self.warnings.append(item)

    def need(self, item, have, detail, missing):
        if have is None:
            self.add("UNKNOWN", item, "not in the report")
        elif have:
            self.add("PASS", item, detail)
        else:
            self.add("FAIL", item, missing)

    def prefer(self, item, have, detail, fallback):
        if have is None:
            self.add("UNKNOWN", item, f"not in the report; without it: {fallback}")
        elif have:
            self.add("PASS", item, detail)
        else:
            self.add("FALLBACK", item, fallback)

    def at_least(self, item, name, floor):
        value = self.report.property(name)
        if value is None:
            self.add("UNKNOWN", item, f"{name} is not in the report (needs {floor})")
        elif value >= floor:
            self.add("PASS", item, f"{name} {value} >= {floor}")
        else:
            self.add("FAIL", item, f"{name} {value} < {floor}")
        return value


def walk(report):
    w = Walk(report)
    r = report
    api = r.api
    core12 = api >= (1, 2)
    core13 = api >= (1, 3)

    # ---- pipeline.cpp, CreateDevice ----------------------------------------
    w.need("Vulkan 1.1 (SPIR-V 1.3 modules)", api >= (1, 1), "%d.%d.%d" % api,
           "the device lacks Vulkan 1.1, which the renderer needs")

    w.prefer("robustBufferAccess", r.feature("robustBufferAccess"), "an out-of-range vertex fetch reads zero",
             "none: an out-of-range vertex fetch will fault (the runtime warns)")
    bc = r.feature("textureCompressionBC")
    if r.vendor == VENDOR_QUALCOMM and bc:
        w.add("FALLBACK", "BC textures (textureCompressionBC)",
              "offered, and decoded in software all the same: the Adreno workaround (MW2_ADRENO_BC)")
        bc = False
    else:
        w.prefer("BC textures (textureCompressionBC)", bc, "the driver samples BC1 to BC5",
                 "bc_decoder.cpp decodes BC1 to BC5 into RGBA8, RG8 and R8 at upload")
    w.prefer("occlusionQueryPrecise", r.feature("occlusionQueryPrecise"), "sample counts",
             "a query answers some or none")
    w.prefer("samplerAnisotropy", r.feature("samplerAnisotropy"),
             f"up to {r.property('maxSamplerAnisotropy')}:1", "no anisotropic filtering")
    w.prefer("VK_KHR_image_format_list", r.extension("VK_KHR_image_format_list"), "enabled",
             "a resolve's copy is a mutable image without the list")
    w.prefer("VK_EXT_pipeline_creation_feedback", core13 or r.extension("VK_EXT_pipeline_creation_feedback"),
             "enabled", "the report cannot count pipelines the cache did not hold")
    w.prefer("non-seamless cube maps", bool(r.feature("nonSeamlessCubeMap")), "each face filtered on its own",
             "cube maps blend across their edges")

    rendering = r.feature("dynamicRendering") and (core13 or r.extension("VK_KHR_dynamic_rendering"))
    state = r.feature("extendedDynamicState") and (core13 or r.extension("VK_EXT_extended_dynamic_state"))
    legacy = not rendering or not state
    w.prefer("dynamic rendering and extended dynamic state", not legacy, "pipelines fit every target",
             "legacy mode: VkRenderPass and framebuffers, cull, winding and depth tests baked into each pipeline")
    w.prefer("pipelineCreationCacheControl", bool(r.feature("pipelineCreationCacheControl")),
             "the pipeline cache is several files under a budget", "the pipeline cache is one file")

    indexing = [r.feature("descriptorBindingSampledImageUpdateAfterBind"),
                r.feature("descriptorBindingPartiallyBound"),
                r.feature("descriptorBindingUpdateUnusedWhilePending")]
    if any(have is None for have in indexing) and not core12 and not r.extension("VK_EXT_descriptor_indexing"):
        indexing = [False]      # no extension and no core: the feature query answers no
    w.need("descriptor indexing (update after bind, partially bound, update unused while pending)",
           None if any(have is None for have in indexing) else all(indexing), "all three",
           "the device lacks descriptor indexing, which the renderer needs")
    w.need("shaderSampledImageArrayDynamicIndexing", r.feature("shaderSampledImageArrayDynamicIndexing"),
           "yes", "the device lacks dynamically indexed textures, which the renderer needs")
    parameters = r.feature("shaderDrawParameters")
    if parameters is None and r.extension("VK_KHR_shader_draw_parameters"):
        # A 1.1 report made before the site read the core 1.1 features: the
        # extension is the same feature under its first name.
        w.add("PASS", "shaderDrawParameters", "VK_KHR_shader_draw_parameters is offered (the feature is not in the report)")
    else:
        w.need("shaderDrawParameters", parameters, "yes",
               "the device lacks shader draw parameters, which the renderer needs")
    nonuniform = r.feature("shaderSampledImageArrayNonUniformIndexing")
    if nonuniform is None and not core12 and not r.extension("VK_EXT_descriptor_indexing"):
        nonuniform = False
    w.need("shaderSampledImageArrayNonUniformIndexing", nonuniform, "yes",
           "the device lacks non-uniformly indexed textures, which the renderer needs")
    for name, floor in [("maxPerStageDescriptorUpdateAfterBindSampledImages", IMAGE_COUNT),
                        ("maxDescriptorSetUpdateAfterBindSampledImages", IMAGE_COUNT),
                        ("maxPerStageDescriptorUpdateAfterBindSamplers", SAMPLER_COUNT),
                        ("maxDescriptorSetUpdateAfterBindSamplers", SAMPLER_COUNT),
                        ("maxPerStageDescriptorUpdateAfterBindStorageBuffers", 2),
                        ("maxDescriptorSetUpdateAfterBindStorageBuffers", 2),
                        ("maxPerStageUpdateAfterBindResources", IMAGE_COUNT + 2 + 1),
                        ("maxUpdateAfterBindDescriptorsInAllPools", IMAGE_COUNT + SAMPLER_COUNT)]:
        w.at_least("room for every texture in one set", name, floor)
    for name in ("maxVertexOutputComponents", "maxFragmentInputComponents"):
        w.at_least("seventeen interpolators", name, 4 * (RECORD_LOCATION + 1))

    stages = r.property("subgroupSupportedStages") or r.property("supportedStages")
    operations = r.property("subgroupSupportedOperations") or r.property("supportedOperations")
    if stages is None or operations is None:
        ballot = None
        w.add("UNKNOWN", "subgroup ballot in the fragment stage",
              "not in the report; without it: a pixel shader reads its own draw record and indexes textures"
              " non-uniformly, float constants in a uniform block")
    else:
        ballot = "VK_SHADER_STAGE_FRAGMENT_BIT" in stages and "VK_SUBGROUP_FEATURE_BALLOT_BIT" in operations
        if ballot and r.vendor == VENDOR_ARM:
            # Offered, in subgroups of 16, and not taken: the record a pixel is
            # the form a phone has drawn levels with (pipeline.cpp).
            ballot = False
            w.add("FALLBACK", "subgroup ballot in the fragment stage",
                  "offered, and a pixel shader reads its own draw record all the same: a Mali's default"
                  " (MW2_PIXEL_RECORDS=subgroup)")
        else:
            w.prefer("subgroup ballot in the fragment stage", ballot,
                     f"subgroups of {r.property('subgroupSize')}: a pixel shader runs once for each draw record"
                     " its subgroup holds",
                     "a pixel shader reads its own draw record and indexes textures non-uniformly")
    uniform = not ballot or r.vendor in (VENDOR_QUALCOMM, VENDOR_ARM)
    w.add("PASS" if uniform else "FALLBACK", "pixel float constants",
          "a uniform block, bound when a draw's differ" if uniform else "fetched from the arena by every pixel")

    turnip = r.property("driverID") == "VK_DRIVER_ID_MESA_TURNIP"
    libraries = (not legacy and not turnip and r.extension("VK_KHR_pipeline_library") and
                 r.extension("VK_EXT_graphics_pipeline_library") and bool(r.feature("graphicsPipelineLibrary")))
    w.prefer("graphics pipeline libraries", libraries, "a shader is compiled when the title loads it",
             "whole pipelines, compiled at the first draw or by the warm-up"
             + (" (Turnip faulted building libraries)" if turnip else ""))
    w.prefer("VK_EXT_multi_draw", bool(r.feature("multiDraw")) and r.extension("VK_EXT_multi_draw"),
             "runs of draws without indices are one call", "each such draw is its own call")
    w.prefer("mirror clamp to edge", r.extension("VK_KHR_sampler_mirror_clamp_to_edge") or
             bool(r.feature("samplerMirrorClampToEdge")), "enabled", "such a sampler mirrors and repeats")

    # ---- CreateSetLayouts, the pipeline layout ------------------------------
    w.at_least("three descriptor sets", "maxBoundDescriptorSets", 3)
    w.at_least("set 2's two views of the arena", "maxPerStageDescriptorStorageBuffers", 2)
    w.at_least("the pixel constants' dynamic uniform buffer", "maxDescriptorSetUniformBuffersDynamic", 1)
    w.at_least("256 vec4 in one uniform block", "maxUniformBufferRange", PIXEL_CONSTANT_BYTES)
    w.at_least("the backdrop's push constants (the pipeline layout itself has none)", "maxPushConstantsSize", 64)

    # ---- pipeline cache ------------------------------------------------------
    # runtime/gpu/vulkan/pipeline_cache_rules.h: a Mali's is kept from r38 on.
    release = re.match(r"r(\d+)p", r.release())
    if r.vendor == VENDOR_ARM and not (release and int(release.group(1)) >= 38):
        w.add("FALLBACK", "pipeline cache on disk",
              "kept for the run only under a Mali driver older than r38"
              " (MW2_PERSIST_PIPELINE_CACHE=1, Settings > Keep pipeline cache between runs)")
    else:
        w.add("PASS", "pipeline cache on disk", "written as it grows, read at the next start")

    # ---- renderer.cpp, Initialise --------------------------------------------
    d24 = r.format("D24_UNORM_S8_UINT", "DEPTH_STENCIL_ATTACHMENT_BIT")
    d32 = r.format("D32_SFLOAT_S8_UINT", "DEPTH_STENCIL_ATTACHMENT_BIT")
    if d24:
        w.add("PASS", "depth format", "D24_UNORM_S8_UINT")
    elif d32:
        w.add("FALLBACK", "depth format", "D32_SFLOAT_S8_UINT (no D24_UNORM_S8_UINT)")
    else:
        w.add("FAIL", "depth format", "renderer: no depth format")
    depth = "D24_UNORM_S8_UINT" if d24 else "D32_SFLOAT_S8_UINT"
    if (d24 or d32) and not r.format(depth, "SAMPLED_IMAGE_BIT"):
        w.add("WARN", "depth resolves sampled", f"{depth} cannot be sampled: shadow maps and depth reads fail")

    index = r.property("maxDrawIndexedIndexValue")
    w.prefer("32-bit indices", None if index is None else index >= 0x7FFFFFFF, "every draw table entry",
             f"a draw table holds {((index or 0) >> 16) + 1} entries: more hardware draws")
    resolve = r.property("supportedDepthResolveModes")
    samples = None
    counts = [r.property(name) for name in ("framebufferColorSampleCounts", "framebufferDepthSampleCounts",
                                            "framebufferStencilSampleCounts")]
    if all(c is not None for c in counts):
        samples = set(counts[0]) & set(counts[1]) & set(counts[2])
    if resolve is None or samples is None:
        w.add("UNKNOWN", "multisampling", "not in the report; without it: the title's 2x and 4x surfaces at 1x")
    else:
        usable = "VK_RESOLVE_MODE_SAMPLE_ZERO_BIT" in resolve and samples - {"VK_SAMPLE_COUNT_1_BIT"}
        w.prefer("multisampling (first-sample depth resolve)", bool(usable),
                 "offered: " + " ".join(sorted(s[len("VK_SAMPLE_COUNT_"):-len("_BIT")] + "x" for s in samples))
                 + " (Android leaves it off unless the player turns it on)",
                 "the title's 2x and 4x surfaces are drawn at 1x")

    reach = r.property("maxStorageBufferRange")
    if reach is None:
        w.add("UNKNOWN", "frame arena", "maxStorageBufferRange is not in the report")
    else:
        arena = min(ARENA_WANTED, (reach * FRAME_SLOTS) & ~0xFFFFF)
        if arena < ARENA_LEAST:
            w.add("FAIL", "frame arena", f"renderer: no frame arena (one storage buffer reaches {reach >> 20} MB)")
        else:
            shadow = ARENA_WANTED + SHADOW_BYTES <= reach
            text = (f"one storage buffer reaches {reach >> 20} MB: an arena of up to {arena >> 20} MB"
                    " (Android asks for 192 to 384 by the device's memory)")
            if shadow:
                w.add("PASS", "frame arena and the shadow of guest memory",
                      text + ", and the 512 MB shadow behind it on a device of more than 6 GB")
            else:
                w.add("FALLBACK", "frame arena and the shadow of guest memory",
                      text + "; no shadow: a draw's vertex data is copied into the arena, kept from frame to frame")

    # ---- texture_cache.cpp, renderer_targets.cpp ------------------------------
    missing = [name for name in TEXTURE_FORMATS if not r.format(name, "SAMPLED_IMAGE_BIT")]
    w.need("texture formats sampled", not missing, f"all {len(TEXTURE_FORMATS)}",
           "a texture of these is refused (device cannot sample this format): " + ", ".join(missing))
    unfiltered = [name for name in TEXTURE_FORMATS if r.format(name, "SAMPLED_IMAGE_BIT") and
                  not r.format(name, "SAMPLED_IMAGE_FILTER_LINEAR_BIT")]
    if unfiltered:
        w.add("WARN", "texture formats filtered",
              "sampled but not filtered linearly, where the title's sampler may ask (a match of MW3's"
              " multiplayer uploads none of them): " + ", ".join(unfiltered))
    else:
        w.add("PASS", "texture formats filtered", "all linearly")
    if bc:
        absent = [name for name in BC_FORMATS if not r.format(name, "SAMPLED_IMAGE_BIT")]
        w.need("BC formats sampled", not absent, f"all {len(BC_FORMATS)}",
               "textureCompressionBC is on and these are not sampled: " + ", ".join(absent))

    def draws(name):
        return (r.format(name, "COLOR_ATTACHMENT_BIT") and r.format(name, "COLOR_ATTACHMENT_BLEND_BIT") and
                r.format(name, "SAMPLED_IMAGE_BIT"))

    absent = [name for name in TARGET_FORMATS_USED if not draws(name)]
    w.need("render target formats both games use", not absent, ", ".join(TARGET_FORMATS_USED),
           "cannot be drawn into, blended and sampled: " + ", ".join(absent))
    absent = [name for name in TARGET_FORMATS_OTHER if not draws(name)]
    if absent:
        w.add("WARN", "render target formats neither game uses",
              "not drawn into, blended and sampled (a surface of these would be skipped or unblended): "
              + ", ".join(absent))
    else:
        w.add("PASS", "render target formats neither game uses", "all drawn into, blended and sampled")

    # ---- presenter.cpp --------------------------------------------------------
    w.need("VK_KHR_swapchain", r.extension("VK_KHR_swapchain"), "yes", "no swapchain")
    blit = all(r.format(name, bit) for name in ("R8G8B8A8_UNORM", "B8G8R8A8_UNORM")
               for bit in ("BLIT_SRC_BIT", "BLIT_DST_BIT"))
    w.need("the frame blitted to the window", blit, "RGBA8 and BGRA8 blit both ways", "no blit of the frame")
    w.need("the display's colour table applied in place", r.format("R8G8B8A8_UNORM", "STORAGE_IMAGE_BIT"),
           "RGBA8 is a storage image", "the frame's image cannot be made (renderer.cpp, kPresentImages)")
    # The four that decide which of the renderer's paths a device is on.
    w.paths = [("render passes" if legacy else "dynamic rendering"),
               ("BC by the driver" if bc else "BC in software"),
               ("a record a subgroup" if ballot else "a record a pixel"),
               ("shadow" if reach is not None and ARENA_WANTED + SHADOW_BYTES <= reach else "no shadow")]
    wait = bool(r.feature("presentId")) and bool(r.feature("presentWait"))
    w.prefer("present id and wait", wait, "the display's clock", "frames are paced by the swapchain alone")
    return w


def load(argument, cache):
    argument, _, name = argument.partition("=")
    if os.path.exists(argument):
        path = argument
    elif argument.isdigit():
        os.makedirs(cache, exist_ok=True)
        path = os.path.join(cache, argument + ".json")
        if not os.path.exists(path):
            request = urllib.request.Request(PROFILE_URL.format(argument), headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(request, timeout=120) as response, open(path, "wb") as out:
                out.write(response.read())
    else:
        raise SystemExit(f"{argument}: neither a file nor a report id")
    with open(path, encoding="utf-8") as file:
        report = Report(json.load(file))
    if name:
        report.name = name
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("reports", nargs="+", help="profile JSON files, or vulkan.gpuinfo.org report ids")
    parser.add_argument("--cache", default="device-reports", help="where downloaded reports are kept")
    parser.add_argument("--summary", action="store_true", help="only the table across the reports")
    args = parser.parse_args()

    failed = False
    table = []
    for argument in args.reports:
        report = load(argument, args.cache)
        result = walk(report)
        failed = failed or bool(result.refused)
        if not args.summary:
            print(f"== {report.name}: {report.label}; Vulkan {'.'.join(map(str, report.api))}; {report.driver()}")
            for status, item, detail in result.lines:
                print(f"  {status:8} {item}: {detail}")
            print()
        verdict = "REFUSED" if result.refused else "runs"
        notes = result.refused or ", ".join(
            result.paths + [f"{len(result.fallbacks)} fallbacks", f"{len(result.warnings)} warnings"])
        table.append((report.name, report.label, report.release(), ".".join(map(str, report.api)), verdict, notes))

    width = max(len(row[0]) for row in table)
    print(f"{'GPU':{width}}  {'driver':24} {'Vulkan':8} {'verdict':8} the renderer's paths, or why not [the report]")
    for name, label, driver, api, verdict, notes in table:
        print(f"{name:{width}}  {driver:24} {api:8} {verdict:8} {notes}  [{label}]")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
