#!/bin/sh
#
# Install dependencies required to build and test Git on Linux and macOS
#

set -ex

UBUNTU_COMMON_PKGS="make libssl-dev libcurl4-openssl-dev libexpat-dev
 tcl tk gettext zlib1g-dev perl-modules liberror-perl libauthen-sasl-perl
 libemail-valid-perl libio-socket-ssl-perl libnet-smtp-ssl-perl"

CC_PACKAGE=
BREW_CC_PACKAGE=
case "$jobname" in
linux-gcc | linux-TEST-vars)
	CC_PACKAGE=gcc-8
	;;
osx-gcc)
	BREW_CC_PACKAGE=gcc@9
	;;
esac

case "$runs_on_pool" in
ubuntu-latest)
	# The Linux build installs the defined dependency versions below.
	# The OS X build installs much more recent versions, whichever
	# were recorded in the Homebrew database upon creating the OS X
	# image.
	# Keep that in mind when you encounter a broken OS X build!
	LINUX_P4_VERSION="16.2"
	LINUX_GIT_LFS_VERSION="1.5.2"

	P4_PATH="$HOME/custom/p4"
	GIT_LFS_PATH="$HOME/custom/git-lfs"
	PATH="$GIT_LFS_PATH:$P4_PATH:$PATH"
	export PATH
	if test -n "$GITHUB_PATH"
	then
		echo "$PATH" >>"$GITHUB_PATH"
	fi

	P4WHENCE=https://cdist2.perforce.com/perforce/r$LINUX_P4_VERSION
	LFSWHENCE=https://github.com/github/git-lfs/releases/download/v$LINUX_GIT_LFS_VERSION

	sudo apt-get -q update
	sudo apt-get -q -y install language-pack-is libsvn-perl apache2 \
		$UBUNTU_COMMON_PKGS $CC_PACKAGE
	mkdir -p "$P4_PATH"
	(
		cd "$P4_PATH"
		wget --quiet "$P4WHENCE/bin.linux26x86_64/p4d"
		wget --quiet "$P4WHENCE/bin.linux26x86_64/p4"
		chmod u+x p4d
		chmod u+x p4
	)
	mkdir -p "$GIT_LFS_PATH"
	(
		cd "$GIT_LFS_PATH"
		wget --quiet "$LFSWHENCE/git-lfs-linux-amd64-$LINUX_GIT_LFS_VERSION.tar.gz"
		tar --extract --gunzip --file "git-lfs-linux-amd64-$LINUX_GIT_LFS_VERSION.tar.gz"
		cp git-lfs-$LINUX_GIT_LFS_VERSION/git-lfs .
	)
	;;
macos-latest)
	HOMEBREW_NO_AUTO_UPDATE=1
	export HOMEBREW_NO_AUTO_UPDATE
	HOMEBREW_NO_INSTALL_CLEANUP=1
	export HOMEBREW_NO_INSTALL_CLEANUP

	# Uncomment this if you want to run perf tests:
	# brew install gnu-time
	brew link --force gettext
	mkdir -p $HOME/bin
	(
		cd $HOME/bin
		wget -q "https://cdist2.perforce.com/perforce/r21.2/bin.macosx1015x86_64/helix-core-server.tgz" &&
		tar -xf helix-core-server.tgz &&
		sudo xattr -d com.apple.quarantine p4 p4d 2>/dev/null || true
	)
	PATH="$PATH:${HOME}/bin"
	export PATH

	if test -n "$BREW_CC_PACKAGE"
	then
		brew install "$BREW_CC_PACKAGE"
		brew link "$BREW_CC_PACKAGE"
	fi
	;;
esac

case "$jobname" in
StaticAnalysis)
	sudo apt-get -q update
	sudo apt-get -q -y install coccinelle libcurl4-openssl-dev libssl-dev \
		libexpat-dev gettext make
	;;
sparse)
	sudo apt-get -q update -q
	sudo apt-get -q -y install libssl-dev libcurl4-openssl-dev \
		libexpat-dev gettext zlib1g-dev
	;;
Documentation)
	sudo apt-get -q update
	sudo apt-get -q -y install asciidoc xmlto docbook-xsl-ns make

	sudo gem install --version 1.5.8 asciidoctor
	;;
linux-gcc-default)
	sudo apt-get -q update
	sudo apt-get -q -y install $UBUNTU_COMMON_PKGS
	;;
linux32)
	linux32 --32bit i386 sh -c '
		apt update >/dev/null &&
		apt install -y build-essential libcurl4-openssl-dev \
			libssl-dev libexpat-dev gettext python >/dev/null
	'
	;;
linux-musl)
	apk add --update build-base curl-dev openssl-dev expat-dev gettext \
		pcre2-dev python3 musl-libintl perl-utils ncurses >/dev/null
	;;
pedantic)
	dnf -yq update >/dev/null &&
	dnf -yq install make gcc findutils diffutils perl python3 gettext zlib-devel expat-devel openssl-devel curl-devel pcre2-devel >/dev/null
	;;
esac

if type p4d >/dev/null 2>&1 && type p4 >/dev/null 2>&1
then
	echo "$(tput setaf 6)Perforce Server Version$(tput sgr0)"
	p4d -V | grep Rev.
	echo "$(tput setaf 6)Perforce Client Version$(tput sgr0)"
	p4 -V | grep Rev.
else
	echo >&2 "WARNING: perforce wasn't installed, see above for clues why"
fi
if type git-lfs >/dev/null 2>&1
then
	echo "$(tput setaf 6)Git-LFS Version$(tput sgr0)"
	git-lfs version
else
	echo >&2 "WARNING: git-lfs wasn't installed, see above for clues why"
fi
