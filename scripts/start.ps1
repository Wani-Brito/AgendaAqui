$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $projectRoot

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    throw "CMake não foi encontrado. Instale o CMake e tente novamente."
}

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $cppBuildTools = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $cppBuildTools) {
        throw "O compilador C++ não está instalado. Abra o Visual Studio Installer, clique em Modificar e marque Desenvolvimento para desktop com C++."
    }
} else {
    throw "O Visual Studio Build Tools não foi encontrado. Instale o Visual Studio 2022 Build Tools com a carga Desenvolvimento para desktop com C++."
}

$releaseExecutable = "$projectRoot\build-vs\Release\agendaaqui.exe"
$runningApp = Get-CimInstance Win32_Process -Filter "Name='agendaaqui.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ExecutablePath -eq $releaseExecutable }
if ($runningApp) {
    throw "O AgendaAqui já está aberto. Volte ao terminal que está executando o servidor, pressione Ctrl+C e tente novamente."
}

cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) {
    throw "A configuração do projeto falhou. Verifique se um compilador C++ (Visual Studio Build Tools com C++) está instalado."
}

cmake --build build-vs --config Release
if ($LASTEXITCODE -ne 0) {
    throw "A compilação falhou. Verifique a instalação do compilador C++."
}

$executable = "$projectRoot\build-vs\Release\agendaaqui.exe"
if (-not (Test-Path $executable)) {
    $executable = "$projectRoot\build-vs\agendaaqui.exe"
}
if (-not (Test-Path $executable)) {
    throw "O executável do AgendaAqui não foi encontrado após a compilação."
}

& $executable
if ($LASTEXITCODE -ne 0) {
    throw "O servidor terminou com erro."
}
