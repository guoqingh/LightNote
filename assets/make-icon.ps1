$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = 'D:\codex\LightNote'
$dir = Join-Path $root 'assets'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$entries = @()
foreach ($size in @(16,32,48,256)) {
  $bmp = [System.Drawing.Bitmap]::new($size,$size)
  $bmp.SetResolution(96,96)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.Clear([System.Drawing.Color]::Transparent)
  $s = $size / 256.0
  $g.ScaleTransform([float]$s,[float]$s)
  $blue = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(13,125,222))
  $light = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(244,251,255))
  $fold = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(167,216,250))
  $line = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(51,156,227),14)
  $outer=[System.Drawing.Drawing2D.GraphicsPath]::new()
  $outer.AddArc(12,12,56,56,180,90)
  $outer.AddArc(188,12,56,56,270,90)
  $outer.AddArc(188,188,56,56,0,90)
  $outer.AddArc(12,188,56,56,90,90)
  $outer.CloseFigure()
  $g.FillPath($blue,$outer)
  $paper = [System.Drawing.PointF[]]@(
     [System.Drawing.PointF]::new(68,45),
     [System.Drawing.PointF]::new(159,45),
     [System.Drawing.PointF]::new(198,84),
     [System.Drawing.PointF]::new(198,209),
     [System.Drawing.PointF]::new(68,209))
  $g.FillPolygon($light,$paper)
  $g.FillPolygon($fold,([System.Drawing.PointF[]]@(
      [System.Drawing.PointF]::new(159,45),
      [System.Drawing.PointF]::new(159,84),
      [System.Drawing.PointF]::new(198,84))))
  foreach($y in @(112,143,174)) { $g.DrawLine($line,95,$y,169,$y) }
  $g.Dispose()
  $png = Join-Path $dir ("LightNote-$size.png")
  $bmp.Save($png,[System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  $entries += [pscustomobject]@{Size=$size;Bytes=[System.IO.File]::ReadAllBytes($png)}
}
$ico = Join-Path $dir 'LightNote.ico'
$fs=[System.IO.File]::Open($ico,[System.IO.FileMode]::Create)
$bw=[System.IO.BinaryWriter]::new($fs)
$bw.Write([uint16]0);$bw.Write([uint16]1);$bw.Write([uint16]$entries.Count)
$offset=6+16*$entries.Count
foreach($e in $entries) {
  $dim = if($e.Size -eq 256){0}else{$e.Size}
  $bw.Write([byte]$dim);$bw.Write([byte]$dim)
  $bw.Write([byte]0);$bw.Write([byte]0)
  $bw.Write([uint16]1);$bw.Write([uint16]32)
  $bw.Write([uint32]$e.Bytes.Length);$bw.Write([uint32]$offset)
  $offset += $e.Bytes.Length
}
foreach($e in $entries){$bw.Write([byte[]]$e.Bytes)}
$bw.Dispose();$fs.Dispose()
Write-Output "Icon created: $ico"
