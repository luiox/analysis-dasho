# 检查 CONVENTION.md §3.2 的五条禁止句。
#
#   用法:  pwsh -File tools/check-convention.ps1
#   退出码: 0 = 无 FAIL；1 = 有 FAIL（TODO 不影响退出码）
#
# 三档结果：
#   FAIL  真违规，必须改
#   TODO  已知未迁移——约定已定，代码还没跟上，写在这里当迁移清单
#   OK    通过
#
# 规则 3/5（scan 不许改字节码、proof 不许修改）靠"分区标记内出现改写调用"来近似：
# 文件已经打了 ==== SCAN / REWRITE / PROOF ==== 分区，语义约束就能退化成文本约束。

param([string]$Root = (Split-Path $PSScriptRoot -Parent))

$ErrorActionPreference = 'Stop'
$enc   = New-Object System.Text.UTF8Encoding($false)
$src   = Join-Path $Root 'src\dasho'
$files = Get-ChildItem $src -Filter '*_pass.cpp'

$fail = New-Object System.Collections.ArrayList
$todo = New-Object System.Collections.ArrayList

function Add-Finding([string]$sev, [string]$file, [int]$line, [string]$rule, [string]$text) {
    $rec = [pscustomobject]@{ sev=$sev; file=$file; line=$line; rule=$rule; text=$text.Trim() }
    if ($sev -eq 'FAIL') { [void]$script:fail.Add($rec) } else { [void]$script:todo.Add($rec) }
}

# ── 已知的 scan 判据函数：rewrite 段里出现它们的调用 = 第 4 条违规 ──
$criteria = @(
    'findRegion(', 'isHammock(', 'hasReflectionTriple(', 'hasNoisePredicate(', 'hasResponse(',
    'isFakeExceptionClass(', 'computeReachable(', 'computeLiveness(', 'neutralWindow(',
    'structurallyOk(', 'localsNotEscaping(', 'anchorDepthUniform(', 'isAnchor(',
    'looksLikeCheckRuntimeFacade(', 'collectCheckRuntimeFacades('
)

# ── 适配层词汇的白名单：这些是 mjbase SPI 的名字，迁移 adapter 目录之前留着 ──
$adapterAllowed = @('DashoStringDecryptPass', 'DashoControlFlowPass', 'DashoChecksPass',
                    'DashoCleanupPass', 'DashoRenamePass', 'registerStringDecrypt',
                    'registerControlFlow', 'registerChecks', 'registerCleanup', 'registerRename',
                    'PassNameRegistry', 'IPassPack', 'PreservedAnalyses')

foreach ($f in $files) {
    $L = [System.IO.File]::ReadAllLines($f.FullName, $enc)

    # 划分分区：每个 ==== 横幅之后的行都属于该分区，直到下一个横幅
    $section = New-Object 'int[]' $L.Count      # 0=未分区 1=SCAN 2=REWRITE 3=PROOF
    $cur = 0
    for ($k = 0; $k -lt $L.Count; $k++) {
        if     ($L[$k] -like '*==== SCAN*')     { $cur = 1 }
        elseif ($L[$k] -like '*==== REWRITE*')  { $cur = 2 }
        elseif ($L[$k] -like '*==== PROOF*')    { $cur = 3 }
        $section[$k] = $cur
    }
    if ($cur -eq 0) { Add-Finding 'FAIL' $f.Name 0 '分区' '整个文件没有 ==== SCAN / REWRITE / PROOF ==== 分区标记' }

    for ($k = 0; $k -lt $L.Count; $k++) {
        $x   = $L[$k]
        $n   = $k + 1
        $sec = $section[$k]

        # ── 第 1 条：残留文件不许出现 pass / pipeline / stage 词汇 ──
        # 例外仅限适配层标识符（迁移 adapter 目录后应清空）与库 API（dom.order()）。
        if ($x -match '(?i)\b(pipeline|stage)\b' -or
           ($x -match '(?i)\bpass\b' -and $x -notmatch '(?i)^\s*//\s*----')) {
            $clean = $x
            foreach ($w in $adapterAllowed) { $clean = $clean -replace [regex]::Escape($w), '<adapter>' }
            if ($clean -match '(?i)\bpass\b|\bpipeline\b|\bstage\b') {
                Add-Finding 'TODO' $f.Name $n '§3.2-1 词汇' $x
            }
        }

        # ── 第 2 条：knowledge/ 之外不许有版本字面量 ──
        if ($x -match '\b12\.\d+\b') { Add-Finding 'TODO' $f.Name $n '§3.2-2 版本字面量' $x }
        if ($x -match '45u\s*<<\s*16') {
            Add-Finding 'TODO' $f.Name $n '§3.2-2 版本常量未进 knowledge/' $x
        }

        # ── 第 3 条：SCAN 不许改字节码 / 异常表 / 常量池 ──
        # 注释不计（提到 CodeEditor 这个词不等于用了它）；`.erase(` 不收，
        # 因为 `std::vector::erase` 会全命中。
        $isComment = $x.TrimStart() -like '//*'
        $mutates   = $x -match 'CodeEditor\s+\w+\s*[(&]|\.eraseBatch\(|\.destroyLabel\(|\.exc\s*=|\.setTarget\('
        if ($sec -eq 1 -and -not $isComment -and $mutates) {
            Add-Finding 'FAIL' $f.Name $n '§3.2-3 SCAN 改字节码' $x
        }

        # ── 第 4 条：REWRITE 不许回头找站点 ──
        # 当前有 4 处已知偏离（rewrite 内部仍在调 scan 判据），列为迁移清单；
        # 迁完（改成 consume ScanResult）后把 sev 提成 FAIL。
        if ($sec -eq 2 -and -not $isComment) {
            foreach ($c in $criteria) {
                if ($x -like "*$c*") { Add-Finding 'TODO' $f.Name $n '§3.2-4 REWRITE 回头找站点' $x; break }
            }
        }

        # ── 第 5 条：PROOF 不许修改，只判定 + 回滚 ──
        # 当前 1 处已知偏离（control_flow 在 PROOF 段删死类），同上待迁移。
        if ($sec -eq 3 -and -not $isComment -and $mutates) {
            Add-Finding 'TODO' $f.Name $n '§3.2-5 PROOF 修改了' $x
        }
        if ($sec -eq 3 -and -not $isComment -and $x -match '\beraseResource\(') {
            Add-Finding 'TODO' $f.Name $n '§3.2-5 PROOF 修改了（删死类）' $x
        }
    }
}

# ── 工具箱收口：同一原语不许出现两份实现 ──
foreach ($fn in @('threadGotos', 'pruneUnreachable', 'tidyGotoToNext', 'foldStoreLoadPairs')) {
    $hits = Select-String -Path (Join-Path $src '*_pass.cpp') -Pattern "^\s*(?:inline\s+)?ca::usize\s+$fn\s*\(" -Encoding utf8
    if ($hits) {
        foreach ($h in $hits) { Add-Finding 'FAIL' $h.Filename $h.LineNumber '§7 复用未收口' $h.Line }
    }
}

Write-Output ('=' * 78)
Write-Output ("残留文件: {0} 个   FAIL: {1}   TODO: {2}" -f $files.Count, $fail.Count, $todo.Count)
Write-Output ('=' * 78)
if ($fail.Count) {
    Write-Output "`n--- FAIL（必须改）---"
    $fail | Format-Table -AutoSize | Out-String -Width 200 | Write-Output
}
if ($todo.Count) {
    Write-Output "`n--- TODO（约定已定，代码未迁移）---"
    $todo | Group-Object rule | Sort-Object Name | ForEach-Object {
        Write-Output ("[{0}]  {1} 处" -f $_.Name, $_.Count)
        $_.Group | Select-Object -First 4 | ForEach-Object { Write-Output ("    {0}:{1}  {2}" -f $_.file, $_.line, $_.text) }
        if ($_.Count -gt 4) { Write-Output ("    … 还有 {0} 处" -f ($_.Count - 4)) }
    }
}
exit $(if ($fail.Count) { 1 } else { 0 })
