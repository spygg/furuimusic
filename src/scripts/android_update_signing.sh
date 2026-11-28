#!/usr/bin/env bash
set -euo pipefail

fail() {
    echo "[signing] $*" >&2
    exit 1
}

[[ -n "${RUNNER_TEMP:-}" ]] || fail "RUNNER_TEMP is required"
[[ -n "${ANDROID_HOME:-}" ]] || fail "ANDROID_HOME is required"
[[ -n "${ANDROID_BUILD_TOOLS:-}" ]] || fail "ANDROID_BUILD_TOOLS is required"
[[ -n "${JAVA_HOME:-}" ]] || fail "JAVA_HOME is required"

find_tool() {
    local base="$1" candidate
    shift
    for candidate in "$base" "$@"; do
        if [[ -f "$candidate" && -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

BT="$ANDROID_HOME/build-tools/$ANDROID_BUILD_TOOLS"
JAVA="$(find_tool "$JAVA_HOME/bin/java" "$JAVA_HOME/bin/java.exe")" \
    || fail "java not found under $JAVA_HOME/bin"
KEYTOOL="$(find_tool "$JAVA_HOME/bin/keytool" "$JAVA_HOME/bin/keytool.exe")" \
    || fail "keytool not found under $JAVA_HOME/bin"
ZIPALIGN="$(find_tool "$BT/zipalign" "$BT/zipalign.exe")" \
    || fail "zipalign not found under $BT"
APKSIGNER_JAR="$BT/lib/apksigner.jar"
STATE="$RUNNER_TEMP/furui-signing.env"
LEGACY_KS="$RUNNER_TEMP/furui-legacy.keystore"
RELEASE_KS="$RUNNER_TEMP/furui-release.keystore"
LEGACY_ALIAS="androiddebugkey"

[[ -s "$APKSIGNER_JAR" ]] || fail "apksigner jar not found: $APKSIGNER_JAR"

apksigner() {
    "$JAVA" -jar "$APKSIGNER_JAR" "$@"
}

certificate_sha256() {
    local ks="$1" pass="$2" alias="$3"
    "$KEYTOOL" -J-Duser.language=en -J-Duser.country=US \
        -list -v -keystore "$ks" -storepass "$pass" -alias "$alias" 2>/dev/null \
        | sed -n 's/^[[:space:]]*SHA256:[[:space:]]*//p' \
        | head -n 1 \
        | tr -d ':\r' | tr 'A-F' 'a-f'
}

write_state() {
    {
        printf 'RELEASE_KS=%q\n' "$RELEASE_KS"
        printf 'RELEASE_ALIAS=%q\n' "$RELEASE_ALIAS"
        printf 'LEGACY_KS=%q\n' "$LEGACY_KS"
        printf 'LEGACY_ALIAS=%q\n' "$LEGACY_ALIAS"
        printf 'RELEASE_CERT=%q\n' "$RELEASE_CERT"
        printf 'LEGACY_CERT=%q\n' "$LEGACY_CERT"
    } > "$STATE"
    chmod 600 "$STATE" "$LEGACY_KS" "$RELEASE_KS"
    if [[ -n "${GITHUB_ENV:-}" ]]; then
        printf 'FURUI_ANDROID_RELEASE_CERT_SHA256=%s\n' "$RELEASE_CERT" >> "$GITHUB_ENV"
    fi
}

prepare() {
    mkdir -p "$RUNNER_TEMP"
    base64 -d src/platform/android-qt6/furui-debug.keystore.b64 > "$LEGACY_KS"
    [[ -s "$LEGACY_KS" ]] || fail "legacy keystore is empty"

    if [[ -n "${RELEASE_KEYSTORE_B64:-}" && -n "${RELEASE_KEYSTORE_PASSWORD:-}" \
          && -n "${RELEASE_KEY_ALIAS:-}" && -n "${RELEASE_KEY_PASSWORD:-}" ]]; then
        printf '%s' "$RELEASE_KEYSTORE_B64" | base64 -d > "$RELEASE_KS"
        [[ -s "$RELEASE_KS" ]] || fail "release keystore is empty"
        RELEASE_ALIAS="$RELEASE_KEY_ALIAS"
        RELEASE_CERT="$(certificate_sha256 "$RELEASE_KS" "$RELEASE_KEYSTORE_PASSWORD" "$RELEASE_ALIAS")"
    elif [[ "${REQUIRE_RELEASE_KEY:-0}" = "1" ]]; then
        fail "formal tag release requires all four RELEASE_* signing secrets"
    else
        echo "[signing] generating an ephemeral non-release key for workflow artifacts"
        rm -f "$RELEASE_KS"
        "$KEYTOOL" -genkeypair -keystore "$RELEASE_KS" -storetype PKCS12 \
            -storepass android -keypass android -alias furui-ci-test \
            -keyalg RSA -keysize 2048 -validity 3650 \
            -dname "CN=FuruiMusic CI Test, O=FuruiMusic, C=CN" >/dev/null 2>&1
        RELEASE_ALIAS="furui-ci-test"
        RELEASE_CERT="$(certificate_sha256 "$RELEASE_KS" android "$RELEASE_ALIAS")"
    fi

    LEGACY_CERT="$(certificate_sha256 "$LEGACY_KS" android "$LEGACY_ALIAS")"
    [[ "$RELEASE_CERT" =~ ^[0-9a-f]{64}$ ]] || fail "cannot read release certificate SHA-256"
    [[ "$LEGACY_CERT" =~ ^[0-9a-f]{64}$ ]] || fail "cannot read legacy certificate SHA-256"
    [[ "$RELEASE_CERT" != "$LEGACY_CERT" ]] || fail "release and legacy signing identities must differ"
    write_state
    echo "[signing] release certificate SHA-256: $RELEASE_CERT"
    echo "[signing] legacy certificate SHA-256: $LEGACY_CERT"
}

sign_one() {
    local ks="$1" alias="$2" storepass="$3" keypass="$4" output="$5" expected="$6"
    apksigner sign --ks "$ks" --ks-key-alias "$alias" \
        --ks-pass "pass:$storepass" --key-pass "pass:$keypass" \
        --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true \
        --out "$output" "$ALIGNED"
    apksigner verify --print-certs "$output" >/dev/null
    "$ZIPALIGN" -c -p 4 "$output" >/dev/null
    local actual
    actual="$(apksigner verify --print-certs "$output" \
        | sed -n 's/^.*certificate SHA-256 digest:[[:space:]]*//p' \
        | head -n 1 | tr -d '\r' | tr 'A-F' 'a-f')"
    [[ "$actual" = "$expected" ]] || fail "certificate mismatch for $output"
}

sign_apks() {
    [[ $# -eq 3 ]] || fail "usage: $0 sign <unsigned.apk> <legacy.apk> <signed.apk>"
    [[ -f "$STATE" ]] || fail "run '$0 prepare' first"
    # shellcheck disable=SC1090
    source "$STATE"
    local unsigned="$1" legacy_output="$2" release_output="$3"
    [[ -s "$unsigned" ]] || fail "unsigned APK not found: $unsigned"
    ALIGNED="$RUNNER_TEMP/furui-aligned.apk"
    "$ZIPALIGN" -f -p 4 "$unsigned" "$ALIGNED"

    sign_one "$LEGACY_KS" "$LEGACY_ALIAS" android android "$legacy_output" "$LEGACY_CERT"
    if [[ -n "${RELEASE_KEYSTORE_B64:-}" && -n "${RELEASE_KEYSTORE_PASSWORD:-}" \
          && -n "${RELEASE_KEY_ALIAS:-}" && -n "${RELEASE_KEY_PASSWORD:-}" ]]; then
        sign_one "$RELEASE_KS" "$RELEASE_ALIAS" "$RELEASE_KEYSTORE_PASSWORD" \
            "$RELEASE_KEY_PASSWORD" "$release_output" "$RELEASE_CERT"
    else
        [[ "$RELEASE_ALIAS" = "furui-ci-test" ]] || fail "missing release signing credentials"
        sign_one "$RELEASE_KS" "$RELEASE_ALIAS" android android "$release_output" "$RELEASE_CERT"
    fi
    echo "[signing] legacy APK: $legacy_output"
    echo "[signing] release APK: $release_output"
}

case "${1:-}" in
    prepare) prepare ;;
    sign) shift; sign_apks "$@" ;;
    *) fail "usage: $0 prepare | sign <unsigned.apk> <legacy.apk> <signed.apk>" ;;
esac
