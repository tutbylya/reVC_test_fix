[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ArchivePath,
    [Parameter(Mandatory = $true)][string]$GameDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateSet('Ferrari458Undercover')][string]$Mod = 'Ferrari458Undercover'
)

# Asset preparation only: no downloads, mod execution, game launch, IMG edits,
# or writes into GameDirectory. Add other explicitly reviewed presets here.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$expectedArchive = '4A633DDDBFD44DC11B5B381A6AE932B9415A5D4A99EBADCA6F0F79F9F228D584'
$sourceUri = 'https://libertycity.ru/files/gta-vice-city/246200-ferrari-458-italia-undercover-cop.html'
$archive = (Resolve-Path -LiteralPath $ArchivePath).Path
$game = (Resolve-Path -LiteralPath $GameDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
$trustedRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')).TrimEnd('\')
$trustedBuild = Join-Path $trustedRoot 'build'
if (!$output.StartsWith($trustedBuild.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputDirectory must be a new directory inside this project build directory.'
}
if (Test-Path -LiteralPath $output) { throw 'OutputDirectory must be new; existing data will not be overwritten.' }
if ($output.StartsWith($game.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputDirectory must not be inside the source game installation.'
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedArchive) {
    throw 'Archive SHA-256 does not match the reviewed Ferrari resource preset.'
}
if ((Get-Item -LiteralPath $archive).Length -gt 128MB) { throw 'Archive exceeds the resource size limit.' }
foreach ($protected in @($game, (Split-Path -Parent $archive))) {
    if ($output.TrimEnd('\') -eq $protected.TrimEnd('\')) { throw 'Output must be a separate directory.' }
}
# Reject existing reparse-point ancestors: output must not traverse a junction
# into a live installation. Nonexistent leaf components are created below.
$ancestor = $output
while ($ancestor) {
    if (Test-Path -LiteralPath $ancestor) {
        if ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Output traverses a reparse point: $ancestor"
        }
    }
    # The configured project root is trusted even if its own parent belongs to
    # a managed OneDrive junction. Never trust links below that workspace root.
    if ($ancestor.TrimEnd('\') -eq $trustedRoot) { break }
    $parent = Split-Path -Parent $ancestor
    if ($parent -eq $ancestor) { break }
    $ancestor = $parent
}

$entries = @(& tar.exe -tf $archive)
if ($LASTEXITCODE -ne 0 -or !$entries.Count -or $entries.Count -gt 256) { throw 'Cannot safely list the archive.' }
$entryNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $entries) {
    if (!$entry -or $entry -match '(^[/\\]|^[a-zA-Z]:|(^|[/\\])\.\.([/\\]|$)|:)' -or
        $entry -notmatch '^[a-zA-Z0-9 _()./\-]+$' -or !$entryNames.Add($entry.TrimEnd('/'))) {
        throw "Unsafe or duplicate archive path: $entry"
    }
    if (!$entry.EndsWith('/') -and [IO.Path]::GetExtension($entry).ToLowerInvariant() -notin @('.txt', '.jpg', '.dff', '.txd')) {
        throw "Not an allowed passive asset: $entry"
    }
}
$types = @(& tar.exe -tvf $archive)
if ($LASTEXITCODE -ne 0 -or $types.Count -ne $entries.Count -or @($types | Where-Object { $_ -notmatch '^[-d]' }).Count) {
    throw 'Links or unsupported archive entry types are not allowed.'
}

$stockPath = Join-Path $game 'data\handling.cfg'
$stockHash = (Get-FileHash -LiteralPath $stockPath -Algorithm SHA256).Hash
$stockBytes = [IO.File]::ReadAllBytes($stockPath)
if (@($stockBytes | Where-Object { $_ -gt 127 }).Count) { throw 'Expected an ASCII Vice City handling.cfg.' }
$stock = [Text.Encoding]::ASCII.GetString($stockBytes)
if ([regex]::Matches($stock, '(?m)^BANSHEE[ \t]').Count -ne 1 -or
    [regex]::Matches($stock, '(?m)^;the end\r?$').Count -ne 1 -or !$stock.EndsWith("`n")) {
    throw 'Stock handling.cfg must contain one BANSHEE record and a newline-terminated ;the end marker.'
}

New-Item -ItemType Directory -Path $output | Out-Null
$extracted = Join-Path $output 'extracted'
New-Item -ItemType Directory -Path $extracted | Out-Null
& tar.exe -xf $archive -C $extracted
if ($LASTEXITCODE -ne 0) { throw 'Resource archive extraction failed.' }
$items = @(Get-ChildItem -LiteralPath $extracted -Recurse -Force)
if (@($items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Extracted resources contain a link.' }
$files = @($items | Where-Object { !$_.PSIsContainer })
if (($files | Measure-Object Length -Sum).Sum -gt 512MB) { throw 'Extracted resources exceed the size limit.' }
function Unique-File([string]$name) {
    $found = @($files | Where-Object { $_.Name -ieq $name })
    if ($found.Count -ne 1) { throw "Expected exactly one $name asset." }
    return $found[0].FullName
}
$carDff = Unique-File 'Ferrari_458_UC.dff'
$carTxd = Unique-File 'Ferrari_458_UC.txd'
$wheelDff = Unique-File 'wheels.DFF'
$wheelTxd = Unique-File 'wheels.TXD'
$handlingText = [IO.File]::ReadAllText((Unique-File 'Handling.cfg.txt'))
$handlingRows = [regex]::Matches($handlingText, '(?m)^BANSHEE[ \t][^\r\n]*')
if ($handlingRows.Count -ne 1) { throw 'Expected exactly one Ferrari BANSHEE handling record.' }
$handling = $handlingRows[0].Value.Trim()
$fields = [regex]::Split($handling, '\s+')
if ($fields.Count -ne 33 -or [Text.Encoding]::ASCII.GetByteCount($handling) -ge 201 -or
    $fields[15] -notin @('R', 'F', '4') -or $fields[16] -notin @('P', 'D', 'E') -or $fields[30] -notmatch '^[a-fA-F0-9]{1,8}$') {
    throw 'Invalid Vice City 33-field handling record.'
}
for ($i = 1; $i -lt $fields.Count; ++$i) {
    if ($i -in @(15, 16, 30)) { continue }
    $number = 0.0
    if (![double]::TryParse($fields[$i], [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$number) -or
        [double]::IsNaN($number) -or [double]::IsInfinity($number)) { throw "Invalid numeric handling field $i." }
}
$mergedHandling = [regex]::Replace($stock, '(?m)^BANSHEE[^\r\n]*', $handling)

function Inspect-Rw([string]$path, [uint32]$expectedType) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $textures = [Collections.Generic.List[object]]::new()
    $references = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $plugins = [Collections.Generic.HashSet[string]]::new()
    $geometries = [Collections.Generic.List[object]]::new()
    $versions = [Collections.Generic.HashSet[string]]::new()
    function Walk-Chunks([long]$start, [long]$end, [uint32]$parentType, [int]$depth) {
        if ($depth -gt 32) { throw 'RenderWare nesting exceeds the limit.' }
        $cursor = $start
        while ($cursor -lt $end) {
            if ($end - $cursor -lt 12) {
                for ($p = $cursor; $p -lt $end; ++$p) { if ($bytes[$p]) { throw 'Nonzero truncated RW header.' } }
                return
            }
            $kind = [BitConverter]::ToUInt32($bytes, [int]$cursor)
            $length = [BitConverter]::ToUInt32($bytes, [int]($cursor + 4))
            $lib = [BitConverter]::ToUInt32($bytes, [int]($cursor + 8))
            if (!$kind -and !$length -and !$lib) {
                for ($p = $cursor; $p -lt $end; ++$p) { if ($bytes[$p]) { throw 'Unexpected data after RW padding.' } }
                return
            }
            $data = $cursor + 12
            $next = $data + [long]$length
            if ($next -gt $end) { throw 'RenderWare chunk extends beyond its container.' }
            $version = if ($lib -band 0xFFFF0000L) { (($lib -shr 14 -band 0x3FF00) + 0x30000) -bor ($lib -shr 16 -band 0x3F) } else { $lib -shl 8 }
            $null = $versions.Add(('0x{0:X5}' -f $version))
            if ($parentType -eq 3) { $null = $plugins.Add(('0x{0:X8}' -f $kind)) }
            if ($kind -eq 1 -and $parentType -eq 21 -and $length -ge 88) {
                $name = [Text.Encoding]::ASCII.GetString($bytes, [int]($data + 8), 32).Split([char]0)[0]
                $textures.Add([ordered]@{ name = $name; platform = [BitConverter]::ToUInt32($bytes, [int]$data);
                    width = [BitConverter]::ToUInt16($bytes, [int]($data + 80)); height = [BitConverter]::ToUInt16($bytes, [int]($data + 82));
                    depth = $bytes[$data + 84]; mipLevels = $bytes[$data + 85]; compressionOrFlags = $bytes[$data + 87] })
            }
            if ($kind -eq 1 -and $parentType -eq 15 -and $length -ge 16) {
                $geometries.Add([ordered]@{ flags = ('0x{0:X8}' -f [BitConverter]::ToUInt32($bytes, [int]$data));
                    triangles = [BitConverter]::ToUInt32($bytes, [int]($data + 4)); vertices = [BitConverter]::ToUInt32($bytes, [int]($data + 8)) })
            }
            if ($kind -eq 2 -and $parentType -eq 6) {
                $name = [Text.Encoding]::ASCII.GetString($bytes, [int]$data, [int]$length).Split([char]0)[0]
                if ($name) { $null = $references.Add($name) }
            }
            if ($kind -in @(3, 6, 7, 8, 14, 15, 16, 20, 21, 22, 26)) { Walk-Chunks $data $next $kind ($depth + 1) }
            $cursor = $next
        }
    }
    if ($bytes.Length -lt 12 -or [BitConverter]::ToUInt32($bytes, 0) -ne $expectedType) { throw 'Not the expected DFF/TXD RenderWare resource.' }
    Walk-Chunks 0 $bytes.LongLength 0 0
    return [ordered]@{ file = [IO.Path]::GetFileName($path); bytes = $bytes.Length; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash;
        rwVersions = @($versions); plugins = @($plugins); textures = @($textures.ToArray()); textureReferences = @($references); geometries = @($geometries.ToArray()) }
}
$carModel = Inspect-Rw $carDff 16
$carTextures = Inspect-Rw $carTxd 22
$sharedModel = Inspect-Rw $wheelDff 16
$sharedTextures = Inspect-Rw $wheelTxd 22
$knownNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($texture in $carTextures.textures) { $null = $knownNames.Add($texture.name) }
$missingTextures = @($carModel.textureReferences | Where-Object { !$knownNames.Contains($_) })
if ($missingTextures.Count) { throw "Car references missing TXD textures: $($missingTextures -join ', ')" }

$modDirectory = Join-Path $output 'modloader\Ferrari458Italia'
$optionalDirectory = Join-Path $output 'optional-mods\FerrariSharedWheels\models\generic'
New-Item -ItemType Directory -Path $modDirectory, $optionalDirectory -Force | Out-Null
Copy-Item -LiteralPath $carDff -Destination (Join-Path $modDirectory 'banshee.dff')
Copy-Item -LiteralPath $carTxd -Destination (Join-Path $modDirectory 'banshee.txd')
[IO.File]::WriteAllText((Join-Path $modDirectory 'handling.cfg'), $mergedHandling, [Text.Encoding]::ASCII)
Copy-Item -LiteralPath $wheelDff -Destination (Join-Path $optionalDirectory 'wheels.dff')
Copy-Item -LiteralPath $wheelTxd -Destination (Join-Path $optionalDirectory 'wheels.txd')
if ((Get-FileHash -LiteralPath $stockPath -Algorithm SHA256).Hash -ne $stockHash) { throw 'Stock handling.cfg changed during preparation.' }
$manifest = [ordered]@{ preset = $Mod; source = $sourceUri; archiveSha256 = $expectedArchive; stockHandlingSha256 = $stockHash;
    mergedHandlingSha256 = (Get-FileHash -LiteralPath (Join-Path $modDirectory 'handling.cfg') -Algorithm SHA256).Hash;
    handlingFields = $fields.Count; handlingLine = $handling; carModel = $carModel; carTextures = $carTextures;
    optionalSharedWheelModel = $sharedModel; optionalSharedWheelTextures = $sharedTextures;
    install = 'Copy only modloader/Ferrari458Italia into the isolated game modloader directory. Shared wheels are opt-in and affect other cars.';
    testedInGame = $false; originalGameFilesWritten = $false }
[IO.File]::WriteAllText((Join-Path $output 'manifest.json'), ($manifest | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
Write-Host "Prepared passive Ferrari resources: $modDirectory"
Write-Host 'Shared wheel replacement is separate and disabled by default. No game or mod executable was launched.'
