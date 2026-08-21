#!/usr/bin/env bash
# Installs the PAM service file for the Realmheart Broken Seal lockscreen.
set -euo pipefail

if [ -e /etc/pam.d/realmheart-lockscreen ]; then
    echo "already installed"
    exit 0
fi

sudo tee /etc/pam.d/realmheart-lockscreen > /dev/null <<'EOF'
#%PAM-1.0
auth include login
account include login
password include login
session include login
EOF

echo "installed /etc/pam.d/realmheart-lockscreen"
