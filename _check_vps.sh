#!/bin/bash
# Diagnostic helper for stage-2 fetch issues.
# Upload this script, run on VPS as root:
#   scp _check_vps.sh root@<vps>:/tmp/
#   ssh root@<vps> 'bash /tmp/_check_vps.sh'

echo "===[1] server.py lines about stage2 ==="
journalctl -u rdp-relay --since "10 minutes ago" --no-pager \
    | grep -E -i 'stage2|filemgr|procmgr|defender|sysinfo|encrypt' \
    | tail -50

echo ""
echo "===[2] server.py version markers ==="
grep -n -E 'STAGE2_KNOWN_MODULES|STAGE2_MAX_BLOB' /opt/remotedesk/server.py \
    | head -5

echo ""
echo "===[3] DLL sizes/mtimes ==="
ls -la /opt/remotedesk/stage2/*.dll 2>/dev/null

echo ""
echo "===[4] Cache for my-room-token-123 ==="
ls -la /opt/remotedesk/stage2/cache/my-room-token-123/ 2>/dev/null \
    || echo "  (cache dir missing)"

echo ""
echo "===[5] Disk space ==="
df -h /opt/remotedesk/stage2

echo ""
echo "===[6] Last 100 lines of rdp-relay log ==="
journalctl -u rdp-relay -n 100 --no-pager

echo ""
echo "===[7] Service status ==="
systemctl status rdp-relay --no-pager -n 5
