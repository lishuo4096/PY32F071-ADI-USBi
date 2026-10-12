$ErrorActionPreference = 'Continue'
$root = 'C:\Users\lishuo\Desktop\Vscode\Embedded\EIDE\Alternative_ADI_USBI'
$gcc  = 'C:\Users\lishuo\.eide\tools\gcc_arm\bin\arm-none-eabi-gcc.exe'
$out  = Join-Path $root 'EIDE\_objcheck'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$incs = @(
  "$root\Core\INC",
  "$root\Middlewares\Third_Party\CherryUSB\class",
  "$root\Middlewares\Third_Party\CherryUSB\common",
  "$root\Middlewares\Third_Party\CherryUSB\core",
  "$root\Middlewares\Third_Party\CherryUSB\port",
  "$root\Drivers\CMSIS\Include",
  "$root\Drivers\CMSIS\Device\PY32F071\Include",
  "$root\Drivers\PY32F071_HAL_Driver\Inc"
) | ForEach-Object { "-I$_" }

$cflags = @('-mcpu=cortex-m0plus','-mthumb','-Og','-std=c11','-Wall','-Wextra',
            '-ffunction-sections','-fdata-sections','-DPY32F071xB','-DUSE_HAL_DRIVER') + $incs

# ---- 语法检查：本轮流式补丁碰过的文件 ----
$syntaxTargets = @(
  "$root\Core\SRC\usbi.c",
  "$root\Middlewares\Third_Party\CherryUSB\core\usbd_core.c",
  "$root\Middlewares\Third_Party\CherryUSB\port\usb_dc_py32.c"
)
Write-Host "===== 语法检查（-Wall -Wextra -Wpedantic）====="
foreach ($f in $syntaxTargets) {
  $o = Join-Path $out ((Split-Path $f -Leaf) + '.syntax.o')
  & $gcc @cflags -Wpedantic -fsyntax-only $f 2>&1 | ForEach-Object { Write-Host $_ }
  Write-Host ("{0} -> exit {1}" -f (Split-Path $f -Leaf), $LASTEXITCODE)
}

# ---- 全量编译 + 链接 ----
$srcs = @()
$srcs += Get-ChildItem "$root\Core\SRC" -Filter *.c | ForEach-Object { $_.FullName }
$srcs += "$root\Middlewares\Third_Party\CherryUSB\core\usbd_core.c"
$srcs += "$root\Middlewares\Third_Party\CherryUSB\port\usb_dc_py32.c"
$srcs += "$root\Middlewares\Third_Party\CherryUSB\class\hid\usbd_hid.c"
$srcs += Get-ChildItem "$root\Drivers\PY32F071_HAL_Driver\Src" -Filter *.c |
         Where-Object { $_.Name -notlike '*_template.c' } | ForEach-Object { $_.FullName }

Write-Host "===== 全量编译：$($srcs.Count) 个源文件 ====="
$objs = @()
$fail = 0
foreach ($s in $srcs) {
  $o = Join-Path $out ((Split-Path $s -Leaf) + '.o')
  & $gcc @cflags -c $s -o $o 2>&1 | ForEach-Object { Write-Host $_ }
  if ($LASTEXITCODE -ne 0) { $fail++; Write-Host ("FAIL: {0}" -f $s) } else { $objs += $o }
}
Write-Host ("编译失败数 = {0} / {1}" -f $fail, $srcs.Count)
if ($fail -ne 0) { exit 1 }

Write-Host "===== 链接 ====="
& $gcc '-mcpu=cortex-m0plus' '-mthumb' @objs "$root\EIDE\startup_py32f071.s" `
      '-T' "$root\EIDE\PY32F071K1BU6.ld" `
      '--specs=nosys.specs' '--specs=nano.specs' '-Wl,--gc-sections' `
      '-Wl,-Map=C:\Users\lishuo\Desktop\Vscode\Embedded\EIDE\Alternative_ADI_USBI\EIDE\_check_stream.map' `
      '-o' "$out\_check_stream.elf" 2>&1 | ForEach-Object { Write-Host $_ }
Write-Host ("链接 exit = {0}" -f $LASTEXITCODE)
& 'C:\Users\lishuo\.eide\tools\gcc_arm\bin\arm-none-eabi-size.exe' "$out\_check_stream.elf"
