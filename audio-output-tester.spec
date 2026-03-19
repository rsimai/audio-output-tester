Name:           audio-output-tester
Version:        0.2.0
Release:        0
Summary:        GTK utility to test audio output devices
License:        Apache-2.0
URL:            https://build.opensuse.org/package/show/home:rsimai/audio-output-tester
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(alsa)
BuildRequires:  pkgconfig(gtk+-3.0)

%description
Audio Output Tester is a small GTK application to enumerate available
playback devices and play a short test tone on a selected device.

%prep
%autosetup

%build
%cmake
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%doc README.md
%{_bindir}/audio-device-tester

%changelog
* Thu Mar 19 2026 Robert Simai <robert.simai@suse.com>
- Add --cycle option to silently advance to the next audio output sink
  (PipeWire/Pulse or ALSA), suitable for hotkey bindings.
- Add --help / -h option to print usage information.
* Thu Mar 05 2026 Robert Simai <robert.simai@suse.com>
- Initial package for OBS home project.
