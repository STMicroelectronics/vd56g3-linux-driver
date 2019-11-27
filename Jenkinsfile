#!groovy​

properties([parameters([string(name: 'DOCKER_ID', defaultValue: "docker-registry.st.com/img-sw/d410c-base:debian", description: 'docker image id build environment')]),
            buildDiscarder(logRotator(artifactDaysToKeepStr: '', artifactNumToKeepStr: '', daysToKeepStr: '60', numToKeepStr: '32'))])

node('ubuntu-build-node') {
        try {
            def branch = env.BRANCH_NAME

            stage('checkout source code') {
                    checkout scm
            }
            stage('build') {
                    sh "GIT_COMMIT=\$(git rev-parse --short=12 HEAD) SUDO=\" \" ./jenkins/runme.sh"
            }
            stage('deploy') {
                    /* archive locally artifacts */
                    archiveArtifacts artifacts: 'tmp/export/*', fingerprint: true, onlyIfSuccessful: true
                    /* deploy in debian repo server */
                    withCredentials([string(credentialsId: 'aptly-passphrase', variable: 'PASSPHRASE')]) {
                        sh "./jenkins/publish.sh tmp/export/*.deb $PASSPHRASE"
                    }
            }
        } catch (any) {
            throw any //rethrow exception to prevent the build from proceeding
        }
}