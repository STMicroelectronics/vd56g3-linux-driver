pipeline {
	agent {
		label 'os-ubuntu2004'
	}
	environment {
		http_proxy = credentials('proxy')
		https_proxy = credentials('proxy')
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
				sh 'make KVERSION=$(ls /lib/modules/) -C src'
			}
		}
		stage('Test') {
			steps {
				echo 'No tests yet'
			}
		}
		stage('Package') {
			when { anyOf {
				environment name: 'GIT_BRANCH', value: 'master';
				environment name: 'GIT_BRANCH', value: 'debian'
			}}
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
			when { anyOf {
				environment name: 'GIT_BRANCH', value: 'master';
				environment name: 'GIT_BRANCH', value: 'debian'
			}}
			steps {
				script {
					if (env.GIT_BRANCH == 'debian') {
						rtUpload (
							serverId: 'artifactory-azure',
							spec: '''{ "files": [ {
									"pattern": "../st-vd56g3*.deb",
									"target": "imgswlinux-debian-local/pool/st-vd56g3-dkms/stable/",
									"props": "deb.distribution=stable;deb.component=main;deb.architecture=armhf;deb.architecture=arm64"
								} ] }'''
						)
					} else {
						rtUpload (
							serverId: 'artifactory-azure',
							spec: '''{ "files": [ {
									"pattern": "../st-vd56g3*.deb",
									"target": "imgswlinux-debian-local/pool/st-vd56g3-dkms/unstable/",
									"props": "deb.distribution=unstable;deb.component=main;deb.architecture=armhf;deb.architecture=arm64"
								} ] }'''
						)
					}
				}
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

