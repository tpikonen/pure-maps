# Define Sailfish as it is absent
%if !0%{?fedora}
%define sailfishos 1
%endif

# Prevent brp-python-bytecompile from running.
%define __os_install_post %{___build_post}

%if 0%{?sailfishos}
# "Harbour RPM packages should not provide anything."
%define __provides_exclude_from ^%{_datadir}/.*$
%define __requires_exclude ^libs2|libQMapLibreGL.*$
%endif

%if 0%{?sailfishos}
Name: harbour-pure-maps
%else
Name: pure-maps
%endif

Version: 3.3.0
Release: 1

Summary: Maps and navigation
License: GPLv3+
URL:     https://github.com/rinigus/pure-maps
Source:  %{name}-%{version}.tar.xz
Source1: apikeys.py
%if 0%{?sailfishos}
Source101:  harbour-pure-maps-rpmlintrc
%endif

BuildRequires: gettext
BuildRequires: make
BuildRequires: python(abi) > 3
BuildRequires: pkgconfig(Qt5Core)
BuildRequires: pkgconfig(Qt5Qml)
BuildRequires: pkgconfig(Qt5Quick)
BuildRequires: pkgconfig(Qt5Positioning)
BuildRequires: pkgconfig(Qt5DBus)
BuildRequires: s2geometry-devel
BuildRequires: cmake

%if !0%{?jollastore}
Requires: mapboxgl-qml >= 2.0.0
%else
BuildRequires: mapboxgl-qml >= 2.0.0
%endif

%if 0%{?sailfishos}
BuildRequires: qt5-qttools-linguist
BuildRequires: pkgconfig(sailfishapp) >= 1.0.2
Requires: libkeepalive
Requires: pyotherside-qml-plugin-python3-qt5 >= 1.5.1
Requires: qt5-qtdeclarative-import-multimedia >= 5.2
Requires: qt5-qtdeclarative-import-positioning >= 5.2
Requires: sailfishsilica-qt5
%else
BuildRequires: qt5-linguist
BuildRequires: cmake(KF5Kirigami2)
BuildRequires: pkgconfig(Qt5QuickControls2)
Requires: kf5-kirigami2
Requires: mapboxgl-qml >= 1.7.0
Requires: pyotherside
Requires: qt5-qtmultimedia
Requires: qt5-qtlocation
Requires: mimic
Requires: dbus-tools
%endif

%description
Pure Maps is a full-featured map and navigation application allowing
you to explore maps, search for addresses and points of interest, as
well as assist with navigation. For that, it uses online or offline
service providers.

PackageName: Pure Maps
Type: desktop-application
Categories:
  - Maps
  - Science
Custom:
  Repo: https://github.com/rinigus/pure-maps
Icon: https://raw.githubusercontent.com/rinigus/pure-maps/master/data/pure-maps.svg
Screenshots:
  - https://raw.githubusercontent.com/rinigus/pure-maps/master/screenshots/main.png
  - https://raw.githubusercontent.com/rinigus/pure-maps/master/screenshots/menu_sfos.png
  - https://raw.githubusercontent.com/rinigus/pure-maps/master/screenshots/traffic.png
Url:
  Donation: https://rinigus.github.io/donate

%prep
%setup -q
cp %{SOURCE1} tools/
#tools/manage-keys inject poor || true


%build

mkdir build-rpm || true

cd build-rpm

%if 0%{?sailfishos}
# SFOS RPM cmake macro disables RPATH
cmake \
    -DCMAKE_INSTALL_PREFIX:PATH=/usr \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_RPATH=%{_datadir}/%{name}/lib: \
    -DCMAKE_VERBOSE_MAKEFILE=ON \
    -DPM_VERSION='%{version}-%{release}' \
    -DAPP_ORG="io.github.rinigus" \
    -DFLAVOR=silica \
    -DUSE_BUNDLED_GPXPY=ON \
%if 0%{?jollastore}
    -DQML_IMPORT_PATH=\"%{_datadir}/%{name}/lib/qml\" \
    -DAPP_VARIANT_JOLLA_STORE=ON \
%endif
    -DPYTHON_EXE=python3 ..
%else
%cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_VERBOSE_MAKEFILE=ON \
    -DPM_VERSION='%{version}-%{release}' \
    -DFLAVOR=kirigami \
    -DUSE_BUNDLED_GPXPY=ON ..
%endif

make %{?_smp_mflags}

%install
cd build-rpm
rm -rf %{buildroot}
make DESTDIR=%{buildroot} install

%if 0%{?sailfishos}
# ship some shared libraries
mkdir -p %{buildroot}%{_datadir}/%{name}/lib
cp %{_libdir}/libs2.so %{buildroot}%{_datadir}/%{name}/lib

%if 0%{?jollastore}
mkdir -p %{buildroot}%{_datadir}/%{name}/lib/qml/MapboxMap
cp %{_libdir}/qt5/qml/MapboxMap/* %{buildroot}%{_datadir}/%{name}/lib/qml/MapboxMap
cp %{_libdir}/libQMapLibreGL.so.2* %{buildroot}%{_datadir}/%{name}/lib
sed -i 's/QtPositioning 5.3/QtPositioning 5.4/g' %{buildroot}%{_datadir}/%{name}/lib/qml/MapboxMap/MapboxMapGestureArea.qml
%endif

# strip executable bit from all libraries
chmod -x %{buildroot}%{_datadir}/%{name}/lib/*.so*
%if 0%{?jollastore}
chmod -x %{buildroot}%{_datadir}/%{name}/lib/qml/MapboxMap/*.so*
%endif

# sailfishos
%endif

%files
%defattr(-,root,root,-)
%{_bindir}/%{name}
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
%if !0%{?jollastore}
%{_datadir}/applications/%{name}-uri-handler.desktop
%endif
%{_datadir}/icons/hicolor/*/apps/%{name}.png
%if 0%{?sailfishos}
%exclude %{_datadir}/metainfo/%{name}.appdata.xml
%else
%{_datadir}/metainfo/%{name}.appdata.xml
%endif
