# Draws hacbrewpacklz.ico: two arrows pressing together on a rounded square. Run with Windows PowerShell.
Add-Type -AssemblyName System.Drawing

function New-RoundedPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-Icon([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)

    $inset = [single]($size * 0.03)
    $body = New-RoundedPath $inset $inset ($size - 2 * $inset) ($size - 2 * $inset) ([single]($size * 0.22))
    $rect = New-Object System.Drawing.RectangleF 0, 0, $size, $size
    $fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(255, 18, 150, 168)), ([System.Drawing.Color]::FromArgb(255, 9, 92, 107)), 60
    $g.FillPath($fill, $body)

    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([single][Math]::Max($size * 0.105, 1.6))
    $pen.StartCap = 'Round'
    $pen.EndCap = 'Round'
    $pen.LineJoin = 'Round'
    $s = [single]$size

    # A chevron from each side, both pointing at the centre line.
    $g.DrawLines($pen, [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF ($s * 0.22), ($s * 0.28)),
        (New-Object System.Drawing.PointF ($s * 0.41), ($s * 0.50)),
        (New-Object System.Drawing.PointF ($s * 0.22), ($s * 0.72))))
    $g.DrawLines($pen, [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF ($s * 0.78), ($s * 0.28)),
        (New-Object System.Drawing.PointF ($s * 0.59), ($s * 0.50)),
        (New-Object System.Drawing.PointF ($s * 0.78), ($s * 0.72))))

    $g.Dispose()
    return $bmp
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($size in $sizes) {
    $bmp = New-Icon $size
    $stream = New-Object System.IO.MemoryStream
    $bmp.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $stream.ToArray()
}

$target = Join-Path $PSScriptRoot 'hacbrewpacklz.ico'
$file = [System.IO.File]::Create($target)
$writer = New-Object System.IO.BinaryWriter $file
$writer.Write([uint16]0)
$writer.Write([uint16]1)
$writer.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dimension = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $writer.Write([byte]$dimension)
    $writer.Write([byte]$dimension)
    $writer.Write([byte]0)
    $writer.Write([byte]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]32)
    $writer.Write([uint32]$images[$i].Length)
    $writer.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($image in $images) {
    $writer.Write([byte[]]$image)
}
$writer.Dispose()
"Wrote $target"
