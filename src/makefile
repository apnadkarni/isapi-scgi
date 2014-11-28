MODULENAME = isapi_scgi
MODULEMAJOR   = 0
MODULEMINOR   = 8
MODULEBUILD   = 0

# RELEASETYPE: a is alpha, b is beta
RELEASETYPE = b


################################################################
# Compiler and target environment

# Following the Tcl build style, we assume that either the vcvars32.bat
# from Visual C++ (optional) and setenv.cmd have been run
# to set up the compiler and target environment. The "official" build
# uses the Visual C++ 6.0 compiler for 32-bit builds and Platform SDK
# for Windows 2003 SP1 compiler for 64-bit builds.
# So for example, to build the 32-bit version, from the command line
#   "c:\program files\microsoft visual studio\vc98\bin\vcvars32"
#   "c:\program files\microsoft platform sdk\setenv.cmd /2000 /RETAIL
# To build 64-bit version,
#   "c:\program files\microsoft platform sdk\setenv.cmd /X64 /RETAIL

# Set the compilation platform
!ifndef MACHINE
!if "$(CPU)" == "" || "$(CPU)" == "i386"
MACHINE		= X86
!else
!if "$(CPU)" == "AMD64"
MACHINE         = AMD64
!else
!error Unsupport architecture $(CPU)
!endif
!endif
!endif

!if "$(MACHINE)" == "AMD64"
MODULE        = $(MODULENAME)64.dll
!else
!if "$(MACHINE)" == "X86"
MODULE        = $(MODULENAME).dll
!else
!error Unsupport architecture $(MACHINE)
!endif
!endif

MODULEVERSION = $(MODULEMAJOR).$(MODULEMINOR).$(MODULEBUILD)
FILE_VERSION_STR = "$(MODULEVERSION)"

# External version string as stored in a resource file
!if "x$(RELEASETYPE)" == "xa"
MODULE_VERSION_STR = "$(FILE_VERSION_STR) Alpha"
!elseif "x$(RELEASETYPE)" == "xb"
MODULE_VERSION_STR = "$(FILE_VERSION_STR) Beta"
!else
RELEASETYPE = .
MODULE_VERSION_STR = $(FILE_VERSION_STR)
!endif


MODULEDEF     = $(MODULENAME).def
INCLUDES     =  scgi.h buffer.h logger.h zlist.h

# Output directories
!IFNDEF OUTDIR
OUTDIR = build\$(MACHINE)
!ENDIF

!IFDEF DEBUG
OBJDIR = $(OUTDIR)\debug
!ELSE
OBJDIR = $(OUTDIR)\release
!ENDIF

# Make the directory
!if ![if not exist $(OBJDIR) mkdir $(OBJDIR)]
!message Output directory is $(OBJDIR)
!else
!message Output directory $(OBJDIR) exists
!endif

OBJS          = \
	$(OBJDIR)\scgi.obj \
	$(OBJDIR)\buffer.obj \
	$(OBJDIR)\logger.obj


# General tools
DEL           = del
COPY          = copy

# Location of the Visual C++ tools (32 bit assumed)


# Visual C++ version
VCVER=0
!if ![echo VCVERSION=_MSC_VER > $(OBJDIR)\vercl.x] \
    && ![cl -nologo -TC -E $(OBJDIR)\vercl.x > $(OBJDIR)\vercl.p 2>NUL]
!include $(OBJDIR)\vercl.p
!endif

!if $(VCVERSION) >= 1600
VCVER=10
!elseif $(VCVERSION) >= 1500
VCVER=9
!elseif $(VCVERSION) >= 1400
VCVER=8
!elseif $(VCVERSION) >= 1300
VCVER=7
!elseif $(VCVERSION) >= 1200
VCVER=6
!endif

!if $(VCVER) == 0
!error Could not determine compiler version
!else
!message Compiling with Visual C++ $(VCVER).
!endif

!ifdef MSSDK
SDK           = $(MSSDK)
!else
SDK           = c:\program files\microsoft platform sdk
!endif
CC            = cl.exe
LINK          = link.exe
LIBTOOL       = lib.exe
MT            = mt.exe

# INCLUDE       =  -X -I. -I"$(SDK)\include" -I"$(MSVCDIR)\include"
# INCLUDE       =  -I. -I"$(SDK)\include"


# Windows libraries
# WINLIB        = "$(SDK)\lib\kernel32.lib" "$(SDK)\lib\user32.lib" "$(SDK)\lib\ws2_32.lib"
WINLIB        = "kernel32.lib" "user32.lib" "ws2_32.lib"

!if $(VCVER) > 6 && $(VCVER) < 9
WINLIB = $(WINLIB) bufferoverflowU.lib
!endif

# Currently only using Windows libs, not linking to C runtime
LIBS          = $(WINLIB)


# C compiler flags

!IFDEF DEBUG
CDEBUG = -Z7 -Od -D_DEBUG
!ELSE
!IFDEF NOOPTIMIZE
CDEBUG = -Zi -Od
!ELSE
CDEBUG = -O1 -Zi -DNDEBUG -W3
!ENDIF
!ENDIF

!IFDEF INSTRUMENT
CINSTRUMENT = -DINSTRUMENT
!ENDIF

# Note the /Oi option which enables intrinsic functions - namely memcpy - is
# no longer used in favor of #pragma intrinsic(memcpy)
# /GF - pool strings and place into read-only memory
CFLAGS    = $(CDEBUG) /c /nologo /MD /GF /DWIN32 /D_WIN32 /D_WINDOWS -D_DLL -D_WIN32_WINNT=0x0500 -DMODULEMAJOR=$(MODULEMAJOR) -DMODULEMINOR=$(MODULEMINOR) -DMODULEBUILD=$(MODULEBUILD) $(CINSTRUMENT)

!if "$(MACHINE)" == "AMD64" && $(VCVER) < 9
CFLAGS = $(CFLAGS) /Wp64
!endif

# Linker options
LOPT      = /RELEASE /NOLOGO /MACHINE:$(MACHINE) /OPT:REF /dll /def:isapi_scgi.def

# On 64-bits, we cannot avoid the default libs since the intrinsic memcpy
# is only in effect if the number of bytes is a constant
!if "$(MACHINE)" != "AMD64"
LOPT = $(LOPT) /NODEFAULTLIB /entry:DllMain
!endif

!if $(VCVER) == 6
# VC 6 - /OPT:NOWIN98 reduces code size by using 512 byte section alignment
LOPT = $(LOPT) /debug:full /debugtype:cv /OPT:NOWIN98
!else
LOPT = $(LOPT) /debug
!endif



all: $(MODULE)

$(MODULE): $(OBJS) $(MODULEDEF) $(MODULENAME).rc makefile
        rc -DMODULEMAJOR=$(MODULEMAJOR) -DMODULEMINOR=$(MODULEMINOR) -DMODULEBUILD=$(MODULEBUILD) -DFILE_VERSION_STR=\"$(FILE_VERSION_STR)\" -DPRODUCT_VERSION_STR=\"$(MODULE_VERSION_STR)\" $(MODULENAME).rc
        $(LINK) $(LOPT) -out:"$(OBJDIR)\$(MODULE)" $(LIBS) $(OBJS) $(MODULENAME).res

$(OBJS): $(INCLUDES) makefile

distribution: $(MODULE) LICENSE
	-mkdir temp\isapi_scgi-$(MODULEVERSION)
	copy LICENSE temp\isapi_scgi-$(MODULEVERSION)
	copy $(MODULE) temp\isapi_scgi-$(MODULEVERSION)
	copy README temp\isapi_scgi-$(MODULEVERSION)
	cd temp && zip -r isapi_scgi-$(MODULEVERSION).zip isapi_scgi-$(MODULEVERSION)

clean:
        -$(DEL) *.c~ *.h~
        -cd $(OBJDIR) && $(DEL) *.obj *.lib *.exp *.pdb *.res
        -$(DEL) "$(OBJDIR)\$(MODULE)"

.c{$(OBJDIR)}.obj::
       $(CC) $(CFLAGS) -Fo$(OBJDIR)\ $<


