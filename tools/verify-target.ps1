param(
    [Parameter(Mandatory = $true)]
    [string]$GeodePath
)

$ErrorActionPreference = 'Stop'
$expectedPackage = '291C7AA04D9223836665CC37DC345F384644B5ABEDFD163CA78FC4D7B80813BD'
$expectedDll = '27249D1F61C544A42ECD10E7D3084FB86FAAD935E418E1D690C679710F3C330B'
$resolved = (Resolve-Path -LiteralPath $GeodePath).Path
$packageHash = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash
if ($packageHash -ne $expectedPackage) {
    throw "Unsupported .geode SHA-256: $packageHash"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($resolved)
try {
    $entry = $archive.Entries | Where-Object { $_.FullName -eq 'peony.silicate.dll' }
    if (-not $entry) { throw 'peony.silicate.dll is missing from the package' }
    $stream = $entry.Open()
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            $dllHash = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '')
        } finally {
            $sha.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
} finally {
    $archive.Dispose()
}

if ($dllHash -ne $expectedDll) {
    throw "Unsupported DLL SHA-256: $dllHash"
}

Write-Host 'Target verified:'
Write-Host "  package $packageHash"
Write-Host "  DLL     $dllHash"
