Name:           lord-de
Version:        0.8.3
Release:        1%{?dist}
Summary:        Independent Wayland desktop derived from LordOS
License:        MIT AND BSD-2-Clause AND CC0-1.0 AND OFL-1.1 AND CC-BY-4.0 AND CC-BY-3.0-US
URL:            https://github.com/lordbasex/LordOS
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  python3
BuildRequires:  binutils
BuildRequires:  pkgconfig(wayland-server)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-protocols)
BuildRequires:  pkgconfig(xkbcommon)
BuildRequires:  pkgconfig(libseat)
BuildRequires:  pkgconfig(openssl)
BuildRequires:  libxcrypt-devel
Requires:       dbus-daemon
Requires:       xkeyboard-config
Requires:       libX11-common
Requires:       tzdata
Recommends:     pipewire-pulseaudio
Recommends:     xorg-x11-server-Xwayland
Recommends:     polkit

%description
Lord DE packages the LordOS compositor, React desktop and native applications
as an independent desktop. Host networking, packages and login are managed by
the distribution. This experimental package adds a Wayland session entry.
It does not replace the display manager or enable system services.

%prep
%autosetup

%build
# dist.sh includes the frontend built from package-lock.json. No network access
# or Node.js runtime is needed to compile or run this local RPM.
%set_build_flags
%make_build -C server PROFILE=desktop GPU=0

%install
python3 tools/desktop/install.py --prefix %{_prefix} --libexecdir %{_libexecdir} --destdir %{buildroot}

%check
%make_build -C server PROFILE=desktop GPU=0 test-platform
python3 tools/desktop/check.py

%files
%license LICENSE
%{_bindir}/lord-de-session
%{_libexecdir}/lord-de/
%{_datadir}/wayland-sessions/lord-de.desktop
%{_datadir}/doc/lord-de/
%{_datadir}/licenses/lord-de/third_party/

%changelog
* Fri Oct 23 2026 Lord DE contributors - 0.8.3-1
- Extract an independent desktop profile with a Fedora session package.
