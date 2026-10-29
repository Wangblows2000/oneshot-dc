#!/usr/bin/env bash
# OneShot DC - development environment.  Usage:  source scripts/env.sh
# Configurable via environment: KOS_BASE (default /opt/toolchains/dc/kos),
# MKDCDISC (path to mkdcdisc), FLYCAST (path to Flycast).
: "${DC_TOOLCHAINS:=/opt/toolchains/dc}"
: "${KOS_ENVIRON:=${DC_TOOLCHAINS}/kos/environ.sh}"
if [ ! -f "${KOS_ENVIRON}" ]; then
    echo "KallistiOS environ.sh not found at ${KOS_ENVIRON} - run scripts/setup_toolchain.sh first" >&2
    return 1 2>/dev/null || exit 1
fi
# shellcheck disable=SC1090
. "${KOS_ENVIRON}"
# Workaround (KOS master 2026-10, commit 5f834c1): environ_base.sh sources the
# toolchain script before the arch script that defines KOS_LD_SCRIPT, so the
# linker script (and with it _kos_startup.o) is missing from KOS_LDFLAGS.
case " ${KOS_LDFLAGS} " in
    *" ${KOS_LD_SCRIPT} "*) ;;
    *) export KOS_LDFLAGS="${KOS_LDFLAGS} ${KOS_LD_SCRIPT}" ;;
esac
export ONESHOT_ENV_OK=1
