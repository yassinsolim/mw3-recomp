#!/usr/bin/env bash
# perf-setup: turns the relay's performance reports on or off on the host that runs mw2-relay.service
# (docs/perf-reports.md is the runbook; src/perf.rs is what answers). Run as root on a Debian or Ubuntu host.
#
#   perf-setup.sh enable    opens TCP 80 and 443 in iptables, tells the relay's unit where to listen and keep the
#                           reports (a drop-in: the unit file itself is not touched), and puts Caddy in front of it
#                           with a certificate for the host name. Safe to run again: it only does what is missing.
#                           Run it before the relay is updated to a build that takes reports (it prepares, and says
#                           so) and once more after.
#   perf-setup.sh check     what is there and whether it answers; changes nothing.
#   perf-setup.sh disable   the way back: Caddy stopped and removed, the ports closed, the drop-in gone. The reports
#                           stay in /var/lib/mw3-relay unless --delete-reports is given.
#
# PERF_HOST is the name players' apps ask (mw3.yassin.app). Nothing here costs anything: Caddy runs on the VM that
# is already there, and its certificate is Let's Encrypt's.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C.UTF-8 LANG=C.UTF-8

HOST=${PERF_HOST:-mw3.yassin.app}
PORT=3076
BIN=/usr/local/bin/mw2-relay
UNIT=mw2-relay.service
DROPIN=/etc/systemd/system/mw2-relay.service.d/perf.conf
STATE=/var/lib/mw3-relay
CADDYFILE=/etc/caddy/Caddyfile
MARK='# Written by perf-setup.sh'
IPTABLES_RULES=/etc/iptables/rules.v4
FIREWALL_RULE=(-p tcp -m multiport --dports 80,443 -m comment --comment mw3-perf -j ACCEPT)
LOCAL=http://127.0.0.1:$PORT/perf/health

die() { echo "perf-setup: $*" >&2; exit 1; }
step() { echo "== $*"; }

# As mw3-relay.sh's rule for UDP 3074: live, just before Oracle's final REJECT, and in the file for the next boot.
# The file is never reloaded: iptables-restore would drop Docker's chains and the Minecraft server's ports with them.
open_firewall() {
    local line pos
    if ! command -v iptables >/dev/null || [ ! -f "$IPTABLES_RULES" ]; then
        echo "no $IPTABLES_RULES: allow TCP 80 and 443 in this host's firewall by hand"
        return 0
    fi
    if iptables -w -C INPUT "${FIREWALL_RULE[@]}" 2>/dev/null; then
        echo "iptables: TCP 80 and 443 already allowed"
    else
        pos=$(iptables -w -S INPUT | grep -n -- '-j REJECT' | head -n 1 | cut -d: -f1 || true)
        if [ -n "$pos" ]; then
            iptables -w -I INPUT "$((pos - 1))" "${FIREWALL_RULE[@]}"
        else
            iptables -w -A INPUT "${FIREWALL_RULE[@]}"
        fi
        echo "iptables: TCP 80 and 443 allowed"
    fi
    line="-A INPUT ${FIREWALL_RULE[*]}"
    if grep -qxF -- "$line" "$IPTABLES_RULES"; then
        echo "$IPTABLES_RULES: already has the rule"
        return 0
    fi
    grep -q -- '^-A INPUT -j REJECT' "$IPTABLES_RULES" ||
        die "$IPTABLES_RULES has no final INPUT REJECT to put the rule before: add it by hand"
    awk -v rule="$line" '!done && /^-A INPUT -j REJECT/ { print rule; done = 1 } { print }' \
        "$IPTABLES_RULES" > "$IPTABLES_RULES.new"
    chmod --reference="$IPTABLES_RULES" "$IPTABLES_RULES.new"
    iptables-restore --test < "$IPTABLES_RULES.new" || { rm -f "$IPTABLES_RULES.new"; die "the new $IPTABLES_RULES does not parse"; }
    cp -a "$IPTABLES_RULES" "$IPTABLES_RULES.bak-$(date +%Y%m%d-%H%M%S)"
    mv -f "$IPTABLES_RULES.new" "$IPTABLES_RULES"
    echo "$IPTABLES_RULES: rule added before the final REJECT (backup next to it)"
}

close_firewall() {
    local line="-A INPUT ${FIREWALL_RULE[*]}"
    if command -v iptables >/dev/null; then
        while iptables -w -C INPUT "${FIREWALL_RULE[@]}" 2>/dev/null; do iptables -w -D INPUT "${FIREWALL_RULE[@]}"; done
        echo "iptables: TCP 80 and 443 closed"
    fi
    if [ -f "$IPTABLES_RULES" ] && grep -qxF -- "$line" "$IPTABLES_RULES"; then
        cp -a "$IPTABLES_RULES" "$IPTABLES_RULES.bak-$(date +%Y%m%d-%H%M%S)"
        grep -vxF -- "$line" "$IPTABLES_RULES" > "$IPTABLES_RULES.new"
        chmod --reference="$IPTABLES_RULES" "$IPTABLES_RULES.new"
        mv -f "$IPTABLES_RULES.new" "$IPTABLES_RULES"
        echo "$IPTABLES_RULES: rule removed (backup next to it)"
    fi
}

# Only what Caddy holds may be on 80 and 443: anything else there is somebody's, and is left alone.
ports_free() {
    local held
    held=$(ss -H -ltnp '( sport = :80 or sport = :443 )' 2>/dev/null | grep -v '"caddy"' || true)
    [ -z "$held" ] || die "TCP 80 or 443 is already in use on this host, by something that is not Caddy:
$held"
}

# The relay reads where to listen and where to keep the reports from its environment, so the unit file that
# Update-Mw3Relay.ps1 installs with every build stays as it is, and a build without reports ignores both.
write_dropin() {
    install -d -m 0755 "$(dirname "$DROPIN")"
    cat > "$DROPIN.new" <<EOF
$MARK (docs/perf-reports.md): performance reports, on 127.0.0.1 only.
[Service]
Environment=MW2_RELAY_PERF_PORT=$PORT
Environment=MW2_RELAY_PERF_DB=$STATE/perf.sqlite
StateDirectory=$(basename "$STATE")
StateDirectoryMode=0750
EOF
    if [ -f "$DROPIN" ] && cmp -s "$DROPIN.new" "$DROPIN"; then
        rm -f "$DROPIN.new"
        echo "$DROPIN: already there"
        return 0
    fi
    mv -f "$DROPIN.new" "$DROPIN"
    systemctl daemon-reload
    echo "$DROPIN: written (the relay reads it at its next start)"
}

relay_takes_reports() { grep -qa -- '--perf-port' "$BIN" 2>/dev/null; }
answers_locally() { curl -fsS --max-time 3 "$LOCAL" 2>/dev/null | grep -q '"ok":true'; }

copies() { curl -fsS --max-time 2 http://127.0.0.1:3075/ 2>/dev/null | jq -r '.copies | length' 2>/dev/null || echo '?'; }

write_caddyfile() {
    if [ -f "$CADDYFILE" ] && ! grep -qF -- "$MARK" "$CADDYFILE"; then
        cp -a "$CADDYFILE" "$CADDYFILE.bak-$(date +%Y%m%d-%H%M%S)"
        echo "$CADDYFILE: was not this script's; a backup is next to it"
    fi
    install -d -m 0755 "$(dirname "$CADDYFILE")"
    # No "log" line for the site: Caddy writes an access log only where one is asked for. Its own log leaves out
    # the two loggers that would name a client's address when a request fails (http.log.error, http.stdlib).
    cat > "$CADDYFILE.new" <<EOF
$MARK (docs/perf-reports.md). Only /perf is passed on, to the relay on this host;
# everything else is 404. No access log, and no log line that names a client's address.
{
	log {
		exclude http.log http.stdlib
	}
}

$HOST {
	@perf path /perf /perf/*
	handle @perf {
		request_body {
			max_size 4KB
		}
		reverse_proxy 127.0.0.1:$PORT {
			header_up X-Forwarded-For {remote_host}
		}
	}
	handle {
		respond 404
	}
}
EOF
    mv -f "$CADDYFILE.new" "$CADDYFILE"
}

install_caddy() {
    if ! command -v caddy >/dev/null; then
        # The one package and what it needs: nothing else on the host is upgraded (that would restart Docker and
        # the Minecraft server). The Caddyfile is there first, so Caddy never serves its welcome page.
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -q
        apt-get install -y -q --no-install-recommends -o Dpkg::Options::=--force-confold caddy
    fi
    caddy version
    caddy validate --config "$CADDYFILE" --adapter caddyfile >/dev/null 2>&1 ||
        { caddy validate --config "$CADDYFILE" --adapter caddyfile || true; die "Caddy rejects $CADDYFILE"; }
    systemctl enable --quiet caddy
    systemctl restart caddy
}

# Through Caddy as a player's app comes, but for the network between: the name, the certificate, the path.
answers_in_front() { curl -fsS --max-time 5 --resolve "$HOST:443:127.0.0.1" "https://$HOST/perf/health" 2>/dev/null | grep -q '"ok":true'; }

enable() {
    command -v curl >/dev/null && command -v jq >/dev/null || die "curl and jq are not installed (mw3-relay setup installs them)"
    [ -x "$BIN" ] || die "no $BIN: this host does not run the relay"
    step "ports"
    ports_free
    step "firewall"
    open_firewall
    step "the relay's unit"
    write_dropin
    if ! relay_takes_reports; then
        echo
        echo "PREPARED. The relay installed here is a build without performance reports."
        echo "Next: update it (Update-Mw3Relay.ps1 -Commit <the commit docs/perf-reports.md names>), then run"
        echo "'perf-setup.sh enable' again."
        return 0
    fi
    step "the relay"
    if answers_locally; then
        echo "takes reports on 127.0.0.1:$PORT"
    else
        echo "restarting $UNIT so it reads the drop-in ($(copies) game copies connected: they reconnect)"
        systemctl restart "$UNIT"
        for _ in 1 2 3 4 5 6 7 8 9 10; do sleep 1; if answers_locally; then break; fi; done
        answers_locally || die "the relay does not answer on $LOCAL: journalctl -u $UNIT -n 30 (a line 'performance reports: not taken' says why)"
        echo "takes reports on 127.0.0.1:$PORT"
    fi
    step "Caddy"
    write_caddyfile
    install_caddy
    step "certificate for $HOST"
    for _ in $(seq 1 30); do
        if answers_in_front; then break; fi
        sleep 4
    done
    if ! answers_in_front; then
        journalctl -u caddy -n 15 --no-pager 2>/dev/null | cut -c1-300 || true
        echo
        echo "NOT DONE: Caddy has no certificate for $HOST after two minutes."
        echo "The usual reason: Oracle's security list does not let TCP 80 and 443 in from 0.0.0.0/0. That is step 1"
        echo "of docs/perf-reports.md, done in the Oracle console; nothing on this host can open it. The other:"
        echo "$HOST does not resolve to this host's public address ($(getent hosts "$HOST" | cut -d' ' -f1 || true))."
        echo "Fix it and run 'perf-setup.sh enable' again; Caddy keeps trying meanwhile."
        exit 1
    fi
    check
    echo
    echo "DONE. From another machine: curl -sS https://$HOST/perf/health   ->   {\"ok\":true}"
}

check() {
    step "the relay's unit"
    if [ -f "$DROPIN" ]; then cat "$DROPIN"; else echo "no $DROPIN"; fi
    if relay_takes_reports; then echo "the installed relay takes reports"; else echo "the installed relay is a build without reports"; fi
    step "the relay on 127.0.0.1:$PORT"
    curl -sS --max-time 3 "$LOCAL" 2>&1 || true
    echo
    ss -H -ltn "sport = :$PORT" || true
    step "reports kept"
    ls -l "$STATE" 2>/dev/null || echo "no $STATE"
    step "firewall"
    iptables -w -S INPUT 2>/dev/null | grep -F -- 'mw3-perf' || echo "no iptables rule for TCP 80 and 443"
    step "Caddy"
    systemctl --no-pager --lines=0 status caddy 2>&1 | head -n 5 || true
    step "through Caddy, on this host"
    for path in /perf/health /perf/summary /perf /; do
        printf '%-14s %s\n' "$path" "$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 --resolve "$HOST:443:127.0.0.1" "https://$HOST$path" 2>/dev/null || true)"
    done
    echo "(expected: 200 200 200 404)"
    step "addresses in the logs"
    if journalctl -u caddy -u "$UNIT" --since -1h --no-pager 2>/dev/null | grep -c 'performance reports\|remote_ip\|X-Forwarded-For' ; then
        echo "(the count above is of lines about reports or a client's address in the last hour; the relay's one line at"
        echo " its start, 'performance reports: taken ...', is the only one expected)"
    fi
}

disable() {
    step "Caddy"
    if systemctl list-unit-files caddy.service >/dev/null 2>&1 && systemctl cat caddy >/dev/null 2>&1; then
        systemctl disable --now caddy || true
    fi
    if command -v caddy >/dev/null; then
        DEBIAN_FRONTEND=noninteractive apt-get remove -y -q caddy
    fi
    if [ -f "$CADDYFILE" ] && grep -qF -- "$MARK" "$CADDYFILE"; then rm -f "$CADDYFILE"; fi
    echo "Caddy is stopped and removed (its certificate stays in /var/lib/caddy; 'apt-get purge caddy' removes that too)"
    step "firewall"
    close_firewall
    step "the relay's unit"
    rm -f "$DROPIN"
    rmdir "$(dirname "$DROPIN")" 2>/dev/null || true
    systemctl daemon-reload
    if answers_locally; then
        if [ "$(copies)" = 0 ]; then
            systemctl restart "$UNIT"
            echo "drop-in removed, relay restarted: it takes no reports"
        else
            echo "drop-in removed. $(copies) game copies are connected, so the relay was not restarted: it stops taking"
            echo "reports at its next start (systemctl restart $UNIT). Nothing reaches it meanwhile: the ports are closed."
        fi
    else
        echo "drop-in removed"
    fi
    step "reports"
    if [ "${1:-}" = --delete-reports ]; then
        rm -rf "$STATE"
        echo "$STATE deleted"
    else
        echo "kept in $STATE (perf-setup.sh disable --delete-reports removes them)"
    fi
}

[ "$(id -u)" -eq 0 ] || die "run it as root"
case "${1:-}" in
    enable) enable ;;
    check) check ;;
    disable) shift; disable "$@" ;;
    *) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
