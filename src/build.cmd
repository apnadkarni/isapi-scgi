rmdir /s /q build
cmd /c "envset x86 && nmake /s /nologo"
cmd /c "envset x64 && nmake /s /nologo"
mkdir build\isapi_scgi
copy build\x86\release\isapi_scgi.dll build\isapi_scgi
copy build\amd64\release\isapi_scgi64.dll build\isapi_scgi
copy isapi_scgi.ini build\isapi_scgi
copy ..\doc\isapi_scgi.html build\isapi_scgi
copy ..\doc\*.png build\isapi_scgi
del build\isapi_scgi.zip
cd build && zip -r isapi_scgi.zip isapi_scgi
cd ..
