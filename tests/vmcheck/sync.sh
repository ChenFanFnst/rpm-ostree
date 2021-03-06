#!/bin/bash
set -euo pipefail

if test -z "${INSIDE_VM:-}"; then

    # do this in the host
    . ${commondir}/libvm.sh
    vm_setup

    if ! vm_ssh_wait 30; then
      echo "ERROR: A running VM is required for 'make vmcheck'."
      exit 1
    fi

    set -x

    cd ${topsrcdir}
    rm insttree -rf
    make install DESTDIR=$(pwd)/insttree
    vm_rsync

    $SSH "env INSIDE_VM=1 /var/roothome/sync/tests/vmcheck/sync.sh"
    exit 0
else

    # then do this in the VM
    set -x
    ostree admin unlock || :
    rsync -rlv /var/roothome/sync/insttree/usr/ /usr/
    restorecon -v /usr/bin/rpm-ostree
    restorecon -v /usr/libexec/rpm-ostreed
    systemctl restart rpm-ostreed
fi
