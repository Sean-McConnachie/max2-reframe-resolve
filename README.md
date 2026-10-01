# Max2 Reframe for DaVinci Resolve

An OpenFX plugin that reframes GoPro Max 2 `.360` clips straight from the original file. You don't need to
export from GoPro Player first. Stabilization, horizon lock and direction lock come from the camera's gyro
data and are ordinary plugin settings, so changing them never means re-exporting.

## How it works

- Resolve gives OFX effects on the Edit page images at timeline resolution, which is at most UHD in the free
  version. To keep full 8K detail, the plugin reads the clip's source path from Resolve
  (`kOfxImageEffectPropSrcFilePath`) and decodes the `.360` itself. It uses hardware HEVC decoding through
  Media Foundation and D3D11, which works in the free version of Resolve.
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

`install.ps1` copies the plugin to `%LOCALAPPDATA%\Max2Reframe\OFX` and adds that folder to the user's
`OFX_PLUGIN_PATH`, so no admin rights are needed. Restart Resolve after installing.
`install.ps1 -Uninstall` removes it.

## Use

1. Import the `.360` files into Resolve. If Resolve refuses the `.360` extension, run
   `tools\link-mp4.ps1 <folder>`. It creates `NAME.360.mp4` hard links, which use no extra disk space.
2. Put a clip on the timeline. Then go to Effects → OpenFX → GoPro 360 → **Max2 Reframe** and drag it onto
   the clip.
3. Set Pan / Tilt / Roll / Field of View (all keyframeable), and Stabilize / Horizon Lock / Direction Lock.
   Set Projection to "Equirectangular 360" to get a stabilized 2:1 360 output.

The log is at `%LOCALAPPDATA%\Max2Reframe\max2reframe.log`.

## Test tool

`build\max2render.exe file.360 --frame 60 --proj erp --w 1920 --h 960 --dirlock 1 --out out.ppm`
renders frames without Resolve. `--count N` benchmarks a sequence.
