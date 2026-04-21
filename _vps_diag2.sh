#!/bin/bash
echo "===[A] DLL on VPS for host_update ==="
ls -la /srv/www/files/pnpext.dll 2>/dev/null
echo ""
echo "===[B] Version strings in that DLL ==="
strings /srv/www/files/pnpext.dll 2>/dev/null | grep -E '^1\.0\.[0-9]+' | head -5
echo ""
echo "===[C] server.py version markers ==="
grep -n -E 'v1\.0\.[0-9]+|Server v' /opt/remotedesk/server.py | head -10
echo ""
echo "===[D] STAGE2_KNOWN_MODULES ==="
grep -n -A6 "STAGE2_KNOWN_MODULES = frozenset" /opt/remotedesk/server.py | head -10
echo ""
echo "===[E] server.py mtime ==="
ls -la /opt/remotedesk/server.py
echo ""
echo "===[F] Full rdp-relay log for stage2 (last 200 lines with filter) ==="
journalctl -u rdp-relay --no-pager -n 500 | grep -i stage2 | tail -30
echo ""
echo "===[G] All stage2_fetch requests since service start ==="
journalctl -u rdp-relay --no-pager | grep -i "stage2:" | awk 'NR>1' | tail -50
