# Run the pattern generator's unit testbench (sim/tb_patgen.v) in the Vivado simulator.
# Usage (from the repository root): .\sim\run_patgen.ps1
param(
    [string]$Vivado = 'I:\Xilinx\Vivado\2023.2\bin'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'build\sim_patgen'
New-Item -ItemType Directory -Force $out | Out-Null

Push-Location $out
try {
    & "$Vivado\xvlog.bat" -nolog (Join-Path $root 'rtl\patgen.v') (Join-Path $root 'sim\tb_patgen.v') | Select-String 'ERROR|WARNING'
    & "$Vivado\xelab.bat" -nolog -debug off -timescale 1ns/1ps tb_patgen -s patgen_snap | Select-String 'ERROR'
    & "$Vivado\xsim.bat" patgen_snap -nolog -R |
        Where-Object { $_ -notmatch '^(INFO|\*\*\*\*|Vivado Simulator|Time resolution|  \*\*|source |# |ECHO)' }
}
finally {
    Pop-Location
}
