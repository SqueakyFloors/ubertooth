#!/bin/bash
# Publish a nightly build to the upstream nightly-builds repository. Written
# for Travis CI, which this project no longer uses, and kept only because it
# records how the nightly page was produced; it expects TRAVIS_* and
# ARTEFACT_BASE in the environment and will not run without them.

REPO=greatscottgadgets/ubertooth-nightly
# The branch GitHub Pages serves in that repository.
PUBLICATION_BRANCH=master
# set -x
cd "$HOME" || exit 1
# Checkout the branch
git clone --branch="$PUBLICATION_BRANCH" "https://${GITHUB_TOKEN}@github.com/$REPO.git" publish
cd publish || exit 1
# Update pages
cp "$ARTEFACT_BASE/$BUILD_NAME.tar.xz" .
# Write index page
cd "$TRAVIS_BUILD_DIR" || exit 1
COMMITS=$(git log --oneline | awk '{print $1}')
cd "$HOME/publish" || exit 1
echo "
<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.0 Transitional//EN\">
<html><head>
	<meta http-equiv=\"content-type\" content=\"text/html; charset=UTF-8\">
	<title>Ubertooth Nightly Builds</title>
</head>
<body>
<h2>Ubertooth Nightly Builds</h2>
" > index.html

URL=https://greatscottgadgets.github.io/ubertooth-nightly

for commit in $COMMITS; do
    FILENAME=$(find . -maxdepth 1 -name "*-$commit.tar.xz")
    if [ "$FILENAME" != "" ]; then
        FN=${FILENAME:2}
        echo "<a href=\"$URL/$FN\">$FN</a><br />" >> index.html
    fi

done

echo "
</body></html>
" >> index.html

# Commit and push latest version
git add "$BUILD_NAME.tar.xz" index.html
git config user.name  "Travis"
git config user.email "travis@travis-ci.org"
if ! git commit -m "Build products for $SHORT_COMMIT_HASH, built on $TRAVIS_OS_NAME, log: $TRAVIS_BUILD_WEB_URL"; then
    echo "Looks like the commit failed"
fi
git push -fq origin "$PUBLICATION_BRANCH"
