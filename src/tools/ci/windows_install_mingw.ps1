$ErrorActionPreference = "Stop"

$target = $args[0];

echo "downloading msys2"
curl.exe -fsSL "https://github.com/msys2/msys2-installer/releases/download/nightly-x86_64/msys2-base-x86_64-latest.sfx.exe" -o msys2.exe
if (!$?) { throw 'download failed' };

echo "::group::msys2_installation"
# installs to the "target dir"/msys64
.\msys2.exe -y -o"$target";
if (!$?) { throw 'install failed'; };
echo "::endgroup::"

Remove-Item msys2.exe ;


echo "::group::msys2_setup"

# When msys is used for the first time, it performs setup
# tasks. Doing other work in the same invocation does not reliably
# work.
function msys() { &"${target}\msys64\usr\bin\bash.exe" @('-elc') + @Args; if (!$?) { throw 'cmdfail' };} ;
msys ' ' ;

msys 'pacman --noconfirm -Syuu' ;
msys 'pacman --noconfirm -Scc' ;

echo "::endgroup::"
