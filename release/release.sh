#!/bin/bash

if [[ $# -ne 1 ]]; then
	echo "Usage: `basename $0` git-TAG"
	echo "with 'git-TAG' one of the following tags: "
	git tag
	exit 0
fi

tag=$1
git show $tag --quiet
if [[ $? -ne 0 ]]; then
	echo "Tag $tag not found in current git repository"
	exit 1
fi

# Jump to base directory
top_dir=`git rev-parse --show-toplevel`
project_name=`basename $top_dir`
cd $top_dir

# Check the existence of a documentation (sphinx conf.py in 'doc' folder)
git show $tag:doc/conf.py > /dev/null 2>&1
if [ $? -eq 0 ]; then
	is_doc=true
fi

# Generate the documentation (if any)
if [ "$is_doc" = true ] ; then
	# Checkout the expected tag
	git checkout $tag
	cd $top_dir/doc

	# Init virtualenv
	python3 -m venv venv
	source venv/bin/activate
	pip install -r requirements.txt

	# Generate Doc
	make latexpdf
	api_version=`grep "release = " conf.py | awk -F\' '{print $2}'`
	doc=`basename ./_build/latex/*.pdf`
	doc_ver=${doc%.pdf}_v$api_version.pdf
	mv ./_build/latex/$doc $top_dir/$doc_ver

	# Cleanup everything
	rm -rf venv
	rm -rf _build
fi


# Create an archive (note that the .gitattributes is customized to ignore unwanted files/folders)
cd $top_dir
git archive --worktree-attributes --format zip --output $project_name-$tag.zip $tag
# Add the built documentation to the archive
if [ "$is_doc" = true ] ; then
	zip -ur $project_name-$tag.zip $doc_ver
	rm -rf $doc_ver
fi
# Generate a sha1
sha1sum $project_name-$tag.zip > $project_name-$tag.zip.sha1