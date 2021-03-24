Puremaps Debian packaging
=========================

- Make sure to pull git submodules to get:
	path = thirdparty/geomag
	url = https://github.com/rinigus/geomag.git

  by doing `git submodule update --init`

- qttools5-dev-tools provides lconvert

pyotherside requires:
 libqt5gui5 | libqt5gui5-gles
 libqt5quick5 | libqt5quick5-gles
We need to make sure on the pinephone that we get the gles version, I guess.

All files are under the GNU GPL v3+.
(c) Sebastian Spaeth
