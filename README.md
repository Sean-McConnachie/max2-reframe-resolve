# Max2 Reframe for DaVinci Resolve

An OpenFX plugin that reframes GoPro Max 2 `.360` clips straight from the original file. You don't need to
export from GoPro Player first. Stabilization, horizon lock and direction lock come from the camera's gyro
data and are ordinary plugin settings, so changing them never means re-exporting.

## How it works

- Resolve gives OFX effects on the Edit page images at timeline resolution, which is at most UHD in the free
  version. To keep full 8K detail, the plugin reads the clip's source path from Resolve
  (`kOfxImageEffectPropSrcFilePath`) and decodes the `.360` itself. It uses hardware HEVC decoding through
  Media Foundation and D3D11, which works in the free version of Resolve.
- Rendering runs on CUDA or OpenCL, whichever Resolve is set to (Preferences → Memory and GPU → GPU
  Processing Mode), and falls back to the CPU. All three paths share one kernel source,
  `src/reframe_kernel.h`. On an RTX 3060 a UHD frame takes about 8 ms to render, or about 21 ms
  including the upload of a new source frame.
- `.360` layout: two 5888×1920 HEVC streams in GoPro's cube-map (EAC) layout. The first holds the left,
  front and right faces; the second holds the bottom, back and top faces, rotated 90°. The left/right and
  top/bottom faces are split by the lens seam, with a 64 px overlap that gets blended.
- Stabilization model, checked against GoPro Player exports (see `proto/`):
  - Camera orientation = `M · IORI · CORI · Mᵀ`, where `M = diag(-1, 1, -1)` and the quaternions are in
    w,x,y,z order.
  - Horizon lock uses the GRAV vector, smoothed with σ = 3 s.
  - Direction lock fixes the heading to the clip's first frame. It matches GoPro to within 0.2°, apart
    from a constant 1° offset.
  - With direction lock off, the heading follows the camera with a Gaussian smoothing of σ = 0.3 s,
    matching GoPro to within about 0.5°.

## Build and install

```
build.bat
powershell -ExecutionPolicy Bypass -File install.ps1
```

`install.ps1` copies the plugin to `%LOCALAPPDATA%\Max2Reframe\core\Max2ReframeCore.dll`. A small loader in
Resolve's standard plugin folder (`C:\Program Files\Common Files\OFX\Plugins\Max2Reframe.ofx.bundle`)
loads it from there. Installing the loader needs admin (one UAC prompt), but updates afterwards don't.
Restart Resolve after installing. `install.ps1 -Uninstall` removes it.

Resolve doesn't load plugins through a junction, and it doesn't reliably see `OFX_PLUGIN_PATH`, so the loader
is the dependable way to load the plugin from a folder the user can write to.

## Use

1. Import the `.360` files into Resolve. If Resolve refuses the `.360` extension, run
   `tools\link-mp4.ps1 <folder>`. It creates `NAME.360.mp4` hard links, which use no extra disk space.
2. Put a clip on the timeline. Then go to Effects → OpenFX → GoPro 360 → **Max2 Reframe** and drag it onto
   the clip.
3. Set Pan / Tilt / Roll / Field of View (all keyframeable), and Stabilize / Horizon Lock / Direction Lock.
   - Lens Curvature: 0 is rectilinear, 1 is stereographic (tiny planet: Tilt -90, FOV ~280), and 2 is
     an equidistant fisheye (FOV 360 shows the whole sphere).
   - Pan can be keyframed past ±360 for spins.
   - Set Projection to "Equirectangular 360" to get a stabilized 2:1 360 output.
4. Optional: turn on Motion Blur. It blurs along the virtual camera's movement during the shutter interval,
   which comes from keyframed pan/tilt/roll/FOV and the smoothed heading follow. The source frame's pixels
   stay fixed. Shutter Angle sets the exposure (180 = half the frame). The plugin takes about one sample per
   pixel of blur, capped by Max Samples (default 32), and uses a single sample when nothing moves.

The log is at `%LOCALAPPDATA%\Max2Reframe\max2reframe.log`.

## Test tool

`build\max2render.exe file.360 --frame 60 --proj erp --w 1920 --h 960 --dirlock 1 --out out.ppm`
renders frames without Resolve. `--gpu cuda|opencl` uses a GPU path, and `--count N` benchmarks a sequence.
