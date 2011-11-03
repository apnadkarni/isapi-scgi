MODULENAME = isapi_scgi
MODULE        = $(MODULENAME).dll
MODULEMAJOR   = 0
MODULEMINOR   = 7
MODULEBUILD   = 0

# RELEASETYPE: a is alpha, b is beta
RELEASETYPE = b

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
SRCS          = scgi.c buffer.c logger.c
INCLUDES     = 
OBJS          = $(SRCS:.c=.obj)

# General tools
DEL           = del
COPY          = copy

# Location of the Visual C++ tools (32 bit assumed)

TOOLS         = $(MSVCDIR)
!ifdef MSSDK
SDK           = $(MSSDK)
!else
SDK           = c:\program files\microsoft platform sdk
!endif
CC            = $(TOOLS)\bin\cl.exe
LINK          = $(TOOLS)\bin\link.exe
INCLUDE       =  -X -I. -I"$(SDK)\include" -I"$(TOOLS)\include"
MACHINE       = IX86

# C Library needed to build a DLL

#DLLIBC        = msvcrt.lib

# Windows libraries
WINLIB        = "$(SDK)\lib\kernel32.lib" "$(SDK)\lib\user32.lib" "$(SDK)\lib\ws2_32.lib"

LIBS          = $(WINLIB)

# Linker options
LOPT      = -debug:full -debugtype:cv /RELEASE /NOLOGO /MACHINE:$(MACHINE) -dll /OPT:NOWIN98 /nodefaultlib /entry:DllMain /def:isapi_scgi.def

# C compiler flags

!IFDEF DEBUG
CDEBUG = -Z7 -Od -D_DEBUG
!ELSE
CDEBUG = -O2 -Zi -DNDEBUG
#CDEBUG = -Z7 -Od
!ENDIF

!IFDEF INSTRUMENT
CINSTRUMENT = -DINSTRUMENT
!ENDIF

# Note the /Oi option enables intrinsic functions - namely memcpy.
# /GF - pool strings and place into read-only memory
CFLAGS    = $(CDEBUG) /c /nologo /Oi /MD /GF /DWIN32 /D_WIN32 /D_WINDOWS -D_DLL -D_WIN32_WINNT=0x0500 -DMODULEMAJOR=$(MODULEMAJOR) -DMODULEMINOR=$(MODULEMINOR) -DMODULEBUILD=$(MODULEBUILD) $(CINSTRUMENT) $(INCLUDE)

all: $(MODULE)

$(MODULE): $(OBJS) $(MODULEDEF) $(MODULENAME).rc makefile
        set LIB=$(TOOLS)\lib
        rc $(INCLUDE) -DMODULEMAJOR=$(MODULEMAJOR) -DMODULEMINOR=$(MODULEMINOR) -DMODULEBUILD=$(MODULEBUILD) -DFILE_VERSION_STR=\"$(FILE_VERSION_STR)\" -DPRODUCT_VERSION_STR=\"$(MODULE_VERSION_STR)\" $(MODULENAME).rc
        $(LINK) $(LOPT) -out:$(MODULE) $(LIBS) $(OBJS) $(MODULENAME).res

$(OBJS): $(INCLUDES) makefile

distribution: $(MODULE) LICENSE
	-mkdir temp\isapi_scgi-$(MODULEVERSION)
	copy LICENSE temp\isapi_scgi-$(MODULEVERSION)
	copy $(MODULE) temp\isapi_scgi-$(MODULEVERSION)
	copy README temp\isapi_scgi-$(MODULEVERSION)
	cd temp && zip -r isapi_scgi-$(MODULEVERSION).zip isapi_scgi-$(MODULEVERSION)

clean:
        -$(DEL) *.c~ *.h~
        -$(DEL) $(OBJS)
        -$(DEL) *.lib *.exp *.pdb *.res
        -$(DEL) $(MODULE)

.c.obj:
       $(CC) $(CFLAGS) $<

