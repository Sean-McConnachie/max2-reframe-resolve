# Helper for the "Import GoPro 360" Resolve script. Resolve runs menu scripts in a restricted Lua (no require, io or
# os.execute, and Fusion's folder dialog does not open from the Edit page), so this part runs in PowerShell. The Lua
# script starts it through "Import GoPro 360.cmd" and waits for result.txt:
#  1. Shows the Windows folder dialog (or uses -Folder).
#  2. Finds the .360 files in the folder and its subfolders.
#  3. Gives each one an .mp4 hard link in a hidden "_Max2Reframe" folder next to it.
#  4. Writes the list of links to result.txt as a Lua table, for the Lua script to import.
param([string]$Folder = '')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$resultPath = Join-Path $here 'result.txt'
$logPath = Join-Path $here 'import.log'
$settingsPath = Join-Path $here 'last-folder.txt'
$utf8 = New-Object Text.UTF8Encoding $false
if (-not $Folder -and $env:MAX2_TEST_DIR) { $Folder = $env:MAX2_TEST_DIR } # test without the dialog
$Token = [DateTime]::Now.ToString('yyyyMMddHHmmssfff')

function Log($msg) { [IO.File]::AppendAllText($logPath, (Get-Date -Format 'yyyy-MM-dd HH:mm:ss ') + $msg + "`r`n", $utf8) }
function Lua($s) { '[==[' + $s + ']==]' }
function Write-Result($text) {
    # write then rename, so the Lua script never reads a half-written file
    [IO.File]::WriteAllText("$resultPath.tmp", $text, $utf8)
    Move-Item -LiteralPath "$resultPath.tmp" -Destination $resultPath -Force
}

$status = 'ok'
$links = @()
$errors = @()
try {
    Log "started, token $Token"
    Write-Result "{ Token = $(Lua $Token), Status = $(Lua 'running') }`n"

    if (-not $Folder) {
        Add-Type -ReferencedAssemblies System.Windows.Forms -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Max2FolderPicker {
    [ComImport, Guid("DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7")] class FileOpenDialog {}
    [ComImport, Guid("42f85136-db7e-439c-85f1-e4075d135fc8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IFileOpenDialog {
        [PreserveSig] int Show(IntPtr owner);
        void SetFileTypes(); void SetFileTypeIndex(); void GetFileTypeIndex(); void Advise(); void Unadvise();
        void SetOptions(uint fos); void GetOptions(out uint fos);
        void SetDefaultFolder(IShellItem si); void SetFolder(IShellItem si);
        void GetFolder(); void GetCurrentSelection(); void SetFileName(); void GetFileName();
        void SetTitle([MarshalAs(UnmanagedType.LPWStr)] string title);
        void SetOkButtonLabel(); void SetFileNameLabel();
        void GetResult(out IShellItem si);
    }
    [ComImport, Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellItem {
        void BindToHandler(); void GetParent();
        void GetDisplayName(uint sigdn, [MarshalAs(UnmanagedType.LPWStr)] out string name);
    }
    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = false)]
    static extern void SHCreateItemFromParsingName(string path, IntPtr pbc, [MarshalAs(UnmanagedType.LPStruct)] Guid riid, out IShellItem si);

    public static string Pick(string title, string start) {
        var form = new System.Windows.Forms.Form();
        form.TopMost = true; form.ShowInTaskbar = false; form.Opacity = 0;
        form.StartPosition = System.Windows.Forms.FormStartPosition.CenterScreen;
        form.Show(); form.Activate();
        try {
            var dlg = (IFileOpenDialog)new FileOpenDialog();
            dlg.SetOptions(0x20 | 0x40 | 0x800); // pick folders, file system only, path must exist
            dlg.SetTitle(title);
            if (!string.IsNullOrEmpty(start) && System.IO.Directory.Exists(start)) {
                IShellItem si;
                SHCreateItemFromParsingName(start, IntPtr.Zero, typeof(IShellItem).GUID, out si);
                dlg.SetFolder(si);
            }
            if (dlg.Show(form.Handle) != 0) return null;
            IShellItem result; dlg.GetResult(out result);
            string path; result.GetDisplayName(0x80058000, out path); // SIGDN_FILESYSPATH
            return path;
        } finally { form.Close(); }
    }
}
'@
        $start = if (Test-Path $settingsPath) { (Get-Content $settingsPath -Raw).Trim() } else { '' }
        $Folder = [Max2FolderPicker]::Pick('Select the folder with your .360 files', $start)
        if (-not $Folder) { $status = 'cancelled'; Log 'no folder selected' }
        else { [IO.File]::WriteAllText($settingsPath, $Folder, $utf8) }
    }

    if ($status -eq 'ok') {
        Log "scanning $Folder"
        $files = @(Get-ChildItem -LiteralPath $Folder -Recurse -File -Filter '*.360' -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -eq '.360' -and $_.DirectoryName -notmatch '\\_Max2Reframe$' })
        if ($files.Count -eq 0) { $status = 'none' }
        foreach ($f in $files) {
            try {
                # a link made by tools\link-mp4.ps1 next to the file
                $beside = Join-Path $f.DirectoryName ($f.Name + '.mp4')
                if ((Test-Path -LiteralPath $beside) -and (Get-Item -LiteralPath $beside).Length -eq $f.Length) {
                    $links += ,@($beside, $f.Name); continue
                }
                $dir = Join-Path $f.DirectoryName '_Max2Reframe'
                if (-not (Test-Path -LiteralPath $dir)) {
                    $d = New-Item -ItemType Directory -Path $dir
                    $d.Attributes = $d.Attributes -bor [IO.FileAttributes]::Hidden
                }
                $link = Join-Path $dir ($f.Name + '.mp4')
                if (Test-Path -LiteralPath $link) {
                    if ((Get-Item -LiteralPath $link).Length -eq $f.Length) { $links += ,@($link, $f.Name); continue }
                    Remove-Item -LiteralPath $link -Force # the .360 was replaced since
                }
                New-Item -ItemType HardLink -Path $link -Target $f.FullName | Out-Null
                $links += ,@($link, $f.Name)
            } catch {
                $errors += "$($f.FullName): $($_.Exception.Message)"
            }
        }
        Log "$($files.Count) .360 file(s), $($links.Count) link(s), $($errors.Count) error(s)"
    }
} catch {
    $status = 'error'
    $errors += $_.Exception.Message
}
foreach ($e in $errors) { Log "error: $e" }

$out = New-Object Text.StringBuilder
[void]$out.Append("{`n  Token = $(Lua $Token),`n  Status = $(Lua $status),`n  Folder = $(Lua $Folder),`n  Links = {`n")
foreach ($l in $links) { [void]$out.Append("    { Path = $(Lua $l[0]), Name = $(Lua $l[1]) },`n") }
[void]$out.Append("  },`n  Errors = {`n")
foreach ($e in $errors) { [void]$out.Append("    $(Lua $e),`n") }
[void]$out.Append("  },`n}`n")
Write-Result $out.ToString()
