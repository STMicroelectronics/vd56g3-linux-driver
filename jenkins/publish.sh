#!/bin/bash -ex

function get_repo_from_deb_package {
    local tmpdir=$(mktemp -d)
    local changelog
    local release

    dpkg-deb -x $1 $tmpdir
    changelog=$(find $tmpdir -name changelog.gz)
    release=$(dpkg-parsechangelog -l $changelog --show-field Distribution)
    rm -Rf $tmpdir

    echo $release
}

#get script location
SCRIPTDIR=`dirname $0`
SCRIPTDIR=`(cd $SCRIPTDIR ; pwd)`
# build location
TOP=`pwd`

DEB=$1
PASSPHRASE="$2"
SERVER=citools.st.com/projects/gpz/aptly

REPO=$(get_repo_from_deb_package $DEB)
if [ $REPO == "UNRELEASED" ] ; then
    REPO="unstable"
fi

#upload debian package in incoming directory
curl -X POST -F file=@$DEB https://$SERVER/api/files/incoming
#FIXME : add file instead of directory
curl -X POST https://$SERVER/api/repos/st-imaging-${REPO}/file/incoming
#publish
curl -X PUT -H 'Content-Type: application/json' --data '{"Signing": {"Batch": true, "Passphrase": "'"$PASSPHRASE"'"}}' https://$SERVER/api/publish/:./${REPO}
