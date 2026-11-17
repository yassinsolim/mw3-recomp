# Third-party software

## Mesa Turnip (Vulkan driver for Qualcomm Adreno)

The Modern Warfare 3 Android app (`com.mw3.recomp`) uses a build of the Mesa
Turnip driver as its default graphics driver on Qualcomm devices.

- Project: Mesa, <https://gitlab.freedesktop.org/mesa/mesa> (Turnip is its open
  source Vulkan driver for Adreno GPUs)
- Licence: MIT
- Build used: Turnip v27.0.0-R2 by StevenMXZ,
  <https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/download/v39/Turnip_v27.0.0-R2.zip>
  (SHA-256 `8d30be022582d0c03b1918c019540491d00439e14e9e5e1359fe0bb19004f781`)

The binary is downloaded at build time by `android/fetch_deps.sh` (or
`scripts/win_build.ps1` on Windows), checked against the hash above and
packaged into the `mw3` flavor's assets. It is not stored in this repository.

## libadrenotools

Loads a driver the player imported (or the one above) without root.
<https://github.com/bylaws/libadrenotools>, fetched by `android/fetch_deps.sh`
into `third_party/libadrenotools/` under its own licence.
