"""DaVinci Resolve script: Workspace > Scripts > Import GoPro 360

Resolve will not import files with the .360 extension, so this script gives each .360 file a second name ending in
.mp4 (an NTFS hard link: same file, no extra disk space) in a hidden "_Max2Reframe" subfolder next to it, and
imports those into the current Media Pool bin. Clips are renamed back to the original file name.
"""
import os
import sys

LINK_DIR = "_Max2Reframe"


def find_360(folder):
    """All .360 files under folder (skipping our own link folders)."""
    out = []
    for root, dirs, files in os.walk(folder):
        dirs[:] = [d for d in dirs if d != LINK_DIR]
        out += [os.path.join(root, f) for f in files if f.lower().endswith(".360")]
    return sorted(out)


def link_for(path):
    """Create (if needed) and return the .mp4 hard link for a .360 file."""
    folder, name = os.path.split(path)
    # links made earlier by tools\link-mp4.ps1 sit next to the file; reuse those
    beside = path + ".mp4"
    if os.path.exists(beside) and os.path.samefile(beside, path):
        return beside
    link_dir = os.path.join(folder, LINK_DIR)
    if not os.path.isdir(link_dir):
        os.makedirs(link_dir)
        try:
            import ctypes
            ctypes.windll.kernel32.SetFileAttributesW(link_dir, 0x2)  # FILE_ATTRIBUTE_HIDDEN
        except Exception:
            pass
    link = os.path.join(link_dir, name + ".mp4")
    if os.path.exists(link):
        if os.path.samefile(link, path):
            return link
        os.remove(link)  # stale: the .360 was replaced since
    os.link(path, link)
    return link


def get_resolve():
    try:
        return resolve  # injected when run from Resolve's Scripts menu
    except NameError:
        pass
    try:
        return bmd.scriptapp("Resolve")
    except NameError:
        import DaVinciResolveScript as dvr
        return dvr.scriptapp("Resolve")


def main():
    r = get_resolve()
    if r is None:
        print("Import GoPro 360: could not connect to Resolve")
        return
    project = r.GetProjectManager().GetCurrentProject()
    if project is None:
        print("Import GoPro 360: open a project first")
        return
    fusion = r.Fusion()
    folder = fusion.RequestDir(os.path.expanduser("~")) if fusion else None
    if not folder:
        return
    files = find_360(folder)
    if not files:
        print("Import GoPro 360: no .360 files in %s" % folder)
        return

    links, failed = [], []
    for f in files:
        try:
            links.append((f, link_for(f)))
        except OSError as e:
            failed.append("%s (%s)" % (f, e))

    pool = project.GetMediaPool()
    items = pool.ImportMedia([l for _, l in links]) or []
    names = {os.path.normcase(l): os.path.basename(f) for f, l in links}
    for item in items:
        try:
            path = item.GetClipProperty("File Path")
            name = names.get(os.path.normcase(path or ""))
            if name:
                item.SetClipProperty("Clip Name", name)
        except Exception:
            pass

    print("Import GoPro 360: imported %d of %d .360 file(s) from %s" % (len(items), len(files), folder))
    for f in failed:
        print("  could not link: " + f)


main()
