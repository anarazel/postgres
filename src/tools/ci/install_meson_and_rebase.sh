#! /bin/sh

set -e
set -x

case $CIRRUS_OS in
    freebsd | linux | darwin | windows | mingw)
    ;;
    *)
        echo "unsupported operating system ${CIRRUS_OS}"
        exit 1
    ;;
esac

# install meson by using pip
install_meson () {
    python3 -m pip install git+${MESON_REPO}@${MESON_BRANCH}
}

run_rebase_commands() {
    git config user.email 'postgres-ci@example.com'
    git config user.name 'Postgres CI'
    # windows loses the executable bit, causing an unnecessary diff
    git config core.filemode false
    # windows changes file endings to crlf, causing test failures
    git config core.autocrlf false
    git remote add default-postgres https://github.com/postgres/postgres.git
    git fetch default-postgres master
    echo "Rebasing onto: $(git show --no-patch --abbrev-commit --pretty=oneline default-postgres/master)"
    git rebase --autostash --no-verify default-postgres/master
}

# Rebase current branch onto Postgres HEAD
rebase_onto_postgres () {
    # safe directory need to be set because of dubious ownership error
    # debian complains $HOME not set when --global is used
    # macOS complains about permissions when --system is used
    # so, use --global for macOS and --system for others
    case $CIRRUS_OS in
        darwin)
            git config --global --add safe.directory /tmp/cirrus-ci-build
        ;;
        *)
            git config --system --add safe.directory /tmp/cirrus-ci-build
        ;;
    esac
    run_rebase_commands
}

install_meson
rebase_onto_postgres
