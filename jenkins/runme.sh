#!/bin/bash -ex

#get script location
SCRIPTDIR=`dirname $0`
SCRIPTDIR=`(cd $SCRIPTDIR ; pwd)`
# build location
TOP=`pwd`
WHO=`whoami`
GID=`id -g ${WHO}`
SUDO=${SUDO:-sudo}
DOCKER_ID=${DOCKER_ID:-"docker-registry.st.com/guenem/d410c-base:debian"}

PACKAGE=$(dpkg-parsechangelog --show-field Source)
RELEASE=$(dpkg-parsechangelog --show-field Distribution)
VERSION=$(dpkg-parsechangelog --show-field Version)
GIT_COMMIT=${GIT_COMMIT:-local}
VERSION=${VERSION}~$(date -u +%Y%m%d.%H%M%S).g${GIT_COMMIT}

DEBEMAIL=${DEBEMAIL:-Mickael Guene <mickael.guene@st.com>}

# create tmp dir
rm -Rf tmp
mkdir -p tmp/export

# be sure to have latest docker build image
${SUDO} docker pull ${DOCKER_ID}

# script run inside build container
cat << EOF >> tmp/runme.sh
#!/bin/bash -ex

export DEBEMAIL="${DEBEMAIL}"

cp -Rf /root/source_ori /root/source
cd /root/source
if [ $RELEASE == "UNRELEASED" ] ; then
    dch -b --newversion ${VERSION} ${VERSION}
fi
# install build deps
apt-get update
apt-get install equivs
mk-build-deps --install --tool='apt-get -o Debug::pkgProblemResolver=yes --no-install-recommends --yes' debian/control
# build
dpkg-buildpackage -b -us -uc

cp /root/${PACKAGE}_*.deb /root/export/.
chown  ${UID}:${GID} /root/export/${PACKAGE}_*.deb

EOF
chmod +x tmp/runme.sh

# ok let's build it now
${SUDO} docker run -i --rm -v ${TOP}/tmp/export:/root/export \
                           -v ${TOP}/tmp/runme.sh:/root/runme.sh \
                           -v ${TOP}:/root/source_ori \
                           -e http_proxy="http://docker-squid.st.com:3128" \
                           -e https_proxy="http://docker-squid.st.com:3128" \
                           -w /root ${DOCKER_ID} ./runme.sh