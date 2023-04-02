#! /bin/sh
set -e

MESON_REPO=$1
MESON_BRANCH=$2
CURRENT_OS=$3

if [ $# -ne 3 ]; then
    echo "install_meson_and_rebase.sh <MESON_REPO> <MESON_BRANCH> <OS>"
    exit 1
fi

case $CURRENT_OS in
    freebsd|linux|macos|windows|mingw)
    ;;
    *)
        echo "unsupported operating system ${CURRENT_OS}"
        exit 1
    ;;
esac

# After repartition, hidden files are not copied. Copy them to
# working dir
if [ "$CURRENT_OS" = 'freebsd' ]; then
    cp -r $CIRRUS_WORKING_DIR.orig/.[^.]* $CIRRUS_WORKING_DIR/
    chown -R postgres:postgres .[^.]*
fi

install_meson () {

    case $CURRENT_OS in

        freebsd|linux|macos|mingw)
            git clone ${MESON_REPO} -b ${MESON_BRANCH}
            echo "MESON=python3 meson/meson.py" | tee -a ${CIRRUS_ENV}
            ;;

        windows)
            pip install git+${MESON_REPO}@${MESON_BRANCH}
            ;;
    esac
}

rebase_onto_postgres () {

    case $CURRENT_OS in

        freebsd|linux)
su postgres <<-EOF
    git config user.email 'postgres-ci@example.com'
    git config user.name 'Postgres CI'
    git remote add default-postgres https://github.com/postgres/postgres.git
    git fetch default-postgres master
    git rebase --no-verify default-postgres/master
EOF
        ;;

        windows|mingw|macos)
            git config user.email 'postgres-ci@example.com'
            git config user.name 'Postgres CI'
            # There are file permission on Windows. Set filemode to false for windows,
            # then reset to HEAD
            git config core.filemode false
            git reset --hard
            git remote add default-postgres https://github.com/postgres/postgres.git
            git fetch default-postgres master
            git rebase --no-verify default-postgres/master
        ;;
    esac
}

install_meson
rebase_onto_postgres
