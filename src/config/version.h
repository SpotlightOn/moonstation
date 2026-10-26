#ifndef VERSION_H
#define VERSION_H

// What the box reports at boot and on the setup page: which firmware it runs
// and when that build was made. Two macros, no build plumbing: the version is
// bumped by hand with the release tag, the date and time are what the compiler
// was started with, which is the real build stamp of this binary.
#define FW_VERSION "0.4.1"
#define FW_BUILD_DATE __DATE__ " " __TIME__

#endif // VERSION_H
