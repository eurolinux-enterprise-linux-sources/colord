/* config.h.  Generated from config.h.in by configure.  */
/* config.h.in.  Generated from configure.ac by autoheader.  */

/* Daemon user */
#define DAEMON_USER "root"

/* Define to 1 if translation of program messages to the user's native
   language is requested. */
#define ENABLE_NLS 1

/* enable volume search */
#define ENABLE_VOLUME_SEARCH "true"

/* foo */
#define GETTEXT_PACKAGE "colord"

/* Define to 1 if you have the MacOS X function CFLocaleCopyCurrent in the
   CoreFoundation framework. */
/* #undef HAVE_CFLOCALECOPYCURRENT */

/* Define to 1 if you have the MacOS X function CFPreferencesCopyAppValue in
   the CoreFoundation framework. */
/* #undef HAVE_CFPREFERENCESCOPYAPPVALUE */

/* Define if the GNU dcgettext() function is already present or preinstalled.
   */
#define HAVE_DCGETTEXT 1

/* Define to 1 if you have the <dlfcn.h> header file. */
#define HAVE_DLFCN_H 1

/* Use FD fallback */
#define HAVE_FD_FALLBACK 1

/* Define if the GNU gettext() function is already present or preinstalled. */
#define HAVE_GETTEXT 1

/* Use GUDEV support for hardware */
#define HAVE_GUDEV 1

/* whether gusb is available */
#define HAVE_GUSB 1

/* Define if you have the iconv() function and it works. */
/* #undef HAVE_ICONV */

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* cmsGetHeaderCreator is available */
#define HAVE_LCMS_GET_HEADER_CREATOR 1

/* cmsGetToneCurveEstimatedTableEntries is available */
#define HAVE_LCMS_GET_TABLE_ENTRIES 1

/* cmsMLUtranslationsCount is available */
#define HAVE_LCMS_MLU_TRANSLATIONS_COUNT 1

/* Define to 1 if libsystemd-login is available */
#define HAVE_LIBSYSTEMD_LOGIN 1

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* Use SANE support for detecting scanners */
/* #undef HAVE_SANE */

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Define to the sub-directory in which libtool stores uninstalled libraries.
   */
#define LT_OBJDIR ".libs/"

/* Define to 1 if your C compiler doesn't accept -c and -o together. */
/* #undef NO_MINUS_C_MINUS_O */

/* Name of package */
#define PACKAGE "colord"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "http://www.freedesktop.org/software/colord/"

/* Define to the full name of this package. */
#define PACKAGE_NAME "colord"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "colord 1.0.4"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "colord"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
#define PACKAGE_VERSION "1.0.4"

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS 1

/* Full path of colprof binary */
/* #undef TOOL_COLPROF */

/* if we should use PolicyKit */
#define USE_POLKIT 1

/* Version number of package */
#define VERSION "1.0.4"
