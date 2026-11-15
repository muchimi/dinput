param([string]$Generator = "Visual Studio 18 2026")
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -property installationPath
$cmakeFolder = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$cmake = Join-Path $cmakeFolder 'cmake.exe'
$ctest = Join-Path $cmakeFolder 'ctest.exe'
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ('gex-dinput-' + [guid]::NewGuid().ToString('N'))
$sourceFolder = Join-Path $stage 'source'
$buildFolder = Join-Path $stage 'build'
New-Item -ItemType Directory -Path $sourceFolder | Out-Null
foreach ($name in @('src', 'tests', 'CMakeLists.txt', 'dill.rc', 'resource.h', 'LICENSE')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $sourceFolder -Recurse
}
& $cmake -S $sourceFolder -B $buildFolder -G $Generator -A x64
if ($LASTEXITCODE) { throw 'CMake configuration failed' }
& $cmake --build $buildFolder --config Release --parallel 2
if ($LASTEXITCODE) { throw 'DILL build failed' }
& $ctest --test-dir $buildFolder -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'DILL native regression failed' }
$artifacts = Join-Path $PSScriptRoot 'artifacts'
New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildFolder 'Release\dill.dll') -Destination $artifacts -Force
Copy-Item -LiteralPath (Join-Path $buildFolder 'Release\dill.pdb') -Destination $artifacts -Force
Write-Output "Validated DLL and symbols: $artifacts"
