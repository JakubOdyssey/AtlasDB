param([switch]$Clean, [int]$ChaosIterations = 1000)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'build'))
if ($Clean -and (Test-Path -LiteralPath $buildRoot)) {
    if ($buildRoot -ne (Join-Path $projectRoot 'build')) { throw 'Unsafe build path' }
    $item = Get-Item -LiteralPath $buildRoot
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing to clean a linked build directory' }
    Remove-Item -LiteralPath $buildRoot -Recurse -Force
}
$logs = Join-Path $buildRoot 'verification'
New-Item -ItemType Directory -Path $logs -Force | Out-Null
function Invoke-Checked([string]$Log, [string]$Program, [string[]]$Arguments) {
    & $Program @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $logs $Log)
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
Push-Location $projectRoot
try {
    Invoke-Checked 'configure.txt' 'cmake' @('--preset', 'windows')
    Invoke-Checked 'debug-build.txt' 'cmake' @('--build', '--preset', 'windows-debug', '--parallel', '8')
    Invoke-Checked 'debug-tests.txt' 'ctest' @('--preset', 'windows-debug', '--parallel', '2', '--verbose')
    Invoke-Checked 'asan-configure.txt' 'cmake' @('--preset', 'windows-asan')
    Invoke-Checked 'asan-build.txt' 'cmake' @('--build', '--preset', 'windows-asan', '--parallel', '8')
    Invoke-Checked 'asan-tests.txt' 'ctest' @('--preset', 'windows-asan', '--parallel', '2', '--verbose')
    Invoke-Checked 'release-build.txt' 'cmake' @('--build', '--preset', 'windows-release', '--parallel', '8')
    Invoke-Checked 'release-tests.txt' 'ctest' @('--preset', 'windows-release', '--parallel', '2', '--verbose')
    $release = Join-Path $buildRoot 'windows\Release'
    Invoke-Checked 'chaos.txt' (Join-Path $release 'atlas-chaos.exe') @('--iterations', "$ChaosIterations", '--seed', '20260920')
    for ($run=1; $run -le 3; ++$run) {
        Invoke-Checked "benchmark-$run.txt" (Join-Path $release 'atlas-bench.exe') @('--records', '10000')
    }
    $sample = Join-Path $logs 'example.db'
    Invoke-Checked 'example.txt' (Join-Path $release 'atlas-example.exe') @($sample)
    Invoke-Checked 'integrity.txt' (Join-Path $release 'atlasdb.exe') @('verify', $sample)
    Invoke-Checked 'wal-inspection.txt' (Join-Path $release 'atlasdb.exe') @('wal', $sample)
    Invoke-Checked 'tree.dot' (Join-Path $release 'atlasdb.exe') @('tree', $sample)
    Write-Host "Verification complete. Raw evidence: $logs"
} finally { Pop-Location }
