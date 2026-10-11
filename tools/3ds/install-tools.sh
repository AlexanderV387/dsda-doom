#!/bin/sh
# Downloads makerom and bannertool into ./tools-bin and adds them to the
# GitHub Actions PATH. Needs the gh CLI and GH_TOKEN (GitHub-hosted runner).
set -eu
mkdir -p tools-bin dl-makerom dl-bannertool

# Project_CTR publishes makerom and ctrtool separately: take the newest
# release whose tag starts with "makerom".
tag=$(gh release list -R 3DSGuy/Project_CTR --limit 50 --json tagName --jq '.[].tagName' | grep '^makerom' | head -1)
echo "makerom: $tag"
gh release download "$tag" -R 3DSGuy/Project_CTR --pattern '*ubuntu*' --dir dl-makerom \
  || gh release download "$tag" -R 3DSGuy/Project_CTR --pattern '*linux*' --dir dl-makerom
for z in dl-makerom/*.zip; do unzip -o "$z" -d dl-makerom; done
find dl-makerom -type f -name makerom -exec install -m755 {} tools-bin/ \;

# Steveice10/bannertool no longer has releases: use the diasurgical fork
# (DevilutionX), or build from source as a fallback.
if gh release download -R diasurgical/bannertool --pattern '*.zip' --dir dl-bannertool; then
  for z in dl-bannertool/*.zip; do unzip -o "$z" -d dl-bannertool; done
  find dl-bannertool -type f -name bannertool -path '*linux*' -path '*x86_64*' -exec install -m755 {} tools-bin/ \;
fi
if [ ! -x tools-bin/bannertool ]; then
  git clone --recursive --depth 1 https://github.com/Steveice10/bannertool src-bannertool
  make -C src-bannertool
  find src-bannertool/output -type f -name bannertool -exec install -m755 {} tools-bin/ \;
fi

ls -la tools-bin
[ -n "${GITHUB_PATH:-}" ] && echo "$PWD/tools-bin" >> "$GITHUB_PATH"
