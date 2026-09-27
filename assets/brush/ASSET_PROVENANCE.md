# Brush source assets

The brush tips, grains and their packaged derivatives are Vulkana-owned assets,
licensed under the [MIT License](../../LICENSE).

`packaged/v1/` contains deterministic, metadata-free runtime derivatives:

- bitmap tips retain their 1024 x 1024 geometry as metadata-free RGBA8;
  runtime coverage is luminance multiplied by alpha, preserving tonal bristles
  while treating transparent background as exactly empty;
- grains are converted offline to 8-bit grayscale scalar coverage and
  normalized to 512 x 512;
- the runtime treats those stored codes as coverage rather than color and does
  not apply automatic orientation or a color-management transform;
- asset-specific polarity is declared in `registry-v1.json` and is applied
  once when the runtime coverage cache is built.

The 4096 x 4096, 16-bit `T_blend_noise_a.PNG` source is not shipped directly.
Its reviewed 512 x 512 derivative is registered as Multi-Scale Mottle. Any
visual change to a derivative requires a new revision and digest. New brush
families require new semantic IDs; explicit replacements of an existing brush
retain its ID so saved custom presets continue to reference it.

## Transparent tip replacement (September 2026)

The owner requested replacing the original opaque masks with cleaned RGBA
files. The five existing tip IDs now have revision 2, in the same picker order:

| Owner source | Existing brush | Packaged file |
| --- | --- | --- |
| Tip2.png | Radial Scratch | tips/t_ast_07.png |
| Tip1.png | Broad Parallel Bristle | tips/t_ast_103.png |
| Tip4.png | Cross Scratch | tips/t_ast_119.png |
| Tip0.png | Tapered Claw | tips/t_ast_49.png |
| Tip3.png | Four-Tooth Rake | tips/t_ast_78.png |

No threshold, recoloring, resizing, or sharpening is applied. These files
retain meaningful grayscale RGB, so alpha-only would overstate dark bristles.
Existing custom preset settings are unchanged and pick up the replacement
tip through their stable asset IDs. Grain assets are unchanged.

## Reproducible conversion

Run `bash tools/generate_brush_assets.sh` from any location. The script first
verifies every source SHA-256. Tips retain RGB and alpha with 8-bit quantization
and metadata stripping. Grains use grayscale conversion and the
Lanczos filter and an exact 512 x 512 resize. Option order is intentionally
fixed: grayscale conversion and 8-bit quantization happen before resampling.
The reviewed derivatives were produced with ImageMagick 7.1.2-27 Q16-HDRI.
The registry's SHA-256 values verify the resulting PNG byte streams; the
runtime independently verifies those values before decoding.

Source SHA-256 values are embedded beside their explicit source-to-output
mapping in the generator. This is necessary because `Tips/` and `grains/` are
project-owner working assets and intentionally remain outside source control.
