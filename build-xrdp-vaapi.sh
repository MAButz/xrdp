#!/usr/bin/env bash
#
# build-xrdp-vaapi.sh
#
# Baut xrdp (devel) mit dem VA-API-Hardware-H.264-Encoder im xrdp_accel_assist
# plus die passende xorgxrdp-Variante mit GBM-backed glamor.
#
#   Quelle xrdp:     MAButz/xrdp-1 Branch devel-vaapi
#                    (= neutrinolabs/xrdp devel + pletch/xrdp-vaapi-encode)
#   Quelle xorgxrdp: MAButz/xorgxrdp Branch feature/gbm-dmabuf-hwencode
#                    (= pletch/xorgxrdp-glamor-gbm)
#
# Zielsystem: Debian 12/13 oder Ubuntu 22.04/24.04 mit ECHTER GPU (/dev/dri
# vorhanden). Intel Gen9+ (iHD) ist der primaere Zielfall; AMD (radeonsi) geht
# ueber den High/EncSlice-Pfad ebenfalls.
#
# Als normaler User mit sudo-Rechten aufrufen, NICHT als root.
#
#   ./build-xrdp-vaapi.sh check       nur Voraussetzungen pruefen
#   ./build-xrdp-vaapi.sh deps        Build-Abhaengigkeiten installieren
#   ./build-xrdp-vaapi.sh fetch       Repos klonen/aktualisieren
#   ./build-xrdp-vaapi.sh build       beides kompilieren
#   ./build-xrdp-vaapi.sh install     make install + ldconfig
#   ./build-xrdp-vaapi.sh configure   sesman.ini / xrdp.ini / systemd drop-in
#   ./build-xrdp-vaapi.sh verify      Laufzeit-Check nach einer RDP-Session
#   ./build-xrdp-vaapi.sh all         deps -> check -> fetch -> build -> install -> configure
#
# Ueberschreibbar per Umgebungsvariable:
#   XRDP_REPO, XRDP_BRANCH, XORGXRDP_REPO, XORGXRDP_BRANCH, SRC_DIR, PREFIX, JOBS
#
set -euo pipefail

XRDP_REPO="${XRDP_REPO:-https://github.com/MAButz/xrdp-1.git}"
XRDP_BRANCH="${XRDP_BRANCH:-devel-vaapi}"
XORGXRDP_REPO="${XORGXRDP_REPO:-https://github.com/MAButz/xorgxrdp.git}"
XORGXRDP_BRANCH="${XORGXRDP_BRANCH:-feature/gbm-dmabuf-hwencode}"

SRC_DIR="${SRC_DIR:-$HOME/src/xrdp-vaapi}"
PREFIX="${PREFIX:-/usr/local}"
JOBS="${JOBS:-$(nproc)}"

XRDP_SRC="$SRC_DIR/xrdp"
XORGXRDP_SRC="$SRC_DIR/xorgxrdp"

# --------------------------------------------------------------------------- #

if [ -t 1 ]; then
    RED=$(printf '\033[31m'); GRN=$(printf '\033[32m')
    YEL=$(printf '\033[33m'); BLD=$(printf '\033[1m'); RST=$(printf '\033[0m')
else
    RED=""; GRN=""; YEL=""; BLD=""; RST=""
fi

step() { printf '\n%s==> %s%s\n' "$BLD" "$*" "$RST"; }
ok()   { printf '  %s[ ok ]%s %s\n' "$GRN" "$RST" "$*"; }
warn() { printf '  %s[warn]%s %s\n' "$YEL" "$RST" "$*"; }
bad()  { printf '  %s[fail]%s %s\n' "$RED" "$RST" "$*"; }
die()  { bad "$*" >&2; exit 1; }

need_sudo() {
    [ "$(id -u)" -eq 0 ] && die "bitte NICHT als root ausfuehren (sudo wird gezielt benutzt)"
    sudo -v || die "sudo-Rechte werden gebraucht"
}

# --------------------------------------------------------------------------- #
# check
# --------------------------------------------------------------------------- #

cmd_check() {
    local fatal=0

    step "Distribution"
    [ -r /etc/os-release ] || die "/etc/os-release fehlt - nicht unterstuetztes System"
    . /etc/os-release
    ok "${PRETTY_NAME:-unbekannt}"
    case "${ID:-}${ID_LIKE:-}" in
        *debian*|*ubuntu*) ;;
        *) warn "nur fuer Debian/Ubuntu getestet - die Paketnamen unten stimmen evtl. nicht" ;;
    esac

    step "GPU / DRM-Device"
    if [ ! -d /dev/dri ]; then
        bad "/dev/dri fehlt - kein GPU-Zugriff, Hardware-Encode ist unmoeglich."
        echo "         Typisch fuer WSL2 ohne GPU-Passthrough oder eine VM ohne vGPU."
        fatal=1
    else
        if ls /dev/dri/renderD* >/dev/null 2>&1; then
            ok "Render-Nodes: $(ls /dev/dri/renderD* | tr '\n' ' ')"
        else
            bad "kein /dev/dri/renderD* vorhanden"
            fatal=1
        fi
        if id -nG "$USER" | tr ' ' '\n' | grep -qx render; then
            ok "$USER ist in Gruppe 'render'"
        else
            warn "$USER ist NICHT in Gruppe 'render' - der Session-User braucht Zugriff auf renderD128"
            warn "  sudo usermod -aG render,video $USER    (danach neu anmelden)"
        fi
    fi

    step "VA-API Encode-Faehigkeit"
    if ! command -v vainfo >/dev/null 2>&1; then
        warn "vainfo nicht installiert - 'deps' holt es nach"
    else
        local vi
        vi="$(vainfo 2>&1 || true)"
        if printf '%s\n' "$vi" | grep -q 'Driver version'; then
            ok "$(printf '%s\n' "$vi" | grep 'Driver version' | head -1 | sed 's/^ *//')"
        else
            bad "vainfo liefert keinen Treiber - libva findet die GPU nicht"
            fatal=1
        fi
        if printf '%s\n' "$vi" | grep -qE 'VAProfileH264(High|Main).*VAEntrypointEncSliceLP'; then
            ok "H.264 Low-Power-Encode vorhanden (bevorzugter Pfad, High/EncSliceLP)"
        elif printf '%s\n' "$vi" | grep -qE 'VAProfileH264(High|Main).*VAEntrypointEncSlice'; then
            ok "H.264 Encode vorhanden (regulaerer Entrypoint)"
        else
            bad "kein VAEntrypointEncSlice* fuer H.264 - GPU/Treiber kann nicht kodieren"
            echo "         Intel: intel-media-va-driver-non-free installieren, LIBVA_DRIVER_NAME=iHD setzen."
            fatal=1
        fi
    fi

    step "Xorg"
    if pkg-config --exists xorg-server 2>/dev/null; then
        local xv
        xv="$(pkg-config --modversion xorg-server)"
        ok "xorg-server $xv (Header vorhanden)"
        if ! dpkg --compare-versions "$xv" ge 1.19; then
            bad "glamor braucht xorg-server >= 1.19"
            fatal=1
        fi
    else
        warn "xserver-xorg-dev nicht installiert - kommt mit 'deps'"
    fi

    step "Ergebnis"
    [ "$fatal" -eq 0 ] || die "Voraussetzungen nicht erfuellt - siehe [fail] oben"
    ok "System taugt fuer den Hardware-Encode-Pfad"
}

# --------------------------------------------------------------------------- #
# deps
# --------------------------------------------------------------------------- #

cmd_deps() {
    need_sudo
    step "Build-Abhaengigkeiten installieren"

    local pkgs=(
        # Toolchain
        build-essential autoconf automake libtool pkg-config nasm git ca-certificates
        # xrdp Kern
        libssl-dev libpam0g-dev libjpeg-dev libpixman-1-dev libfuse3-dev
        libopus-dev libx264-dev
        # X11
        libx11-dev libxfixes-dev libxrandr-dev libxext-dev libxdamage-dev
        # GL / GPU (accel-assist + glamor)
        libepoxy-dev libdrm-dev libgbm-dev libegl-dev libgl-dev
        # VA-API
        libva-dev libva-drm-dev vainfo
        # xorgxrdp
        xserver-xorg-dev xserver-xorg-core
    )

    sudo apt-get update
    sudo apt-get install -y "${pkgs[@]}"

    step "VA-API-Treiber passend zur GPU"
    if command -v lspci >/dev/null 2>&1; then
        if lspci | grep -qi 'intel'; then
            sudo apt-get install -y intel-media-va-driver-non-free \
                || sudo apt-get install -y intel-media-va-driver \
                || warn "intel-media-va-driver nicht installierbar - non-free-Repo aktiviert?"
        fi
        if lspci | grep -qiE 'amd|ati'; then
            sudo apt-get install -y mesa-va-drivers || warn "mesa-va-drivers nicht installierbar"
        fi
    else
        warn "lspci fehlt - VA-Treiber bitte selbst waehlen (intel-media-va-driver / mesa-va-drivers)"
    fi
    ok "Abhaengigkeiten installiert"
}

# --------------------------------------------------------------------------- #
# fetch
# --------------------------------------------------------------------------- #

sync_repo() {
    local url="$1" branch="$2" dir="$3" name="$4"
    if [ -d "$dir/.git" ]; then
        step "$name aktualisieren ($dir)"
        git -C "$dir" remote set-url origin "$url"
        git -C "$dir" fetch --prune origin
        git -C "$dir" checkout "$branch"
        git -C "$dir" reset --hard "origin/$branch"
    else
        step "$name klonen -> $dir"
        git clone --branch "$branch" "$url" "$dir"
    fi
    git -C "$dir" submodule update --init --recursive
    ok "$name @ $(git -C "$dir" log --oneline -1)"
}

cmd_fetch() {
    mkdir -p "$SRC_DIR"
    sync_repo "$XRDP_REPO"     "$XRDP_BRANCH"     "$XRDP_SRC"     "xrdp"
    sync_repo "$XORGXRDP_REPO" "$XORGXRDP_BRANCH" "$XORGXRDP_SRC" "xorgxrdp"
}

# --------------------------------------------------------------------------- #
# build
# --------------------------------------------------------------------------- #

cmd_build() {
    [ -d "$XRDP_SRC" ]     || die "$XRDP_SRC fehlt - erst 'fetch' laufen lassen"
    [ -d "$XORGXRDP_SRC" ] || die "$XORGXRDP_SRC fehlt - erst 'fetch' laufen lassen"

    step "xrdp konfigurieren und bauen"
    cd "$XRDP_SRC"
    ./bootstrap
    ./configure \
        --prefix="$PREFIX" \
        --sysconfdir=/etc \
        --localstatedir=/var \
        --enable-vaapi \
        --enable-rfxcodec \
        --enable-x264 \
        --enable-jpeg \
        --enable-fuse \
        --enable-opus \
        --enable-pixman
    make -j"$JOBS"
    [ -x xrdp_accel_assist/xrdp-accel-assist ] \
        || die "xrdp-accel-assist wurde nicht gebaut - 'vaapi yes' im configure-Summary pruefen"
    ok "xrdp gebaut, xrdp-accel-assist vorhanden"

    step "xorgxrdp konfigurieren und bauen"
    cd "$XORGXRDP_SRC"
    # xorgxrdp sucht per pkg-config nach xrdp >= 0.10.80. xrdp.pc liegt vor dem
    # 'install' nur im Build-Baum, darum den hier mit in den Suchpfad nehmen.
    export PKG_CONFIG_PATH="$XRDP_SRC:$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig:${PKG_CONFIG_PATH:-}"
    ./bootstrap
    ./configure --prefix="$PREFIX" --enable-glamor
    make -j"$JOBS"
    ok "xorgxrdp gebaut (mit glamor/GBM)"
}

# --------------------------------------------------------------------------- #
# install
# --------------------------------------------------------------------------- #

cmd_install() {
    need_sudo

    step "Dienste stoppen"
    sudo systemctl stop xrdp xrdp-sesman 2>/dev/null || true

    step "xrdp installieren"
    sudo make -C "$XRDP_SRC" install
    sudo ldconfig
    ok "xrdp -> $PREFIX"

    step "xorgxrdp installieren"
    sudo make -C "$XORGXRDP_SRC" install
    ok "xorgxrdp -> Xorg-Moduldir"

    step "Installation pruefen"
    local aa="$PREFIX/libexec/xrdp/xrdp-accel-assist"
    if [ ! -x "$aa" ]; then
        aa="$(find "$PREFIX" -name xrdp-accel-assist -type f 2>/dev/null | head -1)"
    fi
    if [ -n "$aa" ] && [ -x "$aa" ]; then
        ok "accel-assist: $aa"
        if command -v ldd >/dev/null 2>&1; then
            ldd "$aa" | grep -q libva \
                && ok "gegen libva gelinkt - der VA-API-Pfad ist einkompiliert" \
                || die "NICHT gegen libva gelinkt - --enable-vaapi hat nicht gegriffen"
        fi
    else
        die "xrdp-accel-assist nach dem Install nicht gefunden"
    fi
}

# --------------------------------------------------------------------------- #
# configure
# --------------------------------------------------------------------------- #

BEGIN_MARK="# >>> build-xrdp-vaapi.sh >>>"
END_MARK="# <<< build-xrdp-vaapi.sh <<<"

# Ersetzt den markierten Block in einer Datei bzw. haengt ihn an. Idempotent.
put_block() {
    local file="$1" block="$2" tmp
    sudo test -f "$file" || die "$file existiert nicht"
    [ -f "$file.pre-vaapi" ] || sudo cp -a "$file" "$file.pre-vaapi"

    tmp="$(mktemp)"
    sudo awk -v b="$BEGIN_MARK" -v e="$END_MARK" '
        index($0, b) == 1 { skip = 1 }
        skip != 1         { print }
        index($0, e) == 1 { skip = 0 }
    ' "$file" > "$tmp"
    printf '%s\n%s\n%s\n' "$BEGIN_MARK" "$block" "$END_MARK" >> "$tmp"
    sudo cp "$tmp" "$file"
    rm -f "$tmp"
}

cmd_configure() {
    need_sudo

    local sesman_ini=/etc/xrdp/sesman.ini
    local xrdp_ini=/etc/xrdp/xrdp.ini

    step "sesman.ini: [SessionVariables] (erreicht Xorg / xorgxrdp / accel-assist)"
    sudo grep -q '^\[SessionVariables\]' "$sesman_ini" \
        || die "[SessionVariables] fehlt in $sesman_ini - ist das die sesman.ini aus diesem Build?"
    # [SessionVariables] ist der letzte Abschnitt der Default-Datei, darum reicht Anhaengen.
    put_block "$sesman_ini" "XRDP_USE_ACCEL_ASSIST=1
XRDP_VAAPI_QP=26
XRDP_AVC444_CHROMA_INTERVAL=4
XRDP_AVC444_CHROMA_MAX_MS=200
LIBVA_DRIVER_NAME=iHD
# Multi-GPU: hier und in xorgxrdp (DRMDevice) dieselbe Karte pinnen
#XRDP_VAAPI_DEVICE=/dev/dri/renderD128
# Adaptives Capture-Pacing (xorgxrdp) - laut Fork 26 -> 51 fps
XORGXRDP_ADAPTIVE_PACE=1
XORGXRDP_CAPTURE_DEPTH=2"
    ok "$sesman_ini ergaenzt (Original: $sesman_ini.pre-vaapi)"

    step "systemd drop-in: Variablen, die der xrdp-Daemon selbst liest"
    sudo mkdir -p /etc/systemd/system/xrdp.service.d
    sudo tee /etc/systemd/system/xrdp.service.d/10-vaapi.conf >/dev/null <<'UNIT'
# Von build-xrdp-vaapi.sh erzeugt.
# Diese Variablen liest der xrdp-Daemon, nicht die Session:
# [SessionVariables] aus sesman.ini erreicht ihn nicht.
[Service]
Environment=XRDP_GFX_FRAMES_IN_FLIGHT=2
#Environment=XRDP_GFX_FRAME_LOG=1
UNIT
    sudo systemctl daemon-reload
    ok "/etc/systemd/system/xrdp.service.d/10-vaapi.conf"

    step "xrdp.ini: [Xorg] h264_frame_interval"
    if sudo grep -qE '^[[:space:]]*h264_frame_interval' "$xrdp_ini"; then
        sudo sed -i 's/^[[:space:]]*h264_frame_interval.*/h264_frame_interval=33/' "$xrdp_ini"
    else
        sudo sed -i '/^\[Xorg\]/a h264_frame_interval=33' "$xrdp_ini"
    fi
    ok "h264_frame_interval=33 (= 30 fps; an den Inhalt anpassen)"

    step "Rechte auf das Render-Node"
    warn "Jeder User, der eine RDP-Session bekommt, braucht Zugriff auf /dev/dri/renderD128:"
    warn "  sudo usermod -aG render,video <username>"

    step "Dienste starten"
    sudo systemctl restart xrdp-sesman xrdp
    if systemctl is-active --quiet xrdp; then
        ok "xrdp laeuft"
    else
        warn "xrdp laeuft nicht - 'journalctl -u xrdp -n 50'"
    fi
}

# --------------------------------------------------------------------------- #
# verify
# --------------------------------------------------------------------------- #

cmd_verify() {
    step "Pruefung nach einer verbundenen RDP-Session"

    echo
    echo "1) Encoder-Auswahl im Session-Log"
    local alog
    alog="$(ls -t "$HOME"/.local/share/xrdp/xrdp-accel-assist.*.log 2>/dev/null | head -1 || true)"
    if [ -n "$alog" ]; then
        grep -E 'vaapi_init|using H\.264|using EGL|using GLX' "$alog" | tail -10 \
            || warn "keine passenden Zeilen in $alog"
    else
        warn "kein xrdp-accel-assist.*.log unter ~/.local/share/xrdp/"
        warn "  (Pfad ueberschreibbar mit XRDP_ACCEL_ASSIST_LOG_PATH)"
    fi

    echo
    echo "2) Hardware-Pfad im Daemon-Log"
    if sudo grep -m1 'hardware encoding active' /var/log/xrdp.log 2>/dev/null; then
        ok "Hardware-Encode bestaetigt"
    else
        warn "noch keine 'hardware encoding active'-Zeile"
        warn "  Client muss EGFX/H.264 anfordern UND 32 Bit Farbtiefe verwenden"
    fi

    echo
    echo "3) glamor statt llvmpipe im Xorg-Log"
    local xlog
    xlog="$(ls -t /var/log/xrdp-Xorg*.log "$HOME"/.local/share/xorg/Xorg*.log 2>/dev/null | head -1 || true)"
    if [ -n "$xlog" ]; then
        grep -iE 'glamor|llvmpipe|DRI3' "$xlog" | tail -10 || true
    else
        warn "kein Xorg-Log gefunden"
    fi

    echo
    echo "4) GPU-Last waehrend der Session (Intel)"
    echo "   sudo intel_gpu_top    # die Zeile 'Video' muss Last zeigen, nicht nur 'Render/3D'"
}

# --------------------------------------------------------------------------- #

case "${1:-all}" in
    check)     cmd_check ;;
    deps)      cmd_deps ;;
    fetch)     cmd_fetch ;;
    build)     cmd_build ;;
    install)   cmd_install ;;
    configure) cmd_configure ;;
    verify)    cmd_verify ;;
    all)
        cmd_deps
        cmd_check
        cmd_fetch
        cmd_build
        cmd_install
        cmd_configure
        step "Fertig"
        ok "Jetzt mit einem EGFX-faehigen Client bei 32 Bit Farbtiefe verbinden,"
        ok "danach './build-xrdp-vaapi.sh verify' laufen lassen."
        ;;
    *)
        sed -n '2,28p' "$0"
        exit 1
        ;;
esac
