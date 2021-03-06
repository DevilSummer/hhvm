HHVM_DEFINE_EXTENSION("fileinfo"
  SOURCES
    ext_fileinfo.cpp
    libmagic/apprentice.cpp
    libmagic/apptype.cpp
    libmagic/ascmagic.cpp
    libmagic/cdf.cpp
    libmagic/cdf_time.cpp
    libmagic/compress.cpp
    libmagic/encoding.cpp
    libmagic/fsmagic.cpp
    libmagic/funcs.cpp
    libmagic/is_tar.cpp
    libmagic/magic.cpp
    libmagic/print.cpp
    libmagic/readcdf.cpp
    libmagic/readelf.cpp
    libmagic/softmagic.cpp
    libmagic/strlcpy.cpp
  HEADERS
    libmagic/cdf.h
    libmagic/compat.h
    libmagic/elfclass.h
    libmagic/file.h
    libmagic/magic.h
    libmagic/names.h
    libmagic/patchlevel.h
    libmagic/readelf.h
    libmagic/tar.h
  SYSTEMLIB
    ext_fileinfo.php
)