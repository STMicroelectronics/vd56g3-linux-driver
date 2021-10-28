pipeline {
	agent {
		label 'os-ubuntu2004'
	}

	stages {
		stage('Prepare') {
			steps {
				// Checkout SCM is done by Jenkins himself
				sh 'sudo -E apt-get update'
				sh 'sudo -E apt-get install linux-headers-generic dkms equivs devscripts -y'
				//Remove old kernels
				sh 'sudo -E apt-get autoremove -y'
			}
		}
		stage('Build') {
			steps {
				sh 'make KVERSION=$(ls /lib/modules/)'
			}
		}
		stage('Test') {
			steps {
				echo 'No tests yet'
			}
		}
		stage('Package') {
			steps {
				dir('debianizer') {
					git credentialsId: 'af76724f-9593-4f6c-a5cc-1548eb8b0e14',
						url: 'ssh://gitolite@codex.cro.st.com/img-application-sw-linux/debianizer.git'
				}
				script {
				if (env.GIT_BRANCH == 'debian') {
						sh 'debianizer/debianizer.sh'
					} else {
						sh 'debianizer/debianizer.sh --use-origin --snapshot'
					}
				}
			}
		}
		stage('Upload') {
			steps {
				script {
					if (env.GIT_BRANCH == 'debian') {
						rtUpload (
							serverId: 'releases-artifactory-citools-st-com',
							spec: '''{ "files": [ {
									"pattern": "../st-vd56g3*.deb",
									"target":
									"imgappswlinux-codex-st-com/drivers/st-vd56g3/debian/"
								} ] }'''
						)
					} else {
						rtUpload (
							serverId: 'snapshots-artifactory-citools-st-com',
							spec: '''{ "files": [ {
									"pattern": "../st-vd56g3*.deb",
									"target":
									"snapshots-imgappswlinux-codex-st-com/drivers/st-vd56g3/debian/"
								} ] }'''
						)
					}
				}
			}
		}
		stage('Publish') {
			when { environment name: 'GIT_BRANCH', value: 'debian' }
			steps {

				// Query our aptly repository to update our package
				sh "curl -X POST -F file=@\$(realpath ../st-vd56g3*.deb) http://10.129.167.70:8081/api/files/tmp"
				sh "curl -X POST http://10.129.167.70:8081/api/repos/stimglinux-release/file/tmp/\$(basename ../st-vd56g3*.deb)"
				sh "curl -X PUT http://10.129.167.70:8081/api/publish/:./buster"
			}
		}
	}

	post {
		always {
			// Remove out of workspace debian packaging stuff
			sh 'find .. -maxdepth 1 -type f -name "st-vd56g3*" -exec rm -v "{}" \\;'
		}
	}
}

