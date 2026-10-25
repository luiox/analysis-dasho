#!/usr/bin/env bash
# 语料回归：对 run-lab 下全部样本 jar 跑完整 pass 链，检查
#   1) CLI 退出码                 2) 输出 jar 结构完整（unzip -t）
#   3) 类数不增（死类清扫只允许减少）
#   4) 若存在同名 .map 则连带跑 dasho.Rename
#   5) 逐类加载校验（JDK 可用时；每个类都过一遍校验器，含死代码）
#
# 用法：test/corpus-regress.sh [pass 列表]
#   默认 pass 链 = dasho.Checks,dasho.ControlFlow,dasho.StringDecrypt,dasho.Cleanup
#   环境变量：CORPUS（语料根）、OUT（产物目录）、NO_VERIFY=1（跳过校验）
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLI="$REPO/build/windows/x64/release/dasho_cli.exe"
VERIFIER="$REPO/test/tools/VerifyJar.java"
CORPUS="${CORPUS:-}"
OUT="${OUT:-$REPO/build/regress}"

PASSES="${1:-dasho.Checks,dasho.ControlFlow,dasho.StringDecrypt,dasho.Cleanup}"

[ -x "$CLI" ] || { echo "缺少 CLI: $CLI"; exit 2; }
[ -n "$CORPUS" ] || { echo "缺少 CORPUS（语料根，用环境变量指定）"; exit 2; }
mkdir -p "$OUT"

# 校验器需要 JDK；没有就降级为"不校验"，但要显式说出来。
verify_jar() { # $1=jar  → 输出 "classes=N ok=N failed=N" 或空
    [ "${NO_VERIFY:-0}" = "1" ] && return 0
    command -v java >/dev/null 2>&1 || return 0
    java -Xverify:all "$VERIFIER" "$1" --quiet 2>/dev/null
}

classcount() { # $1=jar  → 输出 class 条目数
    unzip -Z1 "$1" 2>/dev/null | grep -c '\.class$'
}

if [ "${NO_VERIFY:-0}" != "1" ] && command -v java >/dev/null 2>&1; then
    echo "校验：java -Xverify:all（逐类加载）"
else
    echo "校验：跳过（NO_VERIFY=1 或无 java）"
fi

total=0; ok=0; bad=0; skipped=0
printf '%-46s %6s %6s  %s\n' "sample" "in" "out" "result"
printf '%s\n' "----------------------------------------------------------------------------"

while IFS= read -r jar; do
    base="$(basename "$jar" .jar)"
    case "$base" in
        *-out|*-dec|*-restored|proto-*) skipped=$((skipped+1)); continue ;;
    esac
    total=$((total+1))
    dst="$OUT/$base.jar"
    rm -f "$dst"

    map=""
    [ -f "${jar%.jar}.map" ] && map="${jar%.jar}.map"

    if [ -n "$map" ]; then
        log=$("$CLI" -i "$jar" -o "$dst" -p "$PASSES,dasho.Rename" -m "$map" 2>&1)
    else
        log=$("$CLI" -i "$jar" -o "$dst" -p "$PASSES" 2>&1)
    fi
    rc=$?

    cin=$(classcount "$jar"); cout=$(classcount "$dst")

    verdict=""
    if [ $rc -ne 0 ]; then
        verdict="FAIL rc=$rc"
    elif [ ! -f "$dst" ] || ! unzip -tq "$dst" >/dev/null 2>&1; then
        verdict="FAIL 输出 jar 损坏"
    elif [ "$cout" -gt "$cin" ]; then
        verdict="FAIL 类数增长 $cin→$cout"
    else
        vres=$(verify_jar "$dst")
        if [ -n "$vres" ] && ! grep -q 'failed=0$' <<<"$vres"; then
            verdict="FAIL 校验 $vres"
        elif grep -qE '^(WARN|ERROR)' <<<"$log"; then
            verdict="WARN $(grep -cE '^(WARN|ERROR)' <<<"$log") 条日志"
        else
            verdict="ok"
        fi
    fi

    case "$verdict" in
        ok) ok=$((ok+1)) ;;
        WARN*) ok=$((ok+1)) ;;
        *)  bad=$((bad+1)); echo "$log" | sed 's/^/      /' ;;
    esac
    printf '%-46s %6s %6s  %s\n' "$base" "$cin" "$cout" "$verdict"
done < <(find "$CORPUS" -name '*.jar' | sort)

printf '%s\n' "----------------------------------------------------------------------------"
printf 'total=%d ok=%d bad=%d skipped=%d\n' "$total" "$ok" "$bad" "$skipped"
[ "$bad" -eq 0 ]
