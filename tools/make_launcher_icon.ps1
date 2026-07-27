[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Destination
)

# Turn the mod's own artwork into a multi-size .ico for the launcher.
#
# Windows picks a different size for the taskbar, the desktop and the alt-tab
# switcher, and scales badly when it has to invent one, so all five are written
# and each is resized from the full-resolution original rather than from the
# previous step down. The 256 entry is stored as PNG, which is what the format
# has expected at that size since Vista; the smaller ones stay uncompressed BMP
# because older shells still read those directly.

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$sizes = @(256, 64, 48, 32, 16)
$original = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $Source))
try {
    $entries = @()
    foreach ($size in $sizes) {
        $bitmap = New-Object System.Drawing.Bitmap $size, $size
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.DrawImage($original, (New-Object System.Drawing.Rectangle 0, 0, $size, $size))
        $graphics.Dispose()

        $stream = New-Object IO.MemoryStream
        if ($size -eq 256) {
            $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
            $payload = $stream.ToArray()
        } else {
            # An icon's BMP has a doubled height and no file header, and carries
            # an AND mask after the colour data. With 32-bit colour the mask is
            # ignored but must still be there and padded to four bytes a row.
            $data = New-Object byte[] ($size * $size * 4)
            $locked = $bitmap.LockBits(
                (New-Object System.Drawing.Rectangle 0, 0, $size, $size),
                [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
            [Runtime.InteropServices.Marshal]::Copy($locked.Scan0, $data, 0, $data.Length)
            $bitmap.UnlockBits($locked)

            $writer = New-Object IO.BinaryWriter $stream
            $writer.Write([uint32]40); $writer.Write([int32]$size); $writer.Write([int32]($size * 2))
            $writer.Write([uint16]1); $writer.Write([uint16]32); $writer.Write([uint32]0)
            $writer.Write([uint32]($size * $size * 4)); $writer.Write([int32]0); $writer.Write([int32]0)
            $writer.Write([uint32]0); $writer.Write([uint32]0)
            for ($row = $size - 1; $row -ge 0; $row--) {
                $writer.Write($data, $row * $size * 4, $size * 4)
            }
            $writer.Write((New-Object byte[] ($size * 4)))
            $writer.Flush()
            $payload = $stream.ToArray()
        }
        $stream.Dispose()
        $entries += [pscustomobject]@{ Size = $size; Payload = $payload }
        $bitmap.Dispose()
    }

    $out = New-Object IO.MemoryStream
    $writer = New-Object IO.BinaryWriter $out
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$entries.Count)
    $offset = 6 + 16 * $entries.Count
    foreach ($entry in $entries) {
        $dimension = if ($entry.Size -ge 256) { 0 } else { $entry.Size }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$entry.Payload.Length); $writer.Write([uint32]$offset)
        $offset += $entry.Payload.Length
    }
    foreach ($entry in $entries) { $writer.Write($entry.Payload) }
    $writer.Flush()
    [IO.File]::WriteAllBytes($Destination, $out.ToArray())
    $out.Dispose()
} finally {
    $original.Dispose()
}
Write-Output ("OK: {0} -> {1} ({2} sizes, {3} bytes)" -f `
    (Split-Path -Leaf $Source), (Split-Path -Leaf $Destination), $sizes.Count,
    (Get-Item -LiteralPath $Destination).Length)
